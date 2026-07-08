#include "tjsCommHead.h"

#include "PlatformView.h"
#include "TVPDebug.h"

#include <SDL3/SDL_render.h>

extern SDL_Renderer* tvp_renderer;
std::vector<std::string> TVPListAllRenderBackend()
{
    ttstr log(TJS_N("Available Render:"));
    std::vector<std::string> backends;
    int count = SDL_GetNumRenderDrivers();
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetRenderDriver(i);
        if (name) {
            backends.push_back(name);
            log += " " + ttstr((const char*)name);
        }
    }
    TVPAddImportantLog(log);
    
    return backends;
}

void TVPCreateTextureBackend(TVPSprite& sp)
{
    sp.texture.swTexture = SDL_CreateTexture(tvp_renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, sp.width, sp.height);
    SDL_BlendMode customBlendMode =
        SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,                 // 源因子
                                   SDL_BLENDFACTOR_ZERO,                // 目标因子
                                   SDL_BLENDOPERATION_ADD,              // 混合操作
                                   SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, // 源因子（Alpha）
                                   SDL_BLENDFACTOR_SRC_ALPHA,           // 目标因子（Alpha）
                                   SDL_BLENDOPERATION_ADD               // 混合操作（Alpha）
        );
    SDL_SetTextureBlendMode((SDL_Texture*)sp.texture.swTexture, customBlendMode);
}

void TVPUpdateTextureBackend(TVPSprite* sp, uint8_t* buff, int width, int height, int pitch)
{
    SDL_UpdateTexture((SDL_Texture*)sp->texture.swTexture, nullptr, buff, pitch);
}

void TVPDestroyTextureBackend(TVPSprite* sp)
{
    SDL_DestroyTexture((SDL_Texture*)sp->texture.swTexture);
}

void TVPRenderTextureBackend(TVPSprite* sp, int posX, int posY, int width, int height)
{
    SDL_FRect rectBuff;
    rectBuff.x = posX;
    rectBuff.y = posY;
    rectBuff.w = width;
    rectBuff.h = height;
    SDL_RenderTexture(tvp_renderer, (SDL_Texture*)sp->texture.swTexture, NULL, &rectBuff);
}