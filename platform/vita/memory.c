#include "port.h"

#include <arm_neon.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>

unsigned long g_ds_off[16];
uint8_t *g_vram_banks;

#define VPAGE 0x4000u
#define VPAGE_COUNT (DS_VRAM_SIZE / VPAGE)
static int8_t s_page_bank[VPAGE_COUNT];
static uint32_t s_page_offset[VPAGE_COUNT];
static pthread_mutex_t s_vram_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_changed;

static bool page_differs(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < VPAGE; i += 256) {
        uint32x4_t acc = vdupq_n_u32(0);
        for (uint32_t j = i; j < i + 256; j += 64) {
            uint32x4_t a0 = vld1q_u32((const uint32_t *) (a + j)), b0 = vld1q_u32((const uint32_t *) (b + j));
            uint32x4_t a1 = vld1q_u32((const uint32_t *) (a + j + 16)), b1 = vld1q_u32((const uint32_t *) (b + j + 16));
            uint32x4_t a2 = vld1q_u32((const uint32_t *) (a + j + 32)), b2 = vld1q_u32((const uint32_t *) (b + j + 32));
            uint32x4_t a3 = vld1q_u32((const uint32_t *) (a + j + 48)), b3 = vld1q_u32((const uint32_t *) (b + j + 48));
            acc = vorrq_u32(acc, vorrq_u32(vorrq_u32(veorq_u32(a0, b0), veorq_u32(a1, b1)),
                                           vorrq_u32(veorq_u32(a2, b2), veorq_u32(a3, b3))));
        }
        uint32x2_t r = vorr_u32(vget_low_u32(acc), vget_high_u32(acc));
        if (vget_lane_u32(r, 0) | vget_lane_u32(r, 1)) return true;
    }
    return false;
}

static void mark_changed(const uint8_t *bank_data, uint32_t size) {
    uint32_t first = (uint32_t) (bank_data - g_vram_banks) / VPAGE;
    uint32_t last = ((uint32_t) (bank_data - g_vram_banks) + size - 1) / VPAGE;
    for (uint32_t i = first; i <= last && i < 64; i++) s_changed |= 1ull << i;
}

void port_vram_lock(void) { pthread_mutex_lock(&s_vram_lock); }
void port_vram_unlock(void) { pthread_mutex_unlock(&s_vram_lock); }

static bool area(uint32_t start, uint32_t size) {
    uint8_t *p = memalign(64, size);
    if (!p) {
        port_log("Memory: cannot reserve %u KB for the game (%08x)", (unsigned) (size / 1024), (unsigned) start);
        return false;
    }
    memset(p, 0, size);
    g_ds_off[start >> 24] = (unsigned long) ((uintptr_t) p - start);
    return true;
}

int port_shared_create(const char *name, size_t size) {
    (void) name, (void) size;
    return -1;
}

bool port_mem_init_fd(int vram_fd) {
    (void) vram_fd;
    return port_mem_init();
}

bool port_mem_init(void) {
    if (!area(DS_ITCM_BASE, DS_ITCM_SIZE) || !area(DS_MAIN_BASE, DS_MAIN_SIZE) ||
        !area(DS_IO_BASE, 0x110000) ||
        !area(DS_PLTT_BASE, 0x800) || !area(DS_VRAM_BASE, DS_VRAM_SIZE) || !area(DS_OAM_BASE, 0x800) ||
        !area(DS_GBA_BASE, 0x4000)) {
        return false;
    }
    g_vram_banks = memalign(64, VRAM_TOTAL_SIZE);
    if (!g_vram_banks) {
        port_log("Memory: cannot reserve the video memory");
        return false;
    }
    memset(g_vram_banks, 0, VRAM_TOTAL_SIZE);
    memset(s_page_bank, -1, sizeof(s_page_bank));
    IO16(0x130) = 0x03ff;
    return true;
}

static uint8_t *view_page(uint32_t page) { return (uint8_t *) DS_PTR(DS_VRAM_BASE + page * VPAGE); }
static uint8_t *bank_page(uint32_t page) {
    return g_vram_banks + g_vram_bank_offset[s_page_bank[page]] + s_page_offset[page];
}

static void vram_remap_(int bank);
void port_vram_remap(int bank) {
    vram_remap_(bank);
}
static void vram_remap_(int bank) {
    static const uint32_t cnt_offset[VRAM_BANK_COUNT] = {0x240, 0x241, 0x242, 0x243, 0x244, 0x245, 0x246, 0x248, 0x249};
    port_vram_lock();
    {
        uint32_t base = port_vram_cpu_address(bank, IO8(cnt_offset[bank]));
        uint32_t held = 0, expected = 0;
        bool same = true;
        for (uint32_t page = 0; page < VPAGE_COUNT; page++) held += s_page_bank[page] == bank;
        if (base != 0) {
            for (uint32_t off = 0; off < g_vram_bank_size[bank]; off += VPAGE) {
                uint32_t page = (base - DS_VRAM_BASE + off) / VPAGE;
                if (page >= VPAGE_COUNT) break;
                expected++;
                if (s_page_bank[page] != bank || s_page_offset[page] != off) same = false;
            }
        }
        if (same && held == expected) {
            port_vram_unlock();
            return;
        }
    }
    for (uint32_t page = 0; page < VPAGE_COUNT; page++) {
        if (s_page_bank[page] == bank) {
            if (page_differs(bank_page(page), view_page(page))) {
                memcpy(bank_page(page), view_page(page), VPAGE);
                mark_changed(bank_page(page), VPAGE);
            }
            s_page_bank[page] = -1;
        }
    }
    uint32_t base = port_vram_cpu_address(bank, IO8(cnt_offset[bank]));
    if (base != 0) {
        for (uint32_t off = 0; off < g_vram_bank_size[bank]; off += VPAGE) {
            uint32_t page = (base - DS_VRAM_BASE + off) / VPAGE;
            if (page >= VPAGE_COUNT) break;
            if (s_page_bank[page] >= 0 && s_page_bank[page] != bank &&
                page_differs(bank_page(page), view_page(page))) {
                memcpy(bank_page(page), view_page(page), VPAGE);
                mark_changed(bank_page(page), VPAGE);
            }
            s_page_bank[page] = (int8_t) bank;
            s_page_offset[page] = off;
            memcpy(view_page(page), bank_page(page), VPAGE);
        }
    }
    port_vram_unlock();
}

void port_vram_sync(void) {
    for (uint32_t page = 0; page < VPAGE_COUNT; page++) {
        if (s_page_bank[page] < 0) continue;
        uint8_t *b = bank_page(page);
        const uint8_t *v = view_page(page);
        if (page_differs(b, v)) {
            memcpy(b, v, VPAGE);
            mark_changed(b, VPAGE);
        }
    }
}

void port_vram_bank_written(int bank) {
    mark_changed(g_vram_banks + g_vram_bank_offset[bank], g_vram_bank_size[bank]);
    for (uint32_t page = 0; page < VPAGE_COUNT; page++) {
        if (s_page_bank[page] == bank) memcpy(view_page(page), bank_page(page), VPAGE);
    }
}

uint8_t *hapi_vram_banks_changed(uint64_t *changed) {
    port_vram_lock();
    port_vram_sync();
    *changed = s_changed;
    s_changed = 0;
    port_vram_unlock();
    return g_vram_banks;
}
