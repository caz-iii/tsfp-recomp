// EXI bus: three channels of CSR/MAR/LENGTH/CR/DATA at 0x6800 + 0x14*ch.
// Device 1 on channel 0 is the IPL chip (RTC, SRAM, ROM). Memory cards and
// debug adapters are reported absent until their devices are implemented.
#include "hw/exi.h"

#include <cstring>
#include <ctime>
#include <vector>

#include "runtime/runtime.h"

namespace {

struct Channel {
    uint32_t csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    int selected = -1;  // device index from CS bits
    // Per-selection IPL state
    bool have_cmd = false;
    uint32_t cmd = 0, cursor = 0;
    uint8_t cmd_bytes[4];
    int cmd_len = 0;
};
Channel g_ch[3];

uint8_t g_sram[64];

void sram_init() {
    memset(g_sram, 0, sizeof g_sram);
    // OSSram: checkSum, checkSumInv, ead0, ead1, counterBias, displayOffsetH, ntd, language, flags
    g_sram[0x12] = 0;     // language: English
    g_sram[0x13] = 0x04;  // flags: stereo sound
    uint16_t sum = 0, inv = 0;
    for (int off = 0x0C; off < 0x14; off += 2) {
        uint16_t w = (uint16_t)(g_sram[off] << 8 | g_sram[off + 1]);
        sum += w;
        inv += (uint16_t)~w;
    }
    g_sram[0] = sum >> 8; g_sram[1] = (uint8_t)sum;
    g_sram[2] = inv >> 8; g_sram[3] = (uint8_t)inv;
}

uint32_t rtc_now() {
    // Seconds since 2000-01-01 (the IPL RTC epoch).
    return (uint32_t)(time(nullptr) - 946684800);
}

// IPL addresses are command >> 6: 0x800000 RTC, 0x800004 SRAM, 0x810000 UART, < 0x200000 ROM.
uint8_t ipl_read_byte(Channel& c) {
    uint32_t addr = (c.cmd >> 6) & 0xFFFFFF;
    uint32_t pos = c.cursor++;
    if (addr >= 0x800000 && addr < 0x800004) {
        uint32_t rtc = rtc_now();
        return (uint8_t)(rtc >> (24 - 8 * (pos & 3)));
    }
    if (addr >= 0x800004 && addr < 0x800044) return g_sram[(pos + (addr - 0x800004) * 4) & 63];
    return 0;  // IPL ROM (fonts) is not available: it ships with the console, not the game
}

void ipl_write_byte(Channel& c, uint8_t b) {
    if (!c.have_cmd) {
        c.cmd_bytes[c.cmd_len++] = b;
        if (c.cmd_len == 4) {
            c.cmd = (uint32_t)c.cmd_bytes[0] << 24 | c.cmd_bytes[1] << 16 | c.cmd_bytes[2] << 8 | c.cmd_bytes[3];
            c.have_cmd = true;
            c.cursor = 0;
        }
        return;
    }
    uint32_t addr = (c.cmd >> 6) & 0xFFFFFF;
    uint32_t pos = c.cursor++;
    if (addr >= 0x800004 && addr < 0x800044) g_sram[(pos + (addr - 0x800004) * 4) & 63] = b;
}

bool device_present(int ch, int dev) { return ch == 0 && dev == 1; }

void transfer(int ch) {
    Channel& c = g_ch[ch];
    bool dma = c.cr & 2;
    uint32_t rw = (c.cr >> 2) & 3;  // 0 read, 1 write, 2 read/write
    bool ipl = c.selected == 1 && ch == 0;
    if (dma) {
        uint32_t mar = c.mar & 0x03FFFFE0, n = c.len & 0x03FFFFE0;
        for (uint32_t k = 0; k < n; k++) {
            if (rw == 0) g_mem1[mar + k] = ipl ? ipl_read_byte(c) : 0;
            else if (ipl) ipl_write_byte(c, g_mem1[mar + k]);
        }
    } else {
        uint32_t n = ((c.cr >> 4) & 3) + 1;
        uint32_t in = 0;
        for (uint32_t k = 0; k < n; k++) {
            uint8_t out = (uint8_t)(c.data >> (24 - 8 * k));
            uint8_t got = 0;
            if (rw != 0 && ipl) ipl_write_byte(c, out);
            if (rw != 1) got = ipl ? ipl_read_byte(c) : (device_present(ch, c.selected) ? 0 : 0xFF);
            in |= (uint32_t)got << (24 - 8 * k);
        }
        if (rw != 1) c.data = in;
    }
    c.cr &= ~1u;
    c.csr |= 1u << 3;  // TCINT
}

}  // namespace

void exi_init() {
    sram_init();
    for (auto& c : g_ch) c = Channel();
}

uint32_t exi_read(uint32_t o) {
    int ch = (int)((o - 0x6800) / 0x14);
    uint32_t r = (o - 0x6800) % 0x14;
    if (ch > 2) return 0;
    Channel& c = g_ch[ch];
    switch (r) {
    case 0x00: {
        uint32_t v = c.csr;
        if (ch == 0 || ch == 1) v &= ~(1u << 12);  // EXT: no memory card inserted
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    default: return c.data;
    }
}

void exi_write(uint32_t o, uint32_t v) {
    int ch = (int)((o - 0x6800) / 0x14);
    uint32_t r = (o - 0x6800) % 0x14;
    if (ch > 2) return;
    Channel& c = g_ch[ch];
    switch (r) {
    case 0x00: {
        // Interrupt status bits (1 EXIINT, 3 TCINT, 11 EXTINT) are write-1-to-clear.
        uint32_t ints = (1u << 1) | (1u << 3) | (1u << 11);
        uint32_t keep = c.csr & ints & ~(v & ints);
        c.csr = (v & ~ints) | keep;
        int sel = -1;
        for (int d = 0; d < 3; d++)
            if (v & (1u << (7 + d))) sel = d;
        if (sel != c.selected) {
            c.selected = sel;
            c.have_cmd = false;
            c.cmd_len = 0;
            c.cursor = 0;
        }
        break;
    }
    case 0x04: c.mar = v; break;
    case 0x08: c.len = v; break;
    case 0x0C:
        c.cr = v;
        if (v & 1) transfer(ch);
        break;
    default: c.data = v; break;
    }
}

bool exi_interrupt_line() {
    for (auto& c : g_ch) {
        if ((c.csr & (1u << 3)) && (c.csr & (1u << 2))) return true;  // TCINT & TCINTMASK
        if ((c.csr & (1u << 1)) && (c.csr & (1u << 0))) return true;  // EXIINT & mask
    }
    return false;
}
