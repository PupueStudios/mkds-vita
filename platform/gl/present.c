// Shared presentation on OpenGL ES 3 (Android and the host test build): turns frame snapshots into the two DS
// screens and draws them into rectangles of the current framebuffer.
//
// For each new frame snapshot: the 3D layer is rasterised on the GPU at a resolution that matches the screen
// (gl3d.c), the 2D engines are drawn in software at the native 256x192 and split into what is above and below the
// 3D layer, and a shader composites them with the colour effects and the master brightness. Frames with a display
// capture (dual-screen 3D, menu blur) are drawn entirely in software, because the capture writes into VRAM that the
// next frames display (the displayed frame itself still uses the GPU path when it has 3D).
//
// Dual-screen 3D (the race intros of the Grand Prix): the game draws its 3D on the two screens in turn, swapping the
// screens every frame (POWCNT bit 15) and showing on the other one the capture of the frame before. Here each
// screen is drawn from its own last frame instead (the GPU path, high resolution, widescreen on the top screen), and
// its 3D is interpolated between that screen's frames, two VBlanks apart.
#include "port.h"
#include "frame.h"
#include "gpu2d.h"
#include "gl3d.h"
#include "present.h"
#include "compose_shader.h"

#ifdef __vita__
#include <vitaGL.h>
#else
#include <GLES3/gl3.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __vita__
static const char *VS = "void main(float2 pos, float2 uv, float2 out v_uv : TEXCOORD0, float4 out gl_Position : POSITION) {\n"
                        "  v_uv = uv; gl_Position = float4(pos, 0.0, 1.0);\n"
                        "}\n";
static const char *FS_PLAIN = "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D tex : TEXUNIT0) : COLOR {\n"
                              "  return float4(tex2D(tex, v_uv).rgb, 1.0);\n"
                              "}\n";
static const char *FS_HUD = "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D tex : TEXUNIT0, uniform float opacity) : COLOR {\n"
                            "  float4 t = tex2D(tex, v_uv);\n"
                            "  return float4(t.rgb, t.a * opacity);\n"
                            "}\n";
static const char *FS_COMPOSE =
    "float4 main(float2 v_uv : TEXCOORD0, uniform sampler2D above : TEXUNIT0, uniform sampler2D below : TEXUNIT1,\n"
    "            uniform sampler2D layer3d : TEXUNIT2, uniform float bright_mode, uniform float bright,\n"
    "            uniform float k2, uniform float k3, uniform float3 backdrop) : COLOR {\n"
    "  float2 uv2 = float2((v_uv.x - 0.5) * k2 + 0.5, v_uv.y);\n"
    "  float in2 = step(0.0, uv2.x) * step(uv2.x, 1.0);\n"
    "  float4 up = tex2D(above, uv2) * in2;\n"
    "  float4 dn = lerp(float4(backdrop, 1.0), tex2D(below, uv2), in2);\n"
    "  float u3 = (v_uv.x - 0.5) * k3 + 0.5;\n"
    "  float4 d3 = tex2D(layer3d, float2(u3, 1.0 - v_uv.y)) * (step(0.0, u3) * step(u3, 1.0));\n"
    "  float w = d3.a <= 0.0 ? 0.0 : (dn.a <= 0.0 ? 1.0 : (d3.a < 0.99 ? d3.a : dn.a));\n"
    "  float3 c = lerp(dn.rgb, d3.rgb, w);\n"
    "  c = lerp(c, up.rgb, up.a);\n"
    "  float bu = bright_mode == 1.0 ? bright : 0.0, bd = bright_mode == 2.0 ? bright : 0.0;\n"
    "  c += (1.0 - c) * bu - c * bd;\n"
    "  return float4(c, 1.0);\n"
    "}\n";
#define SET_BRIGHT_MODE(loc, v) glUniform1f((loc), (float) (v))
#else
#define SET_BRIGHT_MODE(loc, v) glUniform1i((loc), (v))
static const char *VS = "#version 300 es\n"
                        "layout(location = 0) in vec2 pos;\n"
                        "layout(location = 1) in vec2 uv;\n"
                        "out vec2 v_uv;\n"
                        "void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
// A plain 2D screen.
static const char *FS_PLAIN = "#version 300 es\n"
                              "precision mediump float;\n"
                              "uniform sampler2D tex;\n"
                              "in vec2 v_uv;\n"
                              "out vec4 color;\n"
                              "void main() { color = vec4(texture(tex, v_uv).rgb, 1.0); }\n";
static const char *FS_HUD = "#version 300 es\n"
                            "precision mediump float;\n"
                            "uniform sampler2D tex;\n"
                            "uniform float opacity;\n"
                            "in vec2 v_uv;\n"
                            "out vec4 color;\n"
                            "void main() { vec4 t = texture(tex, v_uv); color = vec4(t.rgb, t.a * opacity); }\n";
// Engine A with the 3D layer: below / 3D / above, then the master brightness.
static const char *FS_COMPOSE =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D above;\n"
    "uniform sampler2D below;\n"
    "uniform sampler2D layer3d;\n"
    "uniform int bright_mode;\n"
    "uniform float bright;\n"
    // The screen is k2 times as wide as the 2D layers and k3 times as wide as the 3D layer, both centred.
    "uniform float k2;\n"
    "uniform float k3;\n"
    "uniform vec3 backdrop;\n"
    "in vec2 v_uv;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  vec2 uv2 = vec2((v_uv.x - 0.5) * k2 + 0.5, v_uv.y);\n"
    "  bool inside = uv2.x >= 0.0 && uv2.x <= 1.0;\n"
    "  vec4 up = inside ? texture(above, uv2) : vec4(0.0);\n"
    "  vec4 dn = inside ? texture(below, uv2) : vec4(backdrop, 1.0);\n"
    "  float u3 = (v_uv.x - 0.5) * k3 + 0.5;\n"
    "  vec4 d3 = u3 >= 0.0 && u3 <= 1.0 ? texture(layer3d, vec2(u3, 1.0 - v_uv.y)) : vec4(0.0);\n"
    "  vec3 c;\n"
    "  if (d3.a <= 0.0) c = dn.rgb;\n"
    "  else if (dn.a <= 0.0) c = d3.rgb;\n"
    "  else if (d3.a < 0.99) c = mix(dn.rgb, d3.rgb, d3.a);\n"
    "  else c = mix(dn.rgb, d3.rgb, dn.a);\n"
    "  c = mix(c, up.rgb, up.a);\n"
    "  if (bright_mode == 1) c += (1.0 - c) * bright;\n"
    "  else if (bright_mode == 2) c -= c * bright;\n"
    "  color = vec4(c, 1.0);\n"
    "}\n";
#endif

static GLuint compile(GLenum type, const char *src) {
#ifdef __vita__
    type = type == GL_VERTEX_SHADER ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT;
#endif
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
#ifdef __vita__
    extern GLboolean is_shark_online;
    if (!is_shark_online) port_fatal("%s: the shader compiler (libshacccg.suprx) could not be loaded", "shader");
#endif
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        port_log("Graphics: shader error: %s", log);
    }
    return s;
}

static GLuint program(const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "uv");
    glLinkProgram(p);
    return p;
}

