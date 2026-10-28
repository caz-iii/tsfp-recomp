// Interrupt delivery and the OS context primitives.
//
// Recompiled code polls at loop heads (PPC_POLL). When a device interrupt is
// pending and MSR[EE] is set, we run the OS's own __OSDispatchInterrupt on a
// dedicated interrupt stack, exactly as ExternalInterruptHandler would after
// saving state. The dispatcher finishes with OSLoadContext, which we implement
// as a host longjmp: back to the interrupted code, or to the OSSaveContext
// point of the thread the OS decided to run (the guest's setjmp/longjmp).
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "funcs.h"
#include "hw/hw.h"
#include "runtime/runtime.h"

volatile uint32_t g_ppc_poll = 1;
extern PPCTraceRing g_ring_main, g_ring_idle, g_ring_irq;

namespace {

constexpr uint32_t kIrqStackTop = 0x817EFF00;
constexpr uint32_t kIrqContext = 0x817F0000;
constexpr uint32_t kIdleThread = 0x817F1000;
constexpr uint32_t kPollInterval = 2048;
constexpr uint32_t kMsrEE = 0x8000;
constexpr uint32_t kCurrentThread = 0x800000E4;  // __OSCurrentThread

struct SavedContext {
    jmp_buf jb;
    PPCState state;
};
std::unordered_map<uint32_t, SavedContext*> g_contexts;

jmp_buf g_irq_jb;
PPCState g_irq_saved;
bool g_in_irq = false;
uint32_t g_irq_context = 0;  // context the interrupted code was running on

uint32_t g_dispatch = 0;  // __OSDispatchInterrupt
uint32_t g_dec_callback = 0;  // DecrementerExceptionCallback
bool g_dec_armed = false;
uint64_t g_dec_deadline = 0;
uint32_t g_last_msr = 0;
uint64_t g_services = 0, g_delivered = 0;

struct Idle {
    uint32_t fn = 0, param = 0, stack = 0;
    bool running = false;
} g_idle;

uint32_t lookup(const char* name) {
    for (uint32_t i = 0; i < g_ppc_name_count; i++)
        if (!strcmp(g_ppc_names[i].name, name)) return g_ppc_names[i].addr;
    rt_log("runtime: guest function %s not found", name);
    return 0;
}

// OSContext layout: gpr[32] 0x000, cr 0x080, lr 0x084, ctr 0x088, xer 0x08C,
// fpr[32] 0x090, fpscr 0x194, srr0 0x198, srr1 0x19C, mode 0x1A0, state 0x1A2,
// gqr[8] 0x1A4, psf[32] 0x1C8.
void write_context(uint32_t ctx, const PPCState& s, uint32_t srr0) {
    for (int i = 0; i < 32; i++) wr32(ctx + 4 * i, s.r[i]);
    wr32(ctx + 0x80, cr_get(&s));
    wr32(ctx + 0x84, s.lr);
    wr32(ctx + 0x88, s.ctr);
    wr32(ctx + 0x8C, xer_get(&s));
    for (int i = 0; i < 32; i++) wr64(ctx + 0x90 + 8 * i, s.f[i].u);
    wr32(ctx + 0x194, s.fpscr);
    wr32(ctx + 0x198, srr0);
    wr32(ctx + 0x19C, s.msr);
    for (int i = 0; i < 8; i++) wr32(ctx + 0x1A4 + 4 * i, s.gqr[i]);
    for (int i = 0; i < 32; i++) wrf64(ctx + 0x1C8 + 8 * i, s.ps1[i]);
}

void deliver(PPCState* s, uint32_t exception, uint32_t handler) {
    g_delivered++;
    // Like ExternalInterruptHandler: save into the current context (0x800000D4)
    // and hand that to the dispatcher, which ends with OSLoadContext(context).
    PPCTraceRing* ring = g_ppc_ring;
    g_ppc_ring = &g_ring_irq;
    g_irq_saved = *s;
    g_irq_context = rd32(0x800000D4);
    if (!g_irq_context) g_irq_context = kIrqContext;
    write_context(g_irq_context, *s, s->lr);
    g_in_irq = true;
    if (!setjmp(g_irq_jb)) {
        s->r[1] = kIrqStackTop;
        s->r[3] = exception;
        s->r[4] = g_irq_context;
        s->msr &= ~kMsrEE;
        ppc_call(s, handler);
        rt_log("runtime: exception %u handler returned without OSLoadContext", exception);
    }
    g_in_irq = false;
    *s = g_irq_saved;
    g_ppc_ring = ring;
}

// The SDK idle thread runs the game's stream_player_update, an endless loop.
// It lives on its own host fiber with its own guest register state so it can
// be preempted when an interrupt readies a thread and resumed later exactly
// where it stopped, like the idle thread on hardware.
constexpr uint32_t kRunQueueBits = 0x80613100;  // OSThread.c RunQueueBits
void* g_main_fiber = nullptr;
void* g_idle_fiber = nullptr;
PPCState g_idle_state;

void __stdcall idle_fiber_main(void*) {
    for (;;) {
        g_idle_state.r[1] = (g_idle.stack - 0x40) & ~15u;
        g_idle_state.r[3] = g_idle.param;
        g_idle_state.lr = 0;
        ppc_call(&g_idle_state, g_idle.fn);
        SwitchToFiber(g_main_fiber);  // idle function returned: nothing to do
    }
}

void run_idle(PPCState* s) {
    if (!g_idle.fn || g_idle.running) return;
    if (!g_main_fiber) g_main_fiber = ConvertThreadToFiber(nullptr);
    if (!g_idle_fiber) {
        g_idle_state = *s;  // inherit GQRs, MSR, SDA bases
        g_idle_fiber = CreateFiber(4 << 20, idle_fiber_main, nullptr);
    }
    g_idle.running = true;
    g_ppc_ring = &g_ring_idle;
    SwitchToFiber(g_idle_fiber);
    g_ppc_ring = &g_ring_main;
    g_idle.running = false;
}

// Called on the idle fiber: hand control back once a thread is ready to run.
void idle_maybe_yield() {
    if (g_idle.running && GetCurrentFiber() == g_idle_fiber && rd32(kRunQueueBits) != 0) SwitchToFiber(g_main_fiber);
}

}  // namespace

