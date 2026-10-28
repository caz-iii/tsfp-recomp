#ifndef TSFP_GX_TEXTURE_H
#define TSFP_GX_TEXTURE_H

#include <cstdint>

// Bytes occupied by a w x h texture of GX format `fmt` (tile padded).
uint32_t gx_texture_size(uint32_t fmt, uint32_t w, uint32_t h);

// Decode to RGBA8 (R in the low byte). `tlut` / `tlut_fmt` are used by C4/C8/C14X2.
void gx_decode_texture(uint32_t* out, const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                       const uint8_t* tlut, uint32_t tlut_fmt);

#endif  // TSFP_GX_TEXTURE_H
