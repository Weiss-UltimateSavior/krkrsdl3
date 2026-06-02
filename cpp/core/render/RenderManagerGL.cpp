//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//!@file OpenGL 渲染管理器实现
//---------------------------------------------------------------------------

#include "tjsCommHead.h"
#include "RenderManagerGL.h"
#include "RenderManager.h"
#include "TVPMsg.h"
#include "TVPDebug.h"
#include "LayerBitmap.h"
#include "TVPSystem.h"
#include "TVPThread.h"

#include <glm/glm.hpp>
#include <SDL3/SDL.h>

//---------------------------------------------------------------------------
// 工具函数：编译 shader
//---------------------------------------------------------------------------
static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetShaderInfoLog(shader, 1024, nullptr, log);
        SDL_Log("GL RenderManager: Shader compile error (%s): %s",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    }
    return shader;
}

static GLuint LinkProgram(GLuint vs, GLuint fs)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[1024];
        glGetProgramInfoLog(prog, 1024, nullptr, log);
        SDL_Log("GL RenderManager: Program link error: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

//---------------------------------------------------------------------------
// 全屏四边形 VAO/VBO（静态成员初始化）
//---------------------------------------------------------------------------
GLuint tTVPGLRenderMethod::sFullScreenVAO = 0;
GLuint tTVPGLRenderMethod::sFullScreenVBO = 0;
int tTVPGLRenderMethod::sInitCount = 0;

void tTVPGLRenderMethod::EnsureFullScreenQuad()
{
    if (sFullScreenVAO != 0)
        return;

    float vertices[] = {
        // 位置        // 纹理坐标
        -1.0f, -1.0f, 0.0f, 0.0f, // 左下
         1.0f, -1.0f, 1.0f, 0.0f, // 右下
         1.0f,  1.0f, 1.0f, 1.0f, // 右上
        -1.0f,  1.0f, 0.0f, 1.0f, // 左上
    };
    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0
    };

    glGenVertexArrays(1, &sFullScreenVAO);
    glGenBuffers(1, &sFullScreenVBO);
    GLuint ebo;
    glGenBuffers(1, &ebo);

    glBindVertexArray(sFullScreenVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sFullScreenVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void tTVPGLRenderMethod::DrawFullScreenQuad()
{
    glBindVertexArray(sFullScreenVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

//---------------------------------------------------------------------------
// tTVPGLTexture2D
//---------------------------------------------------------------------------
tTVPGLTexture2D::tTVPGLTexture2D(const void* pixel,
                                 int pitch,
                                 unsigned int w,
                                 unsigned int h,
                                 TVPTextureFormat::e format)
    : iTVPTexture2D(w, h), TextureID(0), FBOID(0), Static_(false), Opaque_(false),
      StagingBuffer(nullptr), StagingPitch(0), Format_(format)
{
    Init(pixel, pitch, w, h, format);
}

tTVPGLTexture2D::tTVPGLTexture2D(tTVPBitmap* bmp)
    : iTVPTexture2D(bmp->GetWidth(), bmp->GetHeight()), TextureID(0), FBOID(0),
      Static_(false), Opaque_(false), StagingBuffer(nullptr), StagingPitch(0)
{
    Format_ = bmp->GetBPP() == 8 ? TVPTextureFormat::Gray : TVPTextureFormat::RGBA;
    Init(bmp->GetBits(), bmp->GetPitch(), bmp->GetWidth(), bmp->GetHeight(), Format_);
}

tTVPGLTexture2D::tTVPGLTexture2D(iTVPTexture2D* tex, unsigned int neww, unsigned int newh)
    : iTVPTexture2D(neww, newh), TextureID(0), FBOID(0), Static_(false), Opaque_(false),
      StagingBuffer(nullptr), StagingPitch(0)
{
    Format_ = tex ? tex->GetFormat() : TVPTextureFormat::RGBA;

    // 创建空纹理
    glGenTextures(1, &TextureID);
    glBindTexture(GL_TEXTURE_2D, TextureID);
    GLint internalFmt = (Format_ == TVPTextureFormat::Gray) ? GL_R8 : GL_RGBA8;
    GLenum inputFmt   = (Format_ == TVPTextureFormat::Gray) ? GL_RED : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, Width, Height, 0, inputFmt, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 如果源纹理有数据，拷贝过来
    if (tex)
    {
        int srcPitch = tex->GetPitch();
        if (srcPitch > 0)
        {
            const void* srcData = tex->GetScanLineForRead(0);
            if (srcData)
            {
                unsigned int copyW = std::min(neww, tex->GetWidth());
                unsigned int copyH = std::min(newh, tex->GetHeight());
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, copyW, copyH,
                                inputFmt, GL_UNSIGNED_BYTE, srcData);
            }
        }
        Opaque_ = tex->IsOpaque();
    }
}

tTVPGLTexture2D::~tTVPGLTexture2D()
{
    if (TextureID)
        glDeleteTextures(1, &TextureID);
    if (FBOID)
        glDeleteFramebuffers(1, &FBOID);
    FreeStaging();
}

void tTVPGLTexture2D::Init(const void* pixel, int pitch, unsigned int w, unsigned int h,
                           TVPTextureFormat::e format)
{
    Width = w;
    Height = h;
    Format_ = format;
    Static_ = (pixel != nullptr); // 有初始数据视为静态（不可变）

    GLint internalFmt = (format == TVPTextureFormat::Gray) ? GL_R8 : GL_RGBA8;
    GLenum inputFmt   = (format == TVPTextureFormat::Gray) ? GL_RED : GL_RGBA;

    glGenTextures(1, &TextureID);
    glBindTexture(GL_TEXTURE_2D, TextureID);

    if (pixel)
    {
        // 有初始数据：直接上传
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, Width, Height, 0,
                     inputFmt, GL_UNSIGNED_BYTE, nullptr);
        Upload(pixel, pitch, tTVPRect(0, 0, w, h));
    }
    else
    {
        // 空纹理
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, Width, Height, 0,
                     inputFmt, GL_UNSIGNED_BYTE, nullptr);
        Static_ = false;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

void tTVPGLTexture2D::Upload(const void* pixel, int pitch, const tTVPRect& rc)
{
    glBindTexture(GL_TEXTURE_2D, TextureID);

    GLenum inputFmt = (Format_ == TVPTextureFormat::Gray) ? GL_RED : GL_RGBA;

    if (pitch > 0 && pitch != (int)(rc.get_width() * (Format_ == TVPTextureFormat::Gray ? 1 : 4)))
    {
        // 有 pitch 时设置行跨度
        glPixelStorei(GL_UNPACK_ROW_LENGTH,
                      pitch / (Format_ == TVPTextureFormat::Gray ? 1 : 4));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    else
    {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    glTexSubImage2D(GL_TEXTURE_2D, 0,
                    rc.left, rc.top,
                    rc.get_width(), rc.get_height(),
                    inputFmt, GL_UNSIGNED_BYTE, pixel);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void tTVPGLTexture2D::FreeStaging()
{
    if (StagingBuffer)
    {
        std::free(StagingBuffer);
        StagingBuffer = nullptr;
        StagingPitch = 0;
    }
}

void tTVPGLTexture2D::AllocStaging()
{
    if (!StagingBuffer)
    {
        int bpp = (Format_ == TVPTextureFormat::Gray) ? 1 : 4;
        StagingPitch = Width * bpp;
        // 对齐到 16 字节
        StagingPitch = (StagingPitch + 15) & ~15;
        StagingBuffer = std::malloc(StagingPitch * Height);
    }
}

// iTVPTexture2D 接口实现
void tTVPGLTexture2D::Update(const void* pixel,
                             TVPTextureFormat::e format,
                             int pitch,
                             const tTVPRect& rc)
{
    if (format != Format_)
    {
        SDL_Log("GLTexture: format mismatch in Update");
        return;
    }

    // 如果尺寸变了，重新分配
    if (rc.get_width() != (int)Width || rc.get_height() != (int)Height)
    {
        Width = rc.get_width();
        Height = rc.get_height();
        glBindTexture(GL_TEXTURE_2D, TextureID);
        GLint internalFmt = (Format_ == TVPTextureFormat::Gray) ? GL_R8 : GL_RGBA8;
        GLenum inputFmt   = (Format_ == TVPTextureFormat::Gray) ? GL_RED : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, Width, Height, 0,
                     inputFmt, GL_UNSIGNED_BYTE, nullptr);
    }

    Upload(pixel, pitch, rc);
    Static_ = false;
    FreeStaging(); // 使 staging buffer 失效
}

const void* tTVPGLTexture2D::GetScanLineForRead(tjs_uint l)
{
    if (!StagingBuffer)
    {
        AllocStaging();
        // 回读纹理数据到 staging buffer
        if (FBOID)
        {
            GLint prevFBO;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, FBOID);
            GLenum inputFmt = (Format_ == TVPTextureFormat::Gray) ? GL_RED : GL_RGBA;
            glReadPixels(0, 0, Width, Height, inputFmt, GL_UNSIGNED_BYTE, StagingBuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
        }
    }
    return (const void*)((uint8_t*)StagingBuffer + StagingPitch * l);
}

void* tTVPGLTexture2D::GetScanLineForWrite(tjs_uint l)
{
    // 写入时确保 staging buffer 存在
    AllocStaging();
    Opaque_ = false;
    return (void*)((uint8_t*)StagingBuffer + StagingPitch * l);
}

tjs_int tTVPGLTexture2D::GetPitch() const
{
    if (StagingBuffer)
        return StagingPitch;
    return Width * (Format_ == TVPTextureFormat::Gray ? 1 : 4);
}

uint32_t tTVPGLTexture2D::GetPoint(int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)Width || y >= (int)Height)
        return 0;

    const void* scanline = GetScanLineForRead(y);
    if (!scanline)
        return 0;

    if (Format_ == TVPTextureFormat::RGBA)
        return ((const uint32_t*)scanline)[x];
    else if (Format_ == TVPTextureFormat::Gray)
        return ((const uint8_t*)scanline)[x];
    return 0;
}

void tTVPGLTexture2D::SetPoint(int x, int y, uint32_t clr)
{
    if (x < 0 || y < 0 || x >= (int)Width || y >= (int)Height)
        return;

    void* scanline = GetScanLineForWrite(y);
    if (!scanline)
        return;

    if (Format_ == TVPTextureFormat::RGBA)
        ((uint32_t*)scanline)[x] = clr;
    else if (Format_ == TVPTextureFormat::Gray)
        ((uint8_t*)scanline)[x] = (uint8_t)clr;
    Opaque_ = false;
}

bool tTVPGLTexture2D::IsOpaque()
{
    return Opaque_;
}

bool tTVPGLTexture2D::GetTextureData(void* picData, tjs_int& pic_pitch)
{
    if (StagingBuffer)
    {
        void** p = (void**)picData;
        *p = StagingBuffer;
        pic_pitch = StagingPitch;
        return false; // caller should not free
    }
    return false;
}

GLuint tTVPGLTexture2D::GetFBO() const
{
    return FBOID;
}

void tTVPGLTexture2D::EnsureFBO()
{
    if (!FBOID)
    {
        glGenFramebuffers(1, &FBOID);
        glBindFramebuffer(GL_FRAMEBUFFER, FBOID);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, TextureID, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            SDL_Log("GLTexture: FBO incomplete, status=0x%x", status);
            glDeleteFramebuffers(1, &FBOID);
            FBOID = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void tTVPGLTexture2D::BindFBO()
{
    EnsureFBO();
    if (FBOID)
        glBindFramebuffer(GL_FRAMEBUFFER, FBOID);
}

//---------------------------------------------------------------------------
// tTVPGLRenderMethod
//---------------------------------------------------------------------------
tTVPGLRenderMethod::tTVPGLRenderMethod(const char* name, const char* vertexSrc,
                                       const char* fragmentSrc)
    : Program(0), VertexShader(0), FragmentShader(0)
{
    SetName(name);
    EnsureFullScreenQuad();
    sInitCount++;

    VertexShader   = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    FragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (VertexShader && FragmentShader)
        Program = LinkProgram(VertexShader, FragmentShader);
}

tTVPGLRenderMethod::~tTVPGLRenderMethod()
{
    if (Program)
        glDeleteProgram(Program);
    sInitCount--;
    if (sInitCount <= 0 && sFullScreenVAO)
    {
        glDeleteVertexArrays(1, &sFullScreenVAO);
        glDeleteBuffers(1, &sFullScreenVBO);
        sFullScreenVAO = 0;
        sFullScreenVBO = 0;
    }
}

int tTVPGLRenderMethod::EnumParameterID(const char* name)
{
    auto it = ParamIDs.find(name);
    if (it != ParamIDs.end())
        return it->second;
    int id = (int)ParamIDs.size();
    ParamIDs[name] = id;
    return id;
}

void tTVPGLRenderMethod::SetParameterUInt(int id, unsigned int Value)
{
    Params[id] = { Parameter::tUInt };
    Params[id].value.u = Value;
}

void tTVPGLRenderMethod::SetParameterInt(int id, int Value)
{
    Params[id] = { Parameter::tInt };
    Params[id].value.i = Value;
}

void tTVPGLRenderMethod::SetParameterFloat(int id, float Value)
{
    Params[id] = { Parameter::tFloat };
    Params[id].value.f = Value;
}

void tTVPGLRenderMethod::SetParameterColor4B(int id, unsigned int clr)
{
    Params[id] = { Parameter::tColor4B };
    Params[id].value.u = clr;
}

void tTVPGLRenderMethod::SetParameterOpa(int id, int Value)
{
    Params[id] = { Parameter::tOpa };
    Params[id].value.i = Value;
}

void tTVPGLRenderMethod::SetParameterFloatArray(int id, float* Value, int nElem)
{
    Params[id] = { Parameter::tFloatArray };
    for (int i = 0; i < nElem && i < 16; i++)
        Params[id].value.fa[i] = Value[i];
}

void tTVPGLRenderMethod::SetParameterPtr(int id, const void* Value)
{
    Params[id] = { Parameter::tPtr };
    Params[id].value.u = (unsigned int)(uintptr_t)Value;
}

iTVPRenderMethod* tTVPGLRenderMethod::SetBlendFuncSeparate(
    int func, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha)
{
    Parameter p;
    p.type = Parameter::tBlendFunc;
    p.value.fa[0] = (float)func;
    p.value.fa[1] = (float)srcRGB;
    p.value.fa[2] = (float)dstRGB;
    p.value.fa[3] = (float)srcAlpha;
    p.value.fa[4] = (float)dstAlpha;
    int id = EnumParameterID("_gl_blend_func");
    Params[id] = p;
    return this;
}

void tTVPGLRenderMethod::Apply(GLuint targetTex, const tTVPRect& rctar,
                               GLuint refTex,
                               const std::vector<std::pair<GLuint, tTVPRect>>& srcTextures)
{
    if (!Program)
        return;

    glUseProgram(Program);

    // 设置视口
    glViewport(rctar.left, rctar.top, rctar.get_width(), rctar.get_height());

    // 绑定源纹理
    int texUnit = 0;
    for (auto& src : srcTextures)
    {
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, src.first);
        char uniformName[32];
        snprintf(uniformName, sizeof(uniformName), "uTex%d", texUnit);
        GLint loc = glGetUniformLocation(Program, uniformName);
        if (loc >= 0)
            glUniform1i(loc, texUnit);
        texUnit++;
    }

    // 如果有参考纹理
    if (refTex)
    {
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, refTex);
        GLint loc = glGetUniformLocation(Program, "uRefTex");
        if (loc >= 0)
            glUniform1i(loc, texUnit);
    }

    // 应用参数
    for (auto& pair : Params)
    {
        // 参数通过 uniform 传递
        // 目前仅支持部分参数，后续可扩展
    }

    // 绘制全屏四边形
    DrawFullScreenQuad();

    glUseProgram(0);
}

//---------------------------------------------------------------------------
// tTVPGLRenderManager
//---------------------------------------------------------------------------
tTVPGLRenderManager::tTVPGLRenderManager()
    : DrawCount(0), TotalVMemSize(0), TempTexture(nullptr)
{
    RegisterBuiltinMethods();
}

tTVPGLRenderManager::~tTVPGLRenderManager()
{
    if (TempTexture)
        TempTexture->Release();
}

void tTVPGLRenderManager::RegisterBuiltinMethods()
{
    // ---- Copy: 简单拷贝 ----
    {
        static const char* vs = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
        static const char* fs = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTex0;
void main() {
    FragColor = texture(uTex0, vTexCoord);
}
)";
        auto* method = new tTVPGLRenderMethod("Copy", vs, fs);
        RegisterRenderMethod("Copy", method);
    }

    // ---- FillARGB: 填充颜色 ----
    {
        static const char* vs = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
        static const char* fs = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform vec4 uColor;
void main() {
    FragColor = uColor;
}
)";
        auto* method = new tTVPGLRenderMethod("FillARGB", vs, fs);
        RegisterRenderMethod("FillARGB", method);
    }

    // ---- AlphaBlend: alpha 混合 ----
    {
        static const char* vs = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
        static const char* fs = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uTex0;
uniform float uOpacity;
void main() {
    vec4 src = texture(uTex0, vTexCoord);
    src.a *= uOpacity;
    FragColor = src;
}
)";
        auto* method = new tTVPGLRenderMethod("AlphaBlend", vs, fs);
        RegisterRenderMethod("AlphaBlend", method);
    }
}

