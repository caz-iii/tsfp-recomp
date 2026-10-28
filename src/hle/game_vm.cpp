// TimeSplitters: Future Perfect's ARAM-backed virtual memory, natively.
//
// On the GameCube the game demand-pages the 0xA0000000 window from ARAM using
// the MMU (mmu_init, vm_page_fault_handler). Natively the window is mapped
// straight onto ARAM (see ppc_vm_host), so pages are always resident and
// coherent; operations that move pages between the window and ARAM are no-ops.
#include "funcs.h"

// Write mapped pages of a range back to ARAM (and optionally unmap them). The
// original waits on per-batch DMA completion of pages that, natively, are
// never mapped, so it would wait forever.
void hle_vm_range_writeback(PPCState*) {}
