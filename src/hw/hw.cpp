// Native implementations of the GameCube hardware blocks the SDK talks to.
// Registers not modelled yet read back what was written; first use is logged.
#include "hw/hw.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "hw/disc.h"
#include "hw/ax.h"
#include "hw/exi.h"
#include "hw/si.h"
#include "gx/gx_fifo.h"
#include "video/video.h"
#include "runtime/runtime.h"

namespace {

uint8_t g_regs[0x10000];
std::unordered_set<uint32_t> g_seen;
std::vector<uint8_t> g_aram(16 * 1024 * 1024);
Disc g_disc;

uint32_t reg32(uint32_t o) { return (uint32_t)g_regs[o] << 24 | g_regs[o + 1] << 16 | g_regs[o + 2] << 8 | g_regs[o + 3]; }
void set32(uint32_t o, uint32_t v) { for (int k = 3; k >= 0; k--, v >>= 8) g_regs[o + k] = (uint8_t)v; }
uint16_t reg16(uint32_t o) { return (uint16_t)(g_regs[o] << 8 | g_regs[o + 1]); }
void set16(uint32_t o, uint16_t v) { g_regs[o] = (uint8_t)(v >> 8); g_regs[o + 1] = (uint8_t)v; }

const char* block_name(uint32_t o) {
    switch (o >> 12) {
    case 0x0: return "CP";
    case 0x1: return "PE";
    case 0x2: return "VI";
    case 0x3: return "PI";
    case 0x4: return "MI";
    case 0x5: return "DSP";
    case 0x6: return (o & 0xFFF) < 0x400 ? "DI" : (o & 0xFFF) < 0x800 ? "SI" : (o & 0xFFF) < 0xC00 ? "EXI" : "AI";
    case 0x8: return "GXFIFO";
    default: return "?";
    }
}

void first_use(uint32_t o, bool write, uint32_t v, int size) {
    if (g_seen.insert((o & ~3u) | (write ? 1u : 0u)).second)
        rt_log("hw %-6s %s 0x0C00%04X size %d value 0x%08X", block_name(o), write ? "write" : "read ", o, size, v);
}

// ---------------------------------------------------------------- PI
// Interrupt cause bits (PI INTSR at 0x3000, mask at 0x3004).
enum : uint32_t {
    PI_DI = 1u << 2, PI_SI = 1u << 3, PI_EXI = 1u << 4, PI_AI = 1u << 5, PI_DSP = 1u << 6,
    PI_VI = 1u << 8, PI_PE_TOKEN = 1u << 9, PI_PE_FINISH = 1u << 10, PI_CP = 1u << 11,
};
uint32_t g_pi_lines = 0;

void pi_line(uint32_t bit, bool on) { g_pi_lines = on ? (g_pi_lines | bit) : (g_pi_lines & ~bit); }

// ---------------------------------------------------------------- VI
// Display interrupts DI0..DI3 at 0x2030..0x203C: bit31 status, bit28 enable.
constexpr uint64_t kTbHz = 40500000;
constexpr uint64_t kFrameTicks = (uint64_t)(kTbHz / 59.94);
uint64_t g_next_retrace = 0;
uint32_t g_frames = 0;
uint64_t g_aidma_next = 0;  // next AI DMA completion (timebase), 0 = idle
uint64_t g_di_done_at = 0;  // pending DI transfer-complete interrupt (timebase), 0 = none
uint64_t aidma_period();

void vi_update_line() {
    bool on = false;
    for (int k = 0; k < 4; k++) {
        uint32_t v = reg32(0x2030 + 4 * k);
        on |= (v & 0x80000000u) && (v & 0x10000000u);
    }
    pi_line(PI_VI, on);
}

void vi_tick(uint64_t now) {
    if (now < g_next_retrace) return;
    g_next_retrace = std::max(g_next_retrace + kFrameTicks, now);
    g_frames++;
    si_frame_counter = g_frames;
    si_poll_frame();
    pi_line(PI_SI, si_interrupt_line());
    if (!video_pump_events()) {
        rt_log("window closed");
        std::exit(0);
    }
    for (int k = 0; k < 2; k++) {
        uint32_t v = reg32(0x2030 + 4 * k);
        if (v & 0x10000000u) set32(0x2030 + 4 * k, v | 0x80000000u);
    }
    vi_update_line();
}

// ---------------------------------------------------------------- DI
// 0x6000 DISR, 0x6004 DICVR, 0x6008.. CMDBUF0-2, 0x6014 DIMAR, 0x6018 DILENGTH,
// 0x601C DICR, 0x6020 DIIMMBUF, 0x6024 DICFG.
void di_update_line() {
    uint32_t sr = reg32(0x6000), cvr = reg32(0x6004);
    bool on = ((sr & 0x04) && (sr & 0x02)) || ((sr & 0x10) && (sr & 0x08)) || ((sr & 0x40) && (sr & 0x20)) ||
              ((cvr & 0x04) && (cvr & 0x02));
    pi_line(PI_DI, on);
}

void di_command() {
    uint32_t cmd0 = reg32(0x6008), cmd1 = reg32(0x600C), cmd2 = reg32(0x6010);
    uint32_t mar = reg32(0x6014) & 0x03FFFFE0, len = reg32(0x6018);
    uint32_t op = cmd0 >> 24;
    static const bool dbg = getenv("TSFP_DEBUG_DVD") != nullptr;
    if (dbg) rt_log("DI cmd %08X %08X %08X -> 0x%08X len 0x%X", cmd0, cmd1, cmd2, mar, len);
    switch (op) {
    case 0xA8: {  // read (subcommand 0x40 = disc ID, also a plain read from offset 0)
        uint64_t off = (uint64_t)cmd1 << 2;
        if ((cmd0 & 0xFF) == 0x40) off = 0;
        std::vector<uint8_t> buf(len);
        if (!g_disc.read(off, buf.data(), len)) rt_log("DI: read past known files at 0x%llX len 0x%X", (unsigned long long)off, len);
        memcpy(g_mem1 + mar, buf.data(), len);
        break;
    }
    case 0x12: {  // inquiry: drive info
        uint8_t info[0x20] = {0, 0, 0, 0, 0x20, 0x02, 0x04, 0x02, 0x00, 0x00, 0x00, 0x61};
        memcpy(g_mem1 + mar, info, std::min<uint32_t>(len, sizeof info));
        break;
    }
    case 0xAB:  // seek
    case 0xE3:  // stop motor
    case 0xE4:  // audio buffer config
    case 0xE1:  // audio stream (streaming audio not modelled yet)
        break;
    case 0xE0:  // request error: none
    case 0xE2:  // audio status: idle
        set32(0x6020, 0);
        break;
    default:
        rt_log("DI: unhandled command %08X %08X %08X", cmd0, cmd1, cmd2);
        break;
    }
    // Data is in memory now; the drive reports completion after realistic
    // latency (~0.5 ms + 8 MB/s). Games rely on the interrupt arriving after
    // the DVD call returns (state is set up after DVDReadAsync).
    uint64_t now = ppc_timebase();
    g_di_done_at = now + kTbHz / 2000 + (uint64_t)len * kTbHz / (8u << 20);
}

void di_tick(uint64_t now) {
    if (!g_di_done_at || now < g_di_done_at) return;
    g_di_done_at = 0;
    if (reg32(0x601C) & 2) set32(0x6018, 0);
    set32(0x601C, reg32(0x601C) & ~1u);
    set32(0x6000, reg32(0x6000) | 0x10);  // TCINT
    di_update_line();
}

void di_write(uint32_t o, uint32_t v) {
    switch (o) {
    case 0x6000: {  // DISR: interrupt status bits are write-1-to-clear
        uint32_t cur = reg32(0x6000);
        uint32_t clear = v & 0x54;
        set32(0x6000, (cur & ~0x2Au & ~clear) | (v & 0x2A) | (v & 1));
        di_update_line();
        return;
    }
    case 0x6004: {
        uint32_t cur = reg32(0x6004);
        set32(0x6004, (cur & ~0x06u & ~(v & 4)) | (v & 2));
        di_update_line();
        return;
    }
    case 0x601C:
        set32(0x601C, v);
        if (v & 1) di_command();
        return;
    default:
        set32(o, v);
    }
}

// ---------------------------------------------------------------- DSP / ARAM / AI
// DSPCR (16-bit at 0x500A): bit5 ARINT, bit6 ARINTMSK, bit3 AIINT, bit4 AIINTMSK, bit7 DSPINT, bit8 DSPINTMSK.
void dsp_update_line() {
    uint16_t cr = reg16(0x500A);
    bool on = ((cr & 0x20) && (cr & 0x40)) || ((cr & 0x08) && (cr & 0x10)) || ((cr & 0x80) && (cr & 0x100));
    pi_line(PI_DSP, on);
}

void aram_dma() {
    uint32_t mm = reg32(0x5020) & 0x03FFFFE0, ar = reg32(0x5024) & 0x00FFFFE0, cnt = reg32(0x5028);
    uint32_t len = cnt & 0x7FFFFFE0;
    if (cnt & 0x80000000u) memcpy(g_mem1 + mm, &g_aram[ar], len);  // ARAM -> MRAM
    else memcpy(&g_aram[ar], g_mem1 + mm, len);
    set32(0x5028, cnt & 0x80000000u);
    set16(0x500A, reg16(0x500A) | 0x20);
    dsp_update_line();
}

// DSP mailboxes: 0x5000/0x5002 CPU->DSP, 0x5004/0x5006 DSP->CPU (bit 15 of the
// high half = mail valid; reading the low half consumes it).
//
// The DSP itself is replaced natively: the ROM boot loader, the task upload
// protocol of the SDK's dsp_task.c, and the AX microcode's frame protocol.
// Mixing of AX command lists hooks in at ax_process_command_list().
enum class DspState { Off, BootStub, Rom, Upload, Ax };
DspState g_dsp = DspState::Off;
std::vector<uint32_t> g_dsp_out;  // queued DSP->CPU mail
std::vector<uint32_t> g_dsp_in;   // mail received in the current exchange
bool g_ax_expect_cl = false;

void dsp_raise() {
    set16(0x500A, reg16(0x500A) | 0x80);  // DSPINT
    dsp_update_line();
}

void dsp_post_mail(uint32_t mail, bool irq) {
    if (reg16(0x5004) & 0x8000) {  // previous mail not read yet: queue it
        g_dsp_out.push_back(mail | (irq ? 0 : 0));
        return;
    }
    set16(0x5004, (uint16_t)(mail >> 16) | 0x8000);
    set16(0x5006, (uint16_t)mail);
    if (irq) dsp_raise();
}


void dsp_receive_mail(uint32_t mail) {
    switch (g_dsp) {
    case DspState::Rom:
    case DspState::Upload:
        g_dsp = DspState::Upload;
        g_dsp_in.push_back(mail);
        if (g_dsp_in.size() == 10) {  // ...0x80F3D001, init vector: task boots
            rt_log("DSP: task booted (IRAM 0x%08X len 0x%X)", g_dsp_in[1], g_dsp_in[5]);
            g_dsp_in.clear();
            g_dsp = DspState::Ax;
            dsp_post_mail(0xDCD10000, true);  // DSP_INIT
        }
        break;
    case DspState::Ax:
        if (g_ax_expect_cl) {
            g_ax_expect_cl = false;
            ax_process_command_list(mail);
            dsp_post_mail(0xDCD10001, true);  // DSP_RESUME: frame done
        } else if ((mail & 0xFFFF0000) == 0xBABE0000) {
            g_ax_expect_cl = true;
        } else if ((mail & 0xFFFF0000) == 0xCDD10000) {
            dsp_post_mail(0xDCD10001, true);  // continue/resume after a yield
        }
        break;
    default:
        break;
    }
}

void dsp_write16(uint32_t o, uint16_t v) {
    if (o == 0x500A) {
        uint16_t cur = reg16(0x500A);
        uint16_t ints = 0x08 | 0x20 | 0x80;
        uint16_t keep = cur & ints & ~(v & ints);  // write 1 to clear interrupt bits
        uint16_t nv = (uint16_t)((v & ~ints & ~0x0603) | keep);  // reset, PIINT and DMA-busy read back 0
        set16(0x500A, nv);
        if (v & 1) {  // reset
            g_dsp = DspState::Off;
            g_dsp_out.clear();
            g_dsp_in.clear();
        }
        if ((cur & 0x04) && !(nv & 0x04)) {  // HALT released
            static bool stub_done = false;  // __OSInitAudioSystem always boots the DSP first
            if (!stub_done) {
                stub_done = true;
                g_dsp = DspState::BootStub;
                dsp_post_mail(0x80544348, false);  // OS audio-system boot stub
            } else {
                g_dsp = DspState::Rom;
                dsp_post_mail(0x8071FEED, false);  // ROM: ready for a task upload
            }
        }
        if ((v & 0x02) && g_dsp == DspState::Ax) dsp_post_mail(0xDCD10002, true);  // DSPAssertTask: yield
        dsp_update_line();
        return;
    }
    set16(o, v);
    if (o == 0x502A) aram_dma();
    if (o == 0x5036 && (v & 0x8000) && !g_aidma_next) g_aidma_next = ppc_timebase() + aidma_period();
    if (o == 0x5036 && !(v & 0x8000)) g_aidma_next = 0;
    if (o == 0x5002) {  // CPU->DSP low half completes a mail; the DSP takes it at once
        uint32_t mail = (uint32_t)(reg16(0x5000) & 0x7FFF) << 16 | v;
        mail |= (uint32_t)(reg16(0x5000) & 0x8000) << 16;
        set16(0x5000, reg16(0x5000) & 0x7FFF);
        dsp_receive_mail(mail);
    }
}

uint16_t dsp_read16(uint32_t o) {
    uint16_t v = reg16(o);
    if (o == 0x5006) {
        set16(0x5004, reg16(0x5004) & 0x7FFF);
        if (!g_dsp_out.empty()) {
            uint32_t next = g_dsp_out.front();
            g_dsp_out.erase(g_dsp_out.begin());
            dsp_post_mail(next, true);
        }
    }
    return v;
}

// AI DMA (DSP block 0x5030..0x503A): plays `blocks` x 32 bytes of 32 kHz stereo
// from main memory and raises AIDINT when done; AX re-arms it every 5 ms frame.
uint64_t aidma_period() {
    uint32_t blocks = reg16(0x5036) & 0x7FFF;
    if (!blocks) blocks = 20;
    return (uint64_t)blocks * 32 / 4 * kTbHz / 32000;
}

void aidma_tick(uint64_t now) {
    if (!g_aidma_next || now < g_aidma_next) return;
    g_aidma_next = (reg16(0x5036) & 0x8000) ? std::max(g_aidma_next + aidma_period(), now) : 0;
    int16_t frame[320];
    if (ax_take_output(frame)) audio_push(frame, 160);
    set16(0x500A, reg16(0x500A) | 0x08);  // AIDINT
    dsp_update_line();
}

// ---------------------------------------------------------------- AI (streaming interface)
// 0x6C00 AICR: bit0 PSTAT, bit1 AFR (1 = 48 kHz), bit2 AIINTMSK, bit3 AIINT (w1c),
// bit4 AIINTVLD, bit5 SCRESET; 0x6C04 volume; 0x6C08 AISCNT; 0x6C0C AIIT.
uint64_t g_ai_base_tb = 0;  // timebase at which AISCNT was 0
uint32_t g_ai_base_cnt = 0;
bool g_ai_playing = false;

uint32_t ai_rate() { return (reg32(0x6C00) & 2) ? 48000 : 32000; }

uint32_t ai_count(uint64_t now) {
    if (!g_ai_playing) return g_ai_base_cnt;
    return g_ai_base_cnt + (uint32_t)((now - g_ai_base_tb) * ai_rate() / kTbHz);
}

void ai_update(uint64_t now) {
    uint32_t cr = reg32(0x6C00);
    if ((cr & 0x10) == 0 && g_ai_playing && ai_count(now) >= reg32(0x6C0C)) {
        // Interrupt when the counter reaches AIIT (AIINTVLD = 0 means enabled).
    }
    if (!(cr & 0x10) && g_ai_playing) {
        uint32_t c = ai_count(now), it = reg32(0x6C0C);
        static uint32_t last = 0;
        if (last < it && c >= it) set32(0x6C00, reg32(0x6C00) | 0x08);
        last = c;
    }
    cr = reg32(0x6C00);
    pi_line(PI_AI, (cr & 0x08) && (cr & 0x04));
}

void ai_write(uint32_t o, uint32_t v) {
    uint64_t now = ppc_timebase();
    if (o == 0x6C00) {
        uint32_t cur = reg32(0x6C00);
        g_ai_base_cnt = ai_count(now);
        g_ai_base_tb = now;
        if (v & 0x20) g_ai_base_cnt = 0;  // SCRESET
        g_ai_playing = v & 1;
        uint32_t nv = (v & ~0x28u) | (cur & 0x08 & ~(v & 0x08));
        set32(0x6C00, nv);
    } else if (o == 0x6C08) {
        g_ai_base_cnt = v;
        g_ai_base_tb = now;
    } else {
        set32(o, v);
    }
    ai_update(now);
}

// ---------------------------------------------------------------- PE
// 0x100A PE_INTRCTRL: bit0 token int enable, bit1 finish int enable,
// bit2 token status (w1c), bit3 finish status (w1c). 0x100E token value.
void pe_update_line() {
    uint16_t c = reg16(0x100A);
    pi_line(PI_PE_TOKEN, (c & 4) && (c & 1));
    pi_line(PI_PE_FINISH, (c & 8) && (c & 2));
}

void pe_write16(uint32_t o, uint16_t v) {
    if (o == 0x100A) {
        uint16_t cur = reg16(0x100A);
        set16(0x100A, (uint16_t)((v & 3) | (cur & 0x0C & ~(v & 0x0C))));
        pe_update_line();
        return;
    }
    set16(o, v);
}

// ---------------------------------------------------------------- dispatch
uint32_t read_reg(uint32_t o, int size) {
    if (o >= 0x6800 && o < 0x683C && size == 4) return exi_read(o);
    if (o >= 0x6400 && o < 0x6500 && size == 4) {
        uint32_t v = si_read(o);
        pi_line(PI_SI, si_interrupt_line());
        return v;
    }
    if (o >= 0x5000 && o < 0x5040) {
        if (size == 2) return dsp_read16(o);
        if (size == 4) { uint32_t hi = dsp_read16(o); return hi << 16 | dsp_read16(o + 2); }
    }
    switch (o) {
    case 0x3000: return g_pi_lines;                 // INTSR
    case 0x3018: return 0;                          // FIFO write pointer wrap state
    case 0x302C: return 0x246500B1;                 // PI revision (Flipper/rev C)
    case 0x0000: return 0x000C;                     // CP status: GP idle for reads and commands
    case 0x6C08: return ai_count(ppc_timebase());   // AISCNT
    case 0x200C:                                     // VI VCT (current line): fake progress
    case 0x202C: return (uint32_t)((ppc_timebase() / 2600) % 525) + 1;
    default: break;
    }
    uint32_t v = 0;
    for (int k = 0; k < size; k++) v = (v << 8) | g_regs[(o + k) & 0xFFFF];
    return v;
}

}  // namespace