//---------------------------------------------------------------------------
// CreateTexture2D 实现
//---------------------------------------------------------------------------
iTVPTexture2D* tTVPGLRenderManager::CreateTexture2D(
    const void* pixel, int pitch, unsigned int w, unsigned int h,
    TVPTextureFormat::e format, int flags)
{
    auto* tex = new tTVPGLTexture2D(pixel, pitch, w, h, format);
    TotalVMemSize += w * h * (format == TVPTextureFormat::Gray ? 1 : 4);
    return tex;
}

iTVPTexture2D* tTVPGLRenderManager::CreateTexture2D(tTVPBitmap* bmp)
{
    auto* tex = new tTVPGLTexture2D(bmp);
    TotalVMemSize += bmp->GetWidth() * bmp->GetHeight() * (bmp->GetBPP() == 8 ? 1 : 4);
    return tex;
}

iTVPTexture2D* tTVPGLRenderManager::CreateTexture2D(TJS::tTJSBinaryStream* s)
{
    // 从流加载 — 暂未实现，返回 nullptr
    SDL_Log("GL RenderManager: CreateTexture2D from stream not yet implemented");
    return nullptr;
}

iTVPTexture2D* tTVPGLRenderManager::CreateTexture2D(
    unsigned int neww, unsigned int newh, iTVPTexture2D* tex)
{
    if (tex)
    {
        // 从已有纹理创建（可能缩小或放大）
        auto* newTex = new tTVPGLTexture2D(tex, neww, newh);
        TotalVMemSize += neww * newh * (tex->GetFormat() == TVPTextureFormat::Gray ? 1 : 4);
        return newTex;
    }

    // 空纹理
    auto* newTex = new tTVPGLTexture2D(nullptr, 0, neww, newh, TVPTextureFormat::RGBA);
    TotalVMemSize += neww * newh * 4;
    return newTex;
}

