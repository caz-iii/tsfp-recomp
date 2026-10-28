// GameCube texture formats -> RGBA8. All formats are stored as tiles in
// big-endian order; TLUT formats read their palette from TMEM.
#include "gx/gx_texture.h"

#include <cstring>

namespace {

inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

uint32_t from565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return rgba(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
}

uint32_t from5a3(uint16_t c) {
    if (c & 0x8000) {
        uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        return rgba(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255);
    }
    uint32_t a = (c >> 12) & 7, r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return rgba(r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1);
}

uint32_t from_ia8(uint16_t c) {
    uint32_t i = c & 0xFF, a = c >> 8;
    return rgba(i, i, i, a);
}

uint32_t palette(const uint8_t* tlut, uint32_t tlut_fmt, uint32_t index) {
    uint16_t c = be16(tlut + 2 * index);
    switch (tlut_fmt) {
    case 0: return from_ia8(c);
    case 1: return from565(c);
    default: return from5a3(c);
    }
}

}  // namespace

uint32_t gx_texture_size(uint32_t fmt, uint32_t w, uint32_t h) {
    uint32_t bw, bh, bpp;
    switch (fmt) {
    case 0: case 8: case 14: bw = 8; bh = 8; bpp = 4; break;
    case 1: case 2: case 9: bw = 8; bh = 4; bpp = 8; break;
    case 6: bw = 4; bh = 4; bpp = 32; break;
    default: bw = 4; bh = 4; bpp = 16; break;
    }
    uint32_t tw = (w + bw - 1) / bw, th = (h + bh - 1) / bh;
    return tw * th * bw * bh * bpp / 8;
}

void gx_decode_texture(uint32_t* out, const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h,
                       const uint8_t* tlut, uint32_t tlut_fmt) {
    auto put = [&](uint32_t x, uint32_t y, uint32_t c) {
        if (x < w && y < h) out[y * w + x] = c;
    };
    switch (fmt) {
    case 0:  // I4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x += 2, src++) {
                        uint32_t a = (*src >> 4) * 17, b = (*src & 15) * 17;
                        put(tx + x, ty + y, rgba(a, a, a, a));
                        put(tx + x + 1, ty + y, rgba(b, b, b, b));
                    }
        break;
    case 1:  // I8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++, src++) put(tx + x, ty + y, rgba(*src, *src, *src, *src));
        break;
    case 2:  // IA4
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++, src++) {
                        uint32_t i = (*src & 15) * 17, a = (*src >> 4) * 17;
                        put(tx + x, ty + y, rgba(i, i, i, a));
                    }
        break;
    case 3: case 4: case 5:  // IA8, RGB565, RGB5A3
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, src += 2) {
                        uint16_t c = be16(src);
                        put(tx + x, ty + y, fmt == 3 ? from_ia8(c) : fmt == 4 ? from565(c) : from5a3(c));
                    }
        break;
    case 6:  // RGBA8: AR plane then GB plane per 4x4 tile
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4, src += 64)
                for (uint32_t k = 0; k < 16; k++) {
                    const uint8_t* ar = src + 2 * k;
                    const uint8_t* gb = src + 32 + 2 * k;
                    put(tx + (k & 3), ty + (k >> 2), rgba(ar[1], gb[0], gb[1], ar[0]));
                }
        break;
    case 8:  // C4
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x += 2, src++) {
                        put(tx + x, ty + y, palette(tlut, tlut_fmt, *src >> 4));
                        put(tx + x + 1, ty + y, palette(tlut, tlut_fmt, *src & 15));
                    }
        break;
    case 9:  // C8
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++, src++) put(tx + x, ty + y, palette(tlut, tlut_fmt, *src));
        break;
    case 10:  // C14X2
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++, src += 2) put(tx + x, ty + y, palette(tlut, tlut_fmt, be16(src) & 0x3FFF));
        break;
    case 14:  // CMPR: 8x8 tiles of four DXT1-style 4x4 blocks
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t sb = 0; sb < 4; sb++, src += 8) {
                    uint16_t c0 = be16(src), c1 = be16(src + 2);
                    uint32_t pal[4];
                    pal[0] = from565(c0);
                    pal[1] = from565(c1);
                    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
                    if (c0 > c1) {
                        for (int k = 0; k < 2; k++) {
                            uint32_t wa = k == 0 ? 2 : 1, wb = 3 - wa;
                            pal[2 + k] = rgba((ch(pal[0], 0) * wa + ch(pal[1], 0) * wb) / 3, (ch(pal[0], 8) * wa + ch(pal[1], 8) * wb) / 3,
                                              (ch(pal[0], 16) * wa + ch(pal[1], 16) * wb) / 3, 255);
                        }
                    } else {
                        pal[2] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
                        pal[3] = rgba(ch(pal[2], 0), ch(pal[2], 8), ch(pal[2], 16), 0);
                    }
                    uint32_t bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
                    for (uint32_t y = 0; y < 4; y++) {
                        uint8_t row = src[4 + y];
                        for (uint32_t x = 0; x < 4; x++) put(bx + x, by + y, pal[(row >> (6 - 2 * x)) & 3]);
                    }
                }
        break;
    default:
        for (uint32_t k = 0; k < w * h; k++) out[k] = rgba(255, 0, 255, 255);
        break;
    }
}
