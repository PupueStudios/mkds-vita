// VBlank: what the DS shows during a frame is captured here for the renderer, then the geometry engine swaps its
// lists and the input registers are updated, before the game's VBlank interrupt handler runs (os.c).
#include "port.h"
#include "frame.h"

#include "hostapi.h"
#include <stdatomic.h>
#include <string.h>

void port_tp_vblank(void);
int GetCurrentSceneNumber(void);

// Aspect ratio of the race cameras (20.12): 4:3 on the DS, the screen's for widescreen (tools/portify.py points the
// camera setup here).
int g_port_aspect = 0x1555;

void (*g_frame_hook)(const FrameSnapshot *snapshot);
void (*g_frame_ready_hook)(void);
// Tests: when set, each captured frame posts `ready` and waits for `done` (host semaphores).
void *g_lockstep_ready, *g_lockstep_done;

uint8_t g_port_hud_oam_pending[2], g_port_hud_oam[2], g_port_hud_fresh;

#define BUFFERS 7
static FrameSnapshot s_buffers[BUFFERS];
static int s_write = 0;
#define QUEUE_DEPTH 2
static int s_queue[QUEUE_DEPTH], s_queue_count;
static int s_spare[BUFFERS], s_spare_count;
static void *s_spare_lock;
static bool s_spare_init;
static int s_read = -1, s_hold = -1;
static atomic_bool s_fresh;
static uint64_t s_number;

static uint64_t s_pending[BUFFERS];
static uint32_t s_version[FRAME_VRAM_PAGES], s_epoch;

#ifdef __vita__
uint8_t *hapi_vram_banks_changed(uint64_t *changed);
#else
static uint8_t s_shadow[VRAM_TOTAL_SIZE];
static uint8_t *hapi_vram_banks_changed(uint64_t *changed) {
    uint8_t *banks = hapi_vram_banks();
    uint64_t c = 0;
    for (uint32_t i = 0; i < FRAME_VRAM_PAGES; i++) {
        uint8_t *p = banks + i * FRAME_VRAM_PAGE, *s = s_shadow + i * FRAME_VRAM_PAGE;
        if (memcmp(p, s, FRAME_VRAM_PAGE) != 0) {
            memcpy(s, p, FRAME_VRAM_PAGE);
            c |= 1ull << i;
        }
    }
    *changed = c;
    return banks;
}
#endif

static uint64_t unused_bank_pages(void) {
    static const uint32_t cnt_offset[VRAM_BANK_COUNT] = {0x240, 0x241, 0x242, 0x243, 0x244, 0x245, 0x246, 0x248, 0x249};
    uint32_t dispcnt = IO32(0x000), capcnt = IO32(0x064);
    if (((dispcnt >> 16) & 3) == 2) return 0;
    if ((capcnt & 0x80000000u) && ((capcnt >> 29) & 3) != 0 && !(capcnt & 0x02000000u)) return 0;
    uint64_t m = 0;
    for (int b = 0; b < VRAM_BANK_COUNT; b++) {
        uint8_t cnt = IO8(cnt_offset[b]);
        if ((cnt & 0x80) && (cnt & 7) != 0) continue;
        uint32_t first = g_vram_bank_offset[b] / FRAME_VRAM_PAGE, n = g_vram_bank_size[b] / FRAME_VRAM_PAGE;
        for (uint32_t i = first; i < first + n && i < 64; i++) m |= 1ull << i;
    }
    return m;
}

static void capture_vram(FrameSnapshot *f, int index) {
    static bool started;
    uint64_t changed = 0;
    const uint8_t *banks = hapi_vram_banks_changed(&changed);
    if (!started) {
        started = true;
        changed = (1ull << FRAME_VRAM_PAGES) - 1;
    }
    if (changed) {
        s_epoch++;
        for (uint32_t i = 0; i < FRAME_VRAM_PAGES; i++) {
            if (changed >> i & 1) s_version[i] = s_epoch;
        }
        for (int b = 0; b < BUFFERS; b++) s_pending[b] |= changed;
    }
    uint64_t hidden = unused_bank_pages();
    uint64_t copy = s_pending[index] & ~hidden;
    s_pending[index] &= hidden;
    for (uint32_t i = 0; i < FRAME_VRAM_PAGES; i++) {
        if (copy >> i & 1) memcpy(f->vram + i * FRAME_VRAM_PAGE, banks + i * FRAME_VRAM_PAGE, FRAME_VRAM_PAGE);
        if (!(s_pending[index] >> i & 1)) f->vram_version[i] = s_version[i];
    }
}

