#include "wram_bank_select.h"

// Writes bank to the WRAM bank control byte at 0x4000247.
void MI_SetWramBank(u32 bank) {
    *(u8 *) DS_ADDR(0x4000247) = bank;
}
