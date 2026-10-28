// Guest CPU state and memory access for statically recompiled Gekko code.
// Included by every generated translation unit, so keep it lean and inline.
#ifndef TSFP_PPC_H
#define TSFP_PPC_H

#include <atomic>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#define PPC_BSWAP16(x) _byteswap_ushort(x)
#define PPC_BSWAP32(x) _byteswap_ulong(x)
#define PPC_BSWAP64(x) _byteswap_uint64(x)
#define PPC_LIKELY(x) (x)
#else
#define PPC_BSWAP16(x) __builtin_bswap16(x)
#define PPC_BSWAP32(x) __builtin_bswap32(x)
#define PPC_BSWAP64(x) __builtin_bswap64(x)
#define PPC_LIKELY(x) __builtin_expect(!!(x), 1)
#endif

union PPCFpr {
    double d;
    uint64_t u;
};

struct PPCState {
    uint32_t r[32];
    PPCFpr f[32];     // ps0 aliases the FPR
    double ps1[32];   // paired-single slot 1
    uint8_t cr[8];    // each field: LT=8 GT=4 EQ=2 SO=1
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr;
    uint32_t gqr[8];
    uint32_t msr;
    uint32_t reserve, reserve_addr;
    uint32_t spr[1024];  // everything without a dedicated field
};

// ---------------------------------------------------------------- memory
// MEM1 is 24 MiB, mirrored at 0x80000000 (cached) and 0xC0000000 (uncached).
// 0xE0000000 is the 16 KiB locked cache. 0xCC000000 is hardware registers.
constexpr uint32_t kMem1Size = 0x01800000;
constexpr uint32_t kLockedCacheSize = 0x4000;

extern uint8_t* g_mem1;
extern uint8_t g_locked_cache[kLockedCacheSize];

uint32_t mmio_read(uint32_t addr, int size);
uint8_t* ppc_vm_host(uint32_t addr);
void mmio_write(uint32_t addr, uint32_t value, int size);
[[noreturn]] void ppc_fault(const char* what, uint32_t addr);

inline uint8_t* ppc_host(uint32_t a) {
    uint32_t top = a >> 24;
    if (PPC_LIKELY((top & 0xBE) == 0x80)) {  // 0x80/0x81 cached, 0xC0/0xC1 uncached
        return g_mem1 + (a & 0x01FFFFFF);
    }
    if ((a >> 28) == 0xE) return g_locked_cache + (a & (kLockedCacheSize - 1));
    if (top == 0xA0) return ppc_vm_host(a);  // game demand-paged window onto ARAM
    if (a < kMem1Size) return g_mem1 + a;  // physical addresses (DMA, OSPhysicalToCached input)
    return nullptr;
}

inline bool ppc_is_mmio(uint32_t a) { return (a >> 24) == 0xCC || (a >> 24) == 0x0C; }

inline uint8_t rd8(uint32_t a) {
    if (uint8_t* p = ppc_host(a)) return *p;
    if (ppc_is_mmio(a)) return (uint8_t)mmio_read(a, 1);
    ppc_fault("read8", a);
}
inline uint16_t rd16(uint32_t a) {
    if (uint8_t* p = ppc_host(a)) { uint16_t v; memcpy(&v, p, 2); return PPC_BSWAP16(v); }
    if (ppc_is_mmio(a)) return (uint16_t)mmio_read(a, 2);
    ppc_fault("read16", a);
}
inline uint32_t rd32(uint32_t a) {
    if (uint8_t* p = ppc_host(a)) { uint32_t v; memcpy(&v, p, 4); return PPC_BSWAP32(v); }
    if (ppc_is_mmio(a)) return mmio_read(a, 4);
    ppc_fault("read32", a);
}
inline uint64_t rd64(uint32_t a) {
    if (uint8_t* p = ppc_host(a)) { uint64_t v; memcpy(&v, p, 8); return PPC_BSWAP64(v); }
    ppc_fault("read64", a);
}
inline void wr8(uint32_t a, uint8_t v) {
    if (uint8_t* p = ppc_host(a)) { *p = v; return; }
    if (ppc_is_mmio(a)) { mmio_write(a, v, 1); return; }
    ppc_fault("write8", a);
}
inline void wr16(uint32_t a, uint16_t v) {
    if (uint8_t* p = ppc_host(a)) { v = PPC_BSWAP16(v); memcpy(p, &v, 2); return; }
    if (ppc_is_mmio(a)) { mmio_write(a, v, 2); return; }
    ppc_fault("write16", a);
}
inline void wr32(uint32_t a, uint32_t v) {
    if (uint8_t* p = ppc_host(a)) { v = PPC_BSWAP32(v); memcpy(p, &v, 4); return; }
    if (ppc_is_mmio(a)) { mmio_write(a, v, 4); return; }
    ppc_fault("write32", a);
}
inline void wr64(uint32_t a, uint64_t v) {
    if (uint8_t* p = ppc_host(a)) { v = PPC_BSWAP64(v); memcpy(p, &v, 8); return; }
    if (ppc_is_mmio(a)) {  // stfd to the write-gather pipe
        mmio_write(a, (uint32_t)(v >> 32), 4);
        mmio_write(a, (uint32_t)v, 4);
        return;
    }
    ppc_fault("write64", a);
}

inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

inline double rdf32(uint32_t a) { return (double)u2f(rd32(a)); }
inline void wrf32(uint32_t a, double d) { wr32(a, f2u((float)d)); }
inline double rdf64(uint32_t a) { return u2d(rd64(a)); }
inline void wrf64(uint32_t a, double d) { wr64(a, d2u(d)); }

// ---------------------------------------------------------------- helpers
inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

