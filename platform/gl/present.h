#pragma once

#include "frame.h"

// Needs a current OpenGL ES 3 context.
void present_init(void);
// Takes a new snapshot (2D drawn now; 3D at 192*scale lines, drawn by present_draw).
void present_update(const FrameSnapshot *f, int scale);
// Draws the top and bottom screens into the rectangles (x, y, w, h each, y down) of a w x h framebuffer 0, each
// screen as large as fits. t in 0..1: how far between the previous and the last snapshot the 3D is drawn (1: the
// last one; less for frames in between at display rates above 60 Hz).
void present_draw(const int layout[8], int w, int h, float t);
void present_refresh_3d(void);
void present_set_scale(int scale);
void present_draw_overlay(const uint32_t *rgba, bool changed, int tw, int th, const int rect[4], int w, int h);
// The framebuffer present_draw draws into, when it is not the window's (0): a headset's swapchain image.
void present_set_target(uint32_t framebuffer);
// Widescreen: the aspect ratio the race cameras use (guest_set_aspect); race frames are drawn that wide.
void present_set_wide(float aspect);
bool present_is_race(void); // the last snapshot was a race frame
bool present_is_dual_3d(void);
bool present_engine_a_on_top(void);

typedef struct PresentStats {
    uint32_t captures;
    uint64_t gpu3d_us, capture_2d_us;
    uint32_t staged;
    uint64_t stage_us;
} PresentStats;
extern PresentStats g_present_stats;
extern uint64_t (*g_present_clock_us)(void);
extern const char *volatile g_present_stage;

bool present_async_start(const FrameSnapshot *(*take)(void), void (*release)(const FrameSnapshot *), int scale);
bool present_async_commit(void);
uint64_t present_shown_time_ns(void);
void present_async_swapped(void);
void present_async_frame_ready(void);
void present_set_screen_areas(int top, int bottom);
typedef struct PresentHudLayout {
    int list[4], time[4], side[4], map[4], round[4], whole[4], pause[4];
} PresentHudLayout;
void present_set_hud(const PresentHudLayout *layout, bool draw);
int present_hud_flags(void);
bool present_hud_usable(void);
bool present_hud_touch(int x, int y, int *ds_x, int *ds_y);
extern void (*g_present_test_hook)(const FrameSnapshot *f);
