#include "port.h"
#include "guest_if.h"
#include "present.h"
#include "gl3d.h"
#include "vita.h"
#include "menu.h"
#include "mods.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH VITA_DATA_DIR "/settings.txt"
#define UNLOCK_PATH VITA_DATA_DIR "/unlock"
#define PANEL_W 480
#define PANEL_H 372
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
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}, {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E},
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E}, {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F},
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},
    {0x0F, 0x11, 0x11, 0x13, 0x0D, 0x01, 0x0E}, {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}, {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}, {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11},
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x1E, 0x10, 0x10},
    {0x0F, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x01}, {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
    {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E}, {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    {0x11, 0x11, 0x11, 0x13, 0x0D, 0x01, 0x0E}, {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') return s_font[39 + c - 'a'];
    if (c >= 'A' && c <= 'Z') return s_font[c - 'A'];
    if (c >= '0' && c <= '9') return s_font[26 + c - '0'];
    if (c == '-') return s_font[36];
    if (c == '<') return s_font[37];
    if (c == '>') return s_font[38];
    if (c == '_') return s_font[65];
    if (c == '.') return s_font[66];
    return NULL;
}

static uint32_t s_pixels[PANEL_W * PANEL_H];
static bool s_open, s_dirty = true, s_block, s_was_touching, s_classic, s_layout_changed, s_unlock;
static int s_msaa, s_scale3d = 2, s_row;
static char s_mods[32][VITA_MOD_NAME], s_mod_active[VITA_MOD_NAME];
static int s_mod_count, s_mod;

enum { ROW_MSAA, ROW_LAYOUT, ROW_SCALE, ROW_MOD, ROW_UNLOCK, ROWS };
enum { ROWS_TOP = 66, ROW_H = 52, VALUE_X = 268, VALUE_CX = 362, BACK_X = 330 };
static const char *const s_labels[ROWS] = {"Anti-aliasing", "Race screen", "3D resolution", "Mod", "Unlock all"};
static const char *const s_names[ROWS][3] = {{"Off", "2x", "4x"}, {"HUD", "Classic"}, {"1x", "2x", "3x"},
                                             {"None"}, {"Unlock"}};
static const int s_counts[ROWS] = {3, 2, 3, 1, 1};

static int option_count(int row) { return row == ROW_MOD ? 1 + s_mod_count : s_counts[row]; }

static const char *option_name(int row, int i) { return row == ROW_MOD && i > 0 ? s_mods[i - 1] : s_names[row][i]; }

static bool mod_pending(void) { return strcmp(s_mod ? s_mods[s_mod - 1] : "", s_mod_active) != 0; }

static int row_top(int row) { return ROWS_TOP + row * ROW_H; }

static int current_option(int row) {
    switch (row) {
    case ROW_MSAA: return s_msaa / 2;
    case ROW_LAYOUT: return s_classic ? 1 : 0;
    case ROW_SCALE: return s_scale3d - 1;
    case ROW_MOD: return s_mod;
    default: return 0;
    }
}

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t) a << 24 | (uint32_t) b << 16 | (uint32_t) g << 8 | (uint32_t) r; }

static void fill(int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i >= 0 && i < PANEL_W && j >= 0 && j < PANEL_H) s_pixels[j * PANEL_W + i] = color;
        }
    }
}

static int text_w(int scale, const char *s) { return (int) strlen(s) * 6 * scale - scale; }

void vita_draw_text(uint32_t *dst, int dw, int dh, int cx, int y, int scale, uint32_t color, const char *s) {
    int len = (int) strlen(s), adv = 6 * scale;
    int x = cx - (len * adv - scale) / 2;
    for (; *s; s++, x += adv) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        int drop = strchr("gpqy", *s) ? 2 * scale : 0;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (!(g[row] >> (4 - col) & 1)) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx, py = y + drop + row * scale + sy;
                        if (px >= 0 && px < dw && py >= 0 && py < dh) dst[py * dw + px] = color;
                    }
                }
            }
        }
    }
}