iTVPTexture2D* tTVPGLRenderManager::GetTempTexture(unsigned int w, unsigned int h)
{
    if (!TempTexture || TempTexture->GetWidth() < w || TempTexture->GetHeight() < h)
    {
        if (TempTexture)
            TempTexture->Release();
        TempTexture = CreateTexture2D(nullptr, 0, w, h, TVPTextureFormat::RGBA);
    }
    return TempTexture;
}

//---------------------------------------------------------------------------
// OperateRect
//---------------------------------------------------------------------------
void tTVPGLRenderManager::OperateRect(iTVPRenderMethod* method,
                                      iTVPTexture2D* tar,
                                      iTVPTexture2D* reftar,
                                      const tTVPRect& rctar,
                                      const tRenderTexRectArray& textures)
{
    DrawCount++;

    auto* glMethod = dynamic_cast<tTVPGLRenderMethod*>(method);
    if (!glMethod)
    {
        SDL_Log("GL RenderManager: method is not a GL method");
        return;
    }

    auto* glTarget = dynamic_cast<tTVPGLTexture2D*>(tar);
    if (!glTarget)
    {
        SDL_Log("GL RenderManager: target is not a GL texture");
        return;
    }

    // 准备源纹理
    std::vector<std::pair<GLuint, tTVPRect>> srcGLTextures;
    for (size_t i = 0; i < textures.size(); i++)
    {
        auto* glTex = dynamic_cast<tTVPGLTexture2D*>(textures[i].first);
        if (glTex)
            srcGLTextures.push_back({ glTex->GetTextureID(), textures[i].second });
    }

    // 绑定目标 FBO
    GLint prevFBO;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    glTarget->BindFBO();

    // 设置视口和裁剪
    glViewport(0, 0, glTarget->GetWidth(), glTarget->GetHeight());
    glScissor(rctar.left, rctar.top, rctar.get_width(), rctar.get_height());
    glEnable(GL_SCISSOR_TEST);

    // 禁用混合 — 由 shader 自己处理
    glDisable(GL_BLEND);

    // 应用渲染方法
    GLuint refTexID = 0;
    if (reftar)
    {
        auto* glRef = dynamic_cast<tTVPGLTexture2D*>(reftar);
        if (glRef)
            refTexID = glRef->GetTextureID();
    }
    glMethod->Apply(glTarget->GetTextureID(), rctar, refTexID, srcGLTextures);

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
}

