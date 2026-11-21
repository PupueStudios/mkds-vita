// The DS 3D rasteriser on OpenGL ES 3, at any internal resolution.
//
// The polygon list from the geometry engine is already in clip space; OpenGL clips and rasterises it. Per frame:
// clear (colour, alpha, depth), opaque polygons, then translucent ones in list order (the game sorts them), with
// shadow volumes through the stencil buffer. Textures are decoded from the frame's VRAM into a cache that is
// dropped whenever the texture VRAM changes.
#include "port.h"
#include "gpu2d.h"
#include "tex.h"
#include "gl3d.h"
#include "hostapi.h"

#ifdef __vita__
#include <vitaGL.h>
#else
#include <GLES3/gl3.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void wrap_forget(GLuint tex);

typedef struct Vertex {
    float x, y, z, w;
    uint8_t r, g, b, a;
    float s, t;
    float mode; // 0 modulate, 1 decal, 2 toon/highlight, 3 shadow; +4 when textured
} Vertex;

#ifdef __vita__
static const char *VS = "void main(float4 pos, float4 color, float2 uv, float mode,\n"
                            "          uniform float2 tex_size, uniform float wide,\n"
                            "          float4 out v_color : TEXCOORD2, float2 out v_uv : TEXCOORD0, float out v_mode : TEXCOORD1,\n"
                            "          float4 out gl_Position : POSITION) {\n"
                            "  v_color = color / 31.0; v_uv = uv / (16.0 * tex_size); v_mode = mode;\n"
                            "  gl_Position = float4(pos.x * wide, pos.y, pos.z, pos.w);\n"
                            "}\n";
static const char *FS = "float4 main(float4 v_color : TEXCOORD2, float2 v_uv : TEXCOORD0, float v_mode : TEXCOORD1,\n"
                        "            uniform sampler2D tex : TEXUNIT0, uniform float alpha_ref, uniform float highlight,\n"
                        "            uniform float3 toon[32]) : COLOR {\n"
                        "  float4 c = v_color;\n"
                        "  float m = floor(v_mode + 0.5);\n"
                        "  bool textured = m >= 4.0;\n"
                        "  if (textured) m -= 4.0;\n"
                        "  float3 add = float3(0.0, 0.0, 0.0);\n"
                        "  if (m == 2.0) {\n"
                        "    float3 tc = toon[(int) clamp(floor(c.r * 31.0 + 0.5), 0.0, 31.0)];\n"
                        "    if (highlight > 0.5) { c.rgb = float3(c.r, c.r, c.r); add = tc; } else c.rgb = tc;\n"
                        "  }\n"
                        "  if (textured) {\n"
                        "    float4 t = tex2D(tex, v_uv);\n"
                        "    if (m == 1.0) c = float4(lerp(c.rgb, t.rgb, t.a), c.a);\n"
                        "    else c *= t;\n"
                        "  }\n"
                        "  c.rgb = min(c.rgb + add, float3(1.0, 1.0, 1.0));\n"
                        "  if (c.a < 0.5 / 31.0 || c.a <= alpha_ref) discard;\n"
                        "  return c;\n"
                        "}\n";
static const char *MESH_VS =
    "void main(float4 p, float4 n, float2 st, float4 col, float4 misc,\n"
    "          uniform float4 m0, uniform float4 m1, uniform float4 m2, uniform float4 m3, uniform float xs,\n"
    "          uniform float4 draw_color, uniform float2 draw_st, uniform float texmode, uniform float4 tm, uniform float2 tm2,\n"
    "          uniform float4 mask, uniform float3 vc0, uniform float3 vc1, uniform float3 vc2,\n"
    "          uniform float3 emi, uniform float3 ldir[4], uniform float3 lhalf[4], uniform float3 lspe[4],\n"
    "          uniform float3 ldif[4], uniform float3 lamb[4], uniform float use_table, uniform float shin[128],\n"
    "          uniform float2 tex_size, uniform float mode,\n"
    "          float4 out v_color : TEXCOORD2, float2 out v_uv : TEXCOORD0, float out v_mode : TEXCOORD1,\n"
    "          float4 out gl_Position : POSITION) {\n"
    "  float3 q = p.xyz / 4096.0;\n"
    "  float4 k = m0 * q.x + m1 * q.y + m2 * q.z + m3;\n"
    "  gl_Position = float4(k.x * xs, k.y, k.z, k.w);\n"
    "  float3 c = draw_color.rgb;\n"
    "  if (col.w > 1.5) {\n"
    "    float3 nn = floor(float3(dot(n.xyz, vc0), dot(n.xyz, vc1), dot(n.xyz, vc2)) / 4096.0);\n"
    "    float4 dd = clamp(floor(-float4(dot(ldir[0], nn), dot(ldir[1], nn), dot(ldir[2], nn), dot(ldir[3], nn)) / 512.0), 0.0, 511.0);\n"
    "    float4 sh = max(floor(-float4(dot(lhalf[0], nn), dot(lhalf[1], nn), dot(lhalf[2], nn), dot(lhalf[3], nn)) / 512.0), 0.0);\n"
    "    sh = min(floor(sh * sh / 512.0), 511.0);\n"
    "    if (use_table > 0.5) sh = float4(shin[(int) floor(sh.x / 4.0)], shin[(int) floor(sh.y / 4.0)], shin[(int) floor(sh.z / 4.0)], shin[(int) floor(sh.w / 4.0)]) * 2.0;\n"
    "    float3 o = emi;\n"
    "    o += mask.x * (floor(lspe[0] * sh.x / 31.0) + floor(ldif[0] * dd.x / 31.0) + lamb[0]);\n"
    "    o += mask.y * (floor(lspe[1] * sh.y / 31.0) + floor(ldif[1] * dd.y / 31.0) + lamb[1]);\n"
    "    o += mask.z * (floor(lspe[2] * sh.z / 31.0) + floor(ldif[2] * dd.z / 31.0) + lamb[2]);\n"
    "    o += mask.w * (floor(lspe[3] * sh.w / 31.0) + floor(ldif[3] * dd.w / 31.0) + lamb[3]);\n"
    "    c = min(floor(o / 512.0), float3(31.0, 31.0, 31.0));\n"
    "  } else if (col.w > 0.5) c = col.rgb;\n"
    "  float2 uv = draw_st;\n"
    "  if (misc.x > 0.5) uv = texmode > 0.5 ? floor((st.x * tm.xy + st.y * tm.zw + tm2) / 4096.0) : st;\n"
    "  v_color = float4(c, draw_color.a) / 31.0; v_uv = uv / (16.0 * tex_size); v_mode = mode;\n"
    "}\n";
#else
static const char *VS = "#version 300 es\n"
                            "layout(location = 0) in vec4 pos;\n"
                            "layout(location = 1) in vec4 color;\n"
                            "layout(location = 2) in vec2 uv;\n"
                            "layout(location = 3) in float mode;\n"

                            "uniform vec2 tex_size;\n"
                            "uniform float wide;\n"
                            "out vec4 v_color;\n"
                            "out vec2 v_uv;\n"
                            "flat out float v_mode;\n"
                            "void main() {\n"
                            "  v_color = color / 31.0; v_uv = uv / (16.0 * tex_size); v_mode = mode;\n"
                            "  gl_Position = vec4(pos.x * wide, pos.y, pos.z, pos.w);\n"
                            "}\n";
static const char *FS = "#version 300 es\n"
                        "precision highp float;\n"
                        "uniform sampler2D tex;\n"
                        "uniform float alpha_ref;\n"
                        "uniform bool highlight;\n"
                        "uniform vec3 toon[32];\n"
                        "in vec4 v_color;\n"
                        "in vec2 v_uv;\n"
                        "flat in float v_mode;\n"
                        "out vec4 color;\n"
                        "void main() {\n"
                        "  vec4 c = v_color;\n"
                        "  float m = v_mode;\n"
                        "  bool textured = m >= 4.0;\n"
                        "  if (textured) m -= 4.0;\n"
                        "  vec3 add = vec3(0.0);\n"
                        "  if (m == 2.0) {\n" // toon / highlight: the vertex red picks a toon table colour
                        "    vec3 tc = toon[int(clamp(floor(c.r * 31.0 + 0.5), 0.0, 31.0))];\n"
                        "    if (highlight) { c.rgb = vec3(c.r); add = tc; } else c.rgb = tc;\n"
                        "  }\n"
                        "  if (textured) {\n"
                        "    vec4 t = texture(tex, v_uv);\n"
                        "    if (m == 1.0) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
                        "    else c *= t;\n"
                        "  }\n"
                        "  c.rgb = min(c.rgb + add, vec3(1.0));\n"
                        // The DS never draws pixels with alpha 0; the alpha test drops those not above the reference.
                        "  if (c.a < 0.5 / 31.0 || c.a <= alpha_ref) discard;\n"
                        "  color = c;\n"
                        "}\n";
