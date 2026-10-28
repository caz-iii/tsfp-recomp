// Host-side runtime services for the recompiled game.
#ifndef TSFP_RUNTIME_H
#define TSFP_RUNTIME_H

#include <string>
#include <vector>

#include "runtime/ppc.h"

struct RtConfig {
    std::string dol_path;          // the user's main.dol
    std::string files_root;        // extracted disc files/ directory
    std::string sys_root;          // extracted disc sys/ directory (boot.bin, fst.bin, ...)
    std::vector<uint8_t> disc_header;  // first 0x440 bytes of the disc (boot.bin)
};

void rt_log(const char* fmt, ...);
bool rt_boot(const RtConfig& cfg, PPCState* s);
void rt_set_timebase(uint64_t v);
void irq_init();
void irq_status();
void irq_set_dec(uint32_t value);
uint32_t irq_get_dec();
void rt_dump_trace(int count);

#endif  // TSFP_RUNTIME_H
