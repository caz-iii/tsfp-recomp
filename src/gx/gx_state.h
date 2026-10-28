// GX register state shared by the command processor and the renderer.
#ifndef TSFP_GX_STATE_H
#define TSFP_GX_STATE_H

#include <cstdint>

extern uint32_t g_gx_cp[256];     // CP: VCD 0x50/0x60, VAT 0x70/0x80/0x90, array base 0xA0, stride 0xB0
extern uint32_t g_gx_xf[0x1100];  // XF memory (matrices, lights) and registers at 0x1000
extern uint32_t g_gx_bp[256];     // BP: TEV, textures, blending, copies
extern uint8_t g_gx_tmem[0x100000];  // texture memory (TLUTs are loaded here)

// TEV color registers and konst colors (BP 0xE0..0xE7 split by the konst bit).
struct GxTevRegs {
    int16_t color[4][4];  // PREV, C0, C1, C2: signed 11-bit RGBA
    uint8_t konst[4][4];  // K0..K3 RGBA
};
extern GxTevRegs g_gx_tev;

inline float gx_xf_float(uint32_t addr) {
    union { uint32_t u; float f; } v;
    v.u = g_gx_xf[addr];
    return v.f;
}

void gx_render_init();
void gx_render_draw(uint8_t primitive, uint32_t vat, uint32_t count, const uint8_t* data, uint32_t stride);
void gx_render_bp(uint32_t reg, uint32_t value);  // side effects of BP writes (copies, TLUT loads)

#endif  // TSFP_GX_STATE_H
