#include "tjsCommHead.h"
#include "TVPCompositor.h"

#ifdef _KRKRSDL3_GL
#include "glad/glad.h"
#else
#include "glad/glad_egl.h"
#include <GLES3/gl3.h>
#endif

#include <unordered_set>
#include "TVPDebug.h"
#include "Platform.h"
#include "TVPSettings.h"

#if 1
//---------------------------------------------------------------------------
// Global compositor instance
//---------------------------------------------------------------------------
static Compositor* gCompositor = nullptr;

Compositor* GetCompositor() { return gCompositor; }
void InitCompositor(RenderBackend* backend) { gCompositor = new Compositor(backend); }

//---------------------------------------------------------------------------
// Surface
//---------------------------------------------------------------------------
Surface::Surface(RenderBackend* be) : backend(be) {}
Surface::~Surface()
{
    if (texture && backend)
        backend->DestroyTexture(texture);
}

void Surface::SetSize(int w, int h)
{
    if (texture)
        backend->DestroyTexture(texture);
    texture = backend->CreateTexture(w, h);
    width = w;
    height = h;
}

void Surface::Update(const uint8_t* data, int pitch)
{
    if (texture && backend)
        backend->UploadTexture(texture, data, width, height, pitch);
}

void Surface::Draw(int viewW, int viewH)
{
    if (isVisible && texture && backend)
    {
        backend->DrawQuad(texture, xPos, yPos, (int)(width * scale), (int)(height * scale),
                          viewW, viewH);
    }
}

//---------------------------------------------------------------------------
// Compositor
//---------------------------------------------------------------------------
void Compositor::BringToFront(Surface* s)
{
    auto it = std::find(m_surfaces.begin(), m_surfaces.end(), s);
    if (it != m_surfaces.end())
        m_surfaces.erase(it);
    m_surfaces.push_back(s);
    m_active = s;
}

void Compositor::RenderAll(int viewW, int viewH)
{
    m_backend->Clear();
    for (auto* s : m_surfaces)
    {
        if (!s->isVisible) continue;
        s->Draw(viewW, viewH);
    }
    m_backend->Present();
}

Surface* Compositor::HitTest(int x, int y)
{
    for (auto it = m_surfaces.rbegin(); it != m_surfaces.rend(); ++it)
    {
        Surface* s = *it;
        if (!s->isVisible) continue;
        int sw = (int)(s->width * s->scale);
        int sh = (int)(s->height * s->scale);
        if (x >= s->xPos && x < s->xPos + sw && y >= s->yPos && y < s->yPos + sh)
            return s;
    }
    return nullptr;
}

//---------------------------------------------------------------------------
// GLBackend implementation
//---------------------------------------------------------------------------
struct GLTexture
{
    GLuint id;
    int w, h;
};

#ifdef _MSC_VER
static inline const char* glErrorString(GLenum) { return "GL error"; }
#else
static inline const char* glErrorString(GLenum err)
{
    switch (err) {
        case GL_INVALID_ENUM: return "INVALID_ENUM";
        case GL_INVALID_VALUE: return "INVALID_VALUE";
        case GL_INVALID_OPERATION: return "INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        default: return "UNKNOWN";
    }
}
#endif

struct GLBackend::Impl
{
    GLuint program = 0;
    GLuint vao = 0, vbo = 0, ebo = 0;
    bool initialized = false;

