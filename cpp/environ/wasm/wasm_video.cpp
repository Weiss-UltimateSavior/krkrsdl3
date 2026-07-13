//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// WASM 原生视频播放 — 基于浏览器 &lt;video&gt; 元素的实现。
//
// 使用场景：
//   - Overlay 模式：把 &lt;video&gt; 元素覆盖在 canvas 上显示
//   - Layer 模式：从 &lt;video&gt; 捕获帧并以 RGBA 像素数据输出
//
// 依赖浏览器原生解码能力（MP4/WebM/OGV），无需 FFmpeg。
//---------------------------------------------------------------------------

#include "tjsCommHead.h"
#include "PlatformVideo.h"
#include "Platform.h"
#include "TVPSystem.h"
#include "TVPStorage.h"
#include "TVPDebug.h"
#include "LayerBitmap.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <string>

// ============================================================
// JS 辅助函数
// ============================================================

// 从 WASM 内存创建一个 Blob URL 供 &lt;video&gt; 加载
EM_JS(char*, wasmVideoCreateBlob, (const char* mime, int dataAddr, int dataLen), {
    try {
        var data = new Uint8Array(Module.HEAPU8.buffer, dataAddr, dataLen);
        var blob = new Blob([data], { type: UTF8ToString(mime) });
        var url = URL.createObjectURL(blob);
        // 返回 URL 字符串（需用 free 释放）
        var len = lengthBytesUTF8(url) + 1;
        var ptr = Module._malloc(len);
        stringToUTF8(url, ptr, len);
        return ptr;
    } catch(e) {
        console.log('[wasm_video] createBlob error:', e);
        return 0;
    }
});

EM_JS(void, wasmVideoFreeBlob, (const char* url), {
    try {
        URL.revokeObjectURL(UTF8ToString(url));
    } catch(e) {}
});

// 创建 &lt;video&gt; 元素（隐藏，用于解码）
EM_JS(int, wasmVideoCreateElement, (const char* url), {
    try {
        var el = document.createElement('video');
        el.crossOrigin = 'anonymous';
        el.preload = 'auto';
        el.muted = true;   // 避免浏览器自动播放限制
        el.playsInline = true;
        el.src = UTF8ToString(url);
        el.load();
        // 存储到全局数组以便后续操作
        if (!window.__wasmVideoEls) window.__wasmVideoEls = [];
        window.__wasmVideoEls.push(el);
        var id = window.__wasmVideoEls.length - 1;
        // 监听元数据加载完成
        return id;
    } catch(e) {
        console.log('[wasm_video] createElement error:', e);
        return -1;
    }
});

// 显示/隐藏 overlay 视频元素
EM_JS(void, wasmVideoShowOverlay, (int vid, int show), {
    try {
        var el = window.__wasmVideoEls[vid];
        if (!el) return;
        if (show) {
            el.style.position = 'fixed';
            el.style.top = '0';
            el.style.left = '0';
            el.style.width = '100vw';
            el.style.height = '100vh';
            el.style.objectFit = 'contain';
            el.style.zIndex = '999';
            el.style.pointerEvents = 'none';
            el.style.background = 'transparent';
            document.body.appendChild(el);
        } else {
            if (el.parentNode) el.parentNode.removeChild(el);
        }
    } catch(e) {
        console.log('[wasm_video] showOverlay error:', e);
    }
});

EM_JS(void, wasmVideoPlay, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; if (el) el.play(); } catch(e) {}
});

EM_JS(void, wasmVideoPause, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; if (el) el.pause(); } catch(e) {}
});

EM_JS(void, wasmVideoStop, (int vid), {
    try {
        var el = window.__wasmVideoEls[vid];
        if (el) { el.pause(); el.currentTime = 0; }
    } catch(e) {}
});

EM_JS(double, wasmVideoGetDuration, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? el.duration : 0; } catch(e) { return 0; }
});

EM_JS(double, wasmVideoGetCurrentTime, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? el.currentTime : 0; } catch(e) { return 0; }
});

EM_JS(void, wasmVideoSetCurrentTime, (int vid, double t), {
    try { var el = window.__wasmVideoEls[vid]; if (el) el.currentTime = t; } catch(e) {}
});

EM_JS(int, wasmVideoGetReadyState, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? el.readyState : 0; } catch(e) { return 0; }
});

EM_JS(int, wasmVideoGetEnded, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? (el.ended ? 1 : 0) : 0; } catch(e) { return 0; }
});

EM_JS(int, wasmVideoGetWidth, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? (el.videoWidth || 0) : 0; } catch(e) { return 0; }
});

EM_JS(int, wasmVideoGetHeight, (int vid), {
    try { var el = window.__wasmVideoEls[vid]; return el ? (el.videoHeight || 0) : 0; } catch(e) { return 0; }
});

