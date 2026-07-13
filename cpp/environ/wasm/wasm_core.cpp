// WASM platform core implementation
// Follows the same pattern as windows_core.cpp / android_core.cpp / linux_core.cpp

#include "tjsCommHead.h"

#include "Platform.h"
#include "PlatformFile.h"
#include "PlatformVideo.h"
#include "TVPMsg.h"
#include "TVPApplication.h"
#include "tjsError.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <sys/stat.h>
#include <unistd.h>

//---------------------------------------------------------------------------
// IDBFS: sync save data to IndexedDB for persistence
//---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
EM_JS(void, wasmSyncIDBFS_JS, (), {
    try {
        if (typeof Module === 'undefined' || !Module.FS) return;
        Module.FS.syncfs(false, function(err) {
            if (err) console.log('[idbfs] sync error:', err);
        });
    } catch(e) {
        console.log('[idbfs] sync exception:', e);
    }
});

void wasmSyncSaveData()
{
    // Only sync from the main browser thread (where IDBFS is mounted)
    if (emscripten_is_main_browser_thread())
        wasmSyncIDBFS_JS();
}
#endif
//---------------------------------------------------------------------------
// tTVPFileMedia is now in wasm_file.cpp
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
static tjs_int TVPCPUType = 0;
static tjs_int TVPCPUFeatures = 0;
static bool TVPCPUChecked = false;
//---------------------------------------------------------------------------
void TVPGetCPUInfo(tjs_int& cpuType, tjs_int& cpuFeatures)
{
    cpuType = TVPCPUType;
    cpuFeatures = TVPCPUFeatures;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
bool TVP_utime(const char* name, time_t modtime)
{
    // WASM: virtual filesystem does not support utime well; skip.
    return true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPGetMemoryInfo(TVPMemoryInfo& m)
{
    m.MemTotal = 256 * 1024;     // KB, approximate
    m.MemFree = 128 * 1024;
    m.SwapTotal = 0;
    m.SwapFree = 0;
    m.VirtualTotal = 512 * 1024;
    m.VirtualUsed = 0;
}
tjs_int TVPGetSystemFreeMemory()
{
    return 128;
}
tjs_int TVPGetSelfUsedMemory()
{
    return 0;
}
void TVPRelinquishCPU()
{
#if defined(__EMSCRIPTEN_PTHREADS__)
    sched_yield();
#else
    emscripten_sleep(0);
#endif
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
std::string TVPGetPackageVersionString()
{
    return "wasm";
}
ttstr TVPGetOSName()
{
    return TJS_N("WASM/Emscripten");
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPPreNormalizeStorageName(ttstr& name)
{
    // if the name is an OS's native expression, change it according
    // with the TVP storage system naming rule.
    tjs_int namelen = name.GetLen();
    if (namelen == 0)
        return;
    if (namelen >= 1)
    {
        if (name[0] == TJS_N('/'))
        {
            name = ttstr(TJS_N("file://.")) + name;
            return;
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPInvokeMenu(int x, int y, void* menu)
{
    // WASM: no native menu support
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTVPMemoryStream* GetResourceStream(const ttstr& filename)
{
    // WASM: resources (fonts, etc.) are preloaded to /Res/
    ttstr path(TJS_N("/Res/"));
    path += filename;
    // Check existence first, then open
    if (!TVPIsExistentStorageNoSearch(path))
        return nullptr;
    tTJSBinaryStream* tmp = TVPCreateBinaryStreamForRead(path, TJS_N(""));
    if (!tmp) return nullptr;
    tTVPMemoryStream* ret = new tTVPMemoryStream(nullptr, tmp->GetSize());
    tmp->ReadBuffer(ret->GetInternalBuffer(), tmp->GetSize());
    delete tmp;
    return ret;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Video player factories — implemented in wasm_video.cpp (HTML5 &lt;video&gt;)
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
bool TVPTruncateFile(const std::string& path, size_t size)
{
    FILE* f = fopen(path.c_str(), "rb+");
    if (!f) return false;
    // Emscripten virtual FS doesn't support ftruncate; just close and return
    fclose(f);
    return true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
uint16_t TVPGetFileAttributes(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0xFFFF;
    uint16_t attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 0x10;
    if (!(st.st_mode & S_IWUSR)) attr |= 0x01;
    return attr;
}
//---------------------------------------------------------------------------
bool TVPSetFileAttributes(const std::string& path, uint16_t attr, uint16_t mask)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    mode_t mode = st.st_mode;
    if (mask & 0x01)
    {
        if (attr & 0x01)
            mode &= ~S_IWUSR;
        else
            mode |= S_IWUSR;
    }
    return chmod(path.c_str(), mode) == 0;
}
//---------------------------------------------------------------------------