static GLuint make_texture(void) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// Uploads a 192-line picture `width` wide (widescreen 2D), reallocating the texture when the width changes.
static void upload(GLuint tex, const uint32_t *pixels, int width, bool unchanged) {
    static GLuint names[8];
    static int widths[8];
    int i = 0;
    while (i < 7 && names[i] && names[i] != tex) i++;
    names[i] = tex;
    if (unchanged && widths[i] == width) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    if (widths[i] != width) {
        widths[i] = width;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
}

static void draw_quad(int x, int y, int w, int h, int surface_w, int surface_h) {
    float x0 = (float) x / surface_w * 2 - 1, x1 = (float) (x + w) / surface_w * 2 - 1;
    float y0 = 1 - (float) y / surface_h * 2, y1 = 1 - (float) (y + h) / surface_h * 2;
    const float v[] = {x0, y0, 0, 0, x1, y0, 1, 0, x0, y1, 0, 1, x1, y1, 1, 1};
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); // client-side arrays
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

typedef struct Presented {
    bool layers;
    Gpu2dCompose comp[2];
    GLuint lay_tex[2][3];
    bool software; // both screens complete in s_tex_plain[0..1]
    bool a_on_top;
    bool race;       // a race frame
    GLuint hud_tex;
    int hud_flags;
    int width_top, width_bottom; // of the 2D pictures of the top and bottom screens (top: widescreen)
    uint16_t bright; // engine A MASTER_BRIGHT
    float backdrop[3];
    int scale;
    const FrameSnapshot *snap; // for the textures; valid until the next snapshot is acquired
} Presented;

static GLuint s_prog_plain, s_prog_compose, s_tex_plain[2], s_tex_above, s_tex_below, s_alt_b;
static GLuint s_prog_layers;
#define HUD_ROWS GPU2D_HUD_ROWS
static GLuint s_prog_hud, s_tex_hud;
static GLint s_u_hud_opacity;
static bool s_layers_ok;
static GLint s_ul_width, s_ul_k3, s_ul_bg0_3d, s_ul_prio, s_ul_enabled, s_ul_first03, s_ul_first45, s_ul_second03,
    s_ul_second45, s_ul_effect, s_ul_backdrop, s_ul_bright;

// Dual-screen 3D: engine A's picture of each screen (index 0 = top), from the last frame it was on that screen.
typedef struct Held {
    bool valid;
    uint64_t number;    // of its last frame
    int width;          // of its 2D (widescreen on the top screen)
    uint16_t bright;
    float backdrop[3];
    GLuint above, below;
    int cur;            // its last 3D list in s_alt3d[screen][cur], the one before in [cur ^ 1]
    bool have_prev;
} Held;
static bool s_alt; // dual-screen 3D
static Held s_held[2];
static Gx3dFrame s_alt3d[2][2];
static GLint s_u_bright_mode, s_u_bright, s_u_k2, s_u_k3, s_u_backdrop;
static Presented s_shown = {.software = true, .a_on_top = true, .width_top = 256, .width_bottom = 256, .scale = 1};
static float s_wide = 1.0f; // the display's aspect ratio / (4/3)
// The 3D lists of the last two frames (interpolation), only the used parts copied.
#define LIST3D(f) ((f)->list3d ? (f)->list3d : &(f)->frame3d)
static Gx3dFrame s_frames3d[2];
static int s_cur3d;
static bool s_have_prev;
static uint64_t s_last_number;

static void copy_list(Gx3dFrame *d, const Gx3dFrame *g) { gx3d_copy_frame(d, g); }
static const Gx3dFrame *s_cur_list;
static const bool s_interpolate = false;
static void take_list(const FrameSnapshot *f) {
    const Gx3dFrame *l = LIST3D(f);
    if (l->gpu_vertices || !s_interpolate) {
        s_cur_list = l;
        s_have_prev = false;
        return;
    }
    s_cur3d ^= 1;
    copy_list(&s_frames3d[s_cur3d], l);
    s_cur_list = &s_frames3d[s_cur3d];
}

void present_set_wide(float aspect) { s_wide = aspect > 4.0f / 3.0f ? aspect / (4.0f / 3.0f) : 1.0f; }
bool present_is_race(void) { return s_shown.race; }
uint64_t present_shown_time_ns(void) { return s_shown.snap ? s_shown.snap->time_ns : 0; }

void present_init(void) {
    s_prog_plain = program(FS_PLAIN);
    s_prog_hud = program(FS_HUD);
    s_u_hud_opacity = glGetUniformLocation(s_prog_hud, "opacity");
    s_prog_compose = program(FS_COMPOSE);
    glUseProgram(s_prog_compose);
    glUniform1i(glGetUniformLocation(s_prog_compose, "above"), 0);
    glUniform1i(glGetUniformLocation(s_prog_compose, "below"), 1);
    glUniform1i(glGetUniformLocation(s_prog_compose, "layer3d"), 2);
    s_u_bright_mode = glGetUniformLocation(s_prog_compose, "bright_mode");
    s_u_bright = glGetUniformLocation(s_prog_compose, "bright");
    s_u_k2 = glGetUniformLocation(s_prog_compose, "k2");
    s_u_k3 = glGetUniformLocation(s_prog_compose, "k3");
    s_u_backdrop = glGetUniformLocation(s_prog_compose, "backdrop");
    {
        GLuint p = glCreateProgram();
        glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
        bool ok = true;
        GLuint fs = compile(GL_FRAGMENT_SHADER, FS_LAYERS);
        GLint st = 0;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &st);
        ok = st != 0;
        glAttachShader(p, fs);
        glBindAttribLocation(p, 0, "pos");
        glBindAttribLocation(p, 1, "uv");
        glLinkProgram(p);
        GLint linked = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &linked);
        (void) ok;
        (void) linked;
        s_layers_ok = false;
        if (s_layers_ok) {
            s_prog_layers = p;
            glUseProgram(p);
            glUniform1i(glGetUniformLocation(p, "l01"), 0);
            glUniform1i(glGetUniformLocation(p, "l23"), 1);
            glUniform1i(glGetUniformLocation(p, "lobj"), 2);
            glUniform1i(glGetUniformLocation(p, "layer3d"), 3);
            s_ul_width = glGetUniformLocation(p, "width");
            s_ul_k3 = glGetUniformLocation(p, "k3");
            s_ul_bg0_3d = glGetUniformLocation(p, "bg0_3d");
            s_ul_prio = glGetUniformLocation(p, "prio");
            s_ul_enabled = glGetUniformLocation(p, "enabled");
            s_ul_first03 = glGetUniformLocation(p, "first03");
            s_ul_first45 = glGetUniformLocation(p, "first45");
            s_ul_second03 = glGetUniformLocation(p, "second03");
            s_ul_second45 = glGetUniformLocation(p, "second45");
            s_ul_effect = glGetUniformLocation(p, "effect");
            s_ul_backdrop = glGetUniformLocation(p, "backdrop");
            s_ul_bright = glGetUniformLocation(p, "bright");
        }
    }
    s_tex_plain[0] = make_texture();
    s_tex_plain[1] = make_texture();
    s_tex_above = make_texture();
    s_tex_below = make_texture();
    s_alt_b = make_texture();
    s_tex_hud = make_texture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, HUD_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    for (int i = 0; i < 2; i++) {
        s_held[i].above = make_texture();
        s_held[i].below = make_texture();
    }
    gl3d_init();
}

