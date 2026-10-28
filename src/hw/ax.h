#ifndef TSFP_AX_H
#define TSFP_AX_H

#include <cstdint>

void ax_init(uint8_t* aram, uint32_t size);
void ax_process_command_list(uint32_t addr);
bool ax_take_output(int16_t* stereo160);  // last mixed frame (L,R interleaved), once

// Host audio output (SDL3), 32 kHz stereo.
void audio_init();
void audio_push(const int16_t* stereo, int frames);

#endif  // TSFP_AX_H
