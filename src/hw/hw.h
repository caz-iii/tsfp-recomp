// Hardware register blocks at 0xCC000000 as seen by recompiled code.
#ifndef TSFP_HW_H
#define TSFP_HW_H

#include <cstdint>

struct RtConfig;

void hw_init(const RtConfig& cfg);
uint32_t hw_read(uint32_t phys, int size);
void hw_write(uint32_t phys, uint32_t value, int size);

void hw_tick(uint64_t now);            // advance timed devices to timebase `now`
uint32_t hw_pending_interrupts();      // PI cause & mask
uint64_t hw_next_event();              // timebase of the next scheduled device event
uint32_t hw_frame_count();

#endif  // TSFP_HW_H
