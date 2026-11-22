#include "port.h"
#include "guest_if.h"
#include "present.h"
#include "gl3d.h"
#include "vita.h"
#include "menu.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH VITA_DATA_DIR "/settings.txt"
#define PANEL_W 480
#define PANEL_H 260
#define CORNER_W 116
#define CORNER_H 136

static const uint8_t s_font[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return s_font[c - 'A'];
    if (c >= '0' && c <= '9') return s_font[26 + c - '0'];
    if (c == '-') return s_font[36];
    return NULL;
}

static uint32_t s_pixels[PANEL_W * PANEL_H];
static bool s_open, s_dirty = true, s_block, s_was_touching, s_classic, s_layout_changed;
static int s_msaa, s_row;

static const int s_choices[3] = {0, 2, 4};
enum { OPT_X = 270, OPT_W = 66, ROW_Y = 86, ROW_TOP = 62, ROW2_Y = 156, ROW2_TOP = 132, ROW_BOTTOM = 196,
       BACK_X = 330 };
static const int s_layout_x[2] = {240, 316}, s_layout_w[2] = {76, 140};

static int button_x(int i) { return OPT_X + i * OPT_W; }

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t) a << 24 | (uint32_t) b << 16 | (uint32_t) g << 8 | (uint32_t) r; }

static void fill(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i >= 0 && i < PANEL_W && j >= 0 && j < PANEL_H) s_pixels[j * PANEL_W + i] = color;
        }
    }
}

static int text_w(int scale, const char *s) { return (int) strlen(s) * 6 * scale - scale; }

static void text(int cx, int y, int scale, uint32_t color, const char *s) {
    int len = (int) strlen(s), adv = 6 * scale;
    int x = cx - (len * adv - scale) / 2;
    for (; *s; s++, x += adv) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(g[row] >> (4 - col) & 1)) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx, py = y + row * scale + sy;
                        if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) s_pixels[py * PANEL_W + px] = color;
                    }
                }
            }
        }
    }
}

static void render(void) {
    const uint32_t white = rgba(255, 255, 255, 255), light = rgba(200, 200, 205, 255), off = rgba(105, 105, 112, 255);
    fill(0, 0, PANEL_W, PANEL_H, light);
    fill(2, 2, PANEL_W - 4, PANEL_H - 4, rgba(14, 14, 18, 230));
    text(24 + text_w(3, "SETTINGS") / 2, 20, 3, white, "SETTINGS");
    fill(24, 56, PANEL_W - 48, 2, rgba(70, 70, 78, 255));
    text(24 + text_w(3, "ANTI-ALIASING") / 2, ROW_Y, 3, s_row == 0 ? white : light, "ANTI-ALIASING");
    text(24 + text_w(3, "RACE SCREEN") / 2, ROW2_Y, 3, s_row == 1 ? white : light, "RACE SCREEN");
    static const char *const layouts[2] = {"HUD", "CLASSIC"};
    for (int i = 0; i < 2; i++) {
        bool on = s_classic == (i == 1);
        int cx = s_layout_x[i] + s_layout_w[i] / 2, w = text_w(3, layouts[i]);
        text(cx, ROW2_Y, 3, on ? white : off, layouts[i]);
        if (on) fill(cx - w / 2 - 3, ROW2_Y + 27, w + 6, 3, white);
    }
    static const char *const names[3] = {"OFF", "2X", "4X"};
    for (int i = 0; i < 3; i++) {
        bool on = s_choices[i] == s_msaa;
        int cx = button_x(i) + OPT_W / 2, w = text_w(3, names[i]);
        text(cx, ROW_Y, 3, on ? white : off, names[i]);
        if (on) fill(cx - w / 2 - 3, ROW_Y + 27, w + 6, 3, white);
    }
    fill(24, ROW_BOTTOM + 4, PANEL_W - 48, 2, rgba(70, 70, 78, 255));
    text(PANEL_W - 24 - text_w(2, "BACK") / 2, 222, 2, light, "BACK");
    s_dirty = false;
}

static void save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "msaa=%d\nlayout=%s\n", s_msaa, s_classic ? "classic" : "hud");
    fclose(f);
}