    void initGL()
    {
        if (initialized) return;
        initialized = true;

        const char* vsSrc;
        const char* fsSrc;
#ifdef _KRKRSDL3_GL
        vsSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
uniform vec2 windowSize;
uniform vec2 texture_Position;
uniform vec2 texture_Size;
void main()
{
    vec2 pixelPos = texture_Position + vec2(texture_Size.x * aPos.x, texture_Size.y * aPos.y);
    vec2 ndcPos = (pixelPos / windowSize) * 2.0 - 1.0;
    gl_Position = vec4(ndcPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";
        fsSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texture1;
void main()
{
    FragColor = texture(texture1, TexCoord);
    if (FragColor.a < 0.01) discard;
}
)";
#else
        vsSrc = R"(
#version 300 es
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
uniform vec2 windowSize;
uniform vec2 texture_Position;
uniform vec2 texture_Size;
void main()
{
    vec2 pixelPos = texture_Position + vec2(texture_Size.x * aPos.x, texture_Size.y * aPos.y);
    vec2 ndcPos = (pixelPos / windowSize) * 2.0 - 1.0;
    gl_Position = vec4(ndcPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";
        fsSrc = R"(
#version 300 es
precision mediump float;
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texture1;
void main()
{
    FragColor = texture(texture1, TexCoord);
    if (FragColor.a < 0.01) discard;
}
)";
#endif
        auto compileShader = [](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, NULL);
            glCompileShader(s);
            GLint ok;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char buf[512];
                glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
                TVPConsoleLog("Shader compile error: %s", buf);
            }
            return s;
        };

        GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        float quad[] = { 0,0,0,0,1, 1,0,0,1,1, 0,1,0,0,0, 1,1,0,1,0 };
        unsigned int idx[] = { 0,1,2, 2,1,3 };
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
};

GLBackend::GLBackend() : p(new Impl) {}
GLBackend::~GLBackend() { delete p; }

void* GLBackend::CreateTexture(int w, int h)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto* t = new GLTexture{id, w, h};
    return t;
}

void GLBackend::DestroyTexture(void* tex)
{
    auto* t = (GLTexture*)tex;
    glDeleteTextures(1, &t->id);
    delete t;
}

void GLBackend::UploadTexture(void* tex, const uint8_t* data, int w, int h, int pitch)
{
    auto* t = (GLTexture*)tex;
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t->w = w;
    t->h = h;
}