void hw_init(const RtConfig& cfg) {
    memset(g_regs, 0, sizeof g_regs);
    set32(0x6004, 0);  // cover closed
    set16(0x5016, 1);  // AR_MODE: ARAM controller initialised
    exi_init();
    gx_init();
    ax_init(g_aram.data(), (uint32_t)g_aram.size());
    audio_init();
    if (!g_disc.open(cfg.files_root, cfg.sys_root)) rt_log("DI: could not build virtual disc from %s", cfg.files_root.c_str());
    g_next_retrace = ppc_timebase() + kFrameTicks;
}

uint32_t hw_read(uint32_t phys, int size) {
    uint32_t o = phys & 0xFFFF;
    uint32_t v = read_reg(o, size);
    first_use(o, false, v, size);
    return v;
}

void hw_write(uint32_t phys, uint32_t value, int size) {
    uint32_t o = phys & 0xFFFF;
    first_use(o, true, value, size);
    if ((o >> 12) == 0x8) { gx_pipe_write(value, size); return; }  // GX write-gather pipe
    if (o >= 0x1000 && o < 0x1100 && size == 2) { pe_write16(o, (uint16_t)value); return; }
    if (o >= 0x6000 && o < 0x6040 && size == 4) { di_write(o, value); return; }
    if (o >= 0x6400 && o < 0x6500 && size == 4) {
        si_write(o, value);
        pi_line(PI_SI, si_interrupt_line());
        return;
    }
    if (o >= 0x6800 && o < 0x683C && size == 4) {
        exi_write(o, value);
        pi_line(PI_EXI, exi_interrupt_line());
        return;
    }
    if (o >= 0x5000 && o < 0x5040) {
        if (size == 2) { dsp_write16(o, (uint16_t)value); return; }
        if (size == 4) { dsp_write16(o, (uint16_t)(value >> 16)); dsp_write16(o + 2, (uint16_t)value); return; }
    }
    if (o == 0x3000) { return; }  // INTSR: write-to-clear of RSW etc.
    if (o >= 0x6C00 && o < 0x6C10 && size == 4) { ai_write(o, value); return; }
    for (int k = size - 1; k >= 0; k--, value >>= 8) g_regs[(o + k) & 0xFFFF] = (uint8_t)value;
    if (o >= 0x2030 && o < 0x2040) vi_update_line();
}