// 从 &lt;video&gt; 捕获当前帧到 RGBA 缓冲区（layer 模式用）
EM_JS(int, wasmVideoCaptureFrame, (int vid, int rgbaAddr, int bufSize), {
    try {
        var el = window.__wasmVideoEls[vid];
        if (!el || !el.videoWidth || !el.videoHeight) return 0;
        var w = el.videoWidth, h = el.videoHeight;
        var needed = w * h * 4;
        if (needed > bufSize) return 0;
        // 使用离屏 canvas 绘制视频帧
        if (!window.__wasmVideoCanvas) {
            window.__wasmVideoCanvas = document.createElement('canvas');
            window.__wasmVideoCtx = window.__wasmVideoCanvas.getContext('2d');
        }
        window.__wasmVideoCanvas.width = w;
        window.__wasmVideoCanvas.height = h;
        if (el.readyState < 1) return 0;
        window.__wasmVideoCtx.drawImage(el, 0, 0, w, h);
        var imageData = window.__wasmVideoCtx.getImageData(0, 0, w, h);
        var dst = new Uint8Array(Module.HEAPU8.buffer, rgbaAddr, needed);
        dst.set(imageData.data);
        return 1;
    } catch(e) {
        console.log('[wasm_video] captureFrame error:', e);
        return 0;
    }
});

// ============================================================
// WasmVideoPlayer — 同时实现 OverlayVideoPlayer 与 LayerVideoPlayer
// ============================================================
class WasmVideoPlayer : public OverlayVideoPlayer, public LayerVideoPlayer
{
    uint32_t ref = 1;

    // 视频数据
    uint8_t* streamData = nullptr;
    size_t streamSize = 0;
    char* blobUrl = nullptr;
    char* mimeType = nullptr;

    // 视频元素 ID
    int videoId = -1;

    // 视频信息
    int vWidth = 0, vHeight = 0;
    double duration = 0;

    // 播放状态
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> ended{false};
    std::atomic<int> pendingSeek{-1}; // ms

    // Layer 模式输出缓冲
    tTVPBaseTexture* bmp[2] = {};
    long bmpSize = 0;
    std::vector<uint8_t> rgbaScratch;

    // Overlay 模式标记
    bool overlayVisible = false;

public:
    WasmVideoPlayer() {}
    ~WasmVideoPlayer() { Close(); }

    void Close()
    {
        playing = false; paused = false; ended = true;
        if (videoId >= 0) {
            wasmVideoStop(videoId);
            wasmVideoShowOverlay(videoId, 0);
        }
        if (blobUrl) { wasmVideoFreeBlob(blobUrl); free(blobUrl); blobUrl = nullptr; }
        if (mimeType) { free(mimeType); mimeType = nullptr; }
        delete[] streamData; streamData = nullptr;
    }

    // ── 工具：从流名推断 MIME ──────────────────────────────
    static const char* GuessMime(const ttstr& streamname)
    {
        ttstr ext = TVPExtractStorageExt(streamname);
        ext.ToLowerCase();
        if (ext == TJS_N(".mp4"))  return "video/mp4";
        if (ext == TJS_N(".webm")) return "video/webm";
        if (ext == TJS_N(".ogv"))  return "video/ogg";
        if (ext == TJS_N(".mkv"))  return "video/x-matroska";
        if (ext == TJS_N(".avi"))  return "video/avi";
        if (ext == TJS_N(".mov"))  return "video/quicktime";
        if (ext == TJS_N(".mpg") || ext == TJS_N(".mpeg")) return "video/mpeg";
        return "video/mp4";
    }

