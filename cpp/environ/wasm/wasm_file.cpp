// WASM platform file I/O
// Uses MEMFS with preloaded files (downloaded via async fetch in HTML shell).

#include "tjsCommHead.h"
#include "PlatformFile.h"
#include "TVPStorage.h"
#include "TVPMsg.h"
#include "UtilStreams.h"

#include <emscripten.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <string>

class tTVPFileMedia : public iTVPStorageMedia
{
    tjs_uint RefCount;
public:
    tTVPFileMedia() { RefCount = 1; }
    ~tTVPFileMedia() { ; }
    void AddRef() { RefCount++; }
    void Release() { if (RefCount == 1) delete this; else RefCount--; }
    void GetName(ttstr& name) { name = TJS_N("file"); }
    void NormalizeDomainName(ttstr& name);
    void NormalizePathName(ttstr& name);
    bool CheckExistentStorage(const ttstr& name);
    tTJSBinaryStream* Open(const ttstr& name, tjs_uint32 flags);
    void GetListAt(const ttstr& name, iTVPStorageLister* lister);
    void GetLocallyAccessibleName(ttstr& name);
};

void tTVPFileMedia::NormalizeDomainName(ttstr& name)
{
    tjs_char* p = name.Independ();
    while (*p) { if (*p >= TJS_N('A') && *p <= TJS_N('Z')) *p += TJS_N('a') - TJS_N('A'); p++; }
}

void tTVPFileMedia::NormalizePathName(ttstr& name)
{
    // WASM virtual FS is case-sensitive; do not lowercase
}

bool tTVPFileMedia::CheckExistentStorage(const ttstr& name)
{
    if (name.IsEmpty()) return false;
    ttstr _name(name);
    GetLocallyAccessibleName(_name);
    if (_name.IsEmpty()) return false;
    FILE* f = fopen(_name.AsStdString().c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

tTJSBinaryStream* tTVPFileMedia::Open(const ttstr& name, tjs_uint32 flags)
{
    if (name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, TJS_N("\"\""));
    ttstr origname = name;
    ttstr _name(name);
    GetLocallyAccessibleName(_name);
    return new tTVPLocalFileStream(origname, _name, flags);
}

void tTVPFileMedia::GetListAt(const ttstr& _name, iTVPStorageLister* lister)
{
    ttstr name(_name);
    GetLocallyAccessibleName(name);
    TVPGetLocalFileListAt(name,
        [lister](const ttstr& name, tTVPLocalFileInfo* s) {
            if (s->Mode & (S_IFREG)) lister->Add(name);
        });
}

void tTVPFileMedia::GetLocallyAccessibleName(ttstr& name)
{
    ttstr newname;
    const tjs_char* ptr = name.c_str();
    if (TJS_strncmp(ptr, TJS_N("file://./"), 9) == 0) {
        ptr += 9;
        if (ptr[1] == TJS_N(':') && ptr[2] == TJS_N('/')) ptr += 3;
        newname = TJS_N("/") + ttstr(ptr);
    } else if (ptr[0] == TJS_N('.') && (ptr[1] == TJS_N('/') || ptr[1] == 0)) {
        newname = TJS_N("/") + ttstr(ptr + (ptr[1] == TJS_N('/') ? 2 : 1));
    } else if (ptr[0] != TJS_N('/')) {
        newname = TJS_N("/") + ttstr(ptr);
    } else {
        newname = name;
    }
    tjs_char* pp = newname.Independ();
    while (*pp) { if (*pp == TJS_N('\\')) *pp = TJS_N('/'); pp++; }
    name = newname;
}

iTVPStorageMedia* TVPCreateFileMedia()
{
    return new tTVPFileMedia;
}