// TimeSplitters' virtual memory (mmu_init / vm_page_fault_handler): virtual
// 0xA0000000 + x is paged in from ARAM at (16 MiB - window) + x, window size in
// the game global at 0x806123F8. Natively we map the window onto ARAM directly.
uint8_t* ppc_vm_host(uint32_t a) {
    uint32_t window = rd32(0x806123F8);
    uint32_t base = 0x1000000 - (window ? window : 0x800000);
    return &g_aram[(base + (a & 0x00FFFFFF)) & 0x00FFFFFF];
}

void hw_tick(uint64_t now) {
    vi_tick(now);
    aidma_tick(now);
    di_tick(now);
    ai_update(now);
}

uint32_t hw_pending_interrupts() { return g_pi_lines & reg32(0x3004); }

uint64_t hw_next_event() {
    uint64_t next = g_next_retrace;
    if (g_aidma_next && g_aidma_next < next) next = g_aidma_next;
    if (g_di_done_at && g_di_done_at < next) next = g_di_done_at;
    return next;
}

uint32_t hw_frame_count() { return g_frames; }

void hw_pe_finish() {
    set16(0x100A, reg16(0x100A) | 8);
    pe_update_line();
}

void hw_pe_token(uint16_t token, bool interrupt) {
    set16(0x100E, token);
    if (interrupt) set16(0x100A, reg16(0x100A) | 4);
    pe_update_line();
}