static const char *MESH_VS =
    "#version 300 es\n"
    "layout(location = 0) in vec4 p;\n"
    "layout(location = 1) in vec4 n;\n"
    "layout(location = 2) in vec2 st;\n"
    "layout(location = 3) in vec4 col;\n"
    "layout(location = 4) in vec4 misc;\n"
    "uniform vec4 m0, m1, m2, m3;\n"
    "uniform float xs;\n"
    "uniform vec4 draw_color;\n"
    "uniform vec2 draw_st;\n"
    "uniform float texmode;\n"
    "uniform vec4 tm;\n"
    "uniform vec2 tm2;\n"
    "uniform vec4 mask;\n"
    "uniform vec3 vc0, vc1, vc2, emi;\n"
    "uniform vec3 ldir[4], lhalf[4], lspe[4], ldif[4], lamb[4];\n"
    "uniform float use_table;\n"
    "uniform float shin[128];\n"
    "uniform vec2 tex_size;\n"
    "uniform float mode;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "flat out float v_mode;\n"
    "void main() {\n"
    "  vec3 q = p.xyz / 4096.0;\n"
    "  vec4 k = m0 * q.x + m1 * q.y + m2 * q.z + m3;\n"
    "  gl_Position = vec4(k.x * xs, k.y, k.z, k.w);\n"
    "  vec3 c = draw_color.rgb;\n"
    "  if (col.w > 1.5) {\n"
    "    vec3 nn = floor(vec3(dot(n.xyz, vc0), dot(n.xyz, vc1), dot(n.xyz, vc2)) / 4096.0);\n"
    "    vec4 dd = clamp(floor(-vec4(dot(ldir[0], nn), dot(ldir[1], nn), dot(ldir[2], nn), dot(ldir[3], nn)) / 512.0), 0.0, 511.0);\n"
    "    vec4 sh = max(floor(-vec4(dot(lhalf[0], nn), dot(lhalf[1], nn), dot(lhalf[2], nn), dot(lhalf[3], nn)) / 512.0), 0.0);\n"
    "    sh = min(floor(sh * sh / 512.0), 511.0);\n"
    "    if (use_table > 0.5) sh = vec4(shin[int(sh.x / 4.0)], shin[int(sh.y / 4.0)], shin[int(sh.z / 4.0)], shin[int(sh.w / 4.0)]) * 2.0;\n"
    "    vec3 o = emi;\n"
    "    o += mask.x * (floor(lspe[0] * sh.x / 31.0) + floor(ldif[0] * dd.x / 31.0) + lamb[0]);\n"
    "    o += mask.y * (floor(lspe[1] * sh.y / 31.0) + floor(ldif[1] * dd.y / 31.0) + lamb[1]);\n"
    "    o += mask.z * (floor(lspe[2] * sh.z / 31.0) + floor(ldif[2] * dd.z / 31.0) + lamb[2]);\n"
    "    o += mask.w * (floor(lspe[3] * sh.w / 31.0) + floor(ldif[3] * dd.w / 31.0) + lamb[3]);\n"
    "    c = min(floor(o / 512.0), vec3(31.0));\n"
    "  } else if (col.w > 0.5) c = col.rgb;\n"
    "  vec2 uv = draw_st;\n"
    "  if (misc.x > 0.5) uv = texmode > 0.5 ? floor((st.x * tm.xy + st.y * tm.zw + tm2) / 4096.0) : st;\n"
    "  v_color = vec4(c, draw_color.a) / 31.0; v_uv = uv / (16.0 * tex_size); v_mode = mode;\n"
    "}\n";
#endif

static GLuint s_prog, s_vao, s_vbo, s_fbo, s_color, s_depth;
static GLint s_u_tex_size, s_u_alpha_ref, s_u_wide, s_u_highlight, s_u_toon;

enum { FK_PLAIN, FK_TEX, FK_DECAL, FK_FULL, FK_COUNT };
#define FV_COUNT (FK_COUNT * 2)
#define DISCARD_LINE "  if (c.a < 0.5 / 31.0 || c.a <= alpha_ref) discard;\n"

static char *fs_variant(int kind, bool keep) {
    char *src = strdup(FS);
    if (kind != FK_FULL) {
#ifdef __vita__
        static const char *const body[3] = {
            "",
            "  c *= tex2D(tex, v_uv);\n",
            "  float4 t = tex2D(tex, v_uv);\n  c = float4(lerp(c.rgb, t.rgb, t.a), c.a);\n"};
#else
        static const char *const body[3] = {
            "",
            "  c *= texture(tex, v_uv);\n",
            "  vec4 t = texture(tex, v_uv);\n  c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"};
#endif
        char *a = strstr(src, "  float m = ");
        if (!a) a = strstr(src, "  float m = v_mode;");
        char *z = strstr(src, DISCARD_LINE);
        if (a && z) {
            size_t head = (size_t) (a - src), tail = strlen(z);
            char *n = malloc(head + strlen(body[kind]) + tail + 1);
            memcpy(n, src, head);
            strcpy(n + head, body[kind]);
            strcat(n, z);
            free(src);
            src = n;
        }
    }
    if (keep) {
        char *cut = strstr(src, DISCARD_LINE);
        if (cut) memmove(cut, cut + strlen(DISCARD_LINE), strlen(cut + strlen(DISCARD_LINE)) + 1);
    }
    return src;
}

static int fs_kind(GLuint tex, uint32_t attr) {
    uint32_t mode = (attr >> 4) & 3;
    if (mode == 2) return FK_FULL;
    if (!tex) return FK_PLAIN;
    return mode == 1 ? FK_DECAL : FK_TEX;
}

static bool fs_keep(bool translucent, GLuint tex, uint32_t attr, uint32_t teximage, bool alpha_ref_on, uint32_t alpha_ref) {
    if (translucent || (alpha_ref_on && alpha_ref >= 31)) return false;
    uint32_t mode = (attr >> 4) & 3, fmt = (teximage >> 26) & 7;
    return !tex || mode == 1 || (fmt >= 2 && fmt <= 4 && !(teximage & (1u << 29)));
}

enum { CU_TEX_SIZE, CU_ALPHA_REF, CU_WIDE, CU_HIGHLIGHT, CU_TOON, CU_COUNT };
static GLuint s_cprog[FV_COUNT];
static GLint s_cl[FV_COUNT][CU_COUNT];
static uint32_t s_cframe[FV_COUNT];
static GLenum s_front_face = GL_CCW;
static int s_w, s_h;
static Vertex *s_verts;
static size_t s_vert_cap;

// ------------------------------------------------------------------------------------------------ textures
typedef struct TexEntry {
    uint32_t key, pltt;
    GLuint tex;
    uint16_t w, h;
    uint32_t hash;
    uint32_t texel_hash, pal_hash;
    uint64_t texel_pages, pal_pages;
    uint32_t texel_seen, pal_seen;
    uint32_t frame;
    uint32_t checked;
    uint64_t pages;
    uint32_t seen;
    uint32_t map;
} TexEntry;
#define TEX_CACHE 1024
static TexEntry s_cache[TEX_CACHE];
static int s_cache_count;
static uint32_t s_frame;
static const uint32_t *s_vram_version;
static uint32_t s_vram_newest, s_map_sig;


static uint64_t s_frame_decode_ns;
#define DECODE_BUDGET_NS 4000000

static void decode_into(GLuint tex, const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt) {
    uint64_t t0 = hapi_now_ns();
    static uint32_t alpha8[32];
    if (!alpha8[31]) {
        for (uint32_t a = 0; a < 32; a++) alpha8[a] = ((a * 255 + 15) / 31) << 24;
    }
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    static uint32_t *texels;
    static uint32_t texels_cap;
    if (texels_cap < w * h) {
        free(texels);
        texels = malloc(w * h * 4);
        texels_cap = w * h;
    }
    tex_decode(map, vram, teximage, pltt, texels);
    for (uint32_t i = 0; i < w * h; i++) texels[i] = (texels[i] & 0xffffff) | alpha8[(texels[i] >> 24) & 31];
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei) w, (GLsizei) h, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    s_frame_decode_ns += hapi_now_ns() - t0;
}

#define TEX_HASH 4096
static int16_t s_tex_index[TEX_HASH];
static int s_last_hit = -1;

static inline uint32_t tex_slot(uint32_t key, uint32_t pltt) {
    uint32_t h = key * 2654435761u ^ pltt * 40503u;
    return (h ^ (h >> 15)) & (TEX_HASH - 1);
}

static void tex_index_add(int i) {
    uint32_t h = tex_slot(s_cache[i].key, s_cache[i].pltt);
    while (s_tex_index[h]) h = (h + 1) & (TEX_HASH - 1);
    s_tex_index[h] = (int16_t) (i + 1);
}

static void tex_index_remove(int i) {
    uint32_t j = tex_slot(s_cache[i].key, s_cache[i].pltt);
    while (s_tex_index[j] != (int16_t) (i + 1)) {
        if (!s_tex_index[j]) return;
        j = (j + 1) & (TEX_HASH - 1);
    }
    s_tex_index[j] = 0;
    for (uint32_t k = (j + 1) & (TEX_HASH - 1); s_tex_index[k]; k = (k + 1) & (TEX_HASH - 1)) {
        int e = s_tex_index[k] - 1;
        uint32_t ideal = tex_slot(s_cache[e].key, s_cache[e].pltt);
        if (((k - ideal) & (TEX_HASH - 1)) >= ((k - j) & (TEX_HASH - 1))) {
            s_tex_index[j] = s_tex_index[k];
            s_tex_index[k] = 0;
            j = k;
        }
    }
    s_last_hit = -1;
}

static void tex_index_rebuild(void) {
    memset(s_tex_index, 0, sizeof(s_tex_index));
    for (int i = 0; i < s_cache_count; i++) tex_index_add(i);
    s_last_hit = -1;
}

static int tex_find(uint32_t key, uint32_t pltt) {
    if (s_last_hit >= 0 && s_cache[s_last_hit].key == key && s_cache[s_last_hit].pltt == pltt) return s_last_hit;
    for (uint32_t h = tex_slot(key, pltt);; h = (h + 1) & (TEX_HASH - 1)) {
        int i = s_tex_index[h] - 1;
        if (i < 0) return -1;
        if (s_cache[i].key == key && s_cache[i].pltt == pltt) return s_last_hit = i;
    }
}

