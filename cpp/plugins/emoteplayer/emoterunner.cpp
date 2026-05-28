#include "emoterunner.h"

#include <SDL3/SDL.h>

#define GLM_ASSERT_VALID(matrix) \
    do \
    { \
        const glm::mat4& m = (matrix); \
        for (int i = 0; i < 4; ++i) \
        { \
            for (int j = 0; j < 4; ++j) \
            { \
                assert(!std::isnan(m[i][j]) && "矩阵包含NaN值"); \
                assert(!std::isinf(m[i][j]) && "矩阵包含无穷大值"); \
            } \
        } \
    } while (0)

namespace emoteplayer
{

#pragma region glprogram

static GLuint emotenodeprogram = 0;
static GLuint emotenodeVAO = 0;
#if _KRKRSDL3_GL
static const char* vertexShaderSrc = R"(
            #version 430 core
            layout (location = 0) in vec2 aPos;

            void main()
            {
                gl_Position = vec4(aPos.xy, 0.0, 1.0);
            }
            )";
static const char* tessControlShaderSrc = R"(
            #version 430 core
            layout (vertices = 16) out;

            void main()
            {
                gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

                if (gl_InvocationID == 0)
                {
                    gl_TessLevelInner[0] = 8.0;
                    gl_TessLevelInner[1] = 8.0;
                    gl_TessLevelOuter[0] = 16.0;
                    gl_TessLevelOuter[1] = 16.0;
                    gl_TessLevelOuter[2] = 16.0;
                    gl_TessLevelOuter[3] = 16.0;
                }
            }
            )";
static const char* tessEvaluationShaderSrc = R"(
            #version 430 core
            layout(quads, equal_spacing, ccw) in;

            out vec2 tessCoord;

            // 最大渲染深度64，应该是够用了
            uniform int surfaceCount;
            uniform mat4 transforms[64];
            uniform vec2 controlPoints[64][16];

            float B0(float t) { return (1.0 - t) * (1.0 - t) * (1.0 - t); }
            float B1(float t) { return 3.0 * t * (1.0 - t) * (1.0 - t); }
            float B2(float t) { return 3.0 * t * t * (1.0 - t); }
            float B3(float t) { return t * t * t; }
            vec2 bezierSurface(vec2 uv, int idx) {
                float u = uv.x;
                float v = uv.y;

                vec2 result = vec2(0.0);

                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        int index = i * 4 + j;

                        // 计算基函数乘积
                        float bu = 0.0;
                        float bv = 0.0;

                        if (i == 0) bu = B0(u);
                        else if (i == 1) bu = B1(u);
                        else if (i == 2) bu = B2(u);
                        else if (i == 3) bu = B3(u);

                        if (j == 0) bv = B0(v);
                        else if (j == 1) bv = B1(v);
                        else if (j == 2) bv = B2(v);
                        else if (j == 3) bv = B3(v);

                        float basis = bu * bv;
                        vec2 controlPoint = controlPoints[idx][index];
                        result += controlPoint * basis;
                    }
                }

                return result;
            }

            void main(void)
            {
                float u = gl_TessCoord.x;
                float v = gl_TessCoord.y;

                vec4 lastPt = vec4(u, v, 0, 0);
                for (int i = 0; i < surfaceCount; i++) {
                    if (i > 0) {
                        lastPt = vec4(1.0 - (lastPt.y / 2.0 + 0.5), 0.5 + lastPt.x / 2.0, 0, 0);
                    }
                    vec2 position = bezierSurface(lastPt.xy, i);
                    lastPt = transforms[i] * vec4(position.xy, 0, 1);
                }
                gl_Position = lastPt * vec4(1.0, -1.0, 1.0, 1.0);

                tessCoord = vec2(gl_TessCoord.y, gl_TessCoord.x);
            }
            )";
static const char* fragmentShaderSrc = R"(
            #version 430 core
            out vec4 FragColor;
            in vec2 tessCoord;
            uniform sampler2D texture1;
            uniform bool enableMask = false;
            uniform vec2 viewportSize;
            uniform sampler2D maskTexture;
            uniform float opa = 1.0;
            uniform bool enableColor = false;
            uniform vec4 uniformColor;
            void main()
            {
                vec4 maskColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
                if (enableMask) {
                    vec2 normalizedCoord = gl_FragCoord.xy / viewportSize;
                    maskColor = texture(maskTexture, normalizedCoord);
                }

                vec4 color = texture(texture1, tessCoord);
                if (enableMask && maskColor.a < 0.5) {
                    discard;
                } else {
                    if(enableColor)
                    {
                        color = vec4(uniformColor.xyz, uniformColor.a * color.a);
                    }
                    color.a = color.a * opa;
                    FragColor = vec4(color.rgba);
                }
            }
        )";
#else
static const char* vertexShaderSrc = R"(#version 320 es
            layout (location = 0) in vec2 aPos;

            void main()
            {
                gl_Position = vec4(aPos.xy, 0.0, 1.0);
            }
            )";
static const char* tessControlShaderSrc = R"(#version 320 es
            layout (vertices = 16) out;

            void main()
            {
                gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

                if (gl_InvocationID == 0)
                {
                    gl_TessLevelInner[0] = 8.0;
                    gl_TessLevelInner[1] = 8.0;
                    gl_TessLevelOuter[0] = 16.0;
                    gl_TessLevelOuter[1] = 16.0;
                    gl_TessLevelOuter[2] = 16.0;
                    gl_TessLevelOuter[3] = 16.0;
                }
            }
            )";
static const char* tessEvaluationShaderSrc = R"(#version 320 es
            layout(quads, equal_spacing, ccw) in;

            out vec2 tessCoord;

            // 最大渲染深度20，应该是够用了?
            uniform int surfaceCount;
            uniform mat4 transforms[20];
            uniform vec2 controlPoints[20][16];

            float B0(float t) { return (1.0 - t) * (1.0 - t) * (1.0 - t); }
            float B1(float t) { return 3.0 * t * (1.0 - t) * (1.0 - t); }
            float B2(float t) { return 3.0 * t * t * (1.0 - t); }
            float B3(float t) { return t * t * t; }
            vec2 bezierSurface(vec2 uv, int idx) {
                float u = uv.x;
                float v = uv.y;

                vec2 result = vec2(0.0);

                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        int index = i * 4 + j;

                        // 计算基函数乘积
                        float bu = 0.0;
                        float bv = 0.0;

                        if (i == 0) bu = B0(u);
                        else if (i == 1) bu = B1(u);
                        else if (i == 2) bu = B2(u);
                        else if (i == 3) bu = B3(u);

                        if (j == 0) bv = B0(v);
                        else if (j == 1) bv = B1(v);
                        else if (j == 2) bv = B2(v);
                        else if (j == 3) bv = B3(v);

                        float basis = bu * bv;
                        vec2 controlPoint = controlPoints[idx][index];
                        result += controlPoint * basis;
                    }
                }

                return result;
            }

            void main(void)
            {
                float u = gl_TessCoord.x;
                float v = gl_TessCoord.y;

                vec4 lastPt = vec4(u, v, 0, 0);
                for (int i = 0; i < surfaceCount; i++) {
                    if (i > 0) {
                        lastPt = vec4(1.0 - (lastPt.y / 2.0 + 0.5), 0.5 + lastPt.x / 2.0, 0.0, 0.0);
                    }
                    vec2 position = bezierSurface(lastPt.xy, i);
                    lastPt = transforms[i] * vec4(position.xy, 0, 1);
                }
                gl_Position = lastPt * vec4(1.0, -1.0, 1.0, 1.0);

                tessCoord = vec2(gl_TessCoord.y, gl_TessCoord.x);
            }
            )";
