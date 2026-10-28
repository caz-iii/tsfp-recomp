#include "video/video.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/runtime.h"

#define TSFP_GL_DEF(ret, name, ...) ret (*gl##name)(__VA_ARGS__) = nullptr;
TSFP_GL_FUNCS(TSFP_GL_DEF)
#undef TSFP_GL_DEF

bool gl_load() {
    bool ok = true;
#define TSFP_GL_LOAD(ret, name, ...)                                                     \
    gl##name = (ret(*)(__VA_ARGS__))SDL_GL_GetProcAddress("gl" #name);                    \
    if (!gl##name) { rt_log("video: missing gl" #name); ok = false; }
    TSFP_GL_FUNCS(TSFP_GL_LOAD)
#undef TSFP_GL_LOAD
    return ok;
}

namespace {

SDL_Window* g_window = nullptr;
SDL_GLContext g_ctx = nullptr;
GLuint g_fbo = 0, g_color = 0, g_depth = 0;
int g_scale = 2;
uint32_t g_presented = 0;
const char* g_dump_dir = nullptr;
uint32_t g_dump_every = 0;

}  // namespace

bool video_init(bool headless) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        rt_log("video: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (const char* s = getenv("TSFP_SCALE")) g_scale = atoi(s) > 0 ? atoi(s) : 2;
    g_dump_dir = getenv("TSFP_DUMP_DIR");
    g_dump_every = getenv("TSFP_DUMP_EVERY") ? (uint32_t)atoi(getenv("TSFP_DUMP_EVERY")) : 60;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("TimeSplitters: Future Perfect (OpenMonkey)", 640 * g_scale, 480 * g_scale,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
    if (!g_window) {
        rt_log("video: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx || !gl_load()) {
        rt_log("video: OpenGL 3.3 context unavailable: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(0);  // pacing comes from the emulated VI

    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEfbWidth * g_scale, kEfbHeight * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenTextures(1, &g_depth);
    glBindTexture(GL_TEXTURE_2D, g_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kEfbWidth * g_scale, kEfbHeight * g_scale, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, nullptr);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        rt_log("video: EFB framebuffer incomplete");
        return false;
    }
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    rt_log("video: %dx%d window, EFB at %dx internal resolution", 640 * g_scale, 480 * g_scale, g_scale);
    return true;
}

int video_scale() { return g_scale; }
void video_bind_efb() { glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); }
GLuint video_efb_color() { return g_color; }
uint32_t video_frames_presented() { return g_presented; }

void video_present(int sx, int sy, int sw, int sh) {
    if (!g_window) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
    // GL's origin is bottom-left; the EFB is stored top-down (y flipped on draw).
    int s = g_scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Keep 4:3 inside the window.
    int dw = ww, dh = ww * 3 / 4;
    if (dh > wh) { dh = wh; dw = wh * 4 / 3; }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    int y0 = (kEfbHeight - sy - sh) * s, y1 = (kEfbHeight - sy) * s;
    glBlitFramebuffer(sx * s, y0, (sx + sw) * s, y1, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    g_presented++;
    if (g_dump_dir && g_dump_every && g_presented % g_dump_every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%05u.bmp", g_dump_dir, g_presented);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        video_dump_bmp(path);
    }
    SDL_GL_SwapWindow(g_window);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}

bool video_pump_events() {
    SDL_Event e;
    bool open = true;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) open = false;
    return open;
}

void video_dump_bmp(const char* path) {
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
    std::vector<uint8_t> px((size_t)ww * wh * 4);
    glReadPixels(0, 0, ww, wh, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)px.size(), file = 54 + data;
    uint8_t h[54] = {'B', 'M'};
    auto put32 = [&](int o, uint32_t v) { for (int k = 0; k < 4; k++) h[o + k] = (uint8_t)(v >> (8 * k)); };
    put32(2, file); put32(10, 54); put32(14, 40); put32(18, (uint32_t)ww); put32(22, (uint32_t)wh);
    h[26] = 1; h[28] = 32; put32(34, data);
    fwrite(h, 1, 54, f);
    fwrite(px.data(), 1, px.size(), f);  // bottom-up rows, as BMP expects
    fclose(f);
}