static GLuint texture(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt) {
    if (((teximage >> 26) & 7) == 0) return 0;
    uint32_t key = teximage & 0x3ff0ffff; // address, size, format, colour 0 transparent (not wrap/flip/transform)
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 7) pltt = 0;
    int found = tex_find(key, pltt);
    if (found >= 0) {
        TexEntry *e = &s_cache[found];
        e->frame = s_frame;
        if (e->checked != s_frame) {
            bool remapped = e->map != s_map_sig;
            bool texels = remapped || tex_pages_changed(e->texel_pages, s_vram_version, e->texel_seen);
            bool palette = remapped || tex_pages_changed(e->pal_pages, s_vram_version, e->pal_seen);
            if ((texels || palette) && s_frame_decode_ns >= DECODE_BUDGET_NS) return e->tex;
            e->checked = s_frame;
            if (texels || palette) {
                uint32_t th = e->texel_hash, ph = e->pal_hash;
                tex_source_hash_parts(map, vram, teximage, pltt, texels, &th, &e->texel_pages, palette, &ph,
                                      &e->pal_pages);
                e->map = s_map_sig;
                if (texels) e->texel_seen = s_vram_newest;
                if (palette) e->pal_seen = s_vram_newest;
                if (th != e->texel_hash || ph != e->pal_hash) {
                    e->texel_hash = th;
                    e->pal_hash = ph;
                    decode_into(e->tex, map, vram, teximage, pltt);
                }
            }
        }
        return e->tex;
    }
    TexEntry *e;
    if (s_cache_count < TEX_CACHE) {
        e = &s_cache[s_cache_count++];
        tex_index_add((int) (e - s_cache));
        glGenTextures(1, &e->tex);
        wrap_forget(e->tex);
    } else {
        int oldest = -1;
        for (int i = 0; i < s_cache_count; i++) {
            if (s_cache[i].frame == s_frame) continue;
            if (oldest < 0 || (int32_t) (s_cache[i].frame - s_cache[oldest].frame) < 0) oldest = i;
        }
        if (oldest < 0) return 0;
        e = &s_cache[oldest];
        glDeleteTextures(1, &e->tex);
        glGenTextures(1, &e->tex);
        wrap_forget(e->tex);
        tex_index_remove(oldest);
        e->key = key;
        e->pltt = pltt;
        tex_index_add(oldest);
    }
    e->key = key;
    e->pltt = pltt;
    e->w = (uint16_t) TEX_WIDTH(teximage);
    e->h = (uint16_t) TEX_HEIGHT(teximage);
    e->frame = e->checked = s_frame;
    tex_source_hash_parts(map, vram, teximage, pltt, true, &e->texel_hash, &e->texel_pages, true, &e->pal_hash,
                          &e->pal_pages);
    e->map = s_map_sig;
    e->texel_seen = e->pal_seen = s_vram_newest;
    decode_into(e->tex, map, vram, teximage, pltt);
    return e->tex;
}

#define WRAP_SLOTS 4096
static struct { GLuint tex; uint8_t wrap; } s_wrap_set[WRAP_SLOTS];
static void wrap_forget(GLuint tex) {
    if (s_wrap_set[tex % WRAP_SLOTS].tex == tex) s_wrap_set[tex % WRAP_SLOTS].tex = 0;
}
static void set_wrap_of(GLuint tex, uint32_t teximage) {
    uint8_t wrap = (uint8_t) ((teximage >> 16) & 15);
    uint32_t slot = tex % WRAP_SLOTS;
    if (s_wrap_set[slot].tex == tex && s_wrap_set[slot].wrap == wrap) return;
    s_wrap_set[slot].tex = tex;
    s_wrap_set[slot].wrap = wrap;
    GLint ws = !(teximage & (1u << 16)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 18)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    GLint wt = !(teximage & (1u << 17)) ? GL_CLAMP_TO_EDGE : (teximage & (1u << 19)) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
}

static struct {
    GLint vp[4];
    int depth_func, offset, tex, stencil, stencil_func, stencil_ref, stencil_fmask, stencil_mask, stencil_op, blend,
        depth_mask, color_mask;
} S;
static void state_reset(void) {
    memset(&S, 0xff, sizeof(S));
}
static void st_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h) return;
    S.vp[0] = x, S.vp[1] = y, S.vp[2] = w, S.vp[3] = h;
    glViewport(x, y, w, h);
}
static void st_depth_func(int f) {
    if (S.depth_func != f) glDepthFunc((GLenum) (S.depth_func = f));
}
static void st_offset(int on) {
    if (S.offset == on) return;
    S.offset = on;
    if (on) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -4.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}
static void st_stencil(int on) {
    if (S.stencil == on) return;
    S.stencil = on;
    if (on) glEnable(GL_STENCIL_TEST);
    else glDisable(GL_STENCIL_TEST);
}
static void st_stencil_mask(int m) {
    if (S.stencil_mask != m) glStencilMask((GLuint) (S.stencil_mask = m));
}
static void st_stencil_func(int func, int ref, int mask) {
    if (S.stencil_func == func && S.stencil_ref == ref && S.stencil_fmask == mask) return;
    S.stencil_func = func;
    S.stencil_ref = ref;
    S.stencil_fmask = mask;
    glStencilFunc((GLenum) func, ref, (GLuint) mask);
}
static void st_stencil_op_replace(int replace_on_pass) {
    if (S.stencil_op == replace_on_pass) return;
    S.stencil_op = replace_on_pass;
    glStencilOp(GL_KEEP, GL_KEEP, replace_on_pass ? GL_REPLACE : GL_ZERO);
}
static void st_blend(int on) {
    if (S.blend == on) return;
    S.blend = on;
    if (on) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}
static void st_depth_mask(int on) {
    if (S.depth_mask != on) glDepthMask((GLboolean) (S.depth_mask = on));
}

// ------------------------------------------------------------------------------------------------ setup
static GLuint compile_checked(GLenum type, const char *src, bool *ok_out);
static GLuint compile(GLenum type, const char *src) { return compile_checked(type, src, NULL); }
static GLuint compile_checked(GLenum type, const char *src, bool *ok_out) {
#ifdef __vita__
    type = type == GL_VERTEX_SHADER ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT;
#endif
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
#ifdef __vita__
    extern GLboolean is_shark_online;
    if (!is_shark_online) port_fatal("%s: the shader compiler (libshacccg.suprx) could not be loaded", "gl3d shader");
#endif
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        port_log("Graphics: shader error: %s", log);
    }
    if (ok_out) *ok_out = ok != 0;
    return s;
}

// Render targets: one per screen (dual-screen 3D draws both screens in one present, at different widths; a target
// recreated for the second one would be gone before the first one is composited, on some drivers).
typedef struct Target {
    GLuint fbo, color, depth;
    int w, h;
} Target;
static Target s_targets[3];
static int s_slot;

void gl3d_use_target(int slot) {
    Target *t = &s_targets[s_slot];
    t->fbo = s_fbo, t->color = s_color, t->depth = s_depth, t->w = s_w, t->h = s_h;
    s_slot = slot >= 0 && slot <= 2 ? slot : 0;
    t = &s_targets[s_slot];
    s_fbo = t->fbo, s_color = t->color, s_depth = t->depth, s_w = t->w, s_h = t->h;
}

static void ensure_target(int w, int h) {
    if (w == s_w && h == s_h) return;
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_color);
        glDeleteRenderbuffers(1, &s_depth);
    }
    s_w = w;
    s_h = h;
    glGenTextures(1, &s_color);
    glBindTexture(GL_TEXTURE_2D, s_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &s_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        port_log("Graphics: cannot create the 3D target");
    }
}

static void calibrate_culling(void);

static bool s_gpu_vertices_ok;

enum { MU_M0, MU_M1, MU_M2, MU_M3, MU_XS, MU_COLOR, MU_ST, MU_TEXMODE, MU_TM, MU_TM2, MU_MASK, MU_VC0, MU_VC1,
       MU_VC2, MU_EMI, MU_LDIR, MU_LHALF, MU_LSPE, MU_LDIF, MU_LAMB, MU_USE_TABLE, MU_SHIN, MU_TEX_SIZE, MU_MODE,
       MU_ALPHA_REF, MU_HIGHLIGHT, MU_TOON, MU_COUNT };
static const char *const s_mesh_uniform_names[MU_COUNT] = {
    "m0", "m1", "m2", "m3", "xs", "draw_color", "draw_st", "texmode", "tm", "tm2", "mask", "vc0", "vc1",
    "vc2", "emi", "ldir", "lhalf", "lspe", "ldif", "lamb", "use_table", "shin", "tex_size", "mode",
    "alpha_ref", "highlight", "toon"};
static GLint s_mlocs[FV_COUNT][MU_COUNT];
static GLuint s_mprog[FV_COUNT], s_mesh_vao;
static int s_mcur, s_mlight[FV_COUNT];
static uint32_t s_mframe[FV_COUNT];
static bool s_mesh_ok;
#define MESH_SLOTS 4096
static GLuint s_mesh_vbo[MESH_SLOTS], s_mesh_ibo[MESH_SLOTS];
static uint32_t s_mesh_vbo_gen[MESH_SLOTS];

static GLuint mesh_link(GLuint vs, GLuint fs, GLint *locs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    static const char *const attrs[5] = {"p", "n", "st", "col", "misc"};
    for (int i = 0; i < 5; i++) glBindAttribLocation(prog, (GLuint) i, attrs[i]);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) return 0;
    for (int i = 0; i < MU_COUNT; i++) locs[i] = glGetUniformLocation(prog, s_mesh_uniform_names[i]);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    return prog;
}

