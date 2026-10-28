// Native AX mixer: executes the command lists the SDK's AX library sends to
// the DSP each 5 ms frame. Voices (parameter blocks, AXPB) are decoded from
// ARAM (ADPCM / PCM16 / PCM8), resampled, enveloped and mixed into L/R/S and
// aux buses; the frame's stereo output is written where the CPU asked.
#include "hw/ax.h"

#include <algorithm>
#include <cstring>

#include "runtime/runtime.h"

namespace {

constexpr int kSamples = 160;  // 5 ms at 32 kHz
constexpr int kPerMs = 32;

int32_t g_main[3][kSamples];  // L, R, S
int32_t g_auxa[3][kSamples];
int32_t g_auxb[3][kSamples];
uint8_t* g_aram = nullptr;
uint32_t g_aram_mask = 0;

int16_t g_last_out[kSamples * 2];
bool g_have_output = false;

uint16_t rd16be(uint32_t a) { return rd16(0x80000000u | (a & 0x01FFFFFF)); }
void wr16be(uint32_t a, uint16_t v) { wr16(0x80000000u | (a & 0x01FFFFFF), v); }
uint32_t rd32hl(uint32_t a) { return (uint32_t)rd16be(a) << 16 | rd16be(a + 2); }
void wr32hl(uint32_t a, uint32_t v) { wr16be(a, (uint16_t)(v >> 16)); wr16be(a + 2, (uint16_t)v); }

uint8_t aram8(uint32_t a) { return g_aram[a & g_aram_mask]; }

// AXPB field offsets (bytes)
enum : uint32_t {
    PB_NEXT = 0x00, PB_STATE = 0x0E, PB_MIX = 0x12, PB_UPDATE = 0x44, PB_VE = 0x64, PB_ADDR = 0x6E, PB_ADPCM = 0x7E,
    PB_SRC = 0xA6, PB_ADPCMLOOP = 0xB4, PB_MIXERCTRL = 0x0C, PB_SIZE = 0xC0,
};

struct Voice {
    uint16_t w[PB_SIZE / 2];
    uint16_t& at(uint32_t off) { return w[off / 2]; }
    uint32_t addr32(uint32_t off) { return (uint32_t)at(off) << 16 | at(off + 2); }
    void set32(uint32_t off, uint32_t v) { at(off) = (uint16_t)(v >> 16); at(off + 2) = (uint16_t)v; }
};

int16_t clamp16(int32_t v) { return (int16_t)std::min(32767, std::max(-32768, v)); }

// Fetch the next source sample at the voice's current address and advance it.
int16_t next_sample(Voice& v, bool& stopped) {
    uint32_t fmt = v.at(PB_ADDR + 2);
    uint32_t cur = v.addr32(PB_ADDR + 12), end = v.addr32(PB_ADDR + 8), loop = v.addr32(PB_ADDR + 4);
    bool looping = v.at(PB_ADDR) != 0;
    int16_t out = 0;
    if (fmt == 0) {  // ADPCM, addresses in nibbles; each 16-nibble frame starts with a header byte
        if ((cur & 15) == 0) {
            v.at(PB_ADPCM + 0x22) = aram8(cur >> 1);  // pred_scale
            cur += 2;
        }
        uint32_t ps = v.at(PB_ADPCM + 0x22);
        int coef = (ps >> 4) & 7, scale = ps & 15;
        int16_t c1 = (int16_t)v.at(PB_ADPCM + coef * 4), c2 = (int16_t)v.at(PB_ADPCM + coef * 4 + 2);
        int16_t yn1 = (int16_t)v.at(PB_ADPCM + 0x24), yn2 = (int16_t)v.at(PB_ADPCM + 0x26);
        uint8_t b = aram8(cur >> 1);
        int nib = (cur & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        int32_t val = ((nib << scale) << 11) + c1 * yn1 + c2 * yn2;
        out = clamp16((val + 1024) >> 11);
        v.at(PB_ADPCM + 0x26) = (uint16_t)yn1;
        v.at(PB_ADPCM + 0x24) = (uint16_t)out;
    } else if (fmt == 0x0A) {  // PCM16, addresses in samples
        out = (int16_t)(aram8(cur * 2) << 8 | aram8(cur * 2 + 1));
    } else {  // PCM8
        out = (int16_t)((int8_t)aram8(cur) << 8);
    }
    if (cur >= end) {
        if (looping) {
            cur = loop;
            if (fmt == 0) {  // streams reload predictor state from the loop context
                v.at(PB_ADPCM + 0x22) = v.at(PB_ADPCMLOOP);
                v.at(PB_ADPCM + 0x24) = v.at(PB_ADPCMLOOP + 2);
                v.at(PB_ADPCM + 0x26) = v.at(PB_ADPCMLOOP + 4);
            }
        } else {
            stopped = true;
        }
    } else {
        cur++;
    }
    v.set32(PB_ADDR + 12, cur);
    return out;
}

void apply_updates(Voice& v, int ms) {
    uint32_t list = v.addr32(PB_UPDATE + 10);
    uint32_t skip = 0;
    for (int k = 0; k < ms; k++) skip += v.at(PB_UPDATE + 2 * k);
    uint32_t n = v.at(PB_UPDATE + 2 * ms);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t e = list + 4 * (skip + k);
        uint16_t off = rd16be(e), val = rd16be(e + 2);
        if (off < PB_SIZE / 2) v.w[off] = val;
    }
}

void process_voice(Voice& v) {
    bool stopped = false;
    // 16.16 resampling ratio; last 4 source samples kept in the PB for interpolation.
    uint32_t ratio = v.addr32(PB_SRC);
    uint32_t frac = v.at(PB_SRC + 4);
    int16_t* last = (int16_t*)&v.at(PB_SRC + 6);
    for (int ms = 0; ms < 5 && !stopped; ms++) {
        apply_updates(v, ms);
        if (v.at(PB_STATE) != 1) return;
        int32_t vol = v.at(PB_VE);
        int16_t vdelta = (int16_t)v.at(PB_VE + 2);
        for (int i = 0; i < kPerMs; i++) {
            uint32_t pos = frac + ratio;
            uint32_t adv = pos >> 16;
            frac = pos & 0xFFFF;
            for (uint32_t k = 0; k < adv && !stopped; k++) {
                last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
                last[3] = next_sample(v, stopped);
            }
            int32_t s = last[2] + (((int32_t)(last[3] - last[2]) * (int32_t)frac) >> 16);
            s = (s * vol) >> 15;
            vol = std::max(0, std::min(0xFFFF, vol + vdelta));
            int idx = ms * kPerMs + i;
            // Mix volumes (0x8000 = unity) for L, R, S and the aux sends.
            uint16_t* mix = &v.at(PB_MIX);
            auto ramp = [&](int vi, int32_t* bus) {
                int32_t mv = mix[vi];
                bus[idx] += (s * mv) >> 15;
                mix[vi] = (uint16_t)std::max(0, std::min(0xFFFF, mv + (int16_t)mix[vi + 1]));
            };
            ramp(0, g_main[0]);   // vL
            ramp(2, g_main[1]);   // vR
            ramp(4, g_auxa[0]);   // vAuxAL
            ramp(6, g_auxa[1]);   // vAuxAR
            ramp(8, g_auxb[0]);   // vAuxBL
            ramp(10, g_auxb[1]);  // vAuxBR
            ramp(12, g_auxb[2]);  // vAuxBS
            ramp(14, g_main[2]);  // vS
            ramp(16, g_auxa[2]);  // vAuxAS
            if (stopped) break;
        }
        v.at(PB_VE) = (uint16_t)vol;
    }
    v.at(PB_SRC + 4) = (uint16_t)frac;
    if (stopped) v.at(PB_STATE) = 0;
}

void process_pbs(uint32_t first) {
    uint32_t pb = first;
    for (int guard = 0; pb && guard < 256; guard++) {
        Voice v;
        for (uint32_t k = 0; k < PB_SIZE / 2; k++) v.w[k] = rd16be(pb + 2 * k);
        if (v.at(PB_STATE) == 1) {
            process_voice(v);
            for (uint32_t k = 0; k < PB_SIZE / 2; k++) wr16be(pb + 2 * k, v.w[k]);
        }
        pb = v.addr32(PB_NEXT);
    }
}

void aux_exchange(uint32_t send, uint32_t ret, int32_t (*bus)[kSamples]) {
    // The CPU-side aux callback (reverb etc.) reads `send` and writes `ret` one frame later.
    for (int c = 0; c < 3 && send; c++)
        for (int i = 0; i < kSamples; i++) {
            uint32_t a = send + (uint32_t)(c * kSamples + i) * 4;
            wr32hl(a, (uint32_t)bus[c][i]);
        }
    for (int c = 0; c < 3 && ret; c++)
        for (int i = 0; i < kSamples; i++) g_main[c][i] += (int32_t)rd32hl(ret + (uint32_t)(c * kSamples + i) * 4);
}

}  // namespace