PresentStats g_present_stats;
uint64_t (*g_present_clock_us)(void);
const char *volatile g_present_stage = "idle";
void (*g_present_test_hook)(const FrameSnapshot *f);
static inline uint64_t present_clock_us(void) { return g_present_clock_us ? g_present_clock_us() : 0; }

enum { STAGED_ALT, STAGED_SOFTWARE, STAGED_SPLIT, STAGED_LAYERS };
typedef struct Staged {
    const FrameSnapshot *f;
    int scale, kind;
    bool alt_enter;
    bool a_on_top;
    int top_w, width_a, width_b;
    bool unchanged;
    bool alt, has_3d, capture;
    bool layer3d;
    int set;
    bool skip_a, skip_b;
    int hud_flags;
    Gpu2dCompose comp[2];
} Staged;
static int s_area_top = 1, s_area_bottom = 1;
void present_set_screen_areas(int top, int bottom) {
    s_area_top = top;
    s_area_bottom = bottom;
}

static volatile bool s_hud_on;
static PresentHudLayout s_hud_layout;
static uint32_t s_hud_master[256 * HUD_ROWS];
enum { HUD_LIST, HUD_TIME, HUD_SIDE, HUD_MAP, HUD_PIECES };
static const int s_hud_src[2][HUD_PIECES][5] = {
    {{0, 0, 144, 192, 0}, {120, 0, 136, 24, 0}, {120, 24, 136, 168, 24}, {0, 24, 256, 168, 24}},
    {{0, GPU2D_HUD_ROW_LIST, 256, 192, 0},
     {120, GPU2D_HUD_ROW_TIME, 136, 24, 0},
     {120, GPU2D_HUD_ROW_MAP, 136, 192, 0},
     {0, GPU2D_HUD_ROW_MAP, 256, 192, 0}}};
static const int s_hud_src_whole[5] = {0, GPU2D_HUD_ROW_MAP, 256, 192, 0},
                 s_hud_src_pause[5] = {0, GPU2D_HUD_ROW_PAUSE, 256, 192, 0};

static const int *hud_src(int flags, int piece) { return s_hud_src[(flags & GPU2D_HUD_LIST) ? 1 : 0][piece]; }

static bool s_hud_draw;

void present_set_hud(const PresentHudLayout *layout, bool draw) {
    if (layout) s_hud_layout = *layout;
    else memset(&s_hud_layout, 0, sizeof(s_hud_layout));
    s_hud_on = layout != NULL;
    s_hud_draw = layout != NULL && draw;
}

int present_hud_flags(void) { return s_hud_on && s_shown.race ? s_shown.hud_flags : 0; }

bool present_hud_usable(void) {
    int flags = present_hud_flags();
    return (flags & GPU2D_HUD_SHOWN) && !(flags & GPU2D_HUD_PICTURE);
}

static bool hud_hit(const int r[4], const int src[5], int x, int y, int *dx, int *dy) {
    if (r[2] <= 0 || r[3] <= 0 || x < r[0] || x >= r[0] + r[2] || y < r[1] || y >= r[1] + r[3]) return false;
    *dx = src[0] + (x - r[0]) * src[2] / r[2];
    *dy = src[4] + (y - r[1]) * src[3] / r[3];
    return true;
}

bool present_hud_touch(int x, int y, int *dx, int *dy) {
    int flags = present_hud_flags();
    if (!s_hud_draw || !(flags & GPU2D_HUD_SHOWN)) return false;
    if (flags & GPU2D_HUD_PAUSE) return hud_hit(s_hud_layout.pause, s_hud_src_pause, x, y, dx, dy);
    if (flags & GPU2D_HUD_PICTURE) return hud_hit(s_hud_layout.whole, s_hud_src_whole, x, y, dx, dy);
    const int *list = hud_src(flags, HUD_LIST);
    int list_rect[4] = {s_hud_layout.list[0], s_hud_layout.list[1], s_hud_layout.list[2] * 144 / list[2],
                        s_hud_layout.list[3]};
    int list_src[5] = {list[0], list[1], 144, list[3], list[4]};
    return hud_hit(list_rect, list_src, x, y, dx, dy) ||
           hud_hit(s_hud_layout.time, hud_src(flags, HUD_TIME), x, y, dx, dy) ||
           (flags & GPU2D_HUD_MAP ? hud_hit(s_hud_layout.map, hud_src(flags, HUD_MAP), x, y, dx, dy)
                                  : hud_hit(s_hud_layout.side, hud_src(flags, HUD_SIDE), x, y, dx, dy));
}

typedef struct BufferSet {
    uint32_t a[GPU2D_MAX_W * 192], b[GPU2D_MAX_W * 192], below[GPU2D_MAX_W * 192];
    uint32_t hud[256 * HUD_ROWS];
} BufferSet;
#define SETS 4
static BufferSet s_sets[SETS];
#define DIRECT_SETS 10
typedef struct DirectSet {
    GLuint tex[3][2];
    uint32_t *ptr[3][2];
    GLuint hud_tex;
    uint32_t *hud_ptr;
    int kind, a_on_top, width_a, width_b, top_w;
    uint32_t ver[2][192];
} DirectSet;
static DirectSet s_dsets[DIRECT_SETS];
static bool s_direct;
static int s_set_count = SETS;
static BufferSet s_master;
static uint32_t s_lay_master[2][3][GPU2D_MAX_W * 192];
static uint32_t s_layer3d[256 * 192];
static struct {
    bool alt;
    int alt_misses;
    bool prev_a_on_top;
    uint64_t last_number;
} s_cpu;

static int top_width(void) {
#ifdef __vita__
    int top_w = (int) (192 * 4.0f / 3.0f * s_wide + 0.5f) & ~7;
#else
    int top_w = (int) (192 * 4.0f / 3.0f * s_wide + 0.5f) & ~1;
#endif
    if (top_w < 256) top_w = 256;
    if (top_w > GPU2D_MAX_W) top_w = GPU2D_MAX_W;
    return top_w;
}