static void capture(FrameSnapshot *f, int index) {
    memcpy(f->io, DS_PTR(DS_IO_BASE), 0x70);
    memcpy(f->io + 0x1000, DS_PTR(DS_IO_BASE + 0x1000), 0x70);
    memcpy(f->io + 0x200, DS_PTR(DS_IO_BASE + 0x200), 0x60); // VRAMCNT, POWCNT
    memcpy(f->io + 0x300, DS_PTR(DS_IO_BASE + 0x300), 0x10); // POWCNT1
    memcpy(f->palette, DS_PTR(DS_PLTT_BASE), sizeof(f->palette));
    memcpy(f->oam, DS_PTR(DS_OAM_BASE), sizeof(f->oam));
    capture_vram(f, index);
    memcpy(f->vramcnt, DS_PTR(DS_IO_BASE + 0x240), sizeof(f->vramcnt));
#ifdef __vita__
    if (f->list3d_held) gx3d_release_list(f->list3d);
    f->list3d = gx3d_acquire_displayed();
    f->list3d_held = true;
#else
    gx3d_copy_displayed(&f->frame3d);
    f->list3d = &f->frame3d;
    f->list3d_held = false;
#endif
    f->scene = (uint8_t) GetCurrentSceneNumber();
    f->hud_ok = g_port_hud_fresh > 0;
    if (g_port_hud_fresh) g_port_hud_fresh--;
    f->hud_oam[0] = f->hud_ok ? g_port_hud_oam[0] : 0;
    f->hud_oam[1] = f->hud_ok ? g_port_hud_oam[1] : 0;
    static int s_prev_top = -1;
    static uint32_t s_streak;
    int a_top = (f->io[0x305] & 0x80) != 0;
    s_streak = s_prev_top >= 0 && a_top != s_prev_top ? s_streak + 1 : 0;
    s_prev_top = a_top;
    f->swap_streak = s_streak;
    f->time_ns = hapi_now_ns();
    f->number = ++s_number;
}

static void spare_lock(void) {
    if (!s_spare_lock) s_spare_lock = hapi_mutex_new();
    hapi_mutex_lock(s_spare_lock);
    if (!s_spare_init) {
        s_spare_init = true;
        for (int i = 1; i < BUFFERS; i++) s_spare[s_spare_count++] = i;
    }
}

const FrameSnapshot *port_frame_take(void) {
    if (!atomic_load(&s_fresh)) return NULL;
    spare_lock();
    if (!s_queue_count) {
        atomic_store(&s_fresh, false);
        hapi_mutex_unlock(s_spare_lock);
        return NULL;
    }
    int got = s_queue[0];
    for (int i = 1; i < s_queue_count; i++) s_queue[i - 1] = s_queue[i];
    s_queue_count--;
    atomic_store(&s_fresh, s_queue_count > 0);
    hapi_mutex_unlock(s_spare_lock);
    return &s_buffers[got];
}

void port_frame_release(const FrameSnapshot *f) {
    if (!f) return;
    spare_lock();
    s_spare[s_spare_count++] = (int) (f - s_buffers);
    hapi_mutex_unlock(s_spare_lock);
}

const FrameSnapshot *port_frame_acquire(void) {
    const FrameSnapshot *f = port_frame_take();
    if (!f) return NULL;
    if (s_hold >= 0) port_frame_release(&s_buffers[s_hold]);
    s_hold = s_read;
    s_read = (int) (f - s_buffers);
    return f;
}

static PortInput s_input;
static void *s_input_lock;
static void input_lock(void) {
    if (!s_input_lock) s_input_lock = hapi_mutex_new();
    hapi_mutex_lock(s_input_lock);
}

void port_input_set(const PortInput *in) {
    input_lock();
    s_input = *in;
    hapi_mutex_unlock(s_input_lock);
}

void port_input_get(PortInput *out) {
    input_lock();
    *out = s_input;
    hapi_mutex_unlock(s_input_lock);
}

static void update_input_registers(void) {
    PortInput in;
    port_input_get(&in);
    IO16(0x130) = (uint16_t) (~in.buttons & 0x3ff); // KEYINPUT, active low
    // What the ARM7 leaves at 0x027fffa8, in KEYINPUT positions: X (bit 10), Y (bit 11) and debug (bit 13), active
    // low, and the hinge (bit 15, set when closed).
    uint16_t xy = 0x2000;
    if (!(in.buttons & PAD_X)) xy |= 0x0400;
    if (!(in.buttons & PAD_Y)) xy |= 0x0800;
    if (in.lid_closed) xy |= 0x8000;
    *(volatile uint16_t *) DS_PTR(0x027fffa8) = xy;
}

void port_frame_vblank(void) {
    FrameSnapshot *f = &s_buffers[s_write];
    capture(f, s_write);
    if (g_frame_hook) {
        g_frame_hook(f);
    }
    spare_lock();
    int dropped = -1;
    if (s_queue_count == QUEUE_DEPTH || (!s_spare_count && s_queue_count)) {
        dropped = s_queue[0];
        for (int i = 1; i < s_queue_count; i++) s_queue[i - 1] = s_queue[i];
        s_queue_count--;
    }
    if (dropped >= 0 || s_spare_count) {
        s_queue[s_queue_count++] = s_write;
        s_write = dropped >= 0 ? dropped : s_spare[--s_spare_count];
    }
    atomic_store(&s_fresh, s_queue_count > 0);
    hapi_mutex_unlock(s_spare_lock);
    if (g_frame_ready_hook) g_frame_ready_hook();
    if (g_lockstep_ready) {
        hapi_sem_post(g_lockstep_ready);
        hapi_sem_wait(g_lockstep_done);
    }

    gx3d_vblank();
    update_input_registers();
    port_tp_vblank();
    IO16(0x006) = 192;              // VCOUNT
    IO16(0x004) |= 1;               // DISPSTAT: in VBlank
}