void irq_init() {
    g_dispatch = lookup("__OSDispatchInterrupt");
    g_dec_callback = lookup("DecrementerExceptionCallback");
}

// The decrementer counts down at the timebase rate; the exception fires when it
// goes negative. OS alarms are built on it.
void irq_set_dec(uint32_t value) {
    g_dec_deadline = ppc_timebase() + (uint64_t)(int64_t)(int32_t)value;
    if ((int32_t)value < 0) g_dec_deadline = ppc_timebase();
    g_dec_armed = true;
}

uint32_t irq_get_dec() {
    return (uint32_t)(int32_t)((int64_t)g_dec_deadline - (int64_t)ppc_timebase());
}

uint32_t g_ee_off_lr = 0, g_ee_off_fn = 0;

void ppc_mtmsr(PPCState* s, uint32_t v) {
    if ((s->msr & kMsrEE) && !(v & kMsrEE) && !g_in_irq) {
        g_ee_off_lr = s->lr;
        g_ee_off_fn = g_ppc_ring->buf[(g_ppc_ring->pos - 1) & 1023];
    }
    bool enabling = !(s->msr & kMsrEE) && (v & kMsrEE);
    s->msr = v;
    // Like the hardware, take a pending interrupt as soon as EE is set again.
    if (enabling && !g_in_irq && (hw_pending_interrupts() || (g_dec_armed && ppc_timebase() >= g_dec_deadline)))
        ppc_service(s);
}

const char* rt_name_of(uint32_t addr);

// Symbolised guest backtrace from a saved thread context (EABI back chain:
// [sp] = caller's sp, [caller sp + 4] = return address).
void backtrace_from(const PPCState& st) {
    uint32_t sp = st.r[1];
    rt_log("  backtrace (lr 0x%08X %s)", st.lr, rt_name_of(st.lr));
    for (int depth = 0; depth < 16 && sp && ppc_host(sp); depth++) {
        uint32_t prev = rd32(sp);
        if (!prev || !ppc_host(prev)) break;
        uint32_t ra = rd32(prev + 4);
        rt_log("    #%d 0x%08X %s", depth, ra, rt_name_of(ra));
        sp = prev;
    }
}