static void plan(const FrameSnapshot *f, int scale, Staged *st) {
    memset(st, 0, sizeof(*st));
    st->f = f;
    st->scale = scale;
    st->a_on_top = gpu2d_engine_a_on_top(f);
    // Widescreen: the top screen's 2D is drawn as wide as the display (DS columns added on both sides).
    int top_w = top_width();
    if (top_w > GPU2D_MAX_W) top_w = GPU2D_MAX_W;
    st->top_w = top_w;
    st->width_a = st->a_on_top ? top_w : 256;
    st->width_b = st->a_on_top ? 256 : top_w;
    bool capture = f->io[0x67] & 0x80;
    uint32_t dispcnt_a = (uint32_t) f->io[0] | f->io[1] << 8 | f->io[2] << 16 | (uint32_t) f->io[3] << 24;
    st->has_3d = (dispcnt_a & 8) && ((dispcnt_a >> 16) & 3) == 1;
    st->capture = capture;

    uint32_t capcnt = (uint32_t) f->io[0x64] | f->io[0x65] << 8 | f->io[0x66] << 16 | (uint32_t) f->io[0x67] << 24;
    bool whole_capture = capture && st->has_3d && ((capcnt >> 20) & 3) == 3 && ((capcnt >> 29) & 3) == 0;
    bool alternating = f->swap_streak > 0;
    // The capture bit can be seen already cleared (the game process clears it when the app has drawn the capture,
    // while the next frame is being taken): the mode ends only when the screens stop swapping or a few frames in a
    // row have no capture.
    if (s_cpu.alt && st->has_3d && !whole_capture && (alternating || ++s_cpu.alt_misses < 4)) {
        whole_capture = true;
    } else if (whole_capture) {
        s_cpu.alt_misses = 0;
    }
    if (!whole_capture) {
        s_cpu.alt = false;
        s_cpu.alt_misses = 0;
    } else if (!s_cpu.alt && f->swap_streak >= 2) {
        s_cpu.alt = true;
        st->alt_enter = true;
    }
    s_cpu.prev_a_on_top = st->a_on_top;
    s_cpu.last_number = f->number;
    st->alt = s_cpu.alt;

    if (!st->alt) {
        static uint32_t s_tick;
        s_tick++;
        int area_a = st->a_on_top ? s_area_top : s_area_bottom, area_b = st->a_on_top ? s_area_bottom : s_area_top;
        st->skip_a = area_a == 0 || (area_a * 4 < area_b && (s_tick & 1));
        st->skip_b = area_b == 0 || (area_b * 4 < area_a && (s_tick & 1));
    }

    if (!st->alt && capture) {
        uint32_t source = (capcnt >> 29) & 3;
        st->layer3d = source != 1 && ((capcnt & (1u << 24)) || ((dispcnt_a & 8) && (dispcnt_a & 0x100)));

    }
}

static void plan_gl(const Staged *st) {
    if (!st->layer3d) return;
    uint64_t t0 = present_clock_us();
    gl3d_capture_layer(LIST3D(st->f), st->f, s_layer3d);
    g_present_stats.gpu3d_us += present_clock_us() - t0;
}

static void stage_cpu(Staged *st) {
    const FrameSnapshot *f = st->f;
    uint32_t *s_buf_a = s_master.a, *s_buf_b = s_master.b, *s_buf_below = s_master.below;
    uint64_t t_start = present_clock_us();
    int margin = (st->top_w - 256) / 2;
    int margin_a = st->a_on_top ? margin : 0, margin_b = st->a_on_top ? 0 : margin;
    static uint32_t s_alt_streak;
    s_alt_streak = st->alt && !st->alt_enter ? s_alt_streak + 1 : 0;
    if (st->alt) {
        // No capture needed: the other screen is drawn from its own frame.
        st->kind = STAGED_ALT;
        g_present_stage = "2D (dual-screen 3D)";
        gpu2d_skip_engines(false, s_alt_streak > 3);
        gpu2d_render_split(f, s_buf_a, s_buf_below, s_buf_b, margin_a, margin_b);
        if (g_capture_done) g_capture_done(); // the game waits for the capture
    } else {
        if (st->capture) {
            uint64_t t1 = present_clock_us();
            g_present_stage = "2D (capture)";
            gpu2d_render(f, st->layer3d ? s_layer3d : NULL, s_buf_a, s_buf_b, margin);
            g_present_stats.captures++;
            g_present_stats.capture_2d_us += present_clock_us() - t1;
        }
        g_present_stage = "2D";
        bool done = false;
        if (s_direct && s_layers_ok && !st->capture) {
            uint32_t *const top[3] = {s_lay_master[0][0], s_lay_master[0][1], s_lay_master[0][2]};
            uint32_t *const bottom[3] = {s_lay_master[1][0], s_lay_master[1][1], s_lay_master[1][2]};
            gpu2d_skip_engines(st->skip_a, st->skip_b);
            if (gpu2d_render_layers(f, top, bottom, margin, st->comp)) {
                st->kind = STAGED_LAYERS;
                done = true;
            } else {
                gpu2d_skip_engines(false, false);
            }
        }
        if (done) {
        } else if (!st->has_3d) {
            st->kind = STAGED_SOFTWARE;
            if (!st->capture) {
                gpu2d_skip_engines(st->skip_a, st->skip_b);
                gpu2d_render(f, NULL, s_buf_a, s_buf_b, margin); // buf_a: top screen, buf_b: bottom
            }
        } else {
            st->kind = STAGED_SPLIT;
            gpu2d_skip_engines(st->skip_a, st->skip_b);
            gpu2d_render_split(f, s_buf_a, s_buf_below, s_buf_b, margin_a, margin_b);
        }
    }
    st->unchanged = g_gpu2d_unchanged;
    st->hud_flags = 0;
    if (s_hud_on && f->scene == 2 && f->hud_ok && !st->alt) st->hud_flags = gpu2d_render_hud(f, s_hud_master);
    size_t hud_bytes = (size_t) (st->hud_flags & GPU2D_HUD_PAUSE ? GPU2D_HUD_ROWS : GPU2D_HUD_ROW_PAUSE) * 256 * 4;
    if (s_direct) {
        DirectSet *d = &s_dsets[st->set];
        if (st->hud_flags && d->hud_ptr) memcpy(d->hud_ptr, s_hud_master, hud_bytes);
        int wa = st->width_a != 256, wb = st->width_b != 256, wt = st->top_w != 256;
        const uint32_t *ver[2];
        gpu2d_line_versions(&ver[0], &ver[1]);
        bool same = d->kind == st->kind && d->a_on_top == st->a_on_top && d->width_a == st->width_a &&
                    d->width_b == st->width_b && d->top_w == st->top_w;
        if (st->kind == STAGED_LAYERS) {
            for (int i = 0; i < 3; i++) {
                memcpy(d->ptr[i][1], s_lay_master[0][i], (size_t) st->top_w * 192 * 4);
                memcpy(d->ptr[i][0], s_lay_master[1][i], 256 * 192 * 4);
            }
            same = false;
        } else {
            uint32_t *dst_a, *dst_below = NULL, *dst_b;
            const uint32_t *src_a = s_master.a, *src_b = s_master.b;
            int w_a, w_b, eng_a, eng_b;
            if (st->kind == STAGED_SOFTWARE) {
                dst_a = d->ptr[0][wt];
                dst_b = d->ptr[2][0];
                w_a = st->top_w;
                w_b = 256;
                eng_a = st->a_on_top ? 0 : 1;
                eng_b = 1 - eng_a;
            } else {
                dst_a = d->ptr[0][wa];
                dst_below = d->ptr[1][wa];
                dst_b = d->ptr[2][wb];
                w_a = st->width_a;
                w_b = st->width_b;
                eng_a = 0;
                eng_b = 1;
            }
            for (int y = 0; y < 192; y++) {
                if (!same || d->ver[eng_a][y] != ver[eng_a][y]) {
                    memcpy(dst_a + y * w_a, src_a + y * w_a, (size_t) w_a * 4);
                    if (dst_below) memcpy(dst_below + y * w_a, s_master.below + y * w_a, (size_t) w_a * 4);
                }
                if (!same || d->ver[eng_b][y] != ver[eng_b][y]) {
                    memcpy(dst_b + y * w_b, src_b + y * w_b, (size_t) w_b * 4);
                }
            }
            memcpy(d->ver, ver[0], sizeof(d->ver[0]));
            memcpy(d->ver[1], ver[1], sizeof(d->ver[1]));
            same = true;
        }
        d->kind = same ? st->kind : -1;
        d->a_on_top = st->a_on_top;
        d->width_a = st->width_a;
        d->width_b = st->width_b;
        d->top_w = st->top_w;
    } else {
        BufferSet *d = &s_sets[st->set];
        size_t na = (size_t) st->width_a * 192, nb = (size_t) st->width_b * 192;
        size_t ntop = (size_t) st->top_w * 192;
        if (st->kind == STAGED_SOFTWARE) {
            memcpy(d->a, s_master.a, ntop * 4);
            memcpy(d->b, s_master.b, 256 * 192 * 4);
        } else {
            memcpy(d->a, s_master.a, na * 4);
            memcpy(d->below, s_master.below, na * 4);
            memcpy(d->b, s_master.b, nb * 4);
        }
        if (st->hud_flags) memcpy(d->hud, s_hud_master, hud_bytes);
    }
    g_present_stage = "idle";
    if (g_present_test_hook) g_present_test_hook(st->f);
    g_present_stats.staged++;
    g_present_stats.stage_us += present_clock_us() - t_start;
}