inline uint8_t cmp_s(int32_t a, int32_t b, uint32_t so) { return (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (so & 1)); }
inline uint8_t cmp_u(uint32_t a, uint32_t b, uint32_t so) { return (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (so & 1)); }
inline uint8_t cmp_f(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return 1;  // FU (unordered) in the SO slot
    return (uint8_t)(a < b ? 8 : a > b ? 4 : 2);
}

inline uint32_t cr_get(const PPCState* s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)(s->cr[i] & 15) << (28 - 4 * i);
    return v;
}
inline void cr_set(PPCState* s, uint32_t v, uint32_t crm) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80 >> i)) s->cr[i] = (uint8_t)((v >> (28 - 4 * i)) & 15);
}
inline uint32_t xer_get(const PPCState* s) {
    return (s->xer_so << 31) | (s->xer_ov << 30) | (s->xer_ca << 29) | (s->xer_bc & 0x7F);
}
inline void xer_set(PPCState* s, uint32_t v) {
    s->xer_so = v >> 31; s->xer_ov = (v >> 30) & 1; s->xer_ca = (v >> 29) & 1; s->xer_bc = v & 0x7F;
}

inline int32_t fctiw_sat(double d) {
    if (std::isnan(d)) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
inline double ppc_single(double d) { return (double)(float)d; }
inline double ppc_fres(double d) { return (double)(1.0f / (float)d); }
inline double ppc_frsqrte(double d) { return 1.0 / std::sqrt(d); }

// Paired-single quantisation (GQR type: 0 float, 4 u8, 5 u16, 6 s8, 7 s16).
inline double psq_dequant(uint32_t a, uint32_t type, uint32_t scale, int* step) {
    double mul = std::ldexp(1.0, -(int)((int32_t)(scale << 26) >> 26));
    switch (type) {
    case 4: *step = 1; return rd8(a) * mul;
    case 5: *step = 2; return rd16(a) * mul;
    case 6: *step = 1; return (int8_t)rd8(a) * mul;
    case 7: *step = 2; return (int16_t)rd16(a) * mul;
    default: *step = 4; return rdf32(a);
    }
}
inline void psq_quant(uint32_t a, double v, uint32_t type, uint32_t scale, int* step) {
    double x = v * std::ldexp(1.0, (int)((int32_t)(scale << 26) >> 26));
    auto clampi = [](double y, double lo, double hi) { return y < lo ? lo : y > hi ? hi : y; };
    switch (type) {
    case 4: *step = 1; wr8(a, (uint8_t)clampi(x, 0, 255)); break;
    case 5: *step = 2; wr16(a, (uint16_t)clampi(x, 0, 65535)); break;
    case 6: *step = 1; wr8(a, (uint8_t)(int8_t)clampi(x, -128, 127)); break;
    case 7: *step = 2; wr16(a, (uint16_t)(int16_t)clampi(x, -32768, 32767)); break;
    default: *step = 4; wrf32(a, v); break;
    }
}
inline void psq_load(PPCState* s, int fd, uint32_t a, int w, int i) {
    uint32_t g = s->gqr[i];
    uint32_t type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    int step;
    double v0 = psq_dequant(a, type, scale, &step);
    double v1 = w ? 1.0 : psq_dequant(a + step, type, scale, &step);
    s->f[fd].d = v0;
    s->ps1[fd] = v1;
}
inline void psq_store(PPCState* s, int fs, uint32_t a, int w, int i) {
    uint32_t g = s->gqr[i];
    uint32_t type = g & 7, scale = (g >> 8) & 0x3F;
    int step;
    psq_quant(a, s->f[fs].d, type, scale, &step);
    if (!w) psq_quant(a + step, s->ps1[fs], type, scale, &step);
}

// ---------------------------------------------------------------- interrupts
// Polled at every loop head of recompiled code. The fence stops the host
// compiler from caching guest memory across iterations of busy-wait loops.
extern volatile uint32_t g_ppc_poll;
void ppc_service(PPCState* s);
#define PPC_POLL(s)                                                     \
    do {                                                                \
        std::atomic_signal_fence(std::memory_order_seq_cst);            \
        if (--g_ppc_poll == 0) ppc_service(s);                          \
    } while (0)

// Ring buffer of recent function entries, dumped on faults and hangs.
// One ring per execution context (main thread, idle fiber, interrupts).
struct PPCTraceRing {
    uint32_t buf[1024];
    uint32_t pos;
};
extern PPCTraceRing* g_ppc_ring;
#define PPC_TRACE(addr) (g_ppc_ring->buf[g_ppc_ring->pos++ & 1023] = (addr))

// ---------------------------------------------------------------- calls
typedef void (*PPCFunc)(PPCState*);
struct PPCFuncEntry {
    uint32_t addr;
    PPCFunc fn;
};
struct PPCNamedEntry {
    const char* name;
    uint32_t addr;
};
// OSSaveContext / OSLoadContext: the guest's setjmp/longjmp (see runtime/irq.cpp).
jmp_buf* ppc_context_jmpbuf(uint32_t context);
void ppc_context_saved(PPCState* s);
void ppc_call(PPCState* s, uint32_t addr);  // indirect call through the function table
uint64_t ppc_timebase();
void ppc_syscall(PPCState* s, uint32_t pc);
void ppc_trap(PPCState* s, uint32_t pc);
void ppc_mtspr(PPCState* s, uint32_t spr, uint32_t v);
uint32_t ppc_mfspr(PPCState* s, uint32_t spr);
void ppc_dcbz_l(uint32_t a);
void ppc_mtmsr(PPCState* s, uint32_t v);
void ppc_trace_call(PPCState* s, uint32_t addr);  // entry hook for functions listed as trace:<name>

#endif  // TSFP_PPC_H