static void mesh_init(const GLuint *fsv) {
    GLuint vs = compile(GL_VERTEX_SHADER, MESH_VS);
    s_mprog[FK_FULL * 2] = mesh_link(vs, fsv[FK_FULL * 2], s_mlocs[FK_FULL * 2]);
    if (!s_mprog[FK_FULL * 2]) {
        port_log("Graphics: the 3D model shader failed");
        return;
    }
    for (int v = 0; v < FV_COUNT; v++) {
        if (v != FK_FULL * 2) s_mprog[v] = mesh_link(vs, fsv[v], s_mlocs[v]);
        if (!s_mprog[v]) {
            s_mprog[v] = s_mprog[FK_FULL * 2];
            memcpy(s_mlocs[v], s_mlocs[FK_FULL * 2], sizeof(s_mlocs[v]));
        }
    }
    glGenVertexArrays(1, &s_mesh_vao);
    memset(s_mesh_vbo_gen, 0xff, sizeof(s_mesh_vbo_gen));
    s_mesh_ok = true;
}

bool gl3d_meshes_supported(void) { return s_mesh_ok; }

static void mesh_bind(uint32_t id) {
    glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo[id]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_mesh_ibo[id]);
    GLsizei st = (GLsizei) sizeof(GxMeshVertex);
    glVertexAttribPointer(0, 4, GL_SHORT, GL_FALSE, st, (void *) 0);
    glVertexAttribPointer(1, 4, GL_SHORT, GL_FALSE, st, (void *) 8);
    glVertexAttribPointer(2, 2, GL_SHORT, GL_FALSE, st, (void *) 16);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *) 20);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *) 24);
}

static GLuint mesh_vbo(uint32_t id, uint32_t gen, uint32_t *count) {
    if (id >= MESH_SLOTS || gen != gx3d_mesh_gen()) return 0;
    const GxMesh *m = gx3d_mesh(id);
    if (!m || !m->vertex_count || !m->index_count) return 0;
    *count = m->index_count;
    if (s_mesh_vbo_gen[id] == gen) return s_mesh_vbo[id];
    if (!s_mesh_vbo[id]) glGenBuffers(1, &s_mesh_vbo[id]);
    if (!s_mesh_ibo[id]) glGenBuffers(1, &s_mesh_ibo[id]);
    glBindBuffer(GL_ARRAY_BUFFER, s_mesh_vbo[id]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (m->vertex_count * sizeof(GxMeshVertex)), m->v, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_mesh_ibo[id]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (m->index_count * sizeof(uint16_t)), m->idx, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (gen != gx3d_mesh_gen()) {
        s_mesh_vbo_gen[id] = ~0u;
        return 0;
    }
    s_mesh_vbo_gen[id] = gen;
    return s_mesh_vbo[id];
}
bool gl3d_gpu_vertices_supported(void) { return s_gpu_vertices_ok; }

void gl3d_init(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fsv[FV_COUNT];
    for (int v = 0; v < FV_COUNT; v++) {
        char *src = fs_variant(v / 2, v & 1);
        fsv[v] = compile(GL_FRAGMENT_SHADER, src);
        free(src);
    }
    s_gpu_vertices_ok = true;
    for (int k = 0; k < FV_COUNT; k++) {
        int v = (k + FK_FULL * 2) % FV_COUNT;
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fsv[v]);
        glBindAttribLocation(prog, 0, "pos");
        glBindAttribLocation(prog, 1, "color");
        glBindAttribLocation(prog, 2, "uv");
        glBindAttribLocation(prog, 3, "mode");
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            port_log("Graphics: 3D shader %d failed", v);
            if (v != FK_FULL * 2) {
                s_cprog[v] = s_cprog[FK_FULL * 2];
                memcpy(s_cl[v], s_cl[FK_FULL * 2], sizeof(s_cl[v]));
            }
            continue;
        }
        s_cprog[v] = prog;
        s_cl[v][CU_TEX_SIZE] = glGetUniformLocation(prog, "tex_size");
        s_cl[v][CU_ALPHA_REF] = glGetUniformLocation(prog, "alpha_ref");
        s_cl[v][CU_WIDE] = glGetUniformLocation(prog, "wide");
        s_cl[v][CU_HIGHLIGHT] = glGetUniformLocation(prog, "highlight");
        s_cl[v][CU_TOON] = glGetUniformLocation(prog, "toon");
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    }
    s_prog = s_cprog[FK_FULL * 2];
    s_u_tex_size = s_cl[FK_FULL * 2][CU_TEX_SIZE];
    s_u_alpha_ref = s_cl[FK_FULL * 2][CU_ALPHA_REF];
    s_u_wide = s_cl[FK_FULL * 2][CU_WIDE];
    s_u_highlight = s_cl[FK_FULL * 2][CU_HIGHLIGHT];
    s_u_toon = s_cl[FK_FULL * 2][CU_TOON];
    glUseProgram(s_prog);
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex), (void *) 16);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 20);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *) 28);
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray((GLuint) i);
    glBindVertexArray(0);
    mesh_init(fsv);
    glUseProgram(s_prog);
    calibrate_culling();
}

static void calibrate_culling(void) {
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glUseProgram(s_prog);
    glUniform1f(s_u_wide, 1.0f);
    glUniform1f(s_u_alpha_ref, -1.0f);
    glUniform1f(s_u_highlight, 0.0f);
    glUniform2f(s_u_tex_size, 1, 1);
    const float q[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    Vertex v[6];
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) v[i] = (Vertex) {q[order[i]][0], q[order[i]][1], 0, 1, 31, 31, 31, 31, 0, 0, 0};
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_CULL_FACE);
    uint8_t px[4] = {0, 0, 0, 0};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    s_front_face = px[0] > 128 ? GL_CCW : GL_CW;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}

// ------------------------------------------------------------------------------------------------ frame
typedef struct Batch {
    uint32_t first, count; // in s_verts (triangles)
    GLuint tex;
    uint32_t teximage;
    uint32_t size_bits; // TEXIMAGE_PARAM, for the texture size
    uint32_t attr;
    uint8_t viewport[4];
    uint8_t id; // polygon ID
    int32_t aspect; // of the projection
    bool translucent, shadow, shadow_mask;
    uint8_t cull;
    uint32_t mbase;
    int32_t draw;
    GLuint vbo;
    bool keep;
    uint8_t kind;
} Batch;

static Batch *s_batches;
static size_t s_batch_cap;

// Frame interpolation pairs the models of two lists. A model piece is a vertex block (BEGIN_VTXS): its polygons,
// culled ones included (GxPolygon.hidden), are the same every frame, so a block is found in the previous list by a
// signature of its structure (polygon shapes, texture formats and sizes, attributes); among blocks with the same
// structure (wheels, item boxes, trees), by where it should have been (see match_lists). A block not found, or found
// too far away (a cut), is drawn as it is, whole: the parts of a model never separate.
typedef struct Block {
    uint32_t first, count, sig;
    float cx, cy, cz, cw; // centre in clip space (sums over the vertices / their number)
    uint8_t id;           // polygon ID of its first polygon
    uint32_t obj;         // what the game was drawing (g_port_draw_id), 0 when it did not say
    uint16_t front;       // how many of its polygons face the viewer
} Block;

// Per polygon of the current list, for a paired block that turned a lot (a spinning wheel): it is moved as a whole
// from where it was, keeping its current shape (interpolating its vertices would shrink it: they cut across the
// circle).
typedef struct Rigid {
    float dx, dy, dz, dw; // previous centre - current centre
    bool on;
} Rigid;
static Rigid *s_rigid;
static size_t s_rigid_cap;
static const Gx3dFrame *s_pair_prev, *s_pair_cur;

static Block *s_blocks[2];
static size_t s_block_cap[2];
static int32_t *s_match; // per polygon of the current list: index in the previous list, or -1
static size_t s_match_cap;
static uint8_t *s_used;
static size_t s_used_cap;

static size_t find_blocks(const Gx3dFrame *f, int which) {
    size_t n = 0;
    for (uint32_t i = 0; i < f->polygon_count;) {
        uint32_t j = i + 1;
        while (j < f->polygon_count && f->polygons[j].tag != 0) j++;
        if (n + 1 > s_block_cap[which]) {
            s_block_cap[which] = (n + 1) * 2;
            s_blocks[which] = realloc(s_blocks[which], s_block_cap[which] * sizeof(Block));
        }
        Block *b = &s_blocks[which][n++];
        b->first = i;
        b->count = j - i;
        uint32_t h = 0x811c9dc5u ^ (j - i);
        double sx = 0, sy = 0, sz = 0, sw = 0;
        int nv = 0;
        for (uint32_t k = i; k < j; k++) {
            const GxPolygon *p = &f->polygons[k];
            // The structure only: animated textures (texture coordinates scrolling, frames of a texture animation,
            // palette changes) must not make a piece unrecognisable from one frame to the next.
            h = (h ^ p->count) * 0x01000193u;
            h = (h ^ (p->teximage & 0x3ff00000u)) * 0x01000193u; // format, size, repeat, flip, colour 0
            // Without alpha and polygon ID: the game gives item boxes a new polygon ID every frame.
            h = (h ^ (p->attr & ~0x3f1f0000u)) * 0x01000193u;
            for (int v = 0; v < p->count; v++) {
                const GxVertex *g = &f->vertices[p->first + v];
                sx += g->x;
                sy += g->y;
                sz += g->z;
                sw += g->w;
                nv++;
            }
        }
        b->sig = h;
        b->cx = (float) (sx / nv);
        b->cy = (float) (sy / nv);
        b->cz = (float) (sz / nv);
        b->id = (uint8_t) ((f->polygons[i].attr >> 24) & 63);
        b->obj = f->polygons[i].obj;
        b->front = 0;
        for (uint32_t k = i; k < j; k++) b->front += f->polygons[k].front;
        b->cw = (float) (sw / nv);
        i = j;
    }
    return n;
}

