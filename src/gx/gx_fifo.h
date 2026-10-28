// GX command processor: consumes the command stream the game writes to the
// write-gather pipe (0xCC008000) and to display lists, tracking CP/XF/BP state.
#ifndef TSFP_GX_FIFO_H
#define TSFP_GX_FIFO_H

#include <cstdint>

struct GxStats {
    uint32_t commands = 0, primitives = 0, vertices = 0, display_lists = 0;
    uint32_t draw_done = 0, tokens = 0, copies = 0;
};

void gx_init();
void gx_pipe_write(uint32_t value, int size);  // CPU write to the FIFO pipe
const GxStats& gx_stats();
void gx_reset_frame_stats();

// Hooks the renderer implements (no-ops until it exists).
struct GxDraw {
    uint8_t primitive;  // GX_QUADS 0x80, GX_TRIANGLES 0x90, ...
    uint8_t vat;
    uint16_t count;
    const uint8_t* data;  // big-endian vertex data, `count` vertices of `stride` bytes
    uint32_t stride;
};

// Device-side callbacks into the hardware layer (PE interrupts, token value).
void hw_pe_finish();
void hw_pe_token(uint16_t token, bool interrupt);

#endif  // TSFP_GX_FIFO_H
