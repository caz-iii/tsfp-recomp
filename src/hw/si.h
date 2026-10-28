#ifndef TSFP_SI_H
#define TSFP_SI_H

#include <cstdint>

uint32_t si_read(uint32_t offset);
void si_write(uint32_t offset, uint32_t value);
void si_poll_frame();  // once per VI field
bool si_interrupt_line();

// Automated input for headless runs (TSFP_INPUT), keyed by VI frame.
extern uint32_t si_frame_counter;
void si_scripted_input(uint16_t* buttons, uint8_t* sx, uint8_t* sy);

#endif  // TSFP_SI_H