static const Block *s_sort_blocks;
static int cmp_block_sig(const void *a, const void *b) {
    uint32_t x = s_sort_blocks[*(const uint32_t *) a].sig, y = s_sort_blocks[*(const uint32_t *) b].sig;
    return x < y ? -1 : x > y;
}

static uint32_t *s_order[4];
static size_t s_order_cap[4];

static uint32_t *sorted_by_sig(const Block *blocks, size_t n, int which) {
    if (n + 1 > s_order_cap[which]) {
        s_order_cap[which] = (n + 1) * 2;
        s_order[which] = realloc(s_order[which], s_order_cap[which] * sizeof(uint32_t));
    }
    for (size_t i = 0; i < n; i++) s_order[which][i] = (uint32_t) i;
    s_sort_blocks = blocks;
    qsort(s_order[which], n, sizeof(uint32_t), cmp_block_sig);
    return s_order[which];
}

static float block_distance(const Block *a, const Block *b) {
    return fabsf(a->cx - b->cx) + fabsf(a->cy - b->cy) + fabsf(a->cw - b->cw);
}

// Pairs the blocks of the two lists.
// - A structure found once in each list (most of the scenery, each kart) is its own pair: an anchor, whose move from
//   one list to the other is known.
// - Among identical blocks (item boxes, trees, wheels, posts), where each one was in the previous list is predicted
//   from the move of the nearest anchor (the camera's motion at that place), and the pairs are taken by distance to
//   the prediction, nearest first. Neither the order of the list (the game sorts item boxes by distance, which swaps
//   them) nor the plain nearest block (the camera moves farther than identical pieces are apart) can tell them apart.
typedef struct Pair {
    float d;
    uint32_t c, p;
} Pair;
static Pair *s_pairs;
static size_t s_pair_cap;
static int cmp_pair(const void *a, const void *b) {
    float x = ((const Pair *) a)->d, y = ((const Pair *) b)->d;
    return x < y ? -1 : x > y;
}

typedef struct Anchor {
    float cx, cy, cw, dx, dy, dw;
} Anchor;
static Anchor *s_anchors;
static size_t s_anchor_cap;

static void pair_blocks(const Block *pb, const Block *cb, uint32_t c, uint32_t p) {
    const Block *b = &cb[c], *a = &pb[p];
    for (uint32_t k = 0; k < b->count; k++) s_match[b->first + k] = (int32_t) (a->first + k);
    // How much the piece changed shape about its centre, against its size.
    double dev = 0, rad = 0;
    for (uint32_t k = 0; k < b->count; k++) {
        const GxPolygon *cp = &s_pair_cur->polygons[b->first + k], *pp = &s_pair_prev->polygons[a->first + k];
        for (int v = 0; v < cp->count && v < pp->count; v++) {
            const GxVertex *g = &s_pair_cur->vertices[cp->first + v], *o = &s_pair_prev->vertices[pp->first + v];
            float rx = g->x - b->cx, ry = g->y - b->cy, rz = g->z - b->cz;
            float ox = o->x - a->cx, oy = o->y - a->cy, oz = o->z - a->cz;
            dev += fabsf(rx - ox) + fabsf(ry - oy) + fabsf(rz - oz);
            rad += fabsf(rx) + fabsf(ry) + fabsf(rz);
        }
    }
    bool rigid = rad > 0 && dev > rad * 0.6;
    Rigid r = {a->cx - b->cx, a->cy - b->cy, a->cz - b->cz, a->cw - b->cw, rigid};
    for (uint32_t k = 0; k < b->count; k++) s_rigid[b->first + k] = r;
}

static int cmp_block_obj(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    uint32_t ox = s_sort_blocks[x].obj, oy = s_sort_blocks[y].obj;
    if (ox != oy) return ox < oy ? -1 : 1;
    return x < y ? -1 : x > y;
}

// Blocks of the list ordered by draw identity, then by place in the list (into s_order[2 + which]).
static uint32_t *sorted_by_obj(const Block *blocks, size_t n, int which) {
    which += 2;
    if (n + 1 > s_order_cap[which]) {
        s_order_cap[which] = (n + 1) * 2;
        s_order[which] = realloc(s_order[which], s_order_cap[which] * sizeof(uint32_t));
    }
    for (size_t i = 0; i < n; i++) s_order[which][i] = (uint32_t) i;
    s_sort_blocks = blocks;
    qsort(s_order[which], n, sizeof(uint32_t), cmp_block_obj);
    return s_order[which];
}

// Pairs the blocks of the two lists.
// 1. By identity: the game says which object it draws (g_port_draw_id: each kart, item box, map object, name tag,
//    the course); an object draws its pieces in the same order every frame, so the n-th block of an object is paired
//    with the n-th block of the same object in the previous list (when the structure is the same). No guessing.
// 2. What is left (no identity: menus, effects, the sky; or a piece that changed): a structure found once in each
//    list is its own pair; among identical blocks, where each one was is predicted from the move of the nearest
//    paired piece, and the pairs are taken by distance to the prediction, nearest first.
static void match_lists(const Gx3dFrame *prev, const Gx3dFrame *f) {
    size_t np = find_blocks(prev, 0), nc = find_blocks(f, 1);
    if (f->polygon_count + 1 > s_match_cap) {
        s_match_cap = (f->polygon_count + 1) * 2;
        s_match = realloc(s_match, s_match_cap * sizeof(*s_match));
    }
    if (np + nc + 1 > s_used_cap) {
        s_used_cap = (np + nc + 1) * 2;
        s_used = realloc(s_used, s_used_cap);
    }
    if (nc + 1 > s_anchor_cap) {
        s_anchor_cap = (nc + 1) * 2;
        s_anchors = realloc(s_anchors, s_anchor_cap * sizeof(Anchor));
    }
    if (f->polygon_count + 1 > s_rigid_cap) {
        s_rigid_cap = (f->polygon_count + 1) * 2;
        s_rigid = realloc(s_rigid, s_rigid_cap * sizeof(Rigid));
    }
    s_pair_prev = prev;
    s_pair_cur = f;
    for (uint32_t i = 0; i < f->polygon_count; i++) s_match[i] = -1;
    const Block *pb = s_blocks[0], *cb = s_blocks[1];
    uint8_t *used_p = s_used, *used_c = s_used + np; // by block index
    memset(s_used, 0, np + nc);
    size_t na = 0, judged = 0, jumped = 0;

    // 1. Identity.
    const uint32_t *pq = sorted_by_obj(pb, np, 0), *cq = sorted_by_obj(cb, nc, 1);
    for (size_t pi = 0, ci = 0; pi < np && ci < nc;) {
        uint32_t po_ = pb[pq[pi]].obj, co_ = cb[cq[ci]].obj;
        if (po_ != co_) {
            if (po_ < co_) pi++;
            else ci++;
            continue;
        }
        size_t pe = pi, ce = ci;
        while (pe < np && pb[pq[pe]].obj == po_) pe++;
        while (ce < nc && cb[cq[ce]].obj == co_) ce++;
        if (po_ != 0) {
            for (size_t r = 0; r < pe - pi && r < ce - ci; r++) {
                const Block *a = &pb[pq[pi + r]], *b = &cb[cq[ci + r]];
                if (a->sig != b->sig) continue;
                float d = block_distance(a, b);
                judged++;
                if (d > (fabsf(b->cw) + 4096.0f) * 0.3f) jumped++;
                if (d > fmaxf(fabsf(a->cw), fabsf(b->cw)) + 4096.0f) continue; // a cut
                used_p[pq[pi + r]] = used_c[cq[ci + r]] = 1;
                pair_blocks(pb, cb, cq[ci + r], pq[pi + r]);
                s_anchors[na++] = (Anchor) {b->cx, b->cy, b->cw, b->cx - a->cx, b->cy - a->cy, b->cw - a->cw};
            }
        }
        pi = pe;
        ci = ce;
    }

    // 2. The rest, grouped by structure.
    const uint32_t *po = sorted_by_sig(pb, np, 0), *co = sorted_by_sig(cb, nc, 1);
    for (size_t pi = 0, ci = 0; pi < np && ci < nc;) {
        uint32_t ps = pb[po[pi]].sig, cs = cb[co[ci]].sig;
        if (ps != cs) {
            if (ps < cs) pi++;
            else ci++;
            continue;
        }
        size_t pe = pi, ce = ci, fp = 0, fc = 0, lp = 0, lc = 0;
        while (pe < np && pb[po[pe]].sig == ps) {
            if (!used_p[po[pe]]) fp++, lp = pe;
            pe++;
        }
        while (ce < nc && cb[co[ce]].sig == cs) {
            if (!used_c[co[ce]]) fc++, lc = ce;
            ce++;
        }
        if (fp == 1 && fc == 1) {
            const Block *a = &pb[po[lp]], *b = &cb[co[lc]];
            float d = block_distance(a, b);
            judged++;
            if (d > (fabsf(b->cw) + 4096.0f) * 0.3f) jumped++;
            if (d <= fmaxf(fabsf(a->cw), fabsf(b->cw)) + 4096.0f) {
                used_p[po[lp]] = used_c[co[lc]] = 1;
                pair_blocks(pb, cb, co[lc], po[lp]);
                s_anchors[na++] = (Anchor) {b->cx, b->cy, b->cw, b->cx - a->cx, b->cy - a->cy, b->cw - a->cw};
            }
        }
        pi = pe;
        ci = ce;
    }

    // A camera cut (the course intros change shots): most pieces jumped. Nothing is interpolated then, or the frame
    // would show everything half way between two shots.
    if (judged >= 8 && jumped * 2 > judged) {
        for (uint32_t i = 0; i < f->polygon_count; i++) s_match[i] = -1;
        return;
    }

    // Identical blocks left.
    for (size_t pi = 0, ci = 0; pi < np && ci < nc;) {
        uint32_t ps = pb[po[pi]].sig, cs = cb[co[ci]].sig;
        if (ps != cs) {
            if (ps < cs) pi++;
            else ci++;
            continue;
        }
        size_t pe = pi, ce = ci;
        while (pe < np && pb[po[pe]].sig == ps) pe++;
        while (ce < nc && cb[co[ce]].sig == cs) ce++;
        size_t n = (pe - pi) * (ce - ci);
        if (n > s_pair_cap) {
            s_pair_cap = n * 2;
            s_pairs = realloc(s_pairs, s_pair_cap * sizeof(Pair));
        }
        size_t k = 0;
        for (size_t c = ci; c < ce; c++) {
            if (used_c[co[c]]) continue;
            const Block *b = &cb[co[c]];
            // Where it was: moved back by the motion of the nearest paired piece.
            float px = b->cx, py = b->cy, pw = b->cw, best = -1;
            for (size_t a = 0; a < na; a++) {
                const Anchor *an = &s_anchors[a];
                float d = fabsf(an->cx - b->cx) + fabsf(an->cy - b->cy) + fabsf(an->cw - b->cw);
                if (best < 0 || d < best) {
                    best = d;
                    px = b->cx - an->dx;
                    py = b->cy - an->dy;
                    pw = b->cw - an->dw;
                }
            }
            for (size_t p = pi; p < pe; p++) {
                if (used_p[po[p]]) continue;
                const Block *a = &pb[po[p]];
                float d = fabsf(a->cx - px) + fabsf(a->cy - py) + fabsf(a->cw - pw);
                float penalty = fmaxf(fabsf(a->cw), fabsf(b->cw)) * 0.25f + 1024.0f;
                if (a->obj != b->obj) d += penalty;
                if (a->id != b->id) d += penalty; // each kart's pieces have their own polygon ID
                if (a->front != b->front) d += penalty; // pieces back to back: the one facing the same way
                s_pairs[k++] = (Pair) {d, (uint32_t) co[c], (uint32_t) po[p]};
            }
        }
        qsort(s_pairs, k, sizeof(Pair), cmp_pair);
        for (size_t i = 0; i < k; i++) {
            const Pair *q = &s_pairs[i];
            if (used_c[q->c] || used_p[q->p]) continue;
            const Block *a = &pb[q->p], *b = &cb[q->c];
            // A cut, or a piece that came or went: too far from where it should have been.
            if (q->d > fmaxf(fabsf(a->cw), fabsf(b->cw)) * 0.5f + 4096.0f) continue;
            used_c[q->c] = used_p[q->p] = 1;
            pair_blocks(pb, cb, q->c, q->p);
        }
        pi = pe;
        ci = ce;
    }
}

