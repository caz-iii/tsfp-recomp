#ifndef TSFP_EXI_H
#define TSFP_EXI_H

#include <cstdint>

void exi_init();
uint32_t exi_read(uint32_t offset);
void exi_write(uint32_t offset, uint32_t value);
bool exi_interrupt_line();

#endif  // TSFP_EXI_H
