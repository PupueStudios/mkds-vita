// The state of one displayed frame, captured at VBlank for the renderer thread.
#pragma once

#include "gx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_VRAM_PAGE 0x4000u
#define FRAME_VRAM_PAGES (0xa4000u / FRAME_VRAM_PAGE)

typedef struct FrameSnapshot {
    uint8_t io[0x1070];          // 0x04000000..: both 2D engines' registers (engine B at 0x1000)
    uint8_t palette[0x800];      // 0x05000000
    uint8_t oam[0x800];          // 0x07000000
    uint8_t vram[0xa4000];       // banks A..I (see g_vram_bank_offset)
    uint32_t vram_version[FRAME_VRAM_PAGES];
    uint8_t vramcnt[10];         // 0x04000240..0x04000249
    uint8_t scene;               // the game's scene number (2: race)
    uint32_t swap_streak;
    uint64_t time_ns;            // when it was captured (CLOCK_MONOTONIC, host time)
    const Gx3dFrame *list3d;
    bool list3d_held;
    Gx3dFrame frame3d;           // displayed 3D list (only the used vertices and polygons are copied)
    uint64_t number;
} FrameSnapshot;

const FrameSnapshot *port_frame_acquire(void);
const FrameSnapshot *port_frame_take(void);
extern void (*g_frame_ready_hook)(void);
void port_frame_release(const FrameSnapshot *f);
// Optional hook run on the game thread after each capture (tests).
extern void (*g_frame_hook)(const FrameSnapshot *snapshot);
extern void *g_lockstep_ready, *g_lockstep_done;

#ifdef __cplusplus
}
#endif