// A polygon seen in one of the two frames and culled or out of view in the other (an edge-on side turning away, a
// piece leaving the screen): whether it shows is decided on its interpolated shape, as the DS decides on the frame's,
// or it would blink at the in-between frames.
static bool interp_visible(const Gx3dFrame *f, const GxPolygon *p, const Gx3dFrame *prev, const GxPolygon *pp, float t) {
    float v[8][4];
    int n = p->count < 8 ? p->count : 8;
    const Rigid *r = &s_rigid[p - f->polygons];
    for (int k = 0; k < n; k++) {
        const GxVertex *g = &f->vertices[p->first + k], *o = &prev->vertices[pp->first + k];
        if (r->on) {
            v[k][0] = g->x + r->dx * (1 - t);
            v[k][1] = g->y + r->dy * (1 - t);
            v[k][2] = g->z + r->dz * (1 - t);
            v[k][3] = g->w + r->dw * (1 - t);
        } else {
            v[k][0] = o->x + (g->x - o->x) * t;
            v[k][1] = o->y + (g->y - o->y) * t;
            v[k][2] = o->z + (g->z - o->z) * t;
            v[k][3] = o->w + (g->w - o->w) * t;
        }
    }
    // Entirely outside one clip plane.
    for (int plane = 0; plane < 6; plane++) {
        int outside = 0;
        for (int k = 0; k < n; k++) {
            float c = v[k][plane >> 1];
            if (plane & 1 ? c > v[k][3] : c < -v[k][3]) outside++;
        }
        if (outside == n) return false;
    }
    // Facing, as gx3d.c: winding in homogeneous coordinates.
    double d = (double) v[0][0] * ((double) v[1][1] * v[2][3] - (double) v[1][3] * v[2][1]) -
               (double) v[0][1] * ((double) v[1][0] * v[2][3] - (double) v[1][3] * v[2][0]) +
               (double) v[0][3] * ((double) v[1][0] * v[2][1] - (double) v[1][1] * v[2][0]);
    if (d == 0 && n == 4) {
        d = (double) v[0][0] * ((double) v[2][1] * v[3][3] - (double) v[2][3] * v[3][1]) -
            (double) v[0][1] * ((double) v[2][0] * v[3][3] - (double) v[2][3] * v[3][0]) +
            (double) v[0][3] * ((double) v[2][0] * v[3][1] - (double) v[2][1] * v[3][0]);
    }
    bool front = d >= 0;
    return front ? (p->attr & 0x80) != 0 : (p->attr & 0x40) != 0;
}

static float s_target_aspect = 4.0f / 3.0f; // of the render target

typedef float f32x4 __attribute__((vector_size(16)));

