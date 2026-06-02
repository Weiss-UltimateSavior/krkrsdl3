//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//!@file OpenGL 描画デバイス管理 (GPU Render Manager)
//---------------------------------------------------------------------------
#ifndef RENDERMANAGERGL_H
#define RENDERMANAGERGL_H

#include "RenderManager.h"

#if _KRKRSDL3_GL
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

//---------------------------------------------------------------------------
//! @brief		OpenGL 纹理类，包装 GL 纹理对象
//---------------------------------------------------------------------------
class tTVPGLTexture2D : public iTVPTexture2D
{
public:
    tTVPGLTexture2D(const void* pixel,
                    int pitch,
                    unsigned int w,
                    unsigned int h,
                    TVPTextureFormat::e format);
    tTVPGLTexture2D(tTVPBitmap* bmp);
    tTVPGLTexture2D(iTVPTexture2D* tex, unsigned int neww, unsigned int newh);
    ~tTVPGLTexture2D();

    // iTVPTexture2D 接口
    virtual TVPTextureFormat::e GetFormat() const override { return Format_; }
    virtual const void* GetScanLineForRead(tjs_uint l) override;
    virtual void* GetScanLineForWrite(tjs_uint l) override;
    virtual tjs_int GetPitch() const override;
    virtual void Update(const void* pixel,
                        TVPTextureFormat::e format,
                        int pitch,
                        const tTVPRect& rc) override;
    virtual uint32_t GetPoint(int x, int y) override;
    virtual void SetPoint(int x, int y, uint32_t clr) override;
    virtual bool IsStatic() override { return Static_; }
    virtual bool IsOpaque() override;
    virtual bool GetTextureData(void* picData, tjs_int& pic_pitch) override;

    // GL 特有
    GLuint GetTextureID() const { return TextureID; }
    GLuint GetFBO() const;
    void EnsureFBO();
    void BindFBO();

private:
    void Init(const void* pixel, int pitch, unsigned int w, unsigned int h, TVPTextureFormat::e format);
    void Upload(const void* pixel, int pitch, const tTVPRect& rc);
    void FreeStaging();
    void AllocStaging();

    GLuint TextureID;
    GLuint FBOID;
    bool Static_;                     // 是否只读（静态纹理）
    bool Opaque_;                     // 是否完全不透明
    void* StagingBuffer;              // 用于 CPU 回读（GetScanLineForRead/Write）
    int StagingPitch;
    TVPTextureFormat::e Format_;
};

//---------------------------------------------------------------------------
//! @brief		OpenGL 渲染方法基类（对应一个 shader program）
//---------------------------------------------------------------------------
class tTVPGLRenderMethod : public iTVPRenderMethod
{
public:
    tTVPGLRenderMethod(const char* name, const char* vertexSrc, const char* fragmentSrc);
    ~tTVPGLRenderMethod();

    // iTVPRenderMethod 接口
    virtual int EnumParameterID(const char* name) override;
    virtual void SetParameterUInt(int id, unsigned int Value) override;
    virtual void SetParameterInt(int id, int Value) override;
    virtual void SetParameterPtr(int id, const void* Value) override;
    virtual void SetParameterFloat(int id, float Value) override;
    virtual void SetParameterColor4B(int id, unsigned int clr) override;
    virtual void SetParameterOpa(int id, int Value) override;
    virtual void SetParameterFloatArray(int id, float* Value, int nElem) override;
    virtual iTVPRenderMethod* SetBlendFuncSeparate(
        int func, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) override;

    // 应用此方法到矩形区域
    // 由 tTVPGLRenderManager::OperateRect 调用
    virtual void Apply(GLuint targetTex, const tTVPRect& rctar,
                       GLuint refTex,
                       const std::vector<std::pair<GLuint, tTVPRect>>& srcTextures);

    GLuint GetProgram() const { return Program; }

private:
    struct Parameter
    {
        enum Type { tUInt, tInt, tFloat, tColor4B, tOpa, tFloatArray, tPtr, tBlendFunc } type;
        union
        {
            unsigned int u;
            int i;
            float f;
            float fa[16];
        } value;
    };
    std::unordered_map<int, Parameter> Params;
    std::unordered_map<std::string, int> ParamIDs;

    GLuint Program;
    GLuint VertexShader;
    GLuint FragmentShader;

    // 全屏四边形 VAO（所有方法共用）
    static GLuint sFullScreenVAO;
    static GLuint sFullScreenVBO;
    static int sInitCount;

    static void EnsureFullScreenQuad();
    static void DrawFullScreenQuad();
};

//---------------------------------------------------------------------------
//! @brief		OpenGL 渲染管理器
//---------------------------------------------------------------------------
class tTVPGLRenderManager : public iTVPRenderManager
{
public:
    tTVPGLRenderManager();
    ~tTVPGLRenderManager();

    // iTVPRenderManager 接口
    virtual iTVPTexture2D* CreateTexture2D(const void* pixel,
                                           int pitch,
                                           unsigned int w,
                                           unsigned int h,
                                           TVPTextureFormat::e format,
                                           int flags = RENDER_CREATE_TEXTURE_FLAG_ANY) override;
    virtual iTVPTexture2D* CreateTexture2D(tTVPBitmap* bmp) override;
    virtual iTVPTexture2D* CreateTexture2D(TJS::tTJSBinaryStream* s) override;
    virtual iTVPTexture2D* CreateTexture2D(unsigned int neww,
                                           unsigned int newh,
                                           iTVPTexture2D* tex) override;

    virtual void OperateRect(iTVPRenderMethod* method,
                             iTVPTexture2D* tar,
                             iTVPTexture2D* reftar,
                             const tTVPRect& rctar,
                             const tRenderTexRectArray& textures) override;

    virtual void OperateTriangles(iTVPRenderMethod* method,
                                  int nTriangles,
                                  iTVPTexture2D* target,
                                  iTVPTexture2D* reftar,
                                  const tTVPRect& rcclip,
                                  const tTVPPointD* pttar,
                                  const tRenderTexQuadArray& textures) override;

    virtual void OperatePerspective(iTVPRenderMethod* method,
                                    int nQuads,
                                    iTVPTexture2D* target,
                                    iTVPTexture2D* reftar,
                                    const tTVPRect& rcclip,
                                    const tTVPPointD* pttar /*quad{lt,rt,lb,rb}*/,
                                    const tRenderTexQuadArray& textures) override;

    virtual bool IsSoftware() override { return false; }
    virtual const char* GetName() override { return "opengl"; }
    virtual bool GetRenderStat(unsigned int& drawCount, uint64_t& vmemsize) override;

private:
    void RegisterBuiltinMethods();

    tjs_int32 DrawCount;
    uint64_t TotalVMemSize;

    // 临时纹理池
    iTVPTexture2D* TempTexture;
    iTVPTexture2D* GetTempTexture(unsigned int w, unsigned int h);
};

#endif // RENDERMANAGERGL_H