void ax_init(uint8_t* aram, uint32_t size) {
    g_aram = aram;
    g_aram_mask = size - 1;
}

void ax_process_command_list(uint32_t addr) {
    memset(g_main, 0, sizeof g_main);
    memset(g_auxa, 0, sizeof g_auxa);
    memset(g_auxb, 0, sizeof g_auxb);
    uint32_t pc = addr & 0x01FFFFFF, pbs = 0;
    for (int guard = 0; guard < 64; guard++) {
        uint16_t cmd = rd16be(pc);
        pc += 2;
        auto arg32 = [&]() { uint32_t v = rd32hl(pc); pc += 4; return v; };
        switch (cmd) {
        case 0x00: arg32(); break;                          // studio parameters (depop state)
        case 0x07: case 0x11: arg32(); break;              // surround buffer
        case 0x02: pbs = arg32(); break;                    // first PB
        case 0x03: process_pbs(pbs); break;                 // mix all voices
        case 0x04: { uint32_t s = arg32(), r = arg32(); aux_exchange(s, r, g_auxa); break; }
        case 0x05: { uint32_t s = arg32(), r = arg32(); aux_exchange(s, r, g_auxb); break; }
        case 0x10: arg32(); arg32(); break;
        case 0x13: for (int k = 0; k < 6; k++) arg32(); break;
        case 0x12: pc += 4; arg32(); break;                 // compressor table
        case 0x0E: {                                         // output
            uint32_t sbuf = arg32(), out = arg32();
            (void)sbuf;
            for (int i = 0; i < kSamples; i++) {
                int16_t l = clamp16(g_main[0][i]), r = clamp16(g_main[1][i]);
                g_last_out[2 * i] = l;
                g_last_out[2 * i + 1] = r;
                wr16be(out + 4 * i, (uint16_t)r);  // AI DMA order: right, left
                wr16be(out + 4 * i + 2, (uint16_t)l);
            }
            g_have_output = true;
            break;
        }
        case 0x0F: return;  // end
        default:
            static int unk = 0;
            if (unk++ < 8) rt_log("AX: unknown command 0x%04X", cmd);
            return;
        }
    }
}

bool ax_take_output(int16_t* stereo160) {
    if (!g_have_output) return false;
    memcpy(stereo160, g_last_out, sizeof g_last_out);
    g_have_output = false;
    return true;
}
