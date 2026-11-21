#include "port.h"
#include "vita.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <string.h>
#include <vita2d_vgl.h>
#include <vitaGL.h>

#define LANGUAGE_FILE VITA_DATA_DIR "/language.txt"

static const char *const s_names[5] = {"English", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Espa\xc3\xb1ol"};
static const char *const s_titles[5] = {"Language", "Langue", "Sprache", "Lingua", "Idioma"};
static const char *const s_hints[5][2] = {{"Press", "to start"},
                                          {"Appuyez sur", "pour commencer"},
                                          {"", "dr\xc3\xbc" "cken zum Starten"},
                                          {"Premi", "per iniziare"},
                                          {"Pulsa", "para empezar"}};

#define RGBA(r, g, b, a) ((unsigned) (r) | (unsigned) (g) << 8 | (unsigned) (b) << 16 | (unsigned) (a) << 24)

static int load_saved(void) {
    char buf[8] = "";
    SceUID fd = sceIoOpen(LANGUAGE_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    int language = n > 0 ? buf[0] - '0' : 0;
    return language >= 1 && language <= 5 ? language : 0;
}

static void save(int language) {
    SceUID fd = sceIoOpen(LANGUAGE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    char c = (char) ('0' + language);
    sceIoWrite(fd, &c, 1);
    sceIoClose(fd);
}

#define BAR_W 520
#define BAR_H 58
#define BAR_GAP 14
#define BAR_X ((VITA_SCREEN_W - BAR_W) / 2)
#define BAR_Y0 132

static int bar_at(int x, int y) {
    for (int i = 0; i < 5; i++) {
        int by = BAR_Y0 + i * (BAR_H + BAR_GAP);
        if (x >= BAR_X && x < BAR_X + BAR_W && y >= by && y < by + BAR_H) return i;
    }
    return -1;
}

static void centred(vita2d_pvf *font, int y, unsigned color, float scale, const char *text) {
    int w = vita2d_pvf_text_width(font, scale, text), h = vita2d_pvf_text_height(font, scale, "Ag");
    vita2d_pvf_draw_text(font, (VITA_SCREEN_W - w) / 2, y + h / 2, color, scale, text);
}

static void cross_button(float cx, float cy, float r) {
    vita2d_draw_fill_circle(cx, cy, r, RGBA(40, 40, 60, 255));
    float d = r * 0.5f;
    for (float o = -1.5f; o <= 1.5f; o += 0.5f) {
        vita2d_draw_line(cx - d + o, cy - d, cx + d + o, cy + d, RGBA(140, 180, 255, 255));
        vita2d_draw_line(cx + d + o, cy - d, cx - d + o, cy + d, RGBA(140, 180, 255, 255));
    }
}

static void hint(vita2d_pvf *font, int y, unsigned color, float scale, const char *before, const char *after) {
    int h = vita2d_pvf_text_height(font, scale, "Ag");
    float r = h * 0.8f, gap = h * 0.4f;
    int wb = before[0] ? vita2d_pvf_text_width(font, scale, before) : 0;
    int wa = vita2d_pvf_text_width(font, scale, after);
    float total = wb + (before[0] ? gap : 0) + 2 * r + gap + wa;
    float x = (VITA_SCREEN_W - total) / 2;
    if (before[0]) {
        vita2d_pvf_draw_text(font, (int) x, y + h / 2, color, scale, before);
        x += wb + gap;
    }
    cross_button(x + r, (float) y, r);
    x += 2 * r + gap;
    vita2d_pvf_draw_text(font, (int) x, y + h / 2, color, scale, after);
}

static void draw(vita2d_pvf *font, int selected, uint32_t frame) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    for (int y = 0; y < VITA_SCREEN_H; y += 8) {
        int t = y * 255 / VITA_SCREEN_H;
        vita2d_draw_rectangle(0, (float) y, VITA_SCREEN_W, 8, RGBA(150 - t / 4, 205 - t / 6, 245, 255));
    }
    for (int y = 0; y < VITA_SCREEN_H; y += 48) {
        for (int x = ((y / 48) & 1) * 48; x < VITA_SCREEN_W; x += 96) {
            vita2d_draw_rectangle((float) x, (float) y, 48, 48, RGBA(255, 255, 255, 22));
        }
    }
    vita2d_draw_rectangle(0, 40, VITA_SCREEN_W, 64, RGBA(30, 70, 170, 230));
    vita2d_draw_rectangle(0, 104, VITA_SCREEN_W, 4, RGBA(255, 255, 255, 200));
    centred(font, 72, RGBA(255, 255, 255, 255), 1.6f, s_titles[selected]);
    for (int i = 0; i < 5; i++) {
        int by = BAR_Y0 + i * (BAR_H + BAR_GAP);
        bool lit = i == selected;
        int pulse = lit ? (int) ((frame / 2) % 40) : 0;
        if (pulse > 20) pulse = 40 - pulse;
        vita2d_draw_rectangle(BAR_X + 4, (float) by + 5, BAR_W, BAR_H, RGBA(0, 30, 90, 70));
        vita2d_draw_rectangle(BAR_X, (float) by, BAR_W, BAR_H,
                              lit ? RGBA(255, 200 + pulse, 40, 255) : RGBA(250, 252, 255, 235));
        vita2d_draw_rectangle(BAR_X, (float) by, 10, BAR_H, lit ? RGBA(230, 60, 30, 255) : RGBA(60, 120, 210, 255));
        centred(font, by + BAR_H / 2, lit ? RGBA(40, 30, 10, 255) : RGBA(30, 60, 130, 255), 1.4f, s_names[i]);
    }
    hint(font, VITA_SCREEN_H - 34, RGBA(255, 255, 255, 255), 1.0f, s_hints[selected][0], s_hints[selected][1]);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int vita_language_screen(int system_language) {
    int saved = load_saved();
    int selected = (saved ? saved : (system_language >= 1 && system_language <= 5 ? system_language : 1)) - 1;
    vita2d_init();
    vita2d_set_clear_color(RGBA(150, 205, 245, 255));
    vita2d_pvf *font = vita2d_load_default_pvf();
    if (!font) {
        port_log("Language: no system font, using choice %d", selected + 1);
        vita2d_fini();
        return selected + 1;
    }
    uint32_t prev = ~0u, frame = 0, repeat = 0;
    bool touching = false;
    int chosen = -1, confirm_frames = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t b = pad.buttons;
        if (pad.ly < 128 - 64) b |= SCE_CTRL_UP;
        if (pad.ly > 128 + 64) b |= SCE_CTRL_DOWN;
        uint32_t pressed = b & ~prev;
        if (b & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
            if (++repeat > 20 && repeat % 6 == 0) pressed |= b & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        } else {
            repeat = 0;
        }
        prev = b;
        if (chosen < 0) {
            if (pressed & SCE_CTRL_UP) selected = (selected + 4) % 5;
            if (pressed & SCE_CTRL_DOWN) selected = (selected + 1) % 5;
            if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START)) chosen = selected;
            SceTouchData touch;
            memset(&touch, 0, sizeof(touch));
            sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
            if (touch.reportNum > 0) {
                int bar = bar_at(touch.report[0].x * VITA_SCREEN_W / 1920, touch.report[0].y * VITA_SCREEN_H / 1088);
                if (bar >= 0) selected = bar;
                touching = true;
            } else if (touching) {
                touching = false;
                chosen = selected;
            }
        }
        draw(font, selected, frame++);
        if (chosen >= 0 && ++confirm_frames > 8) break;
    }
    vita2d_fini();
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    save(chosen + 1);
    port_log("Language: %s", s_names[chosen]);
    return chosen + 1;
}