static void text(int cx, int y, int scale, uint32_t color, const char *s) {
    vita_draw_text(s_pixels, PANEL_W, PANEL_H, cx, y, scale, color, s);
}

static void round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0;
            int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
            if (dx * dx + dy * dy > r * r) continue;
            int px = x + i, py = y + j;
            if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) s_pixels[py * PANEL_W + px] = color;
        }
    }
}

static void arrow(int cx, int cy, int size, int dir, uint32_t color) {
    for (int i = 0; i < size; i++) {
        int x = dir < 0 ? cx - size / 2 + i : cx + size / 2 - i;
        fill(x, cy - i, 1, 2 * i + 1, color);
    }
}

static void shadow_text(int cx, int y, int scale, uint32_t color, uint32_t shade, const char *s) {
    text(cx + scale / 2 + 1, y + scale / 2 + 1, scale, shade, s);
    text(cx, y, scale, color, s);
}

static void render(void) {
    const uint32_t white = rgba(255, 255, 255, 255), ink = rgba(34, 34, 40, 255), grey = rgba(150, 150, 158, 255);
    const uint32_t red = rgba(214, 38, 44, 255), dark_red = rgba(120, 14, 20, 255), blue = rgba(30, 92, 206, 255);
    const uint32_t dark_blue = rgba(14, 44, 110, 255), gold = rgba(255, 208, 60, 255), cream = rgba(255, 244, 205, 255);
    fill(0, 0, PANEL_W, PANEL_H, 0);
    round_rect(0, 0, PANEL_W, PANEL_H, 16, ink);
    round_rect(4, 4, PANEL_W - 8, PANEL_H - 8, 13, rgba(246, 246, 242, 255));
    round_rect(4, 4, PANEL_W - 8, 52, 13, red);
    fill(4, 40, PANEL_W - 8, 16, red);
    fill(4, 56, PANEL_W - 8, 4, ink);
    shadow_text(24 + text_w(3, "Settings") / 2, 19, 3, white, dark_red, "Settings");
    for (int row = 0; row < ROWS; row++) {
        int top = row_top(row), y = top + (ROW_H - 21) / 2, mid = top + ROW_H / 2;
        bool sel = s_row == row;
        if (sel) {
            round_rect(12, top + 3, PANEL_W - 24, ROW_H - 6, 10, gold);
            round_rect(15, top + 6, PANEL_W - 30, ROW_H - 12, 8, cream);
        }
        text(28 + text_w(2, s_labels[row]) / 2, y + 4, 2, ink, s_labels[row]);
        if (row == ROW_UNLOCK) {
            const char *name = s_unlock ? "Done" : "Unlock";
            int w = 150;
            round_rect(VALUE_CX - w / 2, mid - 17, w, 36, 18, s_unlock ? grey : dark_red);
            round_rect(VALUE_CX - w / 2, mid - 17, w, 33, 17, s_unlock ? rgba(196, 196, 200, 255) : red);
            shadow_text(VALUE_CX, y, 3, white, s_unlock ? grey : dark_red, name);
            continue;
        }
        int w = 140;
        round_rect(VALUE_CX - w / 2, mid - 17, w, 36, 18, dark_blue);
        round_rect(VALUE_CX - w / 2, mid - 17, w, 33, 17, blue);
        const char *value = option_name(row, current_option(row));
        if (strlen(value) <= 7) {
            shadow_text(VALUE_CX, y, 3, white, dark_blue, value);
        } else {
            char cut[12];
            snprintf(cut, sizeof(cut), "%s", value);
            shadow_text(VALUE_CX, y + 4, 2, white, dark_blue, cut);
        }
        arrow(VALUE_CX - w / 2 - 14, mid, 9, -1, sel ? ink : grey);
        arrow(VALUE_CX + w / 2 + 14, mid, 9, 1, sel ? ink : grey);
    }
    int bottom = row_top(ROWS);
    if (s_unlock || mod_pending()) text(28 + text_w(2, "Restart game to apply") / 2, bottom + 12, 2, red, "Restart game to apply");
    round_rect(PANEL_W - 128, bottom + 3, 108, 32, 16, ink);
    text(PANEL_W - 128 + 54, bottom + 8, 3, white, "Back");
    s_dirty = false;
}

