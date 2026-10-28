// Host window, GL context and the emulated embedded framebuffer (EFB).
#ifndef TSFP_VIDEO_H
#define TSFP_VIDEO_H

#include <cstdint>

#include "video/gl.h"

constexpr int kEfbWidth = 640, kEfbHeight = 528;

bool video_init(bool headless);
int video_scale();                 // EFB pixels -> host pixels
void video_bind_efb();             // render target for GX draws
GLuint video_efb_color();
void video_present(int src_x, int src_y, int src_w, int src_h);  // EFB -> window (display copy)
bool video_pump_events();          // false when the window was closed
void video_dump_bmp(const char* path);
uint32_t video_frames_presented();

#endif  // TSFP_VIDEO_H