void GLBackend::DrawQuad(void* tex,
                          int dstX, int dstY, int dstW, int dstH,
                          int viewW, int viewH, float opacity)
{
    auto* t = (GLTexture*)tex;
    if (!t || !t->id) return;

    p->initGL();

    glUseProgram(p->program);
    glBindVertexArray(p->vao);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->id);

    float ws[2] = { (float)viewW, (float)viewH };
    float pos[2] = { (float)dstX, (float)dstY };
    float sz[2] = { (float)dstW, (float)dstH };

    glUniform2fv(glGetUniformLocation(p->program, "windowSize"), 1, ws);
    glUniform2fv(glGetUniformLocation(p->program, "texture_Position"), 1, pos);
    glUniform2fv(glGetUniformLocation(p->program, "texture_Size"), 1, sz);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void GLBackend::Clear()
{
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void GLBackend::Present()
{
    // SDL_GL_SwapWindow is called externally
}

//---------------------------------------------------------------------------
// SoftwareBackend — CPU pixel buffer, uploads to GL at draw time
//---------------------------------------------------------------------------
struct SWTexture { uint8_t* data; int w, h; };
struct SoftwareBackend::Impl { GLBackend gl; };

SoftwareBackend::SoftwareBackend() : p(new Impl) {}
SoftwareBackend::~SoftwareBackend() { delete p; }

void* SoftwareBackend::CreateTexture(int w, int h)
{
    auto* t = new SWTexture;
    t->data = new uint8_t[w * h * 4];
    t->w = w;
    t->h = h;
    memset(t->data, 0, w * h * 4);
    return p->gl.CreateTexture(w, h); // also create GL backing
}

void SoftwareBackend::DestroyTexture(void* tex)
{
    p->gl.DestroyTexture(tex);
}

void SoftwareBackend::UploadTexture(void* tex, const uint8_t* data, int w, int h, int pitch)
{
    // Upload CPU data to GL backing
    p->gl.UploadTexture(tex, data, w, h, pitch);
}

void SoftwareBackend::DrawQuad(void* tex,
                                int dstX, int dstY, int dstW, int dstH,
                                int viewW, int viewH, float opacity)
{
    p->gl.DrawQuad(tex, dstX, dstY, dstW, dstH, viewW, viewH, opacity);
}

void SoftwareBackend::Clear() { p->gl.Clear(); }
void SoftwareBackend::Present() {}
//---------------------------------------------------------------------------

#endif


std::vector<TVPSprite*> renderTexture;

namespace krkrsdl3
{
// base
static std::unordered_set<std::string> sTVPGLExtensions;
void fetchGLInfo()
{
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);

    ttstr log(TJS_N("OpenGL Vendor: "));
    log += ttstr((const char*)vendor) + TJS_N(" / ") + ttstr((const char*)renderer);
    TVPAddImportantLog(log);
    log = TJS_N("OpenGL Version: ") + ttstr((const char*)version);
    TVPAddImportantLog(log);
    log = TJS_N("GLSL Version: ") + ttstr((const char*)glslVersion);
    TVPAddImportantLog(log);

    GLint numExtensions;
    glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
    log = TJS_N("OpenGL extensions (") + ttstr(numExtensions) + TJS_N("):");
    for (int i = 0; i < numExtensions; i++)
    {
        const GLubyte* ext = glGetStringi(GL_EXTENSIONS, i);
        sTVPGLExtensions.emplace(std::string((const char*)ext));
        log += " " + ttstr((const char*)ext);
    }
    TVPAddImportantLog(log);
}
bool checkGLExtension(const std::string& extname)
{
    return sTVPGLExtensions.find(extname) != sTVPGLExtensions.end();
}

// 全部都在这里搞定
static GLuint krkrsdl3_program = 0, krkrsdl3_vao = 0, krkrsdl3_vbo = 0, krkrsdl3_ebo = 0;
#if _KRKRSDL3_GL
const char* vertexShaderSrc = R"(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;

uniform vec2 windowSize;
uniform vec2 texture_Position;
uniform vec2 texture_Size;

void main()
{
    vec2 pixelPos = texture_Position + vec2(texture_Size.x * aPos.x, texture_Size.y * aPos.y);
    vec2 ndcPos = pixelPos * 2.0 - 1.0;

    gl_Position = vec4(ndcPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";
const char* fragmentShaderSrc = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texture1;

void main()
{
    FragColor = texture(texture1, TexCoord);
}
)";
#else
const char* vertexShaderSrc = R"(#version 300 es
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
out vec2 TexCoord;

uniform vec2 windowSize;
uniform vec2 texture_Position;
uniform vec2 texture_Size;

void main()
{
    vec2 pixelPos = texture_Position + vec2(texture_Size.x * aPos.x, texture_Size.y * aPos.y);
    vec2 ndcPos = pixelPos * 2.0 - 1.0;

    gl_Position = vec4(ndcPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";
const char* fragmentShaderSrc = R"(#version 300 es
precision mediump float;
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D texture1;

void main()
{
    FragColor = texture(texture1, TexCoord);
}
)";
#endif
GLuint compileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        TVPConsoleLog("Shader compile error: %s", log);
    }
    return shader;
}
GLuint createProgram()
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetProgramInfoLog(prog, 512, nullptr, log);
        TVPConsoleLog("Program link error: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static void GL_BaseSet(int w, int h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);
    if (krkrsdl3_program == 0 || glIsProgram(krkrsdl3_program) != GL_TRUE)
    {
        // Create shader program
        krkrsdl3_program = createProgram();
        glGenVertexArrays(1, &krkrsdl3_vao);
        glGenBuffers(1, &krkrsdl3_vbo);
        glGenBuffers(1, &krkrsdl3_ebo);
        // VBO
        glBindVertexArray(krkrsdl3_vao);
        glBindBuffer(GL_ARRAY_BUFFER, krkrsdl3_vbo);
        // 顶点数据
        float vertices[] = {
            // 位置          // 纹理坐标
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f, // 左下
            1.0f, 0.0f, 0.0f, 1.0f, 1.0f, // 右下
            1.0f, 1.0f, 0.0f, 1.0f, 0.0f, // 右上
            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, // 左上
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        // EBO
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, krkrsdl3_ebo);
        unsigned int indices[] = {
            0, 1, 2, // 第一个三角形
            2, 3, 0  // 第二个三角形
        };
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        // 位置属性
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // 纹理坐标属性
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    glUseProgram(krkrsdl3_program);
    glViewport(0, 0, w, h);
    glBindVertexArray(krkrsdl3_vao);
}
static void GL_DrawTexture(TVPSprite* sp, int w, int h)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)sp->texture.gpuTexture);
    glUniform1i(glGetUniformLocation(krkrsdl3_program, "texture1"), 0);
    glUniform2f(glGetUniformLocation(krkrsdl3_program, "windowSize"), w, h);
    float currScale = std::min(((float)w) / sp->width, ((float)h) / sp->height);
    sp->scale = currScale;
    float scaledW = currScale * sp->width;
    float scaledH = currScale * sp->height;
    float xPos = (w - scaledW) / 2.0;
    sp->xPos = xPos;
    float yPos = (h - scaledH) / 2.0;
    sp->yPos = yPos;
    glUniform2f(glGetUniformLocation(krkrsdl3_program, "texture_Position"), xPos / w, yPos / h);
    glUniform2f(glGetUniformLocation(krkrsdl3_program, "texture_Size"), scaledW / w, scaledH / h);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}
