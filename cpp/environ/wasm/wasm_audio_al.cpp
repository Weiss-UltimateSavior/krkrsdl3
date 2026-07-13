//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// WASM OpenAL 音频后端 — OpenAL 映射到 Web Audio API，独立音频线程混音。
//---------------------------------------------------------------------------

#include "tjsCommHead.h"
#include "PlatformAudio.h"
#include "PlatformMutex.h"
#include "PlatformThread.h"
#include "TVPSystem.h"
#include "TickCount.h"
#include "TVPDebug.h"
#include "WaveIntf.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <AL/al.h>
#include <AL/alc.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <vector>

//---------------------------------------------------------------------------
// OpenAL 设备与上下文
//---------------------------------------------------------------------------
static ALCdevice* g_alDevice = nullptr;
static ALCcontext* g_alContext = nullptr;
static bool g_alInited = false;

static bool InitOpenAL()
{
    if (g_alInited) return true;
    g_alDevice = alcOpenDevice(nullptr);
    if (!g_alDevice) { TVPAddLog(ttstr(TJS_W("openal: alcOpenDevice failed"))); return false; }
    g_alContext = alcCreateContext(g_alDevice, nullptr);
    if (!g_alContext) { alcCloseDevice(g_alDevice); g_alDevice = nullptr; TVPAddLog(ttstr(TJS_W("openal: alcCreateContext failed"))); return false; }
    alcMakeContextCurrent(g_alContext);
    g_alInited = true;
    TVPAddLog(ttstr(TJS_W("openal: initialized OK")));
    return true;
}

//---------------------------------------------------------------------------
// tTVPSoundBufferWASM — OpenAL 音源
//---------------------------------------------------------------------------
#ifndef TVP_WSB_ACCESS_FREQ
#define TVP_WSB_ACCESS_FREQ 8
#endif

class tTVPSoundBufferWASM : public iTVPSoundBuffer
{
public:
    bool _playing = false;
    float _volume = 1.0f, _pan = 0.0f;

    tTVPWaveFormat _format;
    int _frame_size = 0, _bytesPerSample = 0;

    ALuint _alSource = 0;
    ALenum _alFormat = AL_FORMAT_STEREO16;
    ALuint* _bufferIds = nullptr;
    ALuint* _unqueueBuf = nullptr;
    tjs_uint* _bufferSize = nullptr;
    tjs_uint _bufferCount = 0;
    int _bufferIdx = -1;
    int _sendedSamples = 0;
    tTJSCriticalSection _mtx;

    tTVPSoundBufferWASM(tTVPWaveFormat& fmt, int bufcount)
      : _frame_size(fmt.BytesPerSample * fmt.Channels),
        _bufferCount(bufcount > 0 ? bufcount : 4)
    {
        _format = fmt;
        _bytesPerSample = fmt.BytesPerSample;

        // 先分配内存，Init() 中创建 OpenAL 对象
        _bufferIds = new ALuint[_bufferCount];
        _unqueueBuf = new ALuint[_bufferCount];
        _bufferSize = new tjs_uint[_bufferCount];
        memset(_bufferSize, 0, sizeof(tjs_uint) * _bufferCount);

        if (fmt.Channels == 1)
            _alFormat = (fmt.BitsPerSample == 16) ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8;
        else
            _alFormat = (fmt.BitsPerSample == 16) ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8;
    }

    virtual ~tTVPSoundBufferWASM()
    {
        Stop();
        if (_alSource) {
            if (alcGetCurrentContext() != g_alContext) alcMakeContextCurrent(g_alContext);
            alDeleteSources(1, &_alSource);
        }
        if (_bufferIds) {
            if (alcGetCurrentContext() != g_alContext) alcMakeContextCurrent(g_alContext);
            alDeleteBuffers(_bufferCount, _bufferIds);
        }
        delete[] _bufferIds;
        delete[] _unqueueBuf;
        delete[] _bufferSize;
    }

    virtual bool Init() override
    {
        if (_format.BitsPerSample != 16 && _format.BitsPerSample != 8)
        {
            TVPAddLog(ttstr(TJS_W("openal: unsupported bits: ")) + ttstr((int)_format.BitsPerSample));
            delete this; return false;
        }
        if (!InitOpenAL()) { delete this; return false; }

        // 确保 OpenAL 上下文在当前线程生效
        if (alcGetCurrentContext() != g_alContext)
            alcMakeContextCurrent(g_alContext);

        alGenSources(1, &_alSource);
        alGenBuffers(_bufferCount, _bufferIds);
        alSourcef(_alSource, AL_GAIN, 1.0f);

        TVPAddLog(ttstr(TJS_W("openal: Init() freq=")) + ttstr((int)_format.SamplesPerSec)
                  + ttstr(TJS_W(" ch=")) + ttstr((int)_format.Channels)
                  + ttstr(TJS_W(" bits=")) + ttstr((int)_format.BitsPerSample));
        return true;
    }

    virtual void Release() override { delete this; }

    virtual void Play() override
    {
        if (!_alSource) return;
        ALenum state; alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(_alSource);
        _playing = true;
    }

    virtual void Pause() override { if (_alSource) alSourcePause(_alSource); _playing = false; }

    virtual void Stop() override
    {
        _playing = false;
        if (_alSource) { alSourceStop(_alSource); alSourcei(_alSource, AL_BUFFER, 0); }
        _bufferIdx = -1; _sendedSamples = 0;
    }