//---------------------------------------------------------------------------
// OperateTriangles
//---------------------------------------------------------------------------
void tTVPGLRenderManager::OperateTriangles(iTVPRenderMethod* method,
                                           int nTriangles,
                                           iTVPTexture2D* target,
                                           iTVPTexture2D* reftar,
                                           const tTVPRect& rcclip,
                                           const tTVPPointD* pttar,
                                           const tRenderTexQuadArray& textures)
{
    // Phase 2: 暂未实现三角形渲染
    SDL_Log("GL RenderManager: OperateTriangles not yet implemented");
}

//---------------------------------------------------------------------------
// OperatePerspective
//---------------------------------------------------------------------------
void tTVPGLRenderManager::OperatePerspective(iTVPRenderMethod* method,
                                             int nQuads,
                                             iTVPTexture2D* target,
                                             iTVPTexture2D* reftar,
                                             const tTVPRect& rcclip,
                                             const tTVPPointD* pttar /*quad{lt,rt,lb,rb}*/,
                                             const tRenderTexQuadArray& textures)
{
    // Phase 3: 暂未实现透视变换渲染
    SDL_Log("GL RenderManager: OperatePerspective not yet implemented");
}

//---------------------------------------------------------------------------
// GetRenderStat
//---------------------------------------------------------------------------
bool tTVPGLRenderManager::GetRenderStat(unsigned int& drawCount, uint64_t& vmemsize)
{
    drawCount = (unsigned int)DrawCount;
    vmemsize = TotalVMemSize;
    return true;
}

//---------------------------------------------------------------------------
// 注册 GL RenderManager
//---------------------------------------------------------------------------
static iTVPRenderManager* __GLRenderManagerFactory()
{
    return new tTVPGLRenderManager();
}

static class __GLRenderManagerAutoRegister
{
public:
    __GLRenderManagerAutoRegister()
    {
        TVPRegisterRenderManager("opengl", __GLRenderManagerFactory);
    }
} __GLRenderManagerAutoRegister_instance;