static void stage_commit(const Staged *st) {
    static int s_last_kind = -1;
    const FrameSnapshot *f = st->f;
    const uint32_t *s_buf_a = s_sets[st->set].a, *s_buf_b = s_sets[st->set].b, *s_buf_below = s_sets[st->set].below;
    s_shown.hud_flags = st->hud_flags;
    if (st->hud_flags) {
        if (s_direct && s_dsets[st->set].hud_ptr) {
            s_shown.hud_tex = s_dsets[st->set].hud_tex;
        } else if (!s_direct) {
            glBindTexture(GL_TEXTURE_2D, s_tex_hud);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, HUD_ROWS, GL_RGBA, GL_UNSIGNED_BYTE, s_sets[st->set].hud);
            s_shown.hud_tex = s_tex_hud;
        } else {
            s_shown.hud_flags = 0;
        }
    }
    if (s_direct) {
        const DirectSet *d = &s_dsets[st->set];
        int wa = st->width_a != 256, wb = st->width_b != 256, wt = st->top_w != 256;
#ifdef PRESENT_TEST_DIRECT
        int ws[2] = {256, st->top_w};
        for (int p = 0; p < 3; p++) {
            for (int w = 0; w < 2; w++) {
                glBindTexture(GL_TEXTURE_2D, d->tex[p][w]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ws[w], 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, d->ptr[p][w]);
            }
        }
#endif
        if (st->kind == STAGED_ALT) {
            if (!s_alt || st->alt_enter) s_held[0].valid = s_held[1].valid = false;
            s_alt = true;
            s_shown.layers = false;
            Held *h = &s_held[st->a_on_top ? 0 : 1];
            h->width = st->width_a;
            h->above = d->tex[0][wa];
            h->below = d->tex[1][wa];
            h->bright = (uint16_t) (f->io[0x6c] | f->io[0x6d] << 8);
            uint16_t bd = (uint16_t) (f->palette[0] | f->palette[1] << 8);
            h->backdrop[0] = (bd & 31) / 31.0f;
            h->backdrop[1] = ((bd >> 5) & 31) / 31.0f;
            h->backdrop[2] = ((bd >> 10) & 31) / 31.0f;
            h->have_prev = h->valid && f->number == h->number + 2;
            h->cur ^= 1;
            copy_list(&s_alt3d[st->a_on_top ? 0 : 1][h->cur], LIST3D(f));
            h->number = f->number;
            h->valid = true;
            s_shown.software = false;
            s_shown.a_on_top = st->a_on_top;
            s_shown.width_top = st->top_w;
            s_shown.width_bottom = 256;
            s_tex_plain[1] = d->tex[2][wb]; // engine B: shown until the other screen has a frame of its own
            s_have_prev = false;
            s_last_kind = st->kind;
            s_shown.race = f->scene == 2;
            s_shown.scale = st->scale;
            s_shown.snap = f;
            s_last_number = f->number;
            return;
        } else {
            if (s_alt) s_held[0].valid = s_held[1].valid = false;
            s_alt = false;
            s_shown.a_on_top = st->a_on_top;
            s_shown.width_top = st->top_w;
            s_shown.width_bottom = 256;
            s_shown.layers = st->kind == STAGED_LAYERS;
            if (st->kind == STAGED_LAYERS) {
                s_shown.software = false;
                s_shown.comp[0] = st->comp[0];
                s_shown.comp[1] = st->comp[1];
                for (int i = 0; i < 3; i++) {
                    s_shown.lay_tex[0][i] = d->tex[i][1];
                    s_shown.lay_tex[1][i] = d->tex[i][0];
                }
                s_have_prev = f->number == s_last_number + 1;
                take_list(f);
            } else if (st->kind == STAGED_SOFTWARE) {
                s_shown.software = true;
                s_tex_plain[0] = d->tex[0][wt];
                s_tex_plain[1] = d->tex[2][0];
            } else {
                s_shown.software = false;
                s_tex_above = d->tex[0][wa];
                s_tex_below = d->tex[1][wa];
                s_tex_plain[1] = d->tex[2][wb];
                s_shown.bright = (uint16_t) (f->io[0x6c] | f->io[0x6d] << 8);
                uint16_t bd = (uint16_t) (f->palette[0] | f->palette[1] << 8);
                s_shown.backdrop[0] = (bd & 31) / 31.0f;
                s_shown.backdrop[1] = ((bd >> 5) & 31) / 31.0f;
                s_shown.backdrop[2] = ((bd >> 10) & 31) / 31.0f;
                s_have_prev = f->number == s_last_number + 1;
                take_list(f);
            }
            s_last_kind = st->kind;
            s_shown.race = f->scene == 2;
            s_shown.scale = st->scale;
            s_shown.snap = f;
            s_last_number = f->number;
            return;
        }
    }
    bool same = st->unchanged && st->kind == s_last_kind && st->kind != STAGED_ALT;
    s_last_kind = st->kind;
    s_shown.layers = false;
    s_shown.a_on_top = st->a_on_top;
    s_shown.width_top = st->top_w;
    s_shown.width_bottom = 256;
    if (st->kind == STAGED_ALT) {
        if (!s_alt || st->alt_enter) s_held[0].valid = s_held[1].valid = false;
        s_alt = true;
        Held *h = &s_held[st->a_on_top ? 0 : 1];
        h->width = st->width_a;
        upload(h->above, s_buf_a, st->width_a, false);
        upload(h->below, s_buf_below, st->width_a, false);
        h->bright = (uint16_t) (f->io[0x6c] | f->io[0x6d] << 8);
        uint16_t bd = (uint16_t) (f->palette[0] | f->palette[1] << 8);
        h->backdrop[0] = (bd & 31) / 31.0f;
        h->backdrop[1] = ((bd >> 5) & 31) / 31.0f;
        h->backdrop[2] = ((bd >> 10) & 31) / 31.0f;
        h->have_prev = h->valid && f->number == h->number + 2;
        h->cur ^= 1;
        copy_list(&s_alt3d[st->a_on_top ? 0 : 1][h->cur], LIST3D(f));
        h->number = f->number;
        h->valid = true;
        s_shown.software = false;
        upload(s_tex_plain[1], s_buf_b, st->width_b, false);
        s_have_prev = false;
    } else {
        if (s_alt) s_held[0].valid = s_held[1].valid = false;
        s_alt = false;
        if (st->kind == STAGED_SOFTWARE) {
            s_shown.software = true;
            upload(s_tex_plain[0], s_buf_a, st->top_w, same);
            upload(s_tex_plain[1], s_buf_b, 256, same);
        } else {
            s_shown.software = false;
            upload(s_tex_above, s_buf_a, st->width_a, same);
            upload(s_tex_below, s_buf_below, st->width_a, same);
            upload(s_tex_plain[1], s_buf_b, st->width_b, same); // engine B
            s_shown.bright = (uint16_t) (f->io[0x6c] | f->io[0x6d] << 8);
            uint16_t bd = (uint16_t) (f->palette[0] | f->palette[1] << 8);
            s_shown.backdrop[0] = (bd & 31) / 31.0f;
            s_shown.backdrop[1] = ((bd >> 5) & 31) / 31.0f;
            s_shown.backdrop[2] = ((bd >> 10) & 31) / 31.0f;
            s_have_prev = f->number == s_last_number + 1;
            take_list(f);
        }
    }
    s_shown.race = f->scene == 2;
    s_shown.scale = st->scale;
    s_shown.snap = f;
    s_last_number = f->number;
}