    // ── OpenStream ──────────────────────────────────────────
    bool OpenStream(TJS::tTJSBinaryStream* stream, const ttstr& streamname) override
    {
        // 读取全部流数据到内存
        streamSize = (size_t)stream->GetSize();
        if (streamSize == 0 || streamSize > 200 * 1024 * 1024) {
            TVPAddLog(ttstr(TJS_W("wasm_video: stream too large or empty")));
            return false;
        }
        streamData = new uint8_t[streamSize];
        stream->SetPosition(0);
        size_t total = 0;
        while (total < streamSize) {
            tjs_uint n = stream->Read(streamData + total, (tjs_uint)(streamSize - total));
            if (n <= 0) break;
            total += n;
        }
        if (total != streamSize) {
            TVPAddLog(ttstr(TJS_W("wasm_video: failed to read entire stream")));
            delete[] streamData; streamData = nullptr;
            return false;
        }

        // 推断 MIME 类型
        const char* mime = GuessMime(streamname);
        mimeType = (char*)malloc(strlen(mime) + 1);
        strcpy(mimeType, mime);

        // 创建 Blob URL
        blobUrl = (char*)wasmVideoCreateBlob(mime, (int)(uintptr_t)streamData, (int)streamSize);
        if (!blobUrl) {
            TVPAddLog(ttstr(TJS_W("wasm_video: failed to create blob URL")));
            delete[] streamData; streamData = nullptr;
            return false;
        }

        // 创建视频元素
        videoId = wasmVideoCreateElement(blobUrl);
        if (videoId < 0) {
            TVPAddLog(ttstr(TJS_W("wasm_video: failed to create video element")));
            wasmVideoFreeBlob(blobUrl); free(blobUrl); blobUrl = nullptr;
            delete[] streamData; streamData = nullptr;
            return false;
        }

        // 等待元数据加载完成
        // 用轮询方式等待 readyState >= 1 (HAVE_METADATA)
        for (int i = 0; i < 200; i++) {
            if (wasmVideoGetReadyState(videoId) >= 1) break;
            emscripten_sleep(50);
        }

        vWidth = wasmVideoGetWidth(videoId);
        vHeight = wasmVideoGetHeight(videoId);
        duration = wasmVideoGetDuration(videoId);

        TVPAddLog(ttstr(TJS_W("wasm_video: opened ")) + streamname +
                  ttstr(TJS_W(" (")) + ttstr((int)vWidth) + ttstr(TJS_W("x")) + ttstr((int)vHeight) +
                  ttstr(TJS_W(", ")) + ttstr((int)duration) + ttstr(TJS_W("s)")));

        if (vWidth > 0 && vHeight > 0)
            rgbaScratch.resize(vWidth * vHeight * 4);

        return true;
    }

    // ── iTVPVideoOverlay ────────────────────────────────────
    void AddRef() override { ref++; }
    void Release() override { if (--ref == 0) delete this; }

    void SetWindow(class tTJSNI_Window*) override {}
    void SetMessageDrainWindow(void*) override {}
    void SetRect(int l, int t, int r, int b) override {}
    void SetVisible(bool v) override {}

    void Play() override
    {
        if (videoId < 0) return;
        if (wasmVideoGetEnded(videoId)) {
            wasmVideoSetCurrentTime(videoId, 0);
        }
        wasmVideoPlay(videoId);
        wasmVideoShowOverlay(videoId, 1);
        overlayVisible = true;
        playing = true; paused = false; ended = false;
    }

    void Stop() override
    {
        if (videoId < 0) return;
        wasmVideoStop(videoId);
        wasmVideoShowOverlay(videoId, 0);
        overlayVisible = false;
        playing = false; paused = false; ended = true;
    }

    void Pause() override
    {
        if (videoId < 0) return;
        wasmVideoPause(videoId);
        paused = true; playing = false;
    }

    void SetPosition(uint64_t tick) override
    {
        if (videoId < 0) return;
        // tick in ms -> seconds
        wasmVideoSetCurrentTime(videoId, tick / 1000.0);
    }

    void GetPosition(uint64_t* tick) override
    {
        if (!tick) return;
        if (videoId < 0) { *tick = 0; return; }
        *tick = (uint64_t)(wasmVideoGetCurrentTime(videoId) * 1000.0);
    }

    void GetStatus(tTVPVideoStatus* s) override
    {
        if (!s) return;
        if (videoId < 0) { *s = vsStopped; return; }
        if (wasmVideoGetEnded(videoId)) { *s = vsEnded; return; }
        if (paused) { *s = vsPaused; return; }
        if (playing) { *s = vsPlaying; return; }
        *s = vsStopped;
    }

    void Rewind() override
    {
        if (videoId >= 0) wasmVideoSetCurrentTime(videoId, 0);
        ended = false;
    }

    void SetFrame(int f) override
    {
        // 近似帧定位
        double fps = 30.0;
        if (videoId >= 0)
            wasmVideoSetCurrentTime(videoId, f / fps);
    }

    void GetFrame(int* f) override
    {
        if (!f) return;
        double t = (videoId >= 0) ? wasmVideoGetCurrentTime(videoId) : 0;
        *f = (int)(t * 30.0); // 近似 30fps
    }

    void GetFPS(double* fps) override { if (fps) *fps = 30.0; }

    void GetNumberOfFrame(int* n) override
    {
        if (!n) return;
        if (duration > 0) *n = (int)(duration * 30.0);
        else *n = 0;
    }

    void GetTotalTime(int64_t* t) override
    {
        if (!t) return;
        *t = (int64_t)(duration * 1000.0);
    }

    void GetVideoSize(long* w, long* h) override
    {
        if (w) *w = vWidth;
        if (h) *h = vHeight;
    }