    virtual void Reset() override
    {
        if (_alSource) { alSourceRewind(_alSource); alSourcei(_alSource, AL_BUFFER, 0); }
        _bufferIdx = -1; _sendedSamples = 0;
    }

    virtual bool IsPlaying() override
    {
        if (!_alSource) return false;
        ALenum state; alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        return state == AL_PLAYING;
    }

    virtual void SetVolume(float v) override { _volume = v; if (_alSource) alSourcef(_alSource, AL_GAIN, v); }
    virtual float GetVolume() override { return _volume; }

    virtual void SetPan(float v) override
    {
        _pan = v;
        if (_alSource) { float pos[] = { v, 0.0f, 0.0f }; alSourcefv(_alSource, AL_POSITION, pos); }
    }
    virtual float GetPan() override { return _pan; }

    virtual void AppendBuffer(const void* buf, unsigned int len) override
    {
        if (!buf || len <= 0 || !_alSource) return;
        tTJSCSH lk(_mtx);

        // 确保当前线程有 OpenAL 上下文
        if (alcGetCurrentContext() != g_alContext)
            alcMakeContextCurrent(g_alContext);

        // 回收已处理完的缓冲区
        ALint processed = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0)
        {
            alSourceUnqueueBuffers(_alSource, processed, _unqueueBuf);
            for (int i = 0; i < processed; ++i)
                for (tjs_uint j = 0; j < _bufferCount; ++j)
                    if (_bufferIds[j] == _unqueueBuf[i])
                    { _sendedSamples += _bufferSize[j] / _frame_size; break; }
        }

        // 检查队列是否已满
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        if (queued >= (ALint)_bufferCount) return;

        ++_bufferIdx;
        if (_bufferIdx >= (int)_bufferCount) _bufferIdx = 0;
        ALuint bufid = _bufferIds[_bufferIdx];
        alBufferData(bufid, _alFormat, buf, len, _format.SamplesPerSec);
        alSourceQueueBuffers(_alSource, 1, &bufid);
        _bufferSize[_bufferIdx] = len;

        // 确保 Source 处于播放状态（如果已停止则重启，避免间隙）
        if (_playing) {
            ALenum state;
            alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING)
                alSourcePlay(_alSource);
        }
    }

    virtual bool IsBufferValid() override
    {
        if (!_alSource) return false;
        ALint processed = 0, queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        if (processed > 0) return true;
        return queued < (ALint)_bufferCount;
    }

    virtual bool IsValidFormat(tTVPWaveFormat& fmt) override
    {
        return _format.SamplesPerSec == fmt.SamplesPerSec &&
               _format.BitsPerSample == fmt.BitsPerSample &&
               _format.Channels == fmt.Channels;
    }

    virtual tjs_uint GetCurrentPlaySamples() override
    {
        if (!_alSource) return 0;
        ALint offset = 0;
        alGetSourcei(_alSource, AL_SAMPLE_OFFSET, &offset);
        return _sendedSamples + offset;
    }

    virtual int GetRemainBuffers() override
    {
        if (!_alSource) return 0;
        ALint processed = 0, queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        int remain = queued - processed;
        if (remain < 0) remain = 0;
        return remain;
    }

    virtual tjs_uint GetLatencySamples() override
    {
        tTJSCSH lk(_mtx);
        if (!_alSource) return 0;
        ALint offset = 0, queued = 0;
        alGetSourcei(_alSource, AL_BYTE_OFFSET, &offset);
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
        int remainBuffers = queued;
        if (remainBuffers <= 0) return 0;
        int total = -offset;
        for (int i = 0; i < remainBuffers; ++i)
        {
            int idx = _bufferIdx + 1 - remainBuffers + i;
            if (idx >= (int)_bufferCount) idx -= _bufferCount;
            else if (idx < 0) idx += _bufferCount;
            total += _bufferSize[idx];
        }
        return total / _frame_size;
    }

    virtual float GetLatencySeconds() override { return (float)GetLatencySamples() / _format.SamplesPerSec; }

    virtual void SetPosition(float x, float y, float z) override
    {
        if (!_alSource) return;
        float pos[] = { x, y, z };
        alSourcefv(_alSource, AL_POSITION, pos);
    }
};

//---------------------------------------------------------------------------
// 全局初始化
//---------------------------------------------------------------------------
static bool g_devInited = false;

void TVPInitDirectSound(int freq)
{
    if (!g_devInited) { g_devInited = true; TVPAddLog(ttstr(TJS_W("openal: TVPInitDirectSound"))); InitOpenAL(); }
}

void TVPUninitDirectSound()
{
    if (alcGetCurrentContext() != g_alContext)
        alcMakeContextCurrent(g_alContext);
    if (g_alContext) { alcMakeContextCurrent(nullptr); alcDestroyContext(g_alContext); g_alContext = nullptr; }
    if (g_alDevice) { alcCloseDevice(g_alDevice); g_alDevice = nullptr; }
    g_alInited = false;
}

iTVPSoundBuffer* TVPCreateSoundBuffer(tTVPWaveFormat& fmt, int bufcount)
{
    tTVPSoundBufferWASM* buf = new tTVPSoundBufferWASM(fmt, bufcount);
    return (buf && buf->Init()) ? buf : nullptr;
}