void present_update(const FrameSnapshot *f, int scale) {
    Staged st;
    plan(f, scale, &st);
    plan_gl(&st);
    stage_cpu(&st);
    stage_commit(&st);
}

#include <pthread.h>

static pthread_mutex_t s_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_async_cond = PTHREAD_COND_INITIALIZER;
static bool s_async_started;
static const FrameSnapshot *(*s_take)(void);
static void (*s_release)(const FrameSnapshot *);
static int s_async_scale;
static bool s_set_busy[DIRECT_SETS];
static Staged s_done[2];
static int s_done_count;
static const Staged *s_gl_request;
static const FrameSnapshot *s_on_screen;
static bool s_frame_ready;

void present_async_frame_ready(void) {
    pthread_mutex_lock(&s_async_lock);
    s_frame_ready = true;
    pthread_cond_broadcast(&s_async_cond);
    pthread_mutex_unlock(&s_async_lock);
}

void present_async_thread_started(void) __attribute__((weak));

static int take_set(void) {
    pthread_mutex_lock(&s_async_lock);
    for (;;) {
        for (int i = 0; i < s_set_count; i++) {
            if (!s_set_busy[i]) {
                s_set_busy[i] = true;
                pthread_mutex_unlock(&s_async_lock);
                return i;
            }
        }
        pthread_cond_wait(&s_async_cond, &s_async_lock);
    }
}

static void *async_main(void *arg) {
    (void) arg;
    if (present_async_thread_started) present_async_thread_started();
    for (;;) {
        const FrameSnapshot *f = s_take();
        if (!f) {
            pthread_mutex_lock(&s_async_lock);
            while (!s_frame_ready) pthread_cond_wait(&s_async_cond, &s_async_lock);
            s_frame_ready = false;
            pthread_mutex_unlock(&s_async_lock);
            continue;
        }
        Staged st;
        plan(f, s_async_scale, &st);
        if (st.layer3d) {
            pthread_mutex_lock(&s_async_lock);
            s_gl_request = &st;
            while (s_gl_request) pthread_cond_wait(&s_async_cond, &s_async_lock);
            pthread_mutex_unlock(&s_async_lock);
        }
        st.set = take_set();
        stage_cpu(&st);
        pthread_mutex_lock(&s_async_lock);
        if (s_done_count == 2) {
            s_done[1].unchanged = s_done[1].unchanged && s_done[0].unchanged;
            s_set_busy[s_done[0].set] = false;
            s_release(s_done[0].f);
            s_done[0] = s_done[1];
            s_done_count = 1;
        }
        s_done[s_done_count++] = st;
        pthread_cond_broadcast(&s_async_cond);
        pthread_mutex_unlock(&s_async_lock);
    }
    return NULL;
}

bool present_async_start(const FrameSnapshot *(*take)(void), void (*release)(const FrameSnapshot *), int scale) {
#ifdef __vita__
    int widths[2] = {256, top_width()};
    for (int k = 0; k < DIRECT_SETS; k++) {
        for (int p = 0; p < 3; p++) {
            for (int w = 0; w < 2; w++) {
                GLuint t = make_texture();
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, widths[w], 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                s_dsets[k].tex[p][w] = t;
                s_dsets[k].ptr[p][w] = vglGetTexDataPointer(GL_TEXTURE_2D);
                if (!s_dsets[k].ptr[p][w]) {
                    port_log("Graphics: low video memory, 2D drawn the slower way");
                    goto no_direct;
                }
                memset(s_dsets[k].ptr[p][w], 0, (size_t) widths[w] * 192 * 4);
            }
        }
        s_dsets[k].hud_tex = make_texture();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, HUD_ROWS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_dsets[k].hud_ptr = vglGetTexDataPointer(GL_TEXTURE_2D);
        if (s_dsets[k].hud_ptr) memset(s_dsets[k].hud_ptr, 0, 256 * HUD_ROWS * 4);
    }
    s_direct = true;
    s_set_count = DIRECT_SETS;
no_direct:
#elif defined(PRESENT_TEST_DIRECT)
    int widths[2] = {256, top_width()};
    for (int k = 0; k < DIRECT_SETS; k++) {
        for (int p = 0; p < 3; p++) {
            for (int w = 0; w < 2; w++) {
                s_dsets[k].tex[p][w] = make_texture();
                s_dsets[k].ptr[p][w] = calloc((size_t) widths[w] * 192, 4);
            }
        }
    }
    s_direct = true;
    s_set_count = DIRECT_SETS;
#endif
    s_take = take;
    s_release = release;
    s_async_scale = scale;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    s_async_started = pthread_create(&t, &attr, async_main, NULL) == 0;
    pthread_attr_destroy(&attr);
    return s_async_started;
}

static int s_retired[DIRECT_SETS * 2];
static uint32_t s_retired_at[DIRECT_SETS * 2];
static int s_retired_count;
static uint32_t s_swaps;

void present_async_swapped(void) {
    pthread_mutex_lock(&s_async_lock);
    s_swaps++;
    int k = 0;
    for (int i = 0; i < s_retired_count; i++) {
        if (s_swaps - s_retired_at[i] >= 3) {
            s_set_busy[s_retired[i]] = false;
        } else {
            s_retired[k] = s_retired[i];
            s_retired_at[k++] = s_retired_at[i];
        }
    }
    if (k != s_retired_count) pthread_cond_broadcast(&s_async_cond);
    s_retired_count = k;
    pthread_mutex_unlock(&s_async_lock);
}

static int s_role_set[3] = {-1, -1, -1};

