#include <cstdint>
#include <vector>

#include "PlatformView.h"

#if 1
//---------------------------------------------------------------------------
// RenderBackend — abstract rendering backend (OpenGL, Software, Vulkan...)
// All blend modes are handled by the existing tTVPSoftwareRenderManager.
// The backend only handles texture creation/upload/drawing/presentation.
//---------------------------------------------------------------------------
class RenderBackend
{
public:
    virtual ~RenderBackend() = default;

    virtual void* CreateTexture(int w, int h) = 0;
    virtual void DestroyTexture(void* tex) = 0;
    virtual void UploadTexture(void* tex, const uint8_t* data, int w, int h, int pitch) = 0;
    virtual void DrawQuad(void* tex,
                          int dstX, int dstY, int dstW, int dstH,
                          int viewW, int viewH, float opacity = 1.0f) = 0;
    virtual void Clear() = 0;
    virtual void Present() = 0;
    virtual const char* GetName() const = 0;
};

//---------------------------------------------------------------------------
// GLBackend — OpenGL / GLES implementation
//---------------------------------------------------------------------------
class GLBackend : public RenderBackend
{
    struct Impl;
    Impl* p;
public:
    GLBackend();
    ~GLBackend() override;
    void* CreateTexture(int w, int h) override;
    void DestroyTexture(void* tex) override;
    void UploadTexture(void* tex, const uint8_t* data, int w, int h, int pitch) override;
    void DrawQuad(void* tex, int dstX, int dstY, int dstW, int dstH,
                  int viewW, int viewH, float opacity) override;
    void Clear() override;
    void Present() override;
    const char* GetName() const override { return "OpenGL"; }
};

//---------------------------------------------------------------------------
// SoftwareBackend — CPU rendering, uploads to GL at present time
//---------------------------------------------------------------------------
class SoftwareBackend : public RenderBackend
{
    struct Impl;
    Impl* p;
public:
    SoftwareBackend();
    ~SoftwareBackend() override;
    void* CreateTexture(int w, int h) override;
    void DestroyTexture(void* tex) override;
    void UploadTexture(void* tex, const uint8_t* data, int w, int h, int pitch) override;
    void DrawQuad(void* tex, int dstX, int dstY, int dstW, int dstH,
                  int viewW, int viewH, float opacity) override;
    void Clear() override;
    void Present() override;
    const char* GetName() const override { return "Software"; }
};

//---------------------------------------------------------------------------
// Surface — a compositable texture with geometry (Wayland wl_surface equivalent)
//---------------------------------------------------------------------------
// Surface layout mirrors SDL_Sprite for ABI compat in KRKR_Get_Current_Sprite
struct Surface
{
    void* texture = nullptr;
    int type = 0;                    // 0:window 1:modal 2:overlay
    int xPos = 0, yPos = 0;
    float scale = 1.0f;
    int width = 0, height = 0;
    bool isVisible = false;
    RenderBackend* backend = nullptr;

    Surface(RenderBackend* be);
    ~Surface();

    void SetSize(int w, int h);
    void Update(const uint8_t* data, int pitch);
    void SetVisible(bool v) { isVisible = v; }
    void SetPosition(int x, int y) { xPos = x; yPos = y; }

    void Draw(int viewW, int viewH);
};

//---------------------------------------------------------------------------
// Compositor — surface tree manager + renderer (Wayland wl_compositor equivalent)
//---------------------------------------------------------------------------
class Compositor
{
    RenderBackend* m_backend;
    std::vector<Surface*> m_surfaces;
    Surface* m_active = nullptr;

public:
    Compositor(RenderBackend* be) : m_backend(be) {}

    RenderBackend* GetBackend() { return m_backend; }

    void AddSurface(Surface* s) { m_surfaces.push_back(s); }
    void BringToFront(Surface* s);

    Surface* GetActive() { return m_active; }
    void SetActive(Surface* s) { m_active = s; }

    void RenderAll(int viewW, int viewH);

    Surface* HitTest(int x, int y);
};

//---------------------------------------------------------------------------
// Global accessor
//---------------------------------------------------------------------------
Compositor* GetCompositor();
void InitCompositor(RenderBackend* backend);

#endif

namespace krkrsdl3
{
// 
void fetchGLInfo();
TVPSprite* KRKR_Get_Current_Sprite();
// 贴图管理
void TVPJoinTexture(TVPSprite* sp);
void TVPDepartTexture(TVPSprite* sp);
// 渲染函数
void TVPRenderOnce(int winWidth, int winHeight);
void TVPCreateTexture(TVPSprite& sp);
void TVPUpdateTexture(TVPSprite* sp, uint8_t* buff, int width, int height, int pitch);
void TVPDestroyTexture(TVPSprite* sp);
} // namespace krkrsdl3