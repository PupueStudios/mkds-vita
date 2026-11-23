#pragma once

#include <stdbool.h>
#include <stdint.h>

void vita_menu_load(void);
bool vita_menu_input(int touches, int x, int y, uint32_t pressed, uint32_t held);
bool vita_menu_open(void);
void vita_menu_draw(void);
bool vita_menu_classic(void);
bool vita_menu_take_layout_change(void);
void vita_menu_mod_setting(char *out, int size);
void vita_menu_set_active_mod(const char *name);
const uint32_t *vita_menu_progress_panel(const char *title, const char *what, int percent, bool error, int *w,
                                         int *h);
void vita_draw_text(uint32_t *dst, int dw, int dh, int cx, int y, int scale, uint32_t color, const char *s);