static void SW_DrawTexture(TVPSprite* sp, int w, int h)
{
    // 基础计算
    float currScale = std::min(((float)w) / sp->width, ((float)h) / sp->height);
    sp->scale = currScale;
    float scaledW = currScale * sp->width;
    float scaledH = currScale * sp->height;
    float xPos = (w - scaledW) / 2.0;
    sp->xPos = xPos;
    float yPos = (h - scaledH) / 2.0;
    sp->yPos = yPos;
    // 完成
    TVPRenderTextureBackend(sp, sp->xPos, sp->yPos, scaledW, scaledH);
}

// 素材加入渲染
void TVPJoinTexture(TVPSprite* sp)
{
    renderTexture.push_back(sp);
}

// 素材离开渲染
void TVPDepartTexture(TVPSprite* sp)
{
    for (size_t i = 0; i < renderTexture.size(); i++)
    {
        if (renderTexture.at(i)->texture.gpuTexture == sp->texture.gpuTexture)
        {
            renderTexture.erase(renderTexture.begin() + i);
            break;
        }
    }
}

void TVPRenderOnce(int winWidth, int winHeight)
{
    if (TVPSettings.renderer == "opengl")
    {
        GL_BaseSet(winWidth, winHeight);
        {
            // 绘制currentSprite
            TVPSprite* retSpr = KRKR_Get_Current_Sprite();
            if (retSpr)
                GL_DrawTexture(retSpr, winWidth, winHeight);

            // 绘制overlay
            for (auto texture : renderTexture)
            {
                if (texture->isVisible && texture->type == 2)
                {
                    GL_DrawTexture(texture, winWidth, winHeight);
                }
            }
        }
    }
    else
    {
        // 绘制currentSprite
        TVPSprite* retSpr = KRKR_Get_Current_Sprite();
        if (retSpr)
            SW_DrawTexture(retSpr, winWidth, winHeight);

        // 绘制overlay
        for (auto texture : renderTexture)
        {
            if (texture->isVisible && texture->type == 2)
            {
                SW_DrawTexture(texture, winWidth, winHeight);
            }
        }
    }
}

// 创建素材
void TVPCreateTexture(TVPSprite& sp)
{
    if (TVPSettings.renderer == "opengl")
    {
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sp.width, sp.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        sp.texture.gpuTexture = texture;
    }
    else
    {
        TVPCreateTextureBackend(sp);
    }
}

// 更新素材
void TVPUpdateTexture(TVPSprite* sp, uint8_t* buff, int width, int height, int pitch)
{
    if (TVPSettings.renderer == "opengl")
    {
        glBindTexture(GL_TEXTURE_2D, sp->texture.gpuTexture);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buff);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    else
    {
        TVPUpdateTextureBackend(sp, buff, width, height, pitch);
    }
}

// 清理素材
void TVPDestroyTexture(TVPSprite* sp)
{
    if (TVPSettings.renderer == "opengl")
    {
        GLuint grp[1] = {(GLuint)sp->texture.gpuTexture};
        glDeleteTextures(1, grp);

        sp->texture.gpuTexture = 0;
    }
    else
    {
        TVPDestroyTextureBackend(sp);
    }
}

} // namespace krkrsdl3