static int det3f(const float *a, const float *b, const float *c) {
    double d = (double) a[0] * ((double) b[1] * c[2] - (double) b[2] * c[1]) -
               (double) a[1] * ((double) b[0] * c[2] - (double) b[2] * c[0]) +
               (double) a[2] * ((double) b[0] * c[1] - (double) b[1] * c[0]);
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void push_polygon(const Gx3dFrame *f, const GxPolygon *p, Vertex **out, GLuint tex, const Gx3dFrame *prev,
                         const GxPolygon *pp, float t, uint32_t mbase) {
    // A polygon keeps the shape its projection gives it: one projected for 4:3 drawn over the whole wide screen is
    // narrowed about the centre (it may lie beyond the 4:3 part: name tags over karts). Partial viewports: below.
    float xs = 1.0f;
    if (p->viewport[0] == 0 && p->viewport[2] == 255) {
        xs = (p->aspect > 0 ? p->aspect / 4096.0f : 4.0f / 3.0f) / s_target_aspect;
        if (xs > 1.0f) xs = 1.0f;
    }

    uint32_t alpha = (p->attr >> 16) & 31;
    if (alpha == 0) alpha = 31; // wireframe: drawn filled
    uint32_t mode = (p->attr >> 4) & 3;
    float m = (float) mode + (tex ? 4.0f : 0.0f);
    Vertex v[4];
    if (f->gpu_vertices) {
        (void) mbase;
        float cxyw[4][3];
        f32x4 c[4];
        for (int k = 0; k < p->count; k++) {
            const GxVertex *g = &f->vertices[p->first + k];
            const float *mt = f->matrices[(uint32_t) g->w < f->matrix_count ? (uint32_t) g->w : 0];
            f32x4 c0, c1, c2, c3;
            memcpy(&c0, mt, 16);
            memcpy(&c1, mt + 4, 16);
            memcpy(&c2, mt + 8, 16);
            memcpy(&c3, mt + 12, 16);
            float x = g->x * (1.0f / 4096.0f), y = g->y * (1.0f / 4096.0f), z = g->z * (1.0f / 4096.0f);
            c[k] = c0 * x + c1 * y + c2 * z + c3;
            cxyw[k][0] = c[k][0];
            cxyw[k][1] = c[k][1];
            cxyw[k][2] = c[k][3];
        }
        int sign = det3f(cxyw[0], cxyw[1], cxyw[2]);
        if (sign == 0 && p->count == 4) sign = det3f(cxyw[0], cxyw[2], cxyw[3]);
        bool front = sign >= 0;
        if ((front && !(p->attr & 0x80)) || (!front && !(p->attr & 0x40))) return;
        for (int k = 0; k < p->count; k++) {
            const GxVertex *g = &f->vertices[p->first + k];
            v[k] = (Vertex) {c[k][0] * xs, c[k][1], c[k][2], c[k][3], g->r, g->g, g->b, (uint8_t) alpha, g->s, g->t, m};
        }
        for (int k = 1; k + 1 < p->count; k++) {
            *(*out)++ = v[0];
            *(*out)++ = v[k];
            *(*out)++ = v[k + 1];
        }
        return;
    }
    for (int k = 0; k < p->count; k++) {
        const GxVertex *g = &f->vertices[p->first + k];
        float x = (float) g->x, y = (float) g->y, z = (float) g->z, w = (float) g->w;
        if (pp && s_rigid[p - f->polygons].on) {
            const Rigid *r = &s_rigid[p - f->polygons];
            x += r->dx * (1.0f - t);
            y += r->dy * (1.0f - t);
            z += r->dz * (1.0f - t);
            w += r->dw * (1.0f - t);
        } else if (pp) {
            const GxVertex *o = &prev->vertices[pp->first + k];
            x = o->x + (x - o->x) * t;
            y = o->y + (y - o->y) * t;
            z = o->z + (z - o->z) * t;
            w = o->w + (w - o->w) * t;
        }
        v[k] = (Vertex) {x * xs / 4096.0f, y / 4096.0f, z / 4096.0f, w / 4096.0f,
                         g->r, g->g, g->b, (uint8_t) alpha, g->s, g->t, m};
    }
    // Triangle fan.
    for (int k = 1; k + 1 < p->count; k++) {
        *(*out)++ = v[0];
        *(*out)++ = v[k];
        *(*out)++ = v[k + 1];
    }
}


static float s_frame_alpha_ref, s_frame_highlight, s_frame_toon[32 * 3];

static void use_mesh_program(int v) {
    s_mcur = v;
    glUseProgram(s_mprog[v]);
    if (s_mframe[v] != s_frame) {
        s_mframe[v] = s_frame;
        glUniform1f(s_mlocs[v][MU_ALPHA_REF], s_frame_alpha_ref);
        glUniform1f(s_mlocs[v][MU_HIGHLIGHT], s_frame_highlight);
        glUniform3fv(s_mlocs[v][MU_TOON], 32, s_frame_toon);
        s_mlight[v] = -1;
    }
}

static void use_cpu_program(int v) {
    glUseProgram(s_cprog[v]);
    if (s_cframe[v] != s_frame) {
        s_cframe[v] = s_frame;
        glUniform1f(s_cl[v][CU_WIDE], 1.0f);
        glUniform1f(s_cl[v][CU_ALPHA_REF], s_frame_alpha_ref);
        glUniform1f(s_cl[v][CU_HIGHLIGHT], s_frame_highlight);
        glUniform3fv(s_cl[v][CU_TOON], 32, s_frame_toon);
    }
}

static void mesh_light_uniforms(const GxLightState *l) {
    float v[4][3];
    glUniform3f(s_mlocs[s_mcur][MU_EMI], (float) l->emi9[0], (float) l->emi9[1], (float) l->emi9[2]);
#define LIGHT_ARRAY(field, u)                                                                                          \
    for (int k = 0; k < 4; k++)                                                                                        \
        for (int j = 0; j < 3; j++) v[k][j] = (float) l->field[k][j];                                                  \
    glUniform3fv(s_mlocs[s_mcur][u], 4, &v[0][0]);
    LIGHT_ARRAY(dir, MU_LDIR)
    LIGHT_ARRAY(half, MU_LHALF)
    LIGHT_ARRAY(spe_lc, MU_LSPE)
    LIGHT_ARRAY(dif_lc, MU_LDIF)
    LIGHT_ARRAY(amb, MU_LAMB)
#undef LIGHT_ARRAY
    glUniform1f(s_mlocs[s_mcur][MU_USE_TABLE], l->use_table ? 1.0f : 0.0f);
    if (l->use_table) {
        float sh[128];
        for (int i = 0; i < 128; i++) sh[i] = l->shininess[i];
        glUniform1fv(s_mlocs[s_mcur][MU_SHIN], 128, sh);
    }
}

static void mesh_draw(const Gx3dFrame *f, const Batch *b) {
    const GxDraw *d = &f->draws[b->draw];
    const float *mt = f->matrices[d->mtx < f->matrix_count ? d->mtx : 0];
    glUniform4fv(s_mlocs[s_mcur][MU_M0], 1, mt);
    glUniform4fv(s_mlocs[s_mcur][MU_M1], 1, mt + 4);
    glUniform4fv(s_mlocs[s_mcur][MU_M2], 1, mt + 8);
    glUniform4fv(s_mlocs[s_mcur][MU_M3], 1, mt + 12);
    float xs = 1.0f;
    if (d->viewport[0] == 0 && d->viewport[2] == 255) {
        xs = (d->aspect > 0 ? d->aspect / 4096.0f : 4.0f / 3.0f) / s_target_aspect;
        if (xs > 1.0f) xs = 1.0f;
    }
    glUniform1f(s_mlocs[s_mcur][MU_XS], xs);
    uint32_t alpha = (d->attr >> 16) & 31;
    if (alpha == 0) alpha = 31;
    glUniform4f(s_mlocs[s_mcur][MU_COLOR], d->color[0], d->color[1], d->color[2], (float) alpha);
    glUniform2f(s_mlocs[s_mcur][MU_ST], d->s, d->t);
    glUniform1f(s_mlocs[s_mcur][MU_TEXMODE], (float) d->texmode);
    glUniform4f(s_mlocs[s_mcur][MU_TM], (float) d->tm[0], (float) d->tm[1], (float) d->tm[2], (float) d->tm[3]);
    glUniform2f(s_mlocs[s_mcur][MU_TM2], (float) d->tm[4], (float) d->tm[5]);
    glUniform1f(s_mlocs[s_mcur][MU_MODE], (float) ((d->attr >> 4) & 3) + (b->tex ? 4.0f : 0.0f));
    if (b->tex) glUniform2f(s_mlocs[s_mcur][MU_TEX_SIZE], (float) TEX_WIDTH(b->size_bits), (float) TEX_HEIGHT(b->size_bits));
    else glUniform2f(s_mlocs[s_mcur][MU_TEX_SIZE], 1, 1);
    if (d->light != 0xffff && d->light < f->light_count) {
        glUniform4f(s_mlocs[s_mcur][MU_MASK], d->mask & 1 ? 1.0f : 0.0f, d->mask & 2 ? 1.0f : 0.0f, d->mask & 4 ? 1.0f : 0.0f,
                    d->mask & 8 ? 1.0f : 0.0f);
        glUniform3f(s_mlocs[s_mcur][MU_VC0], (float) d->vec[0], (float) d->vec[3], (float) d->vec[6]);
        glUniform3f(s_mlocs[s_mcur][MU_VC1], (float) d->vec[1], (float) d->vec[4], (float) d->vec[7]);
        glUniform3f(s_mlocs[s_mcur][MU_VC2], (float) d->vec[2], (float) d->vec[5], (float) d->vec[8]);
        if (s_mlight[s_mcur] != d->light) {
            mesh_light_uniforms(&f->lights[d->light]);
            s_mlight[s_mcur] = d->light;
        }
    }
    bool front = d->attr & 0x80, back = d->attr & 0x40;
    if (front && back) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glFrontFace(s_front_face);
        glCullFace(front ? GL_BACK : GL_FRONT);
    }
    mesh_bind(d->mesh);
    glDrawElements(GL_TRIANGLES, (GLsizei) b->count, GL_UNSIGNED_SHORT, (void *) 0);
}

