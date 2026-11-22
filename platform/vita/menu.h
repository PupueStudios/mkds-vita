#pragma once

#include <stdbool.h>
#include <stdint.h>

void vita_menu_load(void);
bool vita_menu_input(int touches, int x, int y, uint32_t pressed, uint32_t held);
bool vita_menu_open(void);
void vita_menu_draw(void);
bool vita_menu_classic(void);
bool vita_menu_take_layout_change(void);