static void retire_role(int role) {
    int set = s_role_set[role];
    s_role_set[role] = -1;
    if (set < 0) return;
    for (int r = 0; r < 3; r++) {
        if (s_role_set[r] == set) return;
    }
    s_retired[s_retired_count] = set;
    s_retired_at[s_retired_count++] = s_swaps;
}

static void commit_one(const Staged *st) {
    s_cur_list = NULL;
    stage_commit(st);
    pthread_mutex_lock(&s_async_lock);
    if (s_direct) {
        if (st->kind != STAGED_ALT) {
            retire_role(0);
            retire_role(1);
            retire_role(2);
            s_role_set[0] = st->set;
        } else {
            int role = st->a_on_top ? 1 : 2;
            if (st->alt_enter) {
                retire_role(1);
                retire_role(2);
            }
            retire_role(0);
            retire_role(role);
            s_role_set[role] = st->set;
        }
    } else {
        s_set_busy[st->set] = false;
    }
    pthread_cond_broadcast(&s_async_cond);
    pthread_mutex_unlock(&s_async_lock);
    if (s_on_screen) s_release(s_on_screen);
    s_on_screen = st->f;
}

bool present_async_commit(void) {
    pthread_mutex_lock(&s_async_lock);
    if (s_gl_request) {
        plan_gl(s_gl_request);
        s_gl_request = NULL;
        pthread_cond_broadcast(&s_async_cond);
    }
    int n = s_done_count;
    Staged st[2];
    for (int i = 0; i < n; i++) st[i] = s_done[i];
    if (n == 2 && st[0].kind != STAGED_ALT && st[1].kind != STAGED_ALT) {
        s_done[0] = s_done[1];
        s_done_count = 1;
        n = 1;
    } else {
        s_done_count = 0;
    }
    pthread_mutex_unlock(&s_async_lock);
    if (!n) return false;
    for (int i = 0; i < n; i++) commit_one(&st[i]);
    return true;
}

static struct {
    uint64_t number;
    float t, wide;
    int scale;
    GLuint tex;
    bool valid;
} s_last3d;

static GLuint layer3d_texture(float wide, float t) {
    if (s_last3d.valid && s_last3d.number == s_shown.snap->number && s_last3d.t == t && s_last3d.wide == wide &&
        s_last3d.scale == s_shown.scale) {
        return s_last3d.tex;
    }
    GLuint tex3d = gl3d_render(s_cur_list ? s_cur_list : &s_frames3d[s_cur3d], s_have_prev ? &s_frames3d[s_cur3d ^ 1] : NULL, t, s_shown.snap,
                               s_shown.scale, wide);
    s_last3d.number = s_shown.snap->number;
    s_last3d.t = t;
    s_last3d.wide = wide;
    s_last3d.scale = s_shown.scale;
    s_last3d.tex = tex3d;
    s_last3d.valid = true;
    return tex3d;
}

// The largest rectangle of the given aspect ratio centred in r.
static void fit(const int *r, float aspect, int *out) {
    float w = (float) r[2], h = (float) r[3];
    if (w / h > aspect) w = h * aspect;
    else h = w / aspect;
    out[0] = r[0] + (int) ((r[2] - w) / 2);
    out[1] = r[1] + (int) ((r[3] - h) / 2);
    out[2] = (int) w;
    out[3] = (int) h;
}

static bool snap_full(int *r, int w, int h) {
    if (r[0] > 2 || r[1] > 2 || r[2] < w - 4 || r[3] < h - 4 || r[0] + r[2] < w - 2 || r[1] + r[3] < h - 2) return false;
    r[0] = 0, r[1] = 0, r[2] = w, r[3] = h;
    return true;
}

bool present_engine_a_on_top(void) { return s_shown.a_on_top; }

static GLuint s_target; // the framebuffer present_draw draws into (0: the window)
void present_set_target(uint32_t framebuffer) { s_target = framebuffer; }

static void draw_alt(const int layout[8], int w, int h, float t) {
    glBindFramebuffer(GL_FRAMEBUFFER, s_target);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int live = s_shown.a_on_top ? 0 : 1;
    for (int screen = 0; screen < 2; screen++) {
        if (layout[screen * 4 + 2] <= 0 || layout[screen * 4 + 3] <= 0) continue; // hidden
        const Held *hd = &s_held[screen];
        int r[4];
        if (!hd->valid) {
            fit(&layout[screen * 4], 256.0f / 192.0f, r);
            glUseProgram(s_prog_plain);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_plain[1]);
            draw_quad(r[0], r[1], r[2], r[3], w, h);
            continue;
        }
        // Each screen's frames are two VBlanks apart: the live one is half way from its previous frame, the other
        // one goes on from there (the picture lags its frame by two VBlanks, and moves without pauses).
        float ts = screen == live ? t * 0.5f : 0.5f + t * 0.5f;
        if (ts > 1.0f) ts = 1.0f;
        float wide = (float) hd->width / 256.0f;
        const Gx3dFrame *cur = &s_alt3d[screen][hd->cur], *prev = hd->have_prev ? &s_alt3d[screen][hd->cur ^ 1] : NULL;
        gl3d_use_target(screen);
        GLuint tex3d = gl3d_render(cur, prev, prev ? ts : 1.0f, s_shown.snap, s_shown.scale, wide);
        gl3d_use_target(0);
        glBindFramebuffer(GL_FRAMEBUFFER, s_target);
        glViewport(0, 0, w, h);
        float wq = (float) hd->width;
        fit(&layout[screen * 4], wq / 192.0f, r);
        glUseProgram(s_prog_compose);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hd->above);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, hd->below);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex3d);
        glUniform1f(s_u_k2, 1.0f);
        glUniform1f(s_u_k3, 1.0f);
        glUniform3fv(s_u_backdrop, 1, hd->backdrop);
        uint32_t mode = hd->bright >> 14, factor = hd->bright & 31;
        if (factor > 16) factor = 16;
        SET_BRIGHT_MODE(s_u_bright_mode, (mode == 1 || mode == 2) ? (int) mode : 0);
        glUniform1f(s_u_bright, factor / 16.0f);
        glActiveTexture(GL_TEXTURE0);
        draw_quad(r[0], r[1], r[2], r[3], w, h);
    }
}

static GLuint layer3d_texture(float wide, float t);