static const char* fragmentShaderSrc = R"(#version 320 es
            out mediump vec4 FragColor;
            in mediump vec2 tessCoord;
            uniform sampler2D texture1;
            uniform bool enableMask;
            uniform mediump vec2 viewportSize;
            uniform sampler2D maskTexture;
            uniform mediump float opa;
            uniform bool enableColor;
            uniform mediump vec4 uniformColor;
            void main()
            {
                mediump vec4 maskColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
                if (enableMask) {
                    mediump vec2 normalizedCoord = gl_FragCoord.xy / viewportSize;
                    maskColor = texture(maskTexture, normalizedCoord);
                }

                mediump vec4 color = texture(texture1, tessCoord);
                if (enableMask && maskColor.a < 0.5) {
                    discard;
                } else {
                    if(enableColor)
                    {
                        color = vec4(uniformColor.xyz, uniformColor.a * color.a);
                    }
                    color.a = color.a * opa;
                    FragColor = vec4(color.rgba);
                }
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
        SDL_Log("Shader compile error: %s", log);
    }
    return shader;
}
GLuint createRenderProgram()
{
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexShaderSrc);
    GLuint tcs = compileShader(GL_TESS_CONTROL_SHADER, tessControlShaderSrc);
    GLuint tes = compileShader(GL_TESS_EVALUATION_SHADER, tessEvaluationShaderSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, tcs);
    glAttachShader(prog, tes);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success)
    {
        char log[512];
        glGetProgramInfoLog(prog, 512, nullptr, log);
        SDL_Log("Program link error: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(tcs);
    glDeleteShader(tes);
    glDeleteShader(fs);
    return prog;
}
GLuint createEmptyDepthTexture(int width, int height)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, NULL);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}
GLuint createFBO(GLuint texture, GLuint depthtexture)
{
    GLuint result;
    glGenFramebuffers(1, &result);
    glBindFramebuffer(GL_FRAMEBUFFER, result);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthtexture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        SDL_Log("Framebuffer不完整!");
    }

    return result;
}
GLfloat default_control_points[32] = {
    0.000000f, 0.000000f, 0.333333f, 0.000000f, 0.666667f, 0.000000f, 1.000000f, 0.000000f,
    0.000000f, 0.333333f, 0.333333f, 0.333333f, 0.666667f, 0.333333f, 1.000000f, 0.333333f,
    0.000000f, 0.666667f, 0.333333f, 0.666667f, 0.666667f, 0.666667f, 1.000000f, 0.666667f,
    0.000000f, 1.000000f, 0.333333f, 1.000000f, 0.666667f, 1.000000f, 1.000000f, 1.000000f};
