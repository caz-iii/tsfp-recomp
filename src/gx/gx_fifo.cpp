#include "gx/gx_fifo.h"

#include <cstring>
#include <vector>

#include "gx/gx_state.h"
#include "runtime/runtime.h"

uint32_t g_gx_cp[256];
uint32_t g_gx_xf[0x1100];
uint32_t g_gx_bp[256];
uint8_t g_gx_tmem[0x100000];
GxTevRegs g_gx_tev;

namespace {

uint32_t* const g_cp = g_gx_cp;
uint32_t* const g_xf = g_gx_xf;
uint32_t* const g_bp = g_gx_bp;
GxStats g_stats;
std::vector<uint8_t> g_buf;  // pending pipe bytes
size_t g_head = 0;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---------------------------------------------------------------- vertex size
uint32_t comp_size(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
uint32_t color_size(uint32_t fmt) {
    static const uint32_t s[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return s[fmt & 7];
}

uint32_t vertex_size(uint32_t vat) {
    uint32_t vcd_lo = g_cp[0x50], vcd_hi = g_cp[0x60];
    uint32_t a = g_cp[0x70 + vat], b = g_cp[0x80 + vat], c = g_cp[0x90 + vat];
    auto ind = [](uint32_t t) { return t == 2 ? 1u : t == 3 ? 2u : 0u; };
    uint32_t size = 0;
    for (int k = 0; k < 9; k++) size += (vcd_lo >> k) & 1;  // PNMTXIDX, TEX0..7MTXIDX
    // Position
    uint32_t t = (vcd_lo >> 9) & 3;
    if (t == 1) size += comp_size((a >> 1) & 7) * ((a & 1) ? 3 : 2);
    else size += ind(t);
    // Normal (NBT = 3 vectors; NORMALINDEX3 = one index per vector)
    t = (vcd_lo >> 11) & 3;
    bool nbt = (a >> 9) & 1;
    if (t == 1) size += comp_size((a >> 10) & 7) * (nbt ? 9 : 3);
    else if (t) size += ind(t) * ((nbt && (a >> 31)) ? 3 : 1);
    // Colors
    for (int k = 0; k < 2; k++) {
        t = (vcd_lo >> (13 + 2 * k)) & 3;
        uint32_t fmt = k == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        size += t == 1 ? color_size(fmt) : ind(t);
    }
    // Texture coordinates
    const uint32_t cnt[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    const uint32_t fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    for (int k = 0; k < 8; k++) {
        t = (vcd_hi >> (2 * k)) & 3;
        size += t == 1 ? comp_size(fmt[k]) * (cnt[k] ? 2 : 1) : ind(t);
    }
    return size;
}

// ---------------------------------------------------------------- registers
void load_bp(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0x00FFFFFF;
    // BP writes may be masked by a preceding write to the mask register (0xFE).
    uint32_t mask = g_bp[0xFE] ? g_bp[0xFE] : 0x00FFFFFF;
    if (reg != 0xFE) {
        g_bp[reg] = (g_bp[reg] & ~mask) | (val & mask);
        g_bp[0xFE] = 0;
    } else {
        g_bp[0xFE] = val;
    }
    if (reg >= 0xE0 && reg <= 0xE7) {  // TEV color / konst registers
        uint32_t v = g_bp[reg], i = (reg - 0xE0) / 2;
        bool konst = (v >> 23) & 1, ra = !((reg - 0xE0) & 1);
        auto s11 = [](uint32_t x) { return (int16_t)((int32_t)(x << 21) >> 21); };
        if (konst) {
            if (ra) { g_gx_tev.konst[i][0] = (uint8_t)(v & 0xFF); g_gx_tev.konst[i][3] = (uint8_t)((v >> 12) & 0xFF); }
            else { g_gx_tev.konst[i][2] = (uint8_t)(v & 0xFF); g_gx_tev.konst[i][1] = (uint8_t)((v >> 12) & 0xFF); }
        } else {
            if (ra) { g_gx_tev.color[i][0] = s11(v & 0x7FF); g_gx_tev.color[i][3] = s11((v >> 12) & 0x7FF); }
            else { g_gx_tev.color[i][2] = s11(v & 0x7FF); g_gx_tev.color[i][1] = s11((v >> 12) & 0x7FF); }
        }
    }
    gx_render_bp(reg, g_bp[reg]);
    switch (reg) {
    case 0x45:  // PE_DONE: draw done
        g_stats.draw_done++;
        hw_pe_finish();
        break;
    case 0x47:  // PE_TOKEN_INT
        g_stats.tokens++;
        hw_pe_token((uint16_t)val, true);
        break;
    case 0x48:  // PE_TOKEN
        hw_pe_token((uint16_t)val, false);
        break;
    case 0x52:  // PE copy execute (EFB -> texture or XFB)
        g_stats.copies++;
        break;
    default:
        break;
    }
}

// Parse one command at p (len bytes available). Returns bytes consumed, or 0
// if the command is incomplete.
size_t parse(const uint8_t* p, size_t len, int depth);

void run_display_list(uint32_t addr, uint32_t size, int depth) {
    uint8_t* dl = ppc_host(addr);
    if (!dl || depth > 4) return;
    g_stats.display_lists++;
    size_t off = 0;
    while (off < size) {
        size_t n = parse(dl + off, size - off, depth + 1);
        if (!n) break;
        off += n;
    }
}

size_t parse(const uint8_t* p, size_t len, int depth) {
    uint8_t op = p[0];
    g_stats.commands++;
    switch (op) {
    case 0x00:  // NOP
    case 0x48:  // invalidate vertex cache
        return 1;
    case 0x08:  // LOAD_CP_REG
        if (len < 6) return 0;
        g_cp[p[1]] = be32(p + 2);
        return 6;
    case 0x10: {  // LOAD_XF_REG
        if (len < 5) return 0;
        uint32_t count = be16(p + 1) + 1, addr = be16(p + 3);
        if (len < 5 + 4 * count) return 0;
        for (uint32_t k = 0; k < count; k++)
            if (addr + k < 0x1100) g_xf[addr + k] = be32(p + 5 + 4 * k);
        return 5 + 4 * count;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {  // LOAD_INDX_A..D: XF load from arrays 12..15
        if (len < 5) return 0;
        uint32_t v = be32(p + 1), arr = 12 + ((op >> 3) & 3);
        uint32_t index = v >> 16, n = ((v >> 12) & 0xF) + 1, xfaddr = v & 0xFFF;
        uint32_t src = g_cp[0xA0 + arr] + index * g_cp[0xB0 + arr];
        for (uint32_t k = 0; k < n && xfaddr + k < 0x1100; k++) g_xf[xfaddr + k] = rd32(0x80000000u | (src + 4 * k));
        return 5;
    }
    case 0x40:  // CALL_DL
        if (len < 9) return 0;
        run_display_list(be32(p + 1), be32(p + 5), depth);
        return 9;
    case 0x44:  // performance metrics
        return 1;
    case 0x61:  // LOAD_BP_REG
        if (len < 5) return 0;
        load_bp(be32(p + 1));
        return 5;
    default:
        if (op >= 0x80 && op < 0xC0) {  // draw primitive
            if (len < 3) return 0;
            uint32_t vat = op & 7, count = be16(p + 1);
            uint32_t stride = vertex_size(vat);
            size_t need = 3 + (size_t)count * stride;
            if (len < need) return 0;
            g_stats.primitives++;
            g_stats.vertices += count;
            gx_render_draw(op & 0xF8, vat, count, p + 3, stride);
            return need;
        }
        static int unknown = 0;
        if (unknown++ < 16) rt_log("GX: unknown FIFO command 0x%02X", op);
        return 1;
    }
}

}  // namespace

void gx_init() {
    memset(g_gx_cp, 0, sizeof g_gx_cp);
    memset(g_gx_xf, 0, sizeof g_gx_xf);
    memset(g_gx_bp, 0, sizeof g_gx_bp);
    gx_render_init();
    g_buf.clear();
    g_head = 0;
}

void gx_pipe_write(uint32_t value, int size) {
    for (int k = size - 1; k >= 0; k--) g_buf.push_back((uint8_t)(value >> (8 * k)));
    while (g_head < g_buf.size()) {
        size_t n = parse(&g_buf[g_head], g_buf.size() - g_head, 0);
        if (!n) break;
        g_head += n;
    }
    if (g_head > 65536 || g_head == g_buf.size()) {
        g_buf.erase(g_buf.begin(), g_buf.begin() + (ptrdiff_t)g_head);
        g_head = 0;
    }
}

const GxStats& gx_stats() { return g_stats; }
void gx_reset_frame_stats() { g_stats = GxStats(); }