static void draw_layers(const int layout[8], int w, int h, float t) {
    float wide = (float) (s_shown.a_on_top ? s_shown.comp[0].width : 256) / 256.0f;
    bool any3d = s_shown.comp[0].bg0_3d || s_shown.comp[1].bg0_3d;
    GLuint tex3d = any3d && s_shown.snap ? layer3d_texture(wide, t) : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_target);
    glViewport(0, 0, w, h);
    int rects[2][4];
    bool covered = false;
    for (int screen = 0; screen < 2; screen++) {
        if (layout[screen * 4 + 2] <= 0 || layout[screen * 4 + 3] <= 0) continue;
        fit(&layout[screen * 4], s_shown.comp[screen].width / 192.0f, rects[screen]);
        covered |= snap_full(rects[screen], w, h);
    }
    if (!covered) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(s_prog_layers);
    for (int screen = 0; screen < 2; screen++) {
        if (layout[screen * 4 + 2] <= 0 || layout[screen * 4 + 3] <= 0) continue; // hidden
        const Gpu2dCompose *c = &s_shown.comp[screen];
        float w2 = (float) c->width, w3 = 256.0f * wide;
        const int *r = rects[screen];
        for (int i = 0; i < 3; i++) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, s_shown.lay_tex[screen][i]);
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, c->bg0_3d ? tex3d : s_shown.lay_tex[screen][0]);
        glActiveTexture(GL_TEXTURE0);
        glUniform1f(s_ul_width, w2);
        glUniform1f(s_ul_k3, w2 / w3);
        glUniform1f(s_ul_bg0_3d, c->bg0_3d && tex3d ? 1.0f : 0.0f);
        glUniform4f(s_ul_prio, c->prio[0], c->prio[1], c->prio[2], c->prio[3]);
        glUniform4f(s_ul_enabled, c->enabled[0], c->enabled[1], c->enabled[2], c->enabled[3]);
        glUniform3f(s_ul_first03, c->first[0], c->first[1], c->first[2]);
        glUniform3f(s_ul_first45, c->first[3], c->first[4], c->first[5]);
        glUniform3f(s_ul_second03, c->second[0], c->second[1], c->second[2]);
        glUniform3f(s_ul_second45, c->second[3], c->second[4], c->second[5]);
        glUniform4f(s_ul_effect, c->effect, c->eva, c->evb, c->evy);
        glUniform1f(s_ul_backdrop, c->backdrop);
        glUniform1f(s_ul_bright, c->bright);
        draw_quad(r[0], r[1], r[2], r[3], w, h);
    }
}

static void draw_screens(const int layout[8], int w, int h, float t);

static void draw_hud_piece(const int r[4], const int src[4], float opacity, int w, int h) {
    if (r[2] <= 0 || r[3] <= 0) return;
    float x0 = (float) r[0] / w * 2 - 1, x1 = (float) (r[0] + r[2]) / w * 2 - 1;
    float y0 = 1 - (float) r[1] / h * 2, y1 = 1 - (float) (r[1] + r[3]) / h * 2;
    float u0 = src[0] / 256.0f, u1 = (src[0] + src[2]) / 256.0f;
    float v0 = src[1] / (float) HUD_ROWS, v1 = (src[1] + src[3]) / (float) HUD_ROWS;
    const float v[] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
    glUniform1f(s_u_hud_opacity, opacity);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_hud(int w, int h) {
    int flags = present_hud_flags();
    if (!s_hud_draw || !(flags & GPU2D_HUD_SHOWN)) return;
    const PresentHudLayout *l = &s_hud_layout;
    glBindFramebuffer(GL_FRAMEBUFFER, s_target);
    glViewport(0, 0, w, h);
    glUseProgram(s_prog_hud);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_shown.hud_tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (flags & GPU2D_HUD_PICTURE) {
        draw_hud_piece(l->whole, s_hud_src_whole, 1.0f, w, h);
    } else {
        draw_hud_piece(l->list, hud_src(flags, HUD_LIST), 0.9f, w, h);
        draw_hud_piece(l->time, hud_src(flags, HUD_TIME), 1.0f, w, h);
        if (flags & GPU2D_HUD_MAP) draw_hud_piece(l->map, hud_src(flags, HUD_MAP), 0.85f, w, h);
        else draw_hud_piece(l->side, hud_src(flags, HUD_SIDE), 0.85f, w, h);
    }
    if (flags & GPU2D_HUD_PAUSE) draw_hud_piece(l->pause, s_hud_src_pause, 1.0f, w, h);
    glDisable(GL_BLEND);
}

void present_draw(const int layout[8], int w, int h, float t) {
    draw_screens(layout, w, h, t);
    draw_hud(w, h);
}

void present_refresh_3d(void) { s_last3d.valid = false; }

void present_draw_overlay(const uint32_t *rgba, bool changed, int tw, int th, const int rect[4], int w, int h) {
    static GLuint tex;
    static int tex_w, tex_h;
    if (!tex) glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (tw != tex_w || th != tex_h) {
        tex_w = tw, tex_h = th;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (changed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_target);
    glViewport(0, 0, w, h);
    glUseProgram(s_prog_hud);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const int src[4] = {0, 0, 256, HUD_ROWS};
    draw_hud_piece(rect, src, 1.0f, w, h);
    glDisable(GL_BLEND);
}

static void draw_screens(const int layout[8], int w, int h, float t) {
    if (s_alt && s_shown.snap) {
        s_last3d.valid = false;
        draw_alt(layout, w, h, t);
        return;
    }
    if (s_shown.layers) {
        draw_layers(layout, w, h, t);
        return;
    }
    GLuint tex3d = 0;
    // The 3D layer is as wide as the 2D of its screen (widescreen on the top screen).
    // (Each polygon keeps the proportions of its own projection: gl3d narrows those projected for 4:3.)
    float wide = (float) (s_shown.a_on_top ? s_shown.width_top : 256) / 256.0f;
    if (!s_shown.software && s_shown.snap) tex3d = layer3d_texture(wide, t);
    glBindFramebuffer(GL_FRAMEBUFFER, s_target);
    glViewport(0, 0, w, h);
    int rects[2][4];
    float wqs[2];
    bool covered = false;
    for (int screen = 0; screen < 2; screen++) {
        if (layout[screen * 4 + 2] <= 0 || layout[screen * 4 + 3] <= 0) continue;
        bool engine_a = (screen == 0) == s_shown.a_on_top;
        float w2 = (float) (screen == 0 ? s_shown.width_top : s_shown.width_bottom), w3 = 256.0f * wide;
        wqs[screen] = engine_a && !s_shown.software && w3 > w2 ? w3 : w2;
        fit(&layout[screen * 4], wqs[screen] / 192.0f, rects[screen]);
        covered |= snap_full(rects[screen], w, h);
    }
    if (!covered) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    for (int screen = 0; screen < 2; screen++) { // 0 = top rectangle, 1 = bottom rectangle
        bool engine_a = (screen == 0) == s_shown.a_on_top;
        if (layout[screen * 4 + 2] <= 0 || layout[screen * 4 + 3] <= 0) continue; // hidden
        float w2 = (float) (screen == 0 ? s_shown.width_top : s_shown.width_bottom), w3 = 256.0f * wide;
        float wq = wqs[screen]; // the screen's width in DS pixels
        const int *r = rects[screen];
        if (s_shown.software) {
            glUseProgram(s_prog_plain);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_plain[screen]);
        } else if (!engine_a) {
            glUseProgram(s_prog_plain);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_plain[1]);
        } else {
            glUseProgram(s_prog_compose);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_tex_above);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s_tex_below);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, tex3d);
            glUniform1f(s_u_k2, wq / w2);
            glUniform1f(s_u_k3, wq / w3);
            glUniform3fv(s_u_backdrop, 1, s_shown.backdrop);
            uint32_t mode = s_shown.bright >> 14, factor = s_shown.bright & 31;
            if (factor > 16) factor = 16;
            SET_BRIGHT_MODE(s_u_bright_mode, (mode == 1 || mode == 2) ? (int) mode : 0);
            glUniform1f(s_u_bright, factor / 16.0f);
            glActiveTexture(GL_TEXTURE0);
        }
        draw_quad(r[0], r[1], r[2], r[3], w, h);
    }
}
