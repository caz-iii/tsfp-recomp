// Core runtime for recompiled code: guest memory, hardware-register dispatch,
// indirect calls, special-purpose registers and the timebase.
#include "runtime/runtime.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "funcs.h"
#include "hw/hw.h"

uint8_t* g_mem1 = nullptr;
PPCTraceRing g_ring_main, g_ring_idle, g_ring_irq;
PPCTraceRing* g_ppc_ring = &g_ring_main;
uint8_t g_locked_cache[kLockedCacheSize];

namespace {

std::chrono::steady_clock::time_point g_boot_time;
uint64_t g_tb_offset = 0;
FILE* g_log = stderr;

}  // namespace

void rt_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static const char* name_of(uint32_t addr) {
    for (uint32_t i = 0; i < g_ppc_name_count; i++)
        if (g_ppc_names[i].addr == addr) return g_ppc_names[i].name;
    return "?";
}

// Name of the function containing addr (g_ppc_names is sorted by address).
const char* rt_name_of(uint32_t addr) {
    const char* best = "?";
    for (uint32_t i = 0; i < g_ppc_name_count && g_ppc_names[i].addr <= addr; i++) best = g_ppc_names[i].name;
    return best;
}

static void dump_ring(const char* what, const PPCTraceRing& r, int count) {
    rt_log("last %d function entries on the %s (oldest first):", count, what);
    for (int k = count; k > 0; k--) {
        uint32_t a = r.buf[(r.pos - k) & 1023];
        if (a) rt_log("  %08X %s", a, name_of(a));
    }
}

void rt_dump_trace(int count) {
    dump_ring("main thread", g_ring_main, count);
    if (g_ppc_ring != &g_ring_main) dump_ring(g_ppc_ring == &g_ring_idle ? "idle thread" : "interrupt", *g_ppc_ring, count);
}

[[noreturn]] void ppc_fault(const char* what, uint32_t addr) {
    rt_log("FATAL: %s at guest address 0x%08X", what, addr);
    rt_dump_trace(24);
    std::abort();
}

// ---------------------------------------------------------------- calls
static PPCFunc find_func(uint32_t addr) {
    uint32_t lo = 0, hi = g_ppc_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_ppc_funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_ppc_func_count && g_ppc_funcs[lo].addr == addr ? g_ppc_funcs[lo].fn : nullptr;
}

void ppc_call(PPCState* s, uint32_t addr) {
    PPCFunc fn = find_func(addr);
    if (!fn) {
        rt_log("FATAL: indirect call to 0x%08X (no function there), lr=0x%08X", addr, s->lr);
        rt_dump_trace(24);
        std::abort();
    }
    fn(s);
}

// ---------------------------------------------------------------- timebase
// Gekko timebase runs at bus clock / 4 = 40.5 MHz.
constexpr uint64_t kTimebaseHz = 162000000ull / 4;

uint64_t ppc_timebase() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot_time).count();
    return g_tb_offset + (uint64_t)((double)ns * (kTimebaseHz / 1e9));
}

void rt_set_timebase(uint64_t v) {
    g_tb_offset = 0;
    g_tb_offset = v - ppc_timebase();
}

// ---------------------------------------------------------------- SPRs
enum : uint32_t {
    SPR_DEC = 22, SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_W = 284, SPR_TBU_W = 285,
    SPR_HID2 = 920, SPR_WPAR = 921, SPR_DMAU = 922, SPR_DMAL = 923,
};

void ppc_mtspr(PPCState* s, uint32_t spr, uint32_t v) {
    s->spr[spr & 1023] = v;
    switch (spr) {
    case SPR_DEC: irq_set_dec(v); break;
    case SPR_TBL_W: rt_set_timebase((ppc_timebase() & 0xFFFFFFFF00000000ull) | v); break;
    case SPR_TBU_W: rt_set_timebase((ppc_timebase() & 0xFFFFFFFFull) | ((uint64_t)v << 32)); break;
    case SPR_DMAL:
        // Locked-cache DMA: DMAU = mem addr | len hi, DMAL = lc addr | len lo | load bit | trigger.
        if (v & 2) {
            uint32_t mem = s->spr[SPR_DMAU] & ~31u;
            uint32_t lc = v & ~31u;
            uint32_t len = ((s->spr[SPR_DMAU] & 31) << 2) | ((v >> 2) & 3);
            if (!len) len = 128;
            len *= 32;
            bool load = v & 0x10;
            for (uint32_t k = 0; k < len; k++) {
                if (load) wr8(lc + k, rd8(mem + k));
                else wr8(mem + k, rd8(lc + k));
            }
            s->spr[SPR_DMAL] &= ~2u;
        }
        break;
    default: break;
    }
}