static void set_msaa(int samples) {
    if (samples == s_msaa) return;
    s_msaa = samples;
    gl3d_set_msaa(samples);
    present_refresh_3d();
    s_dirty = true;
    save();
    port_log(samples ? "Anti-aliasing: %dx" : "Anti-aliasing: off", samples);
}

static void set_classic(bool classic) {
    if (classic == s_classic) return;
    s_classic = classic;
    s_layout_changed = true;
    s_dirty = true;
    save();
    port_log(classic ? "Race screen: classic" : "Race screen: HUD");
}

bool vita_menu_classic(void) { return s_classic; }

bool vita_menu_take_layout_change(void) {
    bool changed = s_layout_changed;
    s_layout_changed = false;
    return changed;
}

void vita_menu_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    int v = 0;
    if (f) {
        char line[64], word[32];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "msaa=%d", &v) == 1) continue;
            if (sscanf(line, "layout=%31s", word) == 1) s_classic = strcmp(word, "classic") == 0;
        }
        fclose(f);
    }
    s_msaa = v == 2 || v == 4 ? v : 0;
    gl3d_set_msaa(s_msaa);
    if (s_msaa) port_log("Anti-aliasing: %dx", s_msaa);
}

static void close_menu(void) {
    s_open = false;
    s_block = true;
    guest_set_paused(false);
    vita_audio_pause(false);
}

static void panel_rect(int r[4]) {
    r[0] = (VITA_SCREEN_W - PANEL_W) / 2, r[1] = (VITA_SCREEN_H - PANEL_H) / 2, r[2] = PANEL_W, r[3] = PANEL_H;
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

bool vita_menu_input(int touches, int x, int y, uint32_t pressed, uint32_t held) {
    bool touching = touches > 0, tap = touching && !s_was_touching;
    s_was_touching = touching;
    if (!s_open) {
        if (s_block) {
            if (touching || held) return true;
            s_block = false;
        }
        if (tap && touches == 1 && x < CORNER_W && y < CORNER_H) {
            s_open = true;
            s_dirty = true;
            guest_set_paused(true);
            vita_audio_pause(true);
            return true;
        }
        return false;
    }
    if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
        s_row ^= 1;
        s_dirty = true;
    }
    if (s_row == 0) {
        int idx = 0;
        while (idx < 2 && s_choices[idx] != s_msaa) idx++;
        if (pressed & SCE_CTRL_LEFT && idx > 0) set_msaa(s_choices[idx - 1]);
        if (pressed & SCE_CTRL_RIGHT && idx < 2) set_msaa(s_choices[idx + 1]);
    } else {
        if (pressed & SCE_CTRL_LEFT) set_classic(false);
        if (pressed & SCE_CTRL_RIGHT) set_classic(true);
    }
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
        close_menu();
        return true;
    }
    if (tap) {
        int r[4];
        panel_rect(r);
        int px = x - r[0], py = y - r[1];
        if (!inside(px, py, 0, 0, PANEL_W, PANEL_H) ||
            inside(px, py, BACK_X, ROW_BOTTOM + 6, PANEL_W - BACK_X, PANEL_H - ROW_BOTTOM - 6)) {
            close_menu();
            return true;
        }
        for (int i = 0; i < 3; i++) {
            if (inside(px, py, button_x(i), ROW_TOP, OPT_W, ROW2_TOP - ROW_TOP)) set_msaa(s_choices[i]);
        }
        for (int i = 0; i < 2; i++) {
            if (inside(px, py, s_layout_x[i], ROW2_TOP, s_layout_w[i], ROW_BOTTOM - ROW2_TOP)) set_classic(i == 1);
        }
    }
    return true;
}

bool vita_menu_open(void) { return s_open; }

void vita_menu_draw(void) {
    if (!s_open) return;
    bool changed = s_dirty;
    if (changed) render();
    int r[4];
    panel_rect(r);
    present_draw_overlay(s_pixels, changed, PANEL_W, PANEL_H, r, VITA_SCREEN_W, VITA_SCREEN_H);
}