void glBaseSet()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(-1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (emotenodeprogram == 0 || glIsProgram(emotenodeprogram) != GL_TRUE)
    {
        // 程序
        emotenodeprogram = createRenderProgram();
        // array
        glGenVertexArrays(1, &emotenodeVAO);
    }
    glUseProgram(emotenodeprogram);
    glBindVertexArray(emotenodeVAO);
}
void glBaseSetWithoutClear()
{
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepthf(-1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    if (emotenodeprogram == 0 || glIsProgram(emotenodeprogram) != GL_TRUE)
    {
        // 程序
        emotenodeprogram = createRenderProgram();
        // array
        glGenVertexArrays(1, &emotenodeVAO);
    }
    glUseProgram(emotenodeprogram);
    glBindVertexArray(emotenodeVAO);
}

#pragma endregion

void emotenoderef::checkDrawStatus(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    // 不绘制进行节点传递
    if (renderList.size() > 0 && renderList.back().type == 0)
    {
        isNeedDraw = false;
        return;
    }

    // 确定时间轴
    if (currentNode->frameList.size() == 0)
    {
        isNeedDraw = false;
        return;
    }
    frame = nullptr;
    size_t currFrameIdx = -1;
    for (size_t i = 0; i < currentNode->frameList.size(); i++)
    {
        if (currentNode->frameList.at(i)->time <= tick)
        {
            frame = currentNode->frameList.at(i);
            currFrameIdx = i;
        }
        else
            break;
    }

    if (frame == nullptr || !frame->hasContent)
    {
        isNeedDraw = false;
        return;
    }
    nextframe = nullptr;
    if (currFrameIdx >= 0 && currFrameIdx < currentNode->frameList.size() - 1)
        nextframe = currentNode->frameList.at(currFrameIdx + 1);
    if (nextframe != nullptr && !nextframe->hasContent)
        nextframe = nullptr;

    // 节点基础信息获取
    isNeedDraw = true;
    isIcon = false;
    isLayout = false;
    emoteicon* tmpic = currentNode-> _filePtr->findsourceByName(frame->src);
    if (tmpic == nullptr)
        currentMtn = refTop->findmotionByName(frame->src);
    else
        currentMtn = nullptr;
    if (tmpic != nullptr)
    {
        isIcon = true;

        if (tmpic != ic) // 直接比对ic
        {
            ic = tmpic;
            ic->ensureLoad();
            width = ic->width;
            height = ic->height;
            originX = ic->originX;
            originY = ic->originY;
        }
        // 设置混色
        currbm = frame->bm;
    }
    else if (currentMtn != nullptr || strcmp(frame->src.c_str(), "layout") == 0 ||
             strcmp(frame->src.c_str(), "clip") == 0)
    {
        isLayout = true;

        // 直接用父类提供的区域
        if (width != lim.width || height != lim.height)
        {
            width = lim.width;
            height = lim.height;
            originX = lim.originX;
            originY = lim.originY;
        }
    }
    else
    {
        std::istringstream iss(frame->src);
        std::string token;
        std::getline(iss, token, '/');
        if (strcmp(token.c_str(), "blank") == 0)
        {
            std::getline(iss, token, ':');
            int32_t w = std::stoi(token);
            std::getline(iss, token, ':');
            int32_t h = std::stoi(token);
            std::getline(iss, token, ':');
            int32_t ox = std::stoi(token);
            std::getline(iss, token, ':');
            int32_t oy = std::stoi(token);

            if (width != w || height != h)
            {
                width = w;
                height = h;
            }
            originX = ox;
            originY = oy;
        }
        else if (strcmp(token.c_str(), "shape") == 0)
        {
            // shape节点: 不绘制但记录区域信息
            // shape的像素尺寸 = zx/zy * 16 (PSB规范: 单元正方形为16x16)
            // shape子类型从src的第二部分获取: rect(默认)、circle、point、quad
            const tjs_real shapeUnit = 16.0;
            isNeedDraw = false;
            width = (tjs_real)frame->zx * shapeUnit;
            height = (tjs_real)frame->zy * shapeUnit;
            originX = width / 2;
            originY = height / 2;
        }
        else
        {
            SDL_Log("source unsupported!!!--->%s", frame->src.c_str());
            isNeedDraw = false;
            return;
        }
    }
}
void emotenoderef::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    // 参数化时可能改变
    currTick = tick;
    // 对于motion，增加终结机制, 即无法越过selfSyncTime
    if (currentNode->_filePtr->isMotion && currTick > currentNode->_rootmotion->selfSyncTime &&
        currentNode->frameList.size() > 1 &&
        currentNode->frameList.at(currentNode->frameList.size() - 2)->type == 2 &&
        currentNode->frameList.at(currentNode->frameList.size() - 1)->type == 0)
        currTick = currentNode->_rootmotion->selfSyncTime;
    // 再来一个非时间戳节点, 采用时间永驻机制
    if (currentNode->_filePtr->isMotion && currentNode->frameList.size() == 2 &&
        currentNode->frameList.at(0)->type == 2 && currentNode->frameList.at(1)->type == 0)
        currTick = currentNode->frameList.at(0)->time;
    // 再来一个3->0的拦截机制，感觉确实未能理解motion的运行，只能凑合着搞了
    if (currentNode->_filePtr->isMotion && currentNode->frameList.size() > 1 &&
        currentNode->frameList.at(currentNode->frameList.size() - 1)->type == 0 &&
        currentNode->frameList.at(currentNode->frameList.size() - 2)->type == 3)
    {
        currTick = SDL_min(
            currTick, (float)currentNode->frameList.at(currentNode->frameList.size() - 2)->time);
    }
    // 不会处理，先跳过
    if (currentNode->type == 12)
    {
        checkDrawStatus(currTick, renderList, lim);
        isNeedDraw = true;
        isLayout = true;
        originX = lim.originX;
        originY = lim.originY;
        width = lim.width;
        height = lim.height;
    }
    // 对于参数节点 进行参数反查来定位tick
    else if (currentNode->isParameterize)
    {
        if (refMtn != nullptr)
        {
            float new_tick = refMtn->getTickByIdx(currentNode->parameterIdx);
            if (new_tick >= 0.0)
            {
                checkDrawStatus(new_tick, renderList, lim);
                currTick = new_tick;
            }
            else
                isNeedDraw = false;
        }
        else
            isNeedDraw = false;
    }
    else
    {
        // 时间戳判断 绘制信息检测
        checkDrawStatus(currTick, renderList, lim);
    }

    // 构建渲染方法
    renderMethod.clear();
    renderMethod = renderList;
    if ((!isNeedDraw && (width == 0 || height == 0)) || currentNode->type == 7)
    {
        originX = lim.originX;
        originY = lim.originY;
        width = lim.width;
        height = lim.height;
    }
    else
    {
        // 基础参数
        if (nextframe != nullptr &&
            ((frame->type != 2) ||
             (frame->type == 2 && nextframe->type == 2))) // 存在下一帧则对关键帧进行插值
        {
            // 针对nan/inf情形动态完成刷新
            if (std::isnan(frame->coordX))
                frame->coordX = -lim.originX;
            if (std::isnan(frame->coordY))
                frame->coordY = -lim.originY;
            if (std::isnan(nextframe->coordX))
                nextframe->coordX = -lim.originX;
            if (std::isnan(nextframe->coordY))
                nextframe->coordY = -lim.originY;
            if (std::isinf(frame->coordX))
                frame->coordX = lim.width - lim.originX;
            if (std::isinf(frame->coordY))
                frame->coordY = lim.height - lim.originY;
            if (std::isinf(nextframe->coordX))
                nextframe->coordX = lim.width - lim.originX;
            if (std::isinf(nextframe->coordY))
                nextframe->coordY = lim.height - lim.originY;

            // 坐标
            currCoordx =
                (frame->coordX + (nextframe->coordX - frame->coordX) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            currCoordy =
                (frame->coordY + (nextframe->coordY - frame->coordY) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            currCoordz =
                (frame->coordZ + (nextframe->coordZ - frame->coordZ) /
                                     (nextframe->time - frame->time) * (currTick - frame->time));
            // 透明度
            currOpa = (frame->opa + (nextframe->opa - frame->opa) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            // 变换参数(太sb了，感觉180才是分界点)
            if (nextframe->angle < 180 && frame->angle > 180) // 从 小360 到大0
            {
                currAngle = (frame->angle - 360 +
                             (nextframe->angle + 360 - frame->angle) /
                                 (nextframe->time - frame->time) * (currTick - frame->time));
            }
            else if (nextframe->angle > 180 && frame->angle < 180) // 从 大0 到 小360
            {
                currAngle =
                    (frame->angle + (nextframe->angle - 360 - frame->angle) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            }
            else
            {
                currAngle =
                    (frame->angle + (nextframe->angle - frame->angle) /
                                        (nextframe->time - frame->time) * (currTick - frame->time));
            }
            currSx = (frame->sx + (nextframe->sx - frame->sx) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currSy = (frame->sy + (nextframe->sy - frame->sy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currZx = (frame->zx + (nextframe->zx - frame->zx) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currZy = (frame->zy + (nextframe->zy - frame->zy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currOx = (frame->ox + (nextframe->ox - frame->ox) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            currOy = (frame->oy + (nextframe->oy - frame->oy) / (nextframe->time - frame->time) *
                                      (currTick - frame->time));
            // 偏移
            currTimeOffset = (frame->timeOffset + (nextframe->timeOffset - frame->timeOffset) /
                                                      (nextframe->time - frame->time) *
                                                      (currTick - frame->time));
            // 网格参数
            if (frame->hasbp || nextframe->hasbp)
            {
                isNeedBp = true;
                for (size_t i = 0; i < 32; i++)
                    currbp[i] = (frame->bp[i] + (nextframe->bp[i] - frame->bp[i]) /
                                                    (nextframe->time - frame->time) *
                                                    (currTick - frame->time));
            }
            else
                isNeedBp = false;
        }
        else
        {
            // 针对nan/inf情形动态完成刷新
            if (std::isnan(frame->coordX))
                frame->coordX = -lim.originX;
            if (std::isnan(frame->coordY))
                frame->coordY = -lim.originY;
            if (std::isinf(frame->coordX))
                frame->coordX = lim.width - lim.originX;
            if (std::isinf(frame->coordY))
                frame->coordY = lim.height - lim.originY;

            // 计算坐标
            currCoordx = frame->coordX;
            currCoordy = frame->coordY;
            currCoordz = frame->coordZ;
            // 透明度
            currOpa = frame->opa;
            // 变换参数
            currAngle = frame->angle;
            currSx = frame->sx, currSy = frame->sy;
            currZx = frame->zx, currZy = frame->zy;
            currOx = frame->ox, currOy = frame->oy;
            // 偏移
            currTimeOffset = frame->timeOffset;
            // 网格参数
            if (frame->hasbp)
            {
                isNeedBp = true;
                for (size_t i = 0; i < 32; i++)
                    currbp[i] = frame->bp[i];
            }
            else
                isNeedBp = false;
        }

        // 有深度信息时，穿透到最顶层
        if (renderMethod.size() > 0 && currCoordz != 0.0)
        {
            if (renderMethod.at(0).type == 3)
            {
                renderMethod.at(0).matTrans =
                    glm::translate(renderMethod.at(0).matTrans, glm::vec3(0.0, 0.0, currCoordz));
            }
            else
            {
                glm::mat4 proj = glm::ortho(-renderMethod.at(0).originX,
                                            renderMethod.at(0).width - renderMethod.at(0).originX,
                                            renderMethod.at(0).height - renderMethod.at(0).originY,
                                            -renderMethod.at(0).originY, lim.zMax, -lim.zMax);
                glm::mat4 last = glm::mat4(1.0f);
                last = glm::translate(last, glm::vec3(0.0, 0.0, currCoordz));
                last = last * glm::inverse(proj) * renderMethod.at(0).matTrans;
                renderMethod.at(0).matTrans = proj * last;
            }
            GLM_ASSERT_VALID(renderMethod.at(0).matTrans);
        }

        // 有模板信息时，穿透到最顶层
        if (renderMethod.size() > 0 && currentNode->stencilCompositeMaskLayerList.size() > 0 && currentNode->type == 12)
        {
            renderMethod.at(0).hasStencil = true;
            for (auto nodeName : currentNode->stencilCompositeMaskLayerList)
            {
                // 让父类去找节点
                emotenoderef* tmpNode = refMtn->getNodeRef(currentNode->_rootmotion->getNodeByName(nodeName));
                if (tmpNode != nullptr)
                {
                    renderMethod.at(0).layerNode.push_back(tmpNode);
                }
            }
        }

        // 是否需要同步父节点变形 玩不明白啊，反正视觉效果还行，就算了吧
        // if ((inheritMask >> 25 & 0x1) != 0x1 && renderMethod.size() > 0 &&
        // renderMethod.back().type == 1)
        //{
        //    emoteRender emt = renderMethod.back();
        //    renderMethod.pop_back();
        //    emt.type = 2;
        //    renderMethod.push_back(emt);
        //}

        // shape节点: 将currZx/currZy重置为1，避免与width/height双重缩放(px = zx * shapeUnit * 1)
        if (!isIcon && frame != nullptr && frame->src.rfind("shape/", 0) == 0)
        {
            currZx = 1.0f;
            currZy = 1.0f;
        }

        // 构建变换矩阵 平移 currCoordx/currCoordy → 缩放 zx/zy → 剪切 sx/sy → 旋转 angle
        glm::mat4 model = glm::mat4(1.0f); // 注:复合顺序是反过来的
        model = glm::translate(model, glm::vec3(currCoordx, currCoordy, 0));
        model = glm::rotate(model, glm::radians(currAngle), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::mat4(1.0f, currSy, 0.0f, 0.0f, currSx, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                          0.0f, 0.0f, 0.0f, 0.0f, 1.0f) *
                model;
        model = glm::scale(model, glm::vec3(currZx, currZy, 1.0f));

        if (isLayout) // 不绘制，不处理网格数据，只构建变换矩阵
        {
            if (renderMethod.size() > 0 &&
                renderMethod.back().type == 3) // 如果上一级是layout的话，则进行合并
            {
                // 构造渲染方法结构
                emoteRender emt = renderMethod.back();
                renderMethod.pop_back();
                emt.type = 3;
                emt.opa *= currOpa;

                // 更新矩阵
                emt.matTrans = emt.matTrans * model;

                // fbo信息
                emt.originX = originX;
                emt.originY = originY;
                emt.width = width;
                emt.height = height;
                emt.label = currentNode->label;
                GLM_ASSERT_VALID(emt.matTrans);
                renderMethod.push_back(emt);
            }
            else
            {
                // 构造渲染方法结构
                emoteRender emt;
                emt.type = 3;
                emt.opa = currOpa;

                // 如果父类没有网格变形，则进行矩阵解耦并外加合并
                if (renderMethod.size() > 1 && renderMethod.back().type == 2)
                {
                    glm::mat4 demuxMat =
                        glm::scale(renderMethod.back().matTrans,
                                   glm::vec3(1 / renderMethod.back().width,
                                             1 / renderMethod.back().height, 1.0f));
                    demuxMat =
                        glm::translate(demuxMat, glm::vec3(renderMethod.back().originX,
                                                           renderMethod.back().originY, 0.0f));
                    emt.matTrans = demuxMat * model;
                    emt.opa *= renderMethod.back().opa;
                    renderMethod.pop_back();
                    // 绘制层解耦
                    originX = renderMethod.back().originX;
                    originY = renderMethod.back().originY;
                    width = renderMethod.back().width;
                    height = renderMethod.back().height;
                    emt.matTrans =
                        glm::inverse(glm::ortho(-originX, width - originX, height - originY,
                                                -originY, lim.zMax, -lim.zMax)) *
                        emt.matTrans;
                }
                else // 正常更新矩阵
                {
                    emt.matTrans = model;
                }

                // fbo信息
                emt.originX = originX;
                emt.originY = originY;
                emt.width = width;
                emt.height = height;
                emt.label = currentNode->label;
                GLM_ASSERT_VALID(emt.matTrans);
                renderMethod.push_back(emt);
            }
        }
        else // 需要绘制，构建绘制矩阵和变形参数
        {
            if (renderMethod.size() > 0 &&
                renderMethod.back().type == 3) // 如果上一级是layout的话，则进行合并
            {
                // 构造渲染方法结构
                emoteRender emt = renderMethod.back();
                renderMethod.pop_back();
                emt.type = 2;
                emt.opa *= currOpa;
                if (isNeedBp)
                {
                    for (size_t i = 0; i < 32; i++)
                        emt.controlPts[i] = currbp[i];
                    emt.type = 1;
                }

                // 更新矩阵
                glm::mat4 projection =
                    glm::ortho(-lim.originX, lim.width - lim.originX, lim.height - lim.originY,
                               -lim.originY, lim.zMax, -lim.zMax);
                model =
                    glm::translate(model, glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                model = glm::scale(model, glm::vec3(width, height, 1.0f));
                emt.matTrans = projection * emt.matTrans * model;

                // fbo信息
                emt.originX = originX;
                emt.originY = originY;
                emt.width = width;
                emt.height = height;
                emt.label = currentNode->label;
                GLM_ASSERT_VALID(emt.matTrans);
                renderMethod.push_back(emt);
            }
            else
            {
                // 构造渲染方法结构
                emoteRender emt;
                emt.type = 2;
                emt.opa = currOpa;
                if (isNeedBp)
                {
                    for (size_t i = 0; i < 32; i++)
                        emt.controlPts[i] = currbp[i];
                    emt.type = 1;
                }

                // 如果父类没有网格变形，则进行矩阵解耦并外加合并
                if (renderMethod.size() > 0 && renderMethod.back().type == 2)
                {
                    glm::mat4 demuxMat =
                        glm::scale(renderMethod.back().matTrans,
                                   glm::vec3(1 / renderMethod.back().width,
                                             1 / renderMethod.back().height, 1.0f));
                    demuxMat =
                        glm::translate(demuxMat, glm::vec3(renderMethod.back().originX,
                                                           renderMethod.back().originY, 0.0f));
                    model = glm::translate(model,
                                           glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                    model = glm::scale(model, glm::vec3(width, height, 1.0f));
                    emt.matTrans = demuxMat * model;
                    emt.opa *= renderMethod.back().opa;
                    renderMethod.pop_back();
                }
                else // 正常更新矩阵
                {
                    glm::mat4 projection =
                        glm::ortho(-lim.originX, lim.width - lim.originX, lim.height - lim.originY,
                                   -lim.originY, lim.zMax, -lim.zMax);
                    model = glm::translate(model,
                                           glm::vec3(-originX - currOx, -originY - currOy, 0.0f));
                    model = glm::scale(model, glm::vec3(width, height, 1.0f));
                    emt.matTrans = projection * model;
                }

                // fbo信息
                emt.originX = originX;
                emt.originY = originY;
                emt.width = width;
                emt.height = height;
                emt.label = currentNode->label;
                GLM_ASSERT_VALID(emt.matTrans);
                renderMethod.push_back(emt);
            }
        }
    }

    // 对于icon保存大小信息
    if (isIcon && renderMethod.size() > 0)
    {
        // 两个端点就够了
        glm::vec2 pt1(0, 0), pt2(1, 1);
        // 将所有矩阵变形进行作用
        glm::vec4 tmpVec1(0, 0, 0, 0), tmpVec2(1, 1, 0, 0);
        for (int32_t i = renderMethod.size() - 1; i >= 0; i--)
        {
            // 不想改了，这里自适应一下吧
            // [0,1]-matTrans->[-1,1]-compute->[0,1]
            if (i != renderMethod.size() - 1)
            {
                tmpVec1 = glm::vec4(tmpVec1.x / 2 + 0.5, 1 - (tmpVec1.y / 2 + 0.5), 0, 0);
                tmpVec2 = glm::vec4(tmpVec2.x / 2 + 0.5, 1 - (tmpVec2.y / 2 + 0.5), 0, 0);
            }
            tmpVec1 = renderMethod.at(i).matTrans * glm::vec4(tmpVec1.x, tmpVec1.y, 0.0f, 1.0f);
            tmpVec2 = renderMethod.at(i).matTrans * glm::vec4(tmpVec2.x, tmpVec2.y, 0.0f, 1.0f);
        }
        // 边界缩放
        pt1.x = (tmpVec1.x / 2.0 + 0.5) * currentNode->_filePtr->_screenSize.width;
        pt1.y = (1 - (tmpVec1.y / 2.0 + 0.5)) * currentNode->_filePtr->_screenSize.height;
        pt2.x = (tmpVec2.x / 2.0 + 0.5) * currentNode->_filePtr->_screenSize.width;
        pt2.y = (1 - (tmpVec2.y / 2.0 + 0.5)) * currentNode->_filePtr->_screenSize.height;
        // 保存区域(使用独立的shapeList，避免状态重复)
        emoterect tmprect;
        tmprect.left = pt1.x;
        tmprect.top = pt1.y;
        tmprect.width = pt2.x - pt1.x;
        tmprect.height = pt2.y - pt1.y;
        shapeList.push_back(tmprect);
    }

    // 对于shape节点保存面积信息(用于contains检测和getLayerGetter的shape返回)
    // 注: shape节点可能没有hasContent，用src判断即可
    if (!isIcon && frame != nullptr && refMtn != nullptr)
    {
        std::string src(frame->src);
        if (src.rfind("shape/", 0) == 0 && renderMethod.size() > 0)
        {
            // 两个端点就够了
            glm::vec2 pt1(0, 0), pt2(1, 1);
            // 将所有矩阵变形进行作用
            glm::vec4 tmpVec1(0, 0, 0, 0), tmpVec2(1, 1, 0, 0);
            for (int32_t i = renderMethod.size() - 1; i >= 0; i--)
            {
                // 不想改了，这里自适应一下吧
                // [0,1]-matTrans->[-1,1]-compute->[0,1]
                if (i != renderMethod.size() - 1)
                {
                    tmpVec1 = glm::vec4(tmpVec1.x / 2 + 0.5, 1 - (tmpVec1.y / 2 + 0.5), 0, 0);
                    tmpVec2 = glm::vec4(tmpVec2.x / 2 + 0.5, 1 - (tmpVec2.y / 2 + 0.5), 0, 0);
                }
                tmpVec1 = renderMethod.at(i).matTrans * glm::vec4(tmpVec1.x, tmpVec1.y, 0.0f, 1.0f);
                tmpVec2 = renderMethod.at(i).matTrans * glm::vec4(tmpVec2.x, tmpVec2.y, 0.0f, 1.0f);
            }
            // 边界缩放
            pt1.x = (tmpVec1.x / 2.0 + 0.5) * currentNode->_filePtr->_screenSize.width;
            pt1.y = (1 - (tmpVec1.y / 2.0 + 0.5)) * currentNode->_filePtr->_screenSize.height;
            pt2.x = (tmpVec2.x / 2.0 + 0.5) * currentNode->_filePtr->_screenSize.width;
            pt2.y = (1 - (tmpVec2.y / 2.0 + 0.5)) * currentNode->_filePtr->_screenSize.height;
            // 保存区域(使用独立的shapeList，避免状态重复)
            emoterect tmprect;
            tmprect.label = currentNode->label;
            tmprect.left = pt1.x;
            tmprect.top = pt1.y;
            tmprect.width = pt2.x - pt1.x;
            tmprect.height = pt2.y - pt1.y;
            // 根据src后缀确定shape子类型: rect/circle/point/quad
            std::string srcType = src.substr(6); // 去掉"shape/"
            if (srcType == "circle")
                tmprect.shapeType = 1;
            else if (srcType == "point")
                tmprect.shapeType = 0;
            else if (srcType == "quad")
                tmprect.shapeType = 3;
            else
                tmprect.shapeType = 2; // rect默认
            refMtn->shapeNodeAreas.push_back(tmprect);
        }
    }

    // 传递给子类: 通过_parentMotion查找对应ref，避免创建重复状态
    if (refMtn != nullptr)
    {
        for (auto ch : currentNode->children)
        {
            emotenoderef* childRef = refMtn->getNodeRef(ch);
            if (childRef)
            {
                childRef->progress(tick, renderMethod, {originX, originY, width, height, lim.zMax});
                shapeList.insert(shapeList.end(), childRef->getShapeList().begin(), childRef->getShapeList().end());
            }
        }

        // 处理子motion: 创建子emotemotionref递归处理
        if (currentMtn != nullptr)
        {
            // 在引擎中创建持久化的子motion ref
            currentMtnRef = new emotemotionref(currentMtn, refTop);
            refMtn->_subMotionRefs.push_back(currentMtnRef);
            currentMtnRef->progress(tick + currTimeOffset, renderMethod,
                            {originX, originY, width, height, lim.zMax});
            // 收集子motion的shape
            shapeList.insert(shapeList.end(), currentMtnRef->getShapeList().begin(),
                             currentMtnRef->getShapeList().end());
        }
    }
}
void emotenoderef::draw(GLuint targetFbo, emotelimit lim, GLuint exFbo, GLuint exTex)
{
    if (!isNeedDraw || !isIcon || renderMethod.size() < 1 || currentNode->removed)
        return; // 跳过无需绘制的 和 非icon的 和 无method 的节点

    //  提前绘制好蒙版texture
    if (renderMethod.at(0).hasStencil && exFbo != 0) // 进行Stencil过滤 不考虑复合蒙版的情况了
    {
        glBindFramebuffer(GL_FRAMEBUFFER, exFbo);
        glBaseSet();
        for (auto maskLayer : renderMethod.at(0).layerNode)
        {
            if (maskLayer != nullptr)
                maskLayer->draw(exFbo, lim, 0, 0);
        }
    }

    // clear
    glBindFramebuffer(GL_FRAMEBUFFER, targetFbo);
    glUseProgram(emotenodeprogram);
    glViewport(0, 0, lim.width, lim.height);
#if _KRKRSDL3_GL
    if (renderMethod.size() > 64)
#else
    if (renderMethod.size() > 24)
#endif
    {
        SDL_Log("render:%s failed!!!", currentNode->label.c_str());
        return;
    }

    // bm
    bool enableColor = false;
    float uniformColor[4] = {0};
    switch (currbm)
    {
        case 0:
        {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
            break;
        }
        case 1:
        {
            glBlendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            break;
        }
        case 3:
        {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
            break;
        }
        case 4:
        {
            glBlendFuncSeparate(GL_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            break;
        }
        case 21:
        {
            enableColor = true;
            if (frame)
            {
                uniformColor[0] = (frame->color & 0xFF) / 255;
                uniformColor[1] = ((frame->color >> 8) & 0xFF) / 255;
                uniformColor[2] = ((frame->color >> 16) & 0xFF) / 255;
                uniformColor[3] = ((frame->color >> 24) & 0xFF) / 255;
            }
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glBlendEquation(GL_FUNC_ADD);
            break;
        }
        case 6: // TODO
        {
            return;
        }
        default:
        {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
        }
    }
    // renderMethod
    glUniform1i(glGetUniformLocation(emotenodeprogram, "surfaceCount"), renderMethod.size());
    int idxCnt = 0;
    float totalOpa = currOpa;
    for (int32_t i = renderMethod.size() - 1; i >= 0; i--)
    {
        // opa
        totalOpa *= renderMethod.at(i).opa;
        // transform
        char uniformName[32];
        sprintf(uniformName, "transforms[%d]", idxCnt);
        glUniformMatrix4fv(glGetUniformLocation(emotenodeprogram, uniformName), 1, GL_FALSE,
                           glm::value_ptr(renderMethod.at(i).matTrans));
        // controlPoints
        sprintf(uniformName, "controlPoints[%d]", idxCnt);
        if (renderMethod.at(i).type == 1)
            glUniform2fv(glGetUniformLocation(emotenodeprogram, uniformName), 16,
                         renderMethod.at(i).controlPts);
        else
            glUniform2fv(glGetUniformLocation(emotenodeprogram, uniformName), 16,
                         default_control_points);
        idxCnt++;
    }
    // opa
    glUniform1f(glGetUniformLocation(emotenodeprogram, "opa"), totalOpa);
    // texture
    if (renderMethod.at(0).hasStencil && exFbo != 0) // 使用exTex作为蒙版过滤
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ic->selftexture);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "texture1"), 0);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "enableMask"), true);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "enableColor"), false);
        glUniform2f(glGetUniformLocation(emotenodeprogram, "viewportSize"), lim.width, lim.height);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, exTex);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "maskTexture"), 1);
        glDrawArrays(GL_PATCHES, 0, 16);
    }
    else
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ic->selftexture);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "texture1"), 0);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "enableMask"), false);
        glUniform1i(glGetUniformLocation(emotenodeprogram, "enableColor"), enableColor);
        glUniform4f(glGetUniformLocation(emotenodeprogram, "uniformColor"), uniformColor[0],
                    uniformColor[1], uniformColor[2], uniformColor[3]);
        glDrawArrays(GL_PATCHES, 0, 16);
    }
}
float emotenoderef::getCurrentRenderZ()
{
    if (renderMethod.size() > 0)
    {
        return renderMethod.at(0).matTrans[3][2];
    }
    return 0;
}