uint32_t ppc_mfspr(PPCState* s, uint32_t spr) {
    switch (spr) {
    case 268: return (uint32_t)ppc_timebase();
    case 269: return (uint32_t)(ppc_timebase() >> 32);
    case 287: return 0x00083214;  // PVR: Gekko
    case SPR_DEC: return irq_get_dec();
    default: return s->spr[spr & 1023];
    }
}

void ppc_trace_call(PPCState* s, uint32_t addr) {
    static std::unordered_map<uint32_t, int> counts;
    if (counts[addr]++ < 400)
        rt_log("call %s(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) from 0x%08X %s", name_of(addr), s->r[3], s->r[4], s->r[5],
               s->r[6], s->r[7], s->r[8], s->lr, rt_name_of(s->lr));
}

void ppc_dcbz_l(uint32_t a) {
    for (int k = 0; k < 32; k += 4) wr32(a + k, 0);
}

void ppc_syscall(PPCState*, uint32_t) {}  // sc: used by PPCSync and the cache helpers; nothing to do natively

void ppc_trap(PPCState* s, uint32_t pc) {
    rt_log("FATAL: trap/unhandled instruction at 0x%08X (lr=0x%08X)", pc, s->lr);
    rt_dump_trace(24);
    std::abort();
}

// ---------------------------------------------------------------- MMIO
uint32_t mmio_read(uint32_t addr, int size) { return hw_read(addr & 0x0FFFFFFF, size); }
void mmio_write(uint32_t addr, uint32_t value, int size) { hw_write(addr & 0x0FFFFFFF, value, size); }

// ---------------------------------------------------------------- boot
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool rt_boot(const RtConfig& cfg, PPCState* s) {
    g_boot_time = std::chrono::steady_clock::now();
    g_mem1 = (uint8_t*)calloc(0x02000000, 1);  // 32 MiB so masked mirrors stay in bounds
    if (!g_mem1) return false;

    FILE* f = fopen(cfg.dol_path.c_str(), "rb");
    if (!f) {
        rt_log("cannot open %s", cfg.dol_path.c_str());
        return false;
    }
    std::vector<uint8_t> dol;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) dol.insert(dol.end(), buf, buf + n);
    fclose(f);
    if (dol.size() < 0x100) return false;
    for (int i = 0; i < 18; i++) {
        uint32_t off = be32(&dol[i * 4]), addr = be32(&dol[0x48 + i * 4]), size = be32(&dol[0x90 + i * 4]);
        if (size) memcpy(g_mem1 + (addr & 0x01FFFFFF), &dol[off], size);
    }
    uint32_t bss = be32(&dol[0xD8]), bss_size = be32(&dol[0xDC]);
    rt_log("DOL loaded: entry 0x%08X, bss 0x%08X+0x%X", be32(&dol[0xE0]), bss, bss_size);

    // Low-memory globals normally written by the IPL/apploader (OSBootInfo, see os.h).
    if (cfg.disc_header.size() >= 0x20) memcpy(g_mem1, cfg.disc_header.data(), 0x20);
    wr32(0x8000001C, 0xC2339F3D);  // disc magic
    wr32(0x80000020, 0x0D15EA5E);  // boot magic: normal boot
    wr32(0x80000024, 1);           // version
    wr32(0x80000028, kMem1Size);   // physical memory size
    wr32(0x8000002C, 1);           // console type: retail
    wr32(0x80000030, 0);           // arena lo (0 = use linker value)
    wr32(0x80000034, 0x81700000);  // arena hi (below FST)
    wr32(0x80000038, 0x81700000);  // FST location (filled by DVD HLE)
    wr32(0x8000003C, 0);           // FST max length
    wr32(0x800000CC, 0);           // video mode: NTSC
    wr32(0x800000F0, 0x01800000);  // simulated memory size
    wr32(0x800000F8, 162000000);   // bus clock
    wr32(0x800000FC, 486000000);   // CPU clock

    memset(s, 0, sizeof *s);
    s->msr = 0x00002032;  // FP, IR, DR, RI
    s->spr[SPR_HID2] = 0xA0000000;  // paired singles + locked cache enabled
    // The apploader leaves the FST at the top of memory for DVDInit.
    if (FILE* fst = fopen((cfg.sys_root + "/fst.bin").c_str(), "rb")) {
        size_t len = fread(g_mem1 + 0x01700000, 1, 0x80000, fst);
        fclose(fst);
        wr32(0x8000003C, (uint32_t)len);
    }
    hw_init(cfg);
    irq_init();
    return true;
}