const uint32_t *vita_menu_progress_panel(const char *title, const char *what, int percent, bool error, int *w,
                                         int *h) {
    const int ph = 220;
    const uint32_t white = rgba(255, 255, 255, 255), ink = rgba(34, 34, 40, 255), grey = rgba(150, 150, 158, 255);
    const uint32_t red = rgba(214, 38, 44, 255), dark_red = rgba(120, 14, 20, 255), blue = rgba(30, 92, 206, 255);
    const uint32_t dark_blue = rgba(14, 44, 110, 255);
    fill(0, 0, PANEL_W, ph, 0);
    round_rect(0, 0, PANEL_W, ph, 16, ink);
    round_rect(4, 4, PANEL_W - 8, ph - 8, 13, rgba(246, 246, 242, 255));
    round_rect(4, 4, PANEL_W - 8, 52, 13, red);
    fill(4, 40, PANEL_W - 8, 16, red);
    fill(4, 56, PANEL_W - 8, 4, ink);
    char head[24];
    snprintf(head, sizeof(head), "%s", title);
    shadow_text(24 + text_w(3, head) / 2, 19, 3, white, dark_red, head);
    if (error) {
        char line[2][40];
        int n = (int) strlen(what), cut = n;
        if (n > 36) {
            for (cut = 36; cut > 0 && what[cut] != ' '; cut--) {
            }
        }
        snprintf(line[0], sizeof(line[0]), "%.*s", cut, what);
        snprintf(line[1], sizeof(line[1]), "%s", cut < n ? what + cut + 1 : "");
        text(28 + text_w(2, line[0]) / 2, 92, 2, red, line[0]);
        text(28 + text_w(2, line[1]) / 2, 118, 2, red, line[1]);
        text(28 + text_w(2, "The game starts without it") / 2, 168, 2, grey, "The game starts without it");
    } else {
        text(28 + text_w(2, what) / 2, 84, 2, ink, what);
        int bw = PANEL_W - 56, fw = percent < 0 ? 0 : 34 + (bw - 34) * (percent > 100 ? 100 : percent) / 100;
        round_rect(28, 114, bw, 36, 18, dark_blue);
        round_rect(31, 117, bw - 6, 30, 15, rgba(214, 216, 224, 255));
        if (fw) {
            round_rect(28, 114, fw, 36, 18, dark_blue);
            round_rect(28, 114, fw, 33, 17, blue);
        }
        text(28 + text_w(2, "This happens once for each mod") / 2, 172, 2, grey, "This happens once for each mod");
    }
    s_dirty = true;
    *w = PANEL_W;
    *h = ph;
    return s_pixels;
}

static void save(void) {
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "msaa=%d\nlayout=%s\nscale3d=%d\nmod=%s\n", s_msaa, s_classic ? "classic" : "hud", s_scale3d,
            s_mod ? s_mods[s_mod - 1] : "");
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

static void set_scale3d(int scale) {
    if (scale == s_scale3d || scale < 1 || scale > 3) return;
    s_scale3d = scale;
    present_set_scale(scale);
    present_refresh_3d();
    s_dirty = true;
    save();
    port_log("3D resolution: %dx", scale);
}

static void unlock_all(void) {
    if (s_unlock) return;
    FILE *f = fopen(UNLOCK_PATH, "w");
    if (!f) return;
    fclose(f);
    s_unlock = true;
    s_dirty = true;
    port_log("Unlock everything: restart the game to apply");
}

static void set_mod(int i) {
    if (i == s_mod) return;
    s_mod = i;
    s_dirty = true;
    save();
    port_log(i ? "Mod: %s from the next start" : "Mod: none from the next start", i ? s_mods[i - 1] : "");
}