uint32_t gl3d_render(const Gx3dFrame *f, const Gx3dFrame *prev, float t, const FrameSnapshot *snap, int scale,
                     float wide) {
    s_frame_decode_ns = 0;
    int w = (int) (256 * scale * wide + 0.5f), h = 192 * scale;
    float sx = w / 256.0f; // horizontal pixels per DS pixel
    s_target_aspect = 4.0f / 3.0f * wide;
    if (prev && (t >= 1.0f || prev->vertex_count == 0 || f->gpu_vertices || prev->gpu_vertices)) prev = NULL;
    if (prev) match_lists(prev, f);
    ensure_target(w, h);
    VramMap map;
    vram_map_build(&map, snap->vramcnt);
    bool texturing = f->disp3dcnt & 1;
    s_frame++;
    s_vram_version = snap->vram_version;
    s_vram_newest = tex_newest_version(snap->vram_version);
    s_map_sig = tex_map_signature(&map);

    size_t need = (size_t) f->polygon_count * 6 + 6;
    if (need > s_vert_cap) {
        s_vert_cap = need * 2;
        s_verts = realloc(s_verts, s_vert_cap * sizeof(Vertex));
    }
    if (f->polygon_count + f->draw_count + 1 > s_batch_cap) {
        s_batch_cap = (f->polygon_count + f->draw_count + 1) * 2;
        s_batches = realloc(s_batches, s_batch_cap * sizeof(Batch));
    }

    // Opaque polygons first, translucent ones after, each in list order; consecutive polygons with the same state
    // form one draw call.
    size_t nb = 0;
    Vertex *out = s_verts;
    bool any_shadow = false;
    for (uint32_t i = 0; i < f->polygon_count && !any_shadow; i++) any_shadow = ((f->polygons[i].attr >> 4) & 3) == 3;
    bool alpha_ref_on = (f->disp3dcnt & 4) != 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t di = 0;
        for (uint32_t i = 0; i <= f->polygon_count; i++) {
            for (; di < f->draw_count && (f->draws[di].at <= i || i == f->polygon_count); di++) {
                const GxDraw *d = &f->draws[di];
                uint32_t alpha = (d->attr >> 16) & 31;
                uint32_t fmt = (d->teximage >> 26) & 7;
                bool translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6;
                if (translucent != (pass == 1)) continue;
                uint32_t count = 0;
                GLuint vbo = s_mesh_ok ? mesh_vbo(d->mesh, d->gen, &count) : 0;
                if (!vbo || !(d->attr & 0xc0)) continue;
                GLuint tex = texturing ? texture(&map, snap->vram, d->teximage, d->pltt) : 0;
                uint8_t id = (uint8_t) ((d->attr >> 24) & 63);
                if (!translucent && !any_shadow) id = 0;
                Batch *b = &s_batches[nb++];
                memset(b, 0, sizeof(*b));
                b->first = 0;
                b->count = count;
                b->tex = tex;
                b->teximage = d->teximage & 0x000f0000;
                b->size_bits = d->teximage;
                b->attr = d->attr & 0x0000c800;
                memcpy(b->viewport, d->viewport, 4);
                b->translucent = translucent;
                b->id = id;
                b->aspect = d->aspect;
                b->cull = 3;
                b->draw = (int32_t) di;
                b->vbo = vbo;
                b->keep = fs_keep(translucent, tex, d->attr, d->teximage, alpha_ref_on, f->alpha_ref);
                b->kind = (uint8_t) fs_kind(tex, d->attr);
            }
            if (i == f->polygon_count) break;
            const GxPolygon *p = &f->polygons[i];
            const GxPolygon *ip = prev && s_match[i] >= 0 ? &prev->polygons[s_match[i]] : NULL;
            if (ip && p->hidden != ip->hidden) {
                if (!interp_visible(f, p, prev, ip, t)) continue;
            } else if (p->hidden) {
                continue;
            }
            uint32_t alpha = (p->attr >> 16) & 31;
            uint32_t fmt = (p->teximage >> 26) & 7;
            bool shadow = ((p->attr >> 4) & 3) == 3;
            bool translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6 || shadow;
            if (translucent != (pass == 1)) continue;
            GLuint tex = texturing ? texture(&map, snap->vram, p->teximage, p->pltt) : 0;
            Batch *b = nb ? &s_batches[nb - 1] : NULL;
            uint8_t cull = 3;
            uint32_t state_attr = p->attr & 0x0000c800; // depth equal, depth write of translucent, ...
            uint8_t id = (uint8_t) ((p->attr >> 24) & 63);
            if (!translucent && !any_shadow) id = 0;
            bool shadow_mask = shadow && id == 0;
            uint8_t kind = (uint8_t) fs_kind(tex, p->attr);
            bool keep = !shadow && fs_keep(translucent, tex, p->attr, p->teximage, alpha_ref_on, f->alpha_ref);
            if (!b || b->draw >= 0 || b->kind != kind || b->keep != keep || b->tex != tex || b->teximage != (p->teximage & 0x000f0000) || b->size_bits != p->teximage || b->attr != state_attr ||
                memcmp(b->viewport, p->viewport, 4) != 0 || b->translucent != translucent || b->shadow != shadow ||
                b->shadow_mask != shadow_mask || b->id != id || b->aspect != p->aspect || b->cull != cull) {
                b = &s_batches[nb++];
                b->first = (uint32_t) (out - s_verts);
                b->count = 0;
                b->tex = tex;
                b->teximage = p->teximage & 0x000f0000;
                b->size_bits = p->teximage;
                b->attr = state_attr;
                memcpy(b->viewport, p->viewport, 4);
                b->translucent = translucent;
                b->shadow = shadow;
                b->shadow_mask = shadow_mask;
                b->id = id;
                b->aspect = p->aspect;
                b->cull = cull;
                b->draw = -1;
                b->vbo = 0;
                b->kind = kind;
                b->keep = keep;
            }
            Vertex *start = out;
            const GxPolygon *pp = prev && s_match[i] >= 0 ? &prev->polygons[s_match[i]] : NULL;
            push_polygon(f, p, &out, tex, prev, pp, t, 0);
            b->count += (uint32_t) (out - start);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    uint32_t cc = f->clear_color;
    glClearColor((cc & 31) / 31.0f, ((cc >> 5) & 31) / 31.0f, ((cc >> 10) & 31) / 31.0f, ((cc >> 16) & 31) / 31.0f);
    glClearDepthf(((f->clear_depth & 0x7fff) * 0x200 + 0x1ff) / (float) 0xffffff);
    glClearStencil((GLint) ((cc >> 24) & 63)); // the stencil keeps the opaque polygon ID (bits 0-5), shadow mark bit 7
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    float alpha_ref = (f->disp3dcnt & 4) ? f->alpha_ref / 31.0f : 0.0f;
    s_frame_alpha_ref = alpha_ref > 0 ? alpha_ref : -1.0f;
    s_frame_highlight = (f->disp3dcnt & 2) != 0 ? 1.0f : 0.0f;
    for (int i = 0; i < 32; i++) {
        uint16_t c = f->toon_table[i];
        s_frame_toon[i * 3] = (c & 31) / 31.0f;
        s_frame_toon[i * 3 + 1] = ((c >> 5) & 31) / 31.0f;
        s_frame_toon[i * 3 + 2] = ((c >> 10) & 31) / 31.0f;
    }
    int cur_prog = -1;
    bool on_mesh = false;
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) ((out - s_verts) * sizeof(Vertex)), s_verts, GL_STREAM_DRAW);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    bool blending = f->disp3dcnt & 8;

    state_reset();
    for (size_t i = 0; i < nb; i++) {
        Batch *b = &s_batches[i];
        if (!b->count) continue;
        int want = b->kind * 2 + (b->keep ? 1 : 0);
        if (b->draw >= 0) {
            if (!on_mesh) {
                glBindVertexArray(s_mesh_vao);
                for (int k = 0; k < 5; k++) glEnableVertexAttribArray((GLuint) k);
                on_mesh = true;
            }
            if (cur_prog != want + FV_COUNT) {
                cur_prog = want + FV_COUNT;
                use_mesh_program(want);
            }
        } else {
            if (on_mesh) {
                glBindVertexArray(s_vao);
                glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
                glDisable(GL_CULL_FACE);
                on_mesh = false;
            }
            if (cur_prog != want) {
                cur_prog = want;
                use_cpu_program(want);
            }
        }
        int vx = b->viewport[0], vy = b->viewport[1];
        int vw = b->viewport[2] - vx + 1, vh = b->viewport[3] - vy + 1;
        // A polygon keeps the shape its projection gives it: a view projected for 4:3 on a wider target takes the
        // centred 4:3 part of it.
        float xs = 1.0f;
        if (!(b->viewport[0] == 0 && b->viewport[2] == 255)) { // a partial viewport: its 4:3 place, centred
            xs = (b->aspect > 0 ? b->aspect / 4096.0f : 4.0f / 3.0f) / s_target_aspect;
            if (xs > 1.0f) xs = 1.0f;
        }
        float left = (w - w * xs) / 2;
        st_viewport((GLint) (left + vx * sx * xs + 0.5f), vy * scale, (GLsizei) (vw * sx * xs + 0.5f), vh * scale);
        // Depth "equal" (decals such as the karts' emblems): the DS accepts depths within a small margin; a
        // polygon offset towards the viewer with less-or-equal does the same.
        bool equal = b->attr & (1u << 14);
        st_depth_func(equal ? GL_LEQUAL : GL_LESS);
        st_offset(equal);
        if (b->tex) {
            if (S.tex != (int) b->tex) {
                glBindTexture(GL_TEXTURE_2D, b->tex);
                S.tex = (int) b->tex;
            }
            set_wrap_of(b->tex, b->teximage);
            if (b->draw < 0)
                glUniform2f(s_cl[want][CU_TEX_SIZE], (float) TEX_WIDTH(b->size_bits), (float) TEX_HEIGHT(b->size_bits));
        } else if (b->draw < 0) {
            glUniform2f(s_cl[want][CU_TEX_SIZE], 1, 1);
        }
        if (b->shadow) {
            // Shadow volumes: the mask (polygon ID 0) marks the stencil (bit 7) where the depth test fails; a shadow
            // polygon then draws where the mark is, except on pixels of opaque polygons with its own ID, and clears it.
            st_stencil(1);
            st_depth_mask(0);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            st_stencil_mask(0x80);
            if (b->shadow_mask) {
                st_stencil_func(GL_ALWAYS, 0x80, 0xff);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
                S.stencil_op = -1;
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
            } else {
                st_stencil_func(GL_EQUAL, 0x80 | b->id, 0xbf); // own ID: no shadow
                st_stencil_op_replace(0);
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                st_blend(1);
                st_stencil_func(GL_EQUAL, 0x80, 0x80);
                glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            st_stencil_mask(0xff);
            continue;
        }
        st_stencil(1);
        st_stencil_mask(0x7f);
        if (!b->translucent) {
            // Opaque polygons record their ID for the shadows.
            st_stencil_func(GL_ALWAYS, b->id, 0xff);
        } else {
            // A translucent pixel is not drawn over a translucent pixel of the same polygon ID (the DS keeps that ID
            // with bit 6 set); it records its own.
            st_stencil_func(GL_NOTEQUAL, 0x40 | b->id, 0x7f);
        }
        st_stencil_op_replace(1);
        if (b->translucent && blending) {
            st_blend(1);
            st_depth_mask((b->attr & (1u << 11)) ? 1 : 0);
        } else {
            st_blend(0);
            st_depth_mask(1);
        }
        if (b->draw >= 0) {
            mesh_draw(f, b);
            continue;
        }
        glDrawArrays(GL_TRIANGLES, (GLint) b->first, (GLsizei) b->count);
    }
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glStencilMask(0xff);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return s_color;
}

void gl3d_capture_layer(const Gx3dFrame *frame, const FrameSnapshot *snap, uint32_t *out) {
    static uint32_t pixels[256 * 192];
    gl3d_use_target(2);
    gl3d_render(frame, NULL, 1.0f, snap, 1, 1.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl3d_use_target(0);
    for (int y = 0; y < 192; y++) {
        const uint32_t *src = pixels + (191 - y) * 256;
        uint32_t *dst = out + y * 256;
        for (int x = 0; x < 256; x++) {
            uint32_t c = src[x], a = c >> 24;
            dst[x] = (c & 0xffffffu) | ((a * 31 + 127) / 255) << 24;
        }
    }
}