emotemotionref::~emotemotionref()
{
    for (auto sub : _subMotionRefs)
        delete sub;
    _subMotionRefs.clear();
}
float emotemotionref::getTickByIdx(int32_t idx)
{
    if (idx >= currentMotion->parameter.size() || idx < 0)
        return -1.0f;
    float currVal = 0;
    // file系控制
    if (!currentMotion->_filePtr->getTickByName(currentMotion->parameter.at(idx)->id, currVal))
    {
        // mtn系控制
        auto itmMap = currentMotion->parameterCache.find(currentMotion->parameter.at(idx)->id);
        if (itmMap != currentMotion->parameterCache.end())
        {
            return itmMap->second;
        }
    }
    return currentMotion->parameter.at(idx)->transToTick(currVal);
}
emotenoderef* emotemotionref::getNodeRef(emotenode* node)
{
    // 在_nodeCache中查找指定node的ref
    for (auto& ref : _nodeCache)
    {
        if (ref.currentNode == node)
            return &ref;
    }
    return nullptr;
}
void emotemotionref::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    // 起始
    shapeList.clear();
    shapeNodeAreas.clear();
    renderMethod.clear();
    renderMethod = renderList;

    // 按priority顺序构建_nodeCache
    _nodeCache.clear();
    if (currentMotion == nullptr) return;
    size_t count = currentMotion->nodeList.size();
    _nodeCache.reserve(count);
    for (size_t i = 0; i < count; i++)
    {
        _nodeCache.emplace_back(currentMotion->nodeList[i], refTop, this);
    }

    // 清理旧的子motion ref
    for (auto sub : _subMotionRefs)
        delete sub;
    _subMotionRefs.clear();

    //  对每个layer节点调用对应的ref->progress
    //  注意: ref的progress内部会通过_parentMotion递归处理children和sub-motion
    for (auto ch : currentMotion->layer)
    {
        float actualTick = tick;
        if (currentMotion->loopTime >= 0)
        {
            if (currentMotion->loopTime == 0)
                actualTick = std::fmod(tick, currentMotion->lastTime);
            else
                actualTick = std::fmod(tick, currentMotion->loopTime);
        }

        std::vector<emoteRender> localRender = renderList;
        if (localRender.size() > 0 && ch->type == 2)
        {
            localRender.at(0).hasStencil = false;
            localRender.at(0).layerNode.clear();
        }

        emotenoderef* ref = getNodeRef(ch);
        if (ref)
        {
            ref->progress(actualTick, localRender, lim);
            shapeList.insert(shapeList.end(), ref->getShapeList().begin(), ref->getShapeList().end());
        }
    }
}
void emotemotionref::draw(GLuint targetFbo, emotelimit lim, GLuint exFbo, GLuint exTex)
{
    if (_nodeCache.empty()) return;

    // 递归收集所有可绘制ref(展开嵌套子motion)
    std::vector<emotenoderef*> drawList;
    // 先展开所有的motion情形
    std::vector<emotenoderef*> stack;
    for (auto it = _nodeCache.begin();
        it != _nodeCache.end(); ++it)
    {
        stack.push_back(&(*it));
    }
    // 再递归获取全部
    while (!stack.empty())
    {
        emotenoderef* current = stack.back();
        stack.pop_back();

        if (current->currentMtn == nullptr)
        {
            drawList.push_back(current);
        }
        else
        {
            for (auto it = current->currentMtnRef->_nodeCache.begin();
                 it != current->currentMtnRef->_nodeCache.end();
                 ++it)
            {
                stack.push_back(&(*it));
            }
        }
    }

    // 按z排序(同dev分支)
    std::stable_sort(drawList.begin(), drawList.end(),
                     [](emotenoderef* a, emotenoderef* b)
                     { return a->getCurrentRenderZ() < b->getCurrentRenderZ(); });

    // 绘制
    for (auto r : drawList)
    {
        if (r != nullptr)
        {
            r->draw(targetFbo, lim, exFbo, exTex);
        }
    }
}
bool emotemotionref::contains(tjs_real x, tjs_real y)
{
    // 检查icon节点的shapeList
    for (auto mtnRec : shapeList)
    {
        if (x >= mtnRec.left && x < mtnRec.left + mtnRec.width && y >= mtnRec.top && y < mtnRec.top + mtnRec.height)
        {
            return true;
        }
    }
    return false;
}