static void set_option(int row, int i) {
    i = (i + option_count(row)) % option_count(row);
    switch (row) {
    case ROW_MSAA: set_msaa(i * 2); break;
    case ROW_LAYOUT: set_classic(i == 1); break;
    case ROW_SCALE: set_scale3d(i + 1); break;
    case ROW_MOD: set_mod(i); break;
    default: unlock_all(); break;
    }
}

int port_unlock_all(void) {
    if (remove(UNLOCK_PATH) != 0) return 0;
    s_unlock = false;
    s_dirty = true;
    port_log("Unlock everything: done");
    return 1;
}

bool vita_menu_classic(void) { return s_classic; }

bool vita_menu_take_layout_change(void) {
    bool changed = s_layout_changed;
    s_layout_changed = false;
    return changed;
}

void vita_menu_mod_setting(char *out, int size) {
    out[0] = 0;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "mod=", 4) != 0) continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, (size_t) size, "%s", line + 4);
    }
    fclose(f);
}

void vita_menu_set_active_mod(const char *name) { snprintf(s_mod_active, sizeof(s_mod_active), "%s", name); }

void vita_menu_load(void) {
    char wanted[VITA_MOD_NAME];
    vita_menu_mod_setting(wanted, sizeof(wanted));
    s_mod_count = vita_mods_list(s_mods, 32);
    s_mod = 0;
    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i], s_mod_active) == 0) s_mod = i + 1;
    }
    FILE *f = fopen(SETTINGS_PATH, "r");
    int v = 0;
    if (f) {
        char line[64], word[32];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "msaa=%d", &v) == 1) continue;
            if (sscanf(line, "layout=%31s", word) == 1) s_classic = strcmp(word, "classic") == 0;
            int n;
            if (sscanf(line, "scale3d=%d", &n) == 1 && n >= 1 && n <= 3) s_scale3d = n;
        }
        fclose(f);
    }
    s_msaa = v == 2 || v == 4 ? v : 0;
    gl3d_set_msaa(s_msaa);
    present_set_scale(s_scale3d);
    if (s_msaa) port_log("Anti-aliasing: %dx", s_msaa);
    if (s_scale3d != 2) port_log("3D resolution: %dx", s_scale3d);
    FILE *u = fopen(UNLOCK_PATH, "r");
    s_unlock = u != NULL;
    if (u) fclose(u);
    if (s_unlock) port_log("Unlock everything: applying");
    if (strcmp(wanted, s_mod_active) != 0) save();
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
    if (pressed & SCE_CTRL_UP) {
        s_row = (s_row + ROWS - 1) % ROWS;
        s_dirty = true;
    }
    if (pressed & SCE_CTRL_DOWN) {
        s_row = (s_row + 1) % ROWS;
        s_dirty = true;
    }
    {
        int cur = current_option(s_row);
        if (s_row == ROW_UNLOCK) {
            if (pressed & SCE_CTRL_CROSS) unlock_all();
        } else {
            if (pressed & SCE_CTRL_LEFT) set_option(s_row, cur - 1);
            if (pressed & SCE_CTRL_RIGHT) set_option(s_row, cur + 1);
        }
    }
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START) || (pressed & SCE_CTRL_CROSS && s_row != ROW_UNLOCK)) {
        close_menu();
        return true;
    }
    if (tap) {
        int r[4];
        panel_rect(r);
        int px = x - r[0], py = y - r[1];
        if (!inside(px, py, 0, 0, PANEL_W, PANEL_H) ||
            inside(px, py, BACK_X, row_top(ROWS) + 6, PANEL_W - BACK_X, PANEL_H - row_top(ROWS) - 6)) {
            close_menu();
            return true;
        }
        for (int row = 0; row < ROWS; row++) {
            if (inside(px, py, 0, row_top(row), PANEL_W, ROW_H)) {
                s_row = row;
                s_dirty = true;
                bool back = px >= VALUE_X && px < VALUE_CX;
                set_option(row, current_option(row) + (back ? -1 : 1));
            }
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
