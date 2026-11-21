#include "port.h"

const uint32_t g_vram_bank_offset[VRAM_BANK_COUNT] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                                      0x90000, 0x94000, 0x98000, 0xa0000};
const uint32_t g_vram_bank_size[VRAM_BANK_COUNT] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                                    0x4000,  0x4000,  0x8000,  0x4000};
static const uint32_t s_lcdc_addr[VRAM_BANK_COUNT] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                                      0x06890000, 0x06894000, 0x06898000, 0x068a0000};
uint32_t port_vram_cpu_address(int bank, uint8_t cnt) {
    if (!(cnt & 0x80)) {
        return 0;
    }
    uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3;
    switch (bank) {
    case VRAM_A:
    case VRAM_B:
    case VRAM_C:
    case VRAM_D:
        if (bank >= VRAM_C && mst == 4) {
            return bank == VRAM_C ? 0x06200000 : 0x06600000;
        }
        switch (mst & 3) {
        case 0:
            return s_lcdc_addr[bank];
        case 1:
            return 0x06000000 + ofs * 0x20000;
        case 2:
            return bank <= VRAM_B ? 0x06400000 + (ofs & 1) * 0x20000 : 0;
        default:
            return 0;
        }
    case VRAM_E:
        switch (mst) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06000000;
        case 2: return 0x06400000;
        default: return 0;
        }
    case VRAM_F:
    case VRAM_G:
        switch (mst) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06000000 + (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
        case 2: return 0x06400000 + (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
        default: return 0;
        }
    case VRAM_H:
        switch (mst & 3) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06200000;
        default: return 0;
        }
    case VRAM_I:
        switch (mst & 3) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06208000;
        case 2: return 0x06600000;
        default: return 0;
        }
    }
    return 0;
}

static const uint32_t s_area_lo[16] = {[DS_AREA_ITCM] = 0xff8000};
static const uint32_t s_area_hi[16] = {[DS_AREA_ITCM] = 0x1000000, [DS_AREA_MAIN] = 0x800000, [DS_AREA_IO] = 0x110000,
                                       [DS_AREA_PLTT] = 0x800,      [DS_AREA_VRAM] = 0x8a4000, [DS_AREA_OAM] = 0x800,
                                       [DS_AREA_GBA] = 0x4000};

uint32_t port_ds_ofs(const volatile void *ptr) {
    uintptr_t p = (uintptr_t) ptr;
    for (uint32_t area = 1; area < 16; area++) {
        if (!s_area_hi[area]) continue;
        uint32_t start = area << 24;
        uintptr_t host = start + g_ds_off[area];
        if (p >= host + s_area_lo[area] && p < host + s_area_hi[area]) return (uint32_t) (p - g_ds_off[area]);
    }
    return DS_NONE;
}