    tTVPBaseTexture* GetFrontBuffer() override
    {
        if (videoId < 0 || vWidth <= 0 || vHeight <= 0) return nullptr;
        // 捕获当前帧到 scratch buffer
        int capOk = wasmVideoCaptureFrame(videoId,
            (int)(uintptr_t)rgbaScratch.data(),
            (int)rgbaScratch.size());
        if (!capOk) return nullptr;
        // 更新纹理
        if (bmp[0]) {
            bmp[0]->Update(rgbaScratch.data(), vWidth * 4, 0, 0, vWidth, vHeight);
            return bmp[0];
        }
        return nullptr;
    }

    void SetVideoBuffer(tTVPBaseTexture* b1, tTVPBaseTexture* b2, long size) override
    {
        bmp[0] = b1; bmp[1] = b2; bmpSize = size;
    }

    void OnContinuousCallback(tjs_uint64 tick) override
    {
        // 检查视频是否结束
        if (playing && videoId >= 0 && wasmVideoGetEnded(videoId)) {
            playing = false;
            ended = true;
            if (overlayVisible) {
                wasmVideoShowOverlay(videoId, 0);
                overlayVisible = false;
            }
        }
    }

    void SetStopFrame(int) override {}
    void GetStopFrame(int* f) override { if (f) *f = 0; }
    void SetDefaultStopFrame() override {}
    void SetPlayRate(double) override {}
    void GetPlayRate(double* r) override { if (r) *r = 1.0; }
    void SetAudioBalance(long) override {}
    void GetAudioBalance(long* b) override { if (b) *b = 0; }
    void SetAudioVolume(long) override {}
    void GetAudioVolume(long* v) override { if (v) *v = 100; }
    void GetNumberOfAudioStream(unsigned long* c) override { if (c) *c = 1; }
    void SelectAudioStream(unsigned long) override {}
    void GetEnableAudioStreamNum(long* n) override { if (n) *n = 0; }
    void DisableAudioStream() override {}
    void GetNumberOfVideoStream(unsigned long* c) override { if (c) *c = 1; }
    void SelectVideoStream(unsigned long) override {}
    void GetEnableVideoStreamNum(long* n) override { if (n) *n = 1; }
    void SetLoopSegement(int, int) override {}
    void SetMixingBitmap(class tTVPBaseTexture* dest, float alpha) override {}
    void ResetMixingBitmap() override {}
    void SetMixingMovieAlpha(float) override {}
    void GetMixingMovieAlpha(float* a) override { if (a) *a = 1.0f; }
    void SetMixingMovieBGColor(unsigned long) override {}
    void GetMixingMovieBGColor(unsigned long* c) override { if (c) *c = 0xFF000000; }
    void PresentVideoImage() override {}

    void GetContrastRangeMin(float* v) override { if (v) *v = -1.0f; }
    void GetContrastRangeMax(float* v) override { if (v) *v = 1.0f; }
    void GetContrastDefaultValue(float* v) override { if (v) *v = 0.0f; }
    void GetContrastStepSize(float* v) override { if (v) *v = 0.01f; }
    void GetContrast(float* v) override { if (v) *v = 0.0f; }
    void SetContrast(float) override {}

    void GetBrightnessRangeMin(float* v) override { if (v) *v = -1.0f; }
    void GetBrightnessRangeMax(float* v) override { if (v) *v = 1.0f; }
    void GetBrightnessDefaultValue(float* v) override { if (v) *v = 0.0f; }
    void GetBrightnessStepSize(float* v) override { if (v) *v = 0.01f; }
    void GetBrightness(float* v) override { if (v) *v = 0.0f; }
    void SetBrightness(float) override {}

    void GetHueRangeMin(float* v) override { if (v) *v = -180.0f; }
    void GetHueRangeMax(float* v) override { if (v) *v = 180.0f; }
    void GetHueDefaultValue(float* v) override { if (v) *v = 0.0f; }
    void GetHueStepSize(float* v) override { if (v) *v = 1.0f; }
    void GetHue(float* v) override { if (v) *v = 0.0f; }
    void SetHue(float) override {}

    void GetSaturationRangeMin(float* v) override { if (v) *v = -1.0f; }
    void GetSaturationRangeMax(float* v) override { if (v) *v = 1.0f; }
    void GetSaturationDefaultValue(float* v) override { if (v) *v = 0.0f; }
    void GetSaturationStepSize(float* v) override { if (v) *v = 0.01f; }
    void GetSaturation(float* v) override { if (v) *v = 0.0f; }
    void SetSaturation(float) override {}
};

// ============================================================
// Factory functions
// ============================================================
OverlayVideoPlayer* CreateOverlayVideoPlayer() { return new WasmVideoPlayer(); }
LayerVideoPlayer*    CreateLayerVideoPlayer()    { return new WasmVideoPlayer(); }