void irq_status() {
    for (auto& kv : g_contexts) backtrace_from(kv.second->state);
    if (!(g_last_msr & kMsrEE)) rt_log("irq: interrupts last disabled in 0x%08X, returning to 0x%08X", g_ee_off_fn, g_ee_off_lr);
    rt_log("irq: services %llu delivered %llu in_irq %d last msr 0x%08X pending 0x%08X dec armed %d in %lld ticks",
           (unsigned long long)g_services, (unsigned long long)g_delivered, (int)g_in_irq, g_last_msr,
           hw_pending_interrupts(), (int)g_dec_armed, (long long)(g_dec_deadline - ppc_timebase()));
}

void ppc_service(PPCState* s) {
    g_ppc_poll = kPollInterval;
    g_services++;
    g_last_msr = s->msr;
    uint64_t now = ppc_timebase();
    hw_tick(now);
    if (g_in_irq) return;
    if ((s->msr & kMsrEE) && hw_pending_interrupts()) {
        deliver(s, 4, g_dispatch);  // __OS_EXCEPTION_EXTERNAL_INTERRUPT
        idle_maybe_yield();
        return;
    }
    if ((s->msr & kMsrEE) && g_dec_armed && now >= g_dec_deadline) {
        g_dec_armed = false;
        deliver(s, 8, g_dec_callback);  // __OS_EXCEPTION_DECREMENTER
        idle_maybe_yield();
        return;
    }
    if (g_idle.running) {
        idle_maybe_yield();
        return;
    }
    // No current thread once threading is up: the OS is in SelectThread's idle
    // loop waiting for an interrupt. (Before OSInit it is 0 too, so wait for
    // the first real thread.)
    static bool threads_up = false;
    uint32_t cur = rd32(kCurrentThread);
    if (cur) threads_up = true;
    if (threads_up && cur == 0) {
        run_idle(s);
        uint64_t next = hw_next_event();
        if (g_dec_armed && g_dec_deadline < next) next = g_dec_deadline;
        now = ppc_timebase();
        if (next > now) {
            uint64_t us = (next - now) * 1000000 / 40500000;
            if (us > 1000) us = 1000;
            std::this_thread::sleep_for(std::chrono::microseconds(us));
        }
        g_ppc_poll = 64;  // stay responsive while idling
    }
}

// ---------------------------------------------------------------- contexts
jmp_buf* ppc_context_jmpbuf(uint32_t context) {
    SavedContext*& c = g_contexts[context];
    if (!c) c = new SavedContext();
    return &c->jb;
}

void ppc_context_saved(PPCState* s) {
    uint32_t ctx = s->r[3];
    SavedContext* c = g_contexts[ctx];
    c->state = *s;
    c->state.r[3] = 1;  // OSSaveContext returns TRUE when resumed
    write_context(ctx, *s, s->lr);
}

void hle_OSLoadContext(PPCState* s) {
    uint32_t ctx = s->r[3];
    if (g_in_irq && ctx == g_irq_context) longjmp(g_irq_jb, 1);
    auto it = g_contexts.find(ctx);
    if (it != g_contexts.end()) {
        g_in_irq = false;
        g_ppc_ring = &g_ring_main;
        *s = it->second->state;
        longjmp(it->second->jb, 1);
    }
    rt_log("FATAL: OSLoadContext(0x%08X): context was never saved (new thread start is not supported)", ctx);
    std::abort();
}

// The only extra thread this game creates is the SDK idle thread. Run the idle
// function directly whenever the OS has no thread to schedule.
void hle_OSSetIdleFunction(PPCState* s) {
    // Like OSCancelThread on the old idle thread: a new idle function starts
    // from scratch on a fresh fiber. (Never called from the idle fiber itself.)
    if (g_idle_fiber && GetCurrentFiber() != g_idle_fiber) {
        DeleteFiber(g_idle_fiber);
        g_idle_fiber = nullptr;
    }
    g_idle.fn = s->r[3];
    g_idle.param = s->r[4];
    g_idle.stack = s->r[5];
    rt_log("OSSetIdleFunction(0x%08X, param 0x%08X, stack 0x%08X size 0x%X) main sp 0x%08X", s->r[3], s->r[4], s->r[5], s->r[6], s->r[1]);
    s->r[3] = g_idle.fn ? kIdleThread : 0;
}