emoteengine::~emoteengine()
{
    if (_mainMotionRef)
        delete _mainMotionRef;
}
void emoteengine::progress(float tick, std::vector<emoteRender>& renderList, emotelimit lim)
{
    // 清除shape信息
    shapeList.clear();

    // 初始 _mainMotionRef
    if (_mainmotion == nullptr)
        return;
    if (_mainMotionRef == nullptr)
        _mainMotionRef = new emotemotionref(_mainmotion, this);
    if (_mainMotionRef->currentMotion != _mainmotion)
        _mainMotionRef->currentMotion = _mainmotion;

    // progress
    _mainMotionRef->progress(tick, renderList, lim);

    // 收集shape信息
    shapeList = _mainMotionRef->getShapeList();
}
void emoteengine::draw(GLuint targetFbo, emotelimit lim, GLuint exFbo, GLuint exTex)
{
    if (_mainMotionRef)
    {
        _mainMotionRef->draw(targetFbo, lim, exFbo, exTex);
    }
}
void emoteengine::addEmoteFile(emotefile* itm)
{
    _attach.push_back(itm);
}
float emoteengine::getZMax()
{
    float _zMaxTmp = 0.0;
    if (_mainfile) _zMaxTmp = _mainfile->getZMax();
    // 附属
    for (auto itm : _attach)
    {
        if (itm->getZMax() > _zMaxTmp)
            _zMaxTmp = itm->getZMax();
    }
    return _zMaxTmp;
}
emotemotion* emoteengine::findmotionByName(const std::string& name)
{
    std::istringstream iss(name);
    std::string token, token1, token2;

    // 源头
    std::getline(iss, token, '/');
    if (strcmp(token.c_str(), "motion") == 0)
    {
        std::getline(iss, token1, '/');
        std::getline(iss, token2, '/');
        // 一级路径
        auto tmp = _mainfile->_objects.find(token1.c_str());
        if (tmp != _mainfile->_objects.end())
        {
            emoteobject* src = tmp->second;
            // 二级路径
            auto tmp1 = src->motion.find(token2.c_str());
            if (tmp1 != src->motion.end())
            {
                return tmp1->second;
            }
        }
        // 附属
        for (auto itm : _attach)
        {
            // 一级路径
            tmp = itm->_objects.find(token1.c_str());
            if (tmp != itm->_objects.end())
            {
                emoteobject* src = tmp->second;
                // 二级路径
                auto tmp1 = src->motion.find(token2.c_str());
                if (tmp1 != src->motion.end())
                {
                    return tmp1->second;
                }
            }
        }
        SDL_Log("motion find failed!!!");
    }
    else
    {
        return nullptr;
    }
    return nullptr;
}
void emoteengine::updateEyeControl(float tick, bool isMain)
{
    std::default_random_engine dre;
    // 眼动控制
    for (auto itm : _mainfile->_metadata->_eyeControl)
    {
        // 初始化tick
        if (!itm->hasStart)
        {
            itm->hasStart = true;
            itm->lastTick = tick;
        }
        // 初始化间隔
        if (itm->currWaitInterval < 0)
        {
            itm->currWaitInterval = itm->uid(dre);
        }

        // 播放时 进行参数控制
        if (itm->isBlinking)
        {
            if (tick - itm->lastTick > itm->blinkFrameCount) // 是否结束
            {
                itm->isBlinking = false;
                itm->currWaitInterval = -1;
                itm->lastTick = tick;
                // 重置初始值
                auto varPos = _mainfile->_metadata->_varList.find(itm->label);
                if (varPos != _mainfile->_metadata->_varList.end())
                {
                    varPos->second = itm->baseVal;
                }
            }
            else
            {
                // 计算数值
                float realVal = itm->beginFrame;
                if ((tick - itm->lastTick) * 2 < itm->blinkFrameCount) // 闭眼
                {
                    realVal += (tick - itm->lastTick) * 2 * (itm->endFrame - itm->beginFrame) /
                               itm->blinkFrameCount;
                }
                else // 睁开
                {
                    realVal += (itm->blinkFrameCount - (tick - itm->lastTick)) * 2 *
                               (itm->endFrame - itm->beginFrame) / itm->blinkFrameCount;
                }
                realVal = std::max(realVal, itm->beginFrame);
                realVal = std::min(realVal, itm->endFrame);
                // 写入参数
                auto varPos = _mainfile->_metadata->_varList.find(itm->label);
                if (varPos != _mainfile->_metadata->_varList.end())
                {
                    varPos->second = realVal;
                }
            }
        }
        else // 未播放时进行等待
        {
            if (tick - itm->lastTick > itm->currWaitInterval)
            {
                itm->isBlinking = true;
                itm->lastTick = tick;
                // 获取初始值
                auto varPos = _mainfile->_metadata->_varList.find(itm->label);
                if (varPos != _mainfile->_metadata->_varList.end())
                {
                    itm->baseVal = varPos->second;
                }
            }
        }
    }
    for (auto itm : _mainfile->_metadata->_eyeControl)
    {
        // nothing to do
    }
}
void emoteengine::startTimeline(float tick, const std::string& name, bool isMain)
{
    for (auto itm : _mainfile->_metadata->_timelineControl)
    {
        if (strcmp(itm->label.c_str(), name.c_str()) == 0)
        {
            currTimeline.push_back(itm);
            currStartTick = tick;
            return;
        }
    }
}
void emoteengine::stopTimeline(const std::string& name, bool isMain)
{
    for (auto itm : _mainfile->_metadata->_timelineControl)
    {
        if (strcmp(itm->label.c_str(), name.c_str()) == 0)
        {
            currTimeline.erase(std::remove(currTimeline.begin(), currTimeline.end(), itm),
                               currTimeline.end());
            currStartTick = -1.0f;
            return;
        }
    }
}
bool emoteengine::checkTimline(const std::string& name, bool& result, bool isMain)
{
    emotetimeline* matchT = nullptr;
    for (auto itm : _mainfile->_metadata->_timelineControl)
    {
        if (strcmp(itm->label.c_str(), name.c_str()) == 0)
        {
            matchT = itm;
            break;
        }
    }
    if (matchT == nullptr)
        return false;
    for (auto itm : currTimeline)
    {
        if (itm == matchT)
        {
            result = true;
            return true;
        }
    }
    result = false;
    return true;
}
void emoteengine::updateTimelineControl(float tick, bool isMain)
{
    if (currTimeline.size() < 1)
        return;

    for (auto timelineItm : currTimeline)
    {
        // 考察时间
        if (timelineItm->loopEnd > 0.0f &&
            tick - currStartTick + timelineItm->loopBegin > timelineItm->loopEnd)
        {
            // 重置时间戳
            currStartTick = tick;
        }
        float currRelTime = tick - currStartTick + timelineItm->loopBegin;

        // 遍历每一个变量
        for (auto varItm : timelineItm->variableList)
        {
            // 跳过
            if (varItm->frameList.size() == 0)
                continue;

            // 对于select变量进行跳过
            bool isfindInSelect = false;
            for (auto sleItm : _mainfile->_metadata->_selectorControl)
            {
                for (auto _sleItm : sleItm->selectItem)
                {
                    if (strcmp(_sleItm.label.c_str(), varItm->label.c_str()) == 0)
                    {
                        isfindInSelect = true;
                        break;
                    }
                }
                if (isfindInSelect)
                    break;
            }
            if (isfindInSelect)
                continue;

            // 确定帧位置
            emoteTimeVarFrame* currFrame = nullptr;
            size_t currFrameIdx = -1;
            for (size_t i = 0; i < varItm->frameList.size(); i++)
            {
                if (varItm->frameList.at(i)->time <= currRelTime)
                {
                    currFrame = varItm->frameList.at(i);
                    currFrameIdx = i;
                }
                else
                    break;
            }
            if (currFrame == nullptr || !currFrame->hasContent)
                continue;
            emoteTimeVarFrame* nextframe = nullptr;
            if (currFrameIdx >= 0 && currFrameIdx < varItm->frameList.size() - 1)
                nextframe = varItm->frameList.at(currFrameIdx + 1);
            if (nextframe != nullptr && !nextframe->hasContent)
                nextframe = nullptr;

            // 插值
            double realVal = 0.0;
            if (nextframe != nullptr &&
                ((currFrame->type != 2) ||
                 (currFrame->type == 2 && nextframe->type == 2))) // 存在下一帧则对关键帧进行插值
            {
                // val
                realVal = (currFrame->value + (nextframe->value - currFrame->value) /
                                                  (nextframe->time - currFrame->time) *
                                                  (currRelTime - currFrame->time));
            }
            else
            {
                // 计算坐标
                realVal = currFrame->value;
            }

            // 赋予
            auto varPos = _mainfile->_metadata->_varList.find(varItm->label);
            if (varPos != _mainfile->_metadata->_varList.end())
            {
                varPos->second = realVal;
            }
        }
    }
}
emoteVar* emoteengine::findVarByName(const std::string& name)
{
    for (auto obj : _mainfile->_objects)
    {
        emoteVar* ret = nullptr;
        ret = obj.second->findVarByName(name);
        if (ret != nullptr)
            return ret;
    }
    return nullptr;
}
static bool startswith(const std::string& str, const std::string& prefix)
{
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}
static bool endswith(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}
void emoteengine::setVariable(const std::string& name, tjs_real value)
{
    SDL_Log("key->%s val->%f", name.c_str(), value);

    // 所有file
    std::vector<emotefile*> allFiles = _attach;
    allFiles.push_back(_mainfile);
    for (auto tmpFile : allFiles)
    {
        // 设置(metadata _varList / _selectorControl)
        tmpFile->setVariable(name, value);

        // motion_inter (name = "motionName/varName")
        size_t pos = name.find('/');
        if (pos != std::string::npos)
        {
            std::string motionName = name.substr(0, name.find('/'));
            emoteobject* obj = nullptr;
            for (auto objItm : _mainfile->_objects)
            {
                if (objItm.first == motionName)
                {
                    obj = objItm.second;
                    break;
                }
            }
            if (obj)
            {
                std::string varName = name.substr(name.find('/') + 1);
                for (auto mtnItm : obj->motion)
                {
                    for (auto varItm : mtnItm.second->parameter)
                    {
                        if (varItm->id == varName)
                        {
                            mtnItm.second->parameterCache[varItm->id] = value;
                            break;
                        }
                    }
                }
            }
        }
        else
        {
            // 简单名(如"helptext"): 遍历所有motion的parameter, 匹配id后写入parameterCache
            if (tmpFile != nullptr)
            {
                for (auto& objPair : tmpFile->_objects)
                {
                    for (auto& mtnPair : objPair.second->motion)
                    {
                        for (auto varItm : mtnPair.second->parameter)
                        {
                            if (varItm->id == name)
                            {
                                mtnPair.second->parameterCache[varItm->id] = value;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}
tjs_real emoteengine::getVariable(const std::string& name)
{
    size_t pos = name.find('/');
    if (pos != std::string::npos) // motion类
    {
        std::string motionName = name.substr(0, name.find('/'));
        emoteobject* obj = nullptr;
        for (auto objItm : _mainfile->_objects)
        {
            if (objItm.first == motionName)
            {
                obj = objItm.second;
                break;
            }
        }
        if (obj)
        {
            std::string varName = name.substr(name.find('/') + 1);
            for (auto mtnItm : obj->motion)
            {
                for (auto varItm : mtnItm.second->parameter)
                {
                    if (varItm->id == varName)
                    {
                        return mtnItm.second->parameterCache[varItm->id];
                    }
                }
            }
        }
    }
    else // emote类
    {
        auto varPos = _mainfile->_metadata->_varList.find(name);
        if (varPos != _mainfile->_metadata->_varList.end())
        {
            return varPos->second;
        }
    }
    return 0.0;
}
void emoteengine::updatePhysics(float tick)
{
}
}