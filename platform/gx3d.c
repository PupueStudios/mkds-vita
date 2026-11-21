// The DS geometry engine: everything the 3D hardware does before rasterisation.
//
// Commands arrive through the command ports (0x04000440..0x040005ff), the packed GXFIFO port (0x04000400) and DMA,
// and run at once. Vertices are transformed to clip space and lit; polygons are assembled, culled and stored with the
// state the rasteriser needs. Clipping is left to the rasteriser (OpenGL clips against the same planes), which keeps
// whole polygons for the frame interpolation and the widescreen projection.
#include "port.h"
#include "gx.h"

#include "hostapi.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t fx32;
typedef struct Mtx {
    fx32 m[16]; // row-major, row vectors (v' = v * M), 20.12
} Mtx;

static const uint8_t s_param_count[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12, [0x18] = 16,
    [0x19] = 12, [0x1a] = 9, [0x1b] = 3, [0x1c] = 3, [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1,
    [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2a] = 1, [0x2b] = 1, [0x30] = 1, [0x31] = 1,
    [0x32] = 1, [0x33] = 1, [0x34] = 32, [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2,
    [0x72] = 1,
};
static bool valid_command(uint32_t c) { return c == 0x11 || c == 0x15 || c == 0x41 || s_param_count[c] != 0; }


static struct {
    // matrices
    int mode;
    Mtx proj, pos, vec, tex, clip;
    Mtx proj_stack[1], pos_stack[32], vec_stack[32], tex_stack[1];
    int proj_sp, pos_sp, tex_sp;
    bool stack_error;
    bool clip_dirty;
    uint32_t pos_serial, proj_serial, pos_slot_serial[32], proj_slot_serial;
    uint32_t list_gen;
    // current vertex state
    uint8_t color[3]; // 0..31
    int16_t s, t;     // texcoord as given
    int16_t ts, tt;   // after the texture matrix (modes 1 and 2)
    int16_t vx, vy, vz;
    uint32_t polyattr_pending, polyattr, teximage, pltt;
    // lighting
    uint16_t diffuse, ambient, specular, emission;
    bool use_shininess_table, set_vertex_color;
    int32_t light_dir[4][3];   // transformed, s.9
    int32_t light_half[4][3];  // half vectors
    uint16_t light_color[4];
    uint8_t shininess[128];
    // primitive assembly
    int prim;       // 0 tri, 1 quad, 2 tri strip, 3 quad strip, -1 none
    GxVertex strip[4];
    int strip_count;
    uint32_t strip_total;
    uint8_t viewport[4];
    // command decoding
    uint32_t cmd, params[32];
    int param_index;
    uint32_t packed, packed_left;
    // output
    int prev_displayed;
    uint32_t item_vbl;
    uint32_t swap_vbl;
    int building; // index of the list being built
    int displayed;
    bool swap_pending;
    uint32_t swap_param;
    uint32_t draw_tag;
    uint32_t draw_id;
    bool gpu_mtx_stale;
    uint32_t gpu_mtx;
    bool gpu_mode;
    uint32_t prim_index;   // polygons begun in the current vertex block
    int32_t proj_aspect;   // of the projection matrix
    bool proj_aspect_dirty;
} G;

// ------------------------------------------------------------------------------------------------ matrix helpers
static void mtx_identity(Mtx *m) {
    memset(m, 0, sizeof(*m));
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = 0x1000;
}

// r = a * b
static void mtx_mul(Mtx *r, const Mtx *a, const Mtx *b) {
    Mtx t;
    if (a->m[3] == 0 && a->m[7] == 0 && a->m[11] == 0 && a->m[15] == 4096) {
        for (int i = 0; i < 4; i++) {
            const fx32 *ar = &a->m[i * 4];
            for (int j = 0; j < 4; j++) {
                int64_t s = (int64_t) ar[0] * b->m[j] + (int64_t) ar[1] * b->m[4 + j] + (int64_t) ar[2] * b->m[8 + j];
                if (i == 3) s += (int64_t) b->m[12 + j] << 12;
                t.m[i * 4 + j] = (fx32) (s >> 12);
            }
        }
        *r = t;
        return;
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int64_t s = 0;
            for (int k = 0; k < 4; k++) {
                s += (int64_t) a->m[i * 4 + k] * b->m[k * 4 + j];
            }
            t.m[i * 4 + j] = (fx32) (s >> 12);
        }
    }
    *r = t;
}

static void load_params(Mtx *m, const uint32_t *p, int kind) {
    mtx_identity(m);
    switch (kind) {
    case 44:
        for (int i = 0; i < 16; i++) m->m[i] = (fx32) p[i];
        break;
    case 43:
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 3; c++) m->m[r * 4 + c] = (fx32) p[r * 3 + c];
        break;
    case 33:
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) m->m[r * 4 + c] = (fx32) p[r * 3 + c];
        break;
    }
}

static void update_result_registers(void);
static void swap_buffers_ran(void);

static uint32_t s_serial_counter;
static void new_serial(void) {
    if (G.mode == 0) G.proj_serial = ++s_serial_counter;
    else G.pos_serial = ++s_serial_counter;
}

static void apply_load(const Mtx *n) {
    switch (G.mode) {
    case 0: G.proj = *n; G.proj_aspect_dirty = true; break;
    case 1: G.pos = *n; break;
    case 2: G.pos = *n; G.vec = *n; break;
    case 3: G.tex = *n; return;
    }
    new_serial();
    G.clip_dirty = G.gpu_mtx_stale = true;
}

// Current = N * Current. Mode 2 multiplies the direction matrix too (except for SCALE).
static void apply_mult(const Mtx *n, bool scale) {
    switch (G.mode) {
    case 0: mtx_mul(&G.proj, n, &G.proj); G.proj_aspect_dirty = true; break;
    case 1: mtx_mul(&G.pos, n, &G.pos); break;
    case 2:
        mtx_mul(&G.pos, n, &G.pos);
        if (!scale) {
            mtx_mul(&G.vec, n, &G.vec);
        }
        break;
    case 3: mtx_mul(&G.tex, n, &G.tex); return;
    }
    new_serial();
    G.clip_dirty = G.gpu_mtx_stale = true;
}

static float s_clipf[16];

static void update_clip(void) {
    if (G.clip_dirty) {
        mtx_mul(&G.clip, &G.pos, &G.proj);
        for (int i = 0; i < 16; i++) s_clipf[i] = (float) G.clip.m[i] * (1.0f / 4096.0f);
        G.clip_dirty = false;
    }
}

// ------------------------------------------------------------------------------------------------ lighting
static void rgb15(uint16_t c, int out[3]) {
    out[0] = c & 31;
    out[1] = (c >> 5) & 31;
    out[2] = (c >> 10) & 31;
}

static int32_t sext10(uint32_t v) { return (int32_t) (v << 22) >> 22; }

static inline int div31(uint32_t x) { return (int) (((uint64_t) x * 541201u) >> 24); }

typedef struct LightProducts {
    int spe_lc[3], dif_lc[3];
    int amb[3];
} LightProducts;
static struct {
    bool dirty;
    int emi9[3];
    LightProducts l[4];
} s_light = {.dirty = true};
static uint32_t s_light_version;

static void light_products(void) {
    int dif[3], amb[3], spe[3], emi[3];
    rgb15(G.diffuse, dif);
    rgb15(G.ambient, amb);
    rgb15(G.specular, spe);
    rgb15(G.emission, emi);
    for (int i = 0; i < 3; i++) s_light.emi9[i] = emi[i] << 9;
    for (int l = 0; l < 4; l++) {
        int lc[3];
        rgb15(G.light_color[l], lc);
        for (int i = 0; i < 3; i++) {
            s_light.l[l].spe_lc[i] = spe[i] * lc[i];
            s_light.l[l].dif_lc[i] = dif[i] * lc[i];
            s_light.l[l].amb[i] = div31((uint32_t) ((amb[i] * lc[i]) << 9));
        }
    }
    s_light.dirty = false;
}

static Gx3dFrame *building(void);

static void light_core(const GxLightState *st, const int32_t vec9[9], uint32_t normal, uint32_t mask, uint8_t out3[3]) {
    int32_t n0[3] = {sext10(normal), sext10(normal >> 10), sext10(normal >> 20)}; // s.9
    int32_t n[3];
    for (int i = 0; i < 3; i++) {
        int64_t s = (int64_t) n0[0] * vec9[i] + (int64_t) n0[1] * vec9[3 + i] + (int64_t) n0[2] * vec9[6 + i];
        n[i] = (int32_t) (s >> 12); // still s.9
    }
    int out[3] = {st->emi9[0], st->emi9[1], st->emi9[2]};
    for (int l = 0; l < 4; l++) {
        if (!(mask & (1u << l))) continue;
        int32_t d = -(st->dir[l][0] * n[0] + st->dir[l][1] * n[1] + st->dir[l][2] * n[2]) >> 9;
        if (d < 0) d = 0;
        if (d > 0x1ff) d = 0x1ff;
        int32_t sh = -(st->half[l][0] * n[0] + st->half[l][1] * n[1] + st->half[l][2] * n[2]) >> 9;
        if (sh < 0) sh = 0;
        sh = (sh * sh) >> 9; // squared
        if (sh > 0x1ff) sh = 0x1ff;
        if (st->use_table) sh = st->shininess[sh >> 2] << 1;
        if (d == 0 && sh == 0) {
            out[0] += st->amb[l][0];
            out[1] += st->amb[l][1];
            out[2] += st->amb[l][2];
            continue;
        }
        for (int i = 0; i < 3; i++) {
            out[i] += div31((uint32_t) (st->spe_lc[l][i] * sh)) + div31((uint32_t) (st->dif_lc[l][i] * d)) + st->amb[l][i];
        }
    }
    for (int i = 0; i < 3; i++) {
        int c = out[i] >> 9;
        out3[i] = (uint8_t) (c > 31 ? 31 : c);
    }
}

static GxLightState s_ls;
static uint32_t s_ls_version = ~0u;
static const GxLightState *light_state(void) {
    if (s_light.dirty) light_products();
    if (s_ls_version != s_light_version) {
        memcpy(s_ls.emi9, s_light.emi9, sizeof(s_ls.emi9));
        for (int l = 0; l < 4; l++) {
            for (int i = 0; i < 3; i++) {
                s_ls.dir[l][i] = G.light_dir[l][i];
                s_ls.half[l][i] = G.light_half[l][i];
                s_ls.spe_lc[l][i] = s_light.l[l].spe_lc[i];
                s_ls.dif_lc[l][i] = s_light.l[l].dif_lc[i];
                s_ls.amb[l][i] = s_light.l[l].amb[i];
            }
        }
        s_ls.use_table = G.use_shininess_table;
        memcpy(s_ls.shininess, G.shininess, sizeof(s_ls.shininess));
        s_ls_version = s_light_version;
    }
    return &s_ls;
}

static inline void vec9_now(int32_t v[9]) {
    v[0] = G.vec.m[0], v[1] = G.vec.m[1], v[2] = G.vec.m[2];
    v[3] = G.vec.m[4], v[4] = G.vec.m[5], v[5] = G.vec.m[6];
    v[6] = G.vec.m[8], v[7] = G.vec.m[9], v[8] = G.vec.m[10];
}

static void light_vertex(uint32_t normal) {
    static struct {
        bool valid;
        uint32_t normal, lights, version;
        int32_t vec[9];
        uint8_t color[3];
    } last;
    int32_t vec9[9];
    vec9_now(vec9);
    uint32_t lights = G.polyattr & 15;
    if (last.valid && last.normal == normal && last.lights == lights && last.version == s_light_version &&
        !s_light.dirty && memcmp(last.vec, vec9, sizeof(vec9)) == 0) {
        memcpy(G.color, last.color, 3);
        return;
    }
    light_core(light_state(), vec9, normal, lights, G.color);
    last.valid = true;
    last.normal = normal;
    last.lights = lights;
    last.version = s_light_version;
    memcpy(last.vec, vec9, sizeof(vec9));
    memcpy(last.color, G.color, 3);
}

// ------------------------------------------------------------------------------------------------ polygons
#define GX_LISTS 12
static Gx3dFrame *s_lists[GX_LISTS];
static atomic_int s_refs[GX_LISTS];
static Gx3dFrame *building(void) { return s_lists[G.building]; }

static int32_t det3(int64_t a0, int64_t a1, int64_t a2, int64_t b0, int64_t b1, int64_t b2, int64_t c0, int64_t c1,
                    int64_t c2) {
    // Sign of the determinant, computed in floating point to avoid overflow of 20.12 products.
    double d = (double) a0 * ((double) b1 * c2 - (double) b2 * c1) - (double) a1 * ((double) b0 * c2 - (double) b2 * c0) +
               (double) a2 * ((double) b0 * c1 - (double) b1 * c0);
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

// Set by the game around what it draws (see tools/portify.py): the identity of the object, so that the renderer
// pairs each object's pieces with the same object's in the previous frame (frame interpolation).
unsigned int g_port_draw_id;
static volatile uint32_t s_vbl;

bool g_gx3d_keep_hidden = true;

static int32_t current_aspect(Gx3dFrame *f) {
    if (G.proj_aspect_dirty) {
        G.proj_aspect_dirty = false;
        int64_t sx = G.proj.m[0] < 0 ? -(int64_t) G.proj.m[0] : G.proj.m[0];
        int64_t sy = G.proj.m[5] < 0 ? -(int64_t) G.proj.m[5] : G.proj.m[5];
        G.proj_aspect = sx ? (int32_t) (sy * 4096 / sx) : 0x1555;
        if (G.proj_aspect < 0x800 || G.proj_aspect > 0x4000) G.proj_aspect = 0x1555;
    }
    if (!f->proj_aspect) f->proj_aspect = G.proj_aspect;
    return G.proj_aspect;
}

static void emit_polygon(const GxVertex *v, int count) {
    Gx3dFrame *f = building();
    uint32_t index_in_block = G.prim_index++;
    bool hidden = false;
    bool front = true;
    uint32_t attr = G.polyattr;
    if (G.gpu_mode) {
        f->gpu_vertices = 1;
        goto store;
    }
    // Entirely outside one clip plane: dropped, as the DS does (kept hidden).
    for (int plane = 0; plane < 6 && !hidden; plane++) {
        int outside = 0;
        for (int i = 0; i < count; i++) {
            int32_t c = plane < 2 ? v[i].x : plane < 4 ? v[i].y : v[i].z;
            if (plane & 1 ? c > v[i].w : c < -v[i].w) {
                outside++;
            }
        }
        if (outside == count) {
            hidden = true;
        }
    }
    // Facing: winding in homogeneous coordinates (y up), counter-clockwise on screen is front.
    int sign = det3(v[0].x, v[0].y, v[0].w, v[1].x, v[1].y, v[1].w, v[2].x, v[2].y, v[2].w);
    if (sign == 0 && count == 4) {
        sign = det3(v[0].x, v[0].y, v[0].w, v[2].x, v[2].y, v[2].w, v[3].x, v[3].y, v[3].w);
    }
    front = sign >= 0;
    if ((front && !(attr & 0x80)) || (!front && !(attr & 0x40))) {
        hidden = true; // culled (bit 7 renders front faces, bit 6 back faces)
    }
store:
    if (hidden && !g_gx3d_keep_hidden) {
        return;
    }
    if (f->polygon_count >= GX_MAX_POLYGONS || f->vertex_count + count > GX_MAX_VERTICES) {
        return;
    }
    GxPolygon *p = &f->polygons[f->polygon_count++];
    p->first = f->vertex_count;
    p->count = (uint8_t) count;
    memcpy(p->viewport, G.viewport, 4);
    p->front = front;
    p->hidden = hidden;
    p->attr = attr;
    p->teximage = G.teximage;
    p->pltt = G.pltt;
    p->tag = index_in_block;
    p->obj = G.draw_id;
    // The width of the view the projection maps to the screen, relative to its height: wider than 4:3 when the game
    // projects the widescreen view (g_port_aspect).
    p->aspect = current_aspect(f);
    memcpy(&f->vertices[f->vertex_count], v, sizeof(GxVertex) * count);
    f->vertex_count += count;
}

bool g_gx3d_gpu_vertices;

static struct { uint32_t pos, proj, gen, index; } s_clip_cache[64];

static void gpu_matrix(Gx3dFrame *f);

static void submit_vertex(void) {
    GxVertex v;
    if (G.gpu_mode) {
        Gx3dFrame *f = building();
        gpu_matrix(f);
        v.x = G.vx;
        v.y = G.vy;
        v.z = G.vz;
        v.w = (int32_t) G.gpu_mtx;
    } else {
        update_clip();
        const float *m = s_clipf;
        float x = G.vx, y = G.vy, z = G.vz;
        v.x = (int32_t) (x * m[0] + y * m[4] + z * m[8] + 4096.0f * m[12]);
        v.y = (int32_t) (x * m[1] + y * m[5] + z * m[9] + 4096.0f * m[13]);
        v.z = (int32_t) (x * m[2] + y * m[6] + z * m[10] + 4096.0f * m[14]);
        v.w = (int32_t) (x * m[3] + y * m[7] + z * m[11] + 4096.0f * m[15]);
    }
    int64_t x = G.vx, y = G.vy, z = G.vz;
    v.r = G.color[0];
    v.g = G.color[1];
    v.b = G.color[2];
    v.pad = 0;
    // Texture coordinates.
    uint32_t mode = G.teximage >> 30;
    if (mode == 3) {
        const fx32 *t = G.tex.m;
        v.s = (int16_t) (((x * t[0] + y * t[4] + z * t[8]) >> 24) + G.s);
        v.t = (int16_t) (((x * t[1] + y * t[5] + z * t[9]) >> 24) + G.t);
    } else if (mode == 0) {
        v.s = G.s;
        v.t = G.t;
    } else {
        v.s = G.ts;
        v.t = G.tt;
    }
    if (G.prim < 0) {
        return;
    }
    G.strip[G.strip_count++] = v;
    switch (G.prim) {
    case 0: // separate triangles
        if (G.strip_count == 3) {
            emit_polygon(G.strip, 3);
            G.strip_count = 0;
        }
        break;
    case 1: // separate quads
        if (G.strip_count == 4) {
            emit_polygon(G.strip, 4);
            G.strip_count = 0;
        }
        break;
    case 2: // triangle strip: every second triangle is wound the other way
        if (G.strip_count == 3) {
            if (G.strip_total & 1) {
                GxVertex t[3] = {G.strip[1], G.strip[0], G.strip[2]};
                emit_polygon(t, 3);
            } else {
                emit_polygon(G.strip, 3);
            }
            G.strip_total++;
            G.strip[0] = G.strip[1];
            G.strip[1] = G.strip[2];
            G.strip_count = 2;
        }
        break;
    case 3: // quad strip: vertices 0 1 3 2 form the quad
        if (G.strip_count == 4) {
            GxVertex q[4] = {G.strip[0], G.strip[1], G.strip[3], G.strip[2]};
            emit_polygon(q, 4);
            G.strip[0] = G.strip[2];
            G.strip[1] = G.strip[3];
            G.strip_count = 2;
        }
        break;
    }
}

static void gpu_matrix(Gx3dFrame *f) {
    {
        if (G.gpu_mtx_stale || f->matrix_count == 0) {
            uint32_t slot = (G.pos_serial * 2654435761u ^ G.proj_serial * 40503u) >> 26;
            if (s_clip_cache[slot].pos == G.pos_serial && s_clip_cache[slot].proj == G.proj_serial &&
                s_clip_cache[slot].gen == G.list_gen && s_clip_cache[slot].index < f->matrix_count) {
                G.gpu_mtx = s_clip_cache[slot].index;
            } else {
                update_clip();
                if (f->matrix_count < GX_MAX_MATRICES) {
                    float *m = f->matrices[f->matrix_count];
                    for (int i = 0; i < 16; i++) m[i] = (float) G.clip.m[i] * (1.0f / 4096.0f);
                    G.gpu_mtx = f->matrix_count++;
                    s_clip_cache[slot].pos = G.pos_serial;
                    s_clip_cache[slot].proj = G.proj_serial;
                    s_clip_cache[slot].gen = G.list_gen;
                    s_clip_cache[slot].index = G.gpu_mtx;
                }
            }
            G.gpu_mtx_stale = false;
        }
    }
}

// ------------------------------------------------------------------------------------------------ tests and results
static void update_result_registers(void) {
    update_clip();
    for (int i = 0; i < 16; i++) {
        IO32(0x640 + i * 4) = (uint32_t) G.clip.m[i];
    }
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            IO32(0x680 + (r * 3 + c) * 4) = (uint32_t) G.vec.m[r * 4 + c];
        }
    }
}

static void update_gxstat(void) {
    uint32_t stat = IO32(0x600) & 0x3; // test bits
    stat |= (uint32_t) (G.pos_sp & 0x1f) << 8;
    stat |= (uint32_t) (G.proj_sp & 1) << 13;
    stat |= G.stack_error ? 0x8000 : 0;
    stat |= (1u << 25) | (1u << 26); // FIFO less than half full, empty
    stat |= IO32(0x600) & 0xc0000000; // FIFO interrupt mode
    IO32(0x600) = stat;
    IO32(0x604) = building()->polygon_count | (building()->vertex_count << 16);
}

static void box_test(void) {
    update_clip();
    int16_t p[6] = {(int16_t) G.params[0], (int16_t) (G.params[0] >> 16), (int16_t) G.params[1],
                    (int16_t) (G.params[1] >> 16), (int16_t) G.params[2], (int16_t) (G.params[2] >> 16)};
    int64_t x0 = p[0], y0 = p[1], z0 = p[2], x1 = p[0] + p[3], y1 = p[1] + p[4], z1 = p[2] + p[5];
    const fx32 *m = G.clip.m;
    int outside_mask = 0x3f; // a plane that has every corner outside
    for (int i = 0; i < 8; i++) {
        int64_t x = i & 1 ? x1 : x0, y = i & 2 ? y1 : y0, z = i & 4 ? z1 : z0;
        int64_t cx = (x * m[0] + y * m[4] + z * m[8] + 0x1000ll * m[12]) >> 12;
        int64_t cy = (x * m[1] + y * m[5] + z * m[9] + 0x1000ll * m[13]) >> 12;
        int64_t cz = (x * m[2] + y * m[6] + z * m[10] + 0x1000ll * m[14]) >> 12;
        int64_t cw = (x * m[3] + y * m[7] + z * m[11] + 0x1000ll * m[15]) >> 12;
        int out = (cx < -cw) | (cx > cw) << 1 | (cy < -cw) << 2 | (cy > cw) << 3 | (cz < -cw) << 4 | (cz > cw) << 5;
        outside_mask &= out;
    }
    if (outside_mask) {
        IO32(0x600) &= ~2u;
    } else {
        IO32(0x600) |= 2u;
    }
}

static void pos_test(void) {
    G.vx = (int16_t) G.params[0];
    G.vy = (int16_t) (G.params[0] >> 16);
    G.vz = (int16_t) G.params[1];
    update_clip();
    const fx32 *m = G.clip.m;
    int64_t x = G.vx, y = G.vy, z = G.vz;
    for (int i = 0; i < 4; i++) {
        IO32(0x620 + i * 4) = (uint32_t) ((x * m[i] + y * m[4 + i] + z * m[8 + i] + 0x1000ll * m[12 + i]) >> 12);
    }
}

static void vec_test(void) {
    uint32_t p = G.params[0];
    int32_t n[3] = {sext10(p), sext10(p >> 10), sext10(p >> 20)};
    for (int i = 0; i < 3; i++) {
        int64_t s = 0;
        for (int k = 0; k < 3; k++) {
            s += (int64_t) (n[k] << 3) * G.vec.m[k * 4 + i];
        }
        int32_t r = (int32_t) (s >> 12);
        if (r & 0x1000) r |= ~0xfff; // 4.12 with the sign repeated above
        IO16(0x630 + i * 2) = (uint16_t) r;
    }
}

// ------------------------------------------------------------------------------------------------ commands
static void execute_cold(uint32_t cmd);
static inline __attribute__((always_inline)) void execute(uint32_t cmd) {
    const uint32_t *p = G.params;
    switch (cmd) {
    case 0x20: {
        uint32_t c = p[0];
        G.color[0] = (uint8_t) (c & 31);
        G.color[1] = (uint8_t) ((c >> 5) & 31);
        G.color[2] = (uint8_t) ((c >> 10) & 31);
        return;
    }
    case 0x22:
        if ((G.teximage >> 30) == 1) break;
        G.s = G.ts = (int16_t) p[0];
        G.t = G.tt = (int16_t) (p[0] >> 16);
        return;
    case 0x23:
        G.vx = (int16_t) p[0];
        G.vy = (int16_t) (p[0] >> 16);
        G.vz = (int16_t) p[1];
        submit_vertex();
        return;
    case 0x24:
        G.vx = (int16_t) ((p[0] & 0x3ff) << 6);
        G.vy = (int16_t) (((p[0] >> 10) & 0x3ff) << 6);
        G.vz = (int16_t) (((p[0] >> 20) & 0x3ff) << 6);
        submit_vertex();
        return;
    case 0x25: G.vx = (int16_t) p[0]; G.vy = (int16_t) (p[0] >> 16); submit_vertex(); return;
    case 0x26: G.vx = (int16_t) p[0]; G.vz = (int16_t) (p[0] >> 16); submit_vertex(); return;
    case 0x27: G.vy = (int16_t) p[0]; G.vz = (int16_t) (p[0] >> 16); submit_vertex(); return;
    case 0x28:
        G.vx = (int16_t) (G.vx + sext10(p[0]));
        G.vy = (int16_t) (G.vy + sext10(p[0] >> 10));
        G.vz = (int16_t) (G.vz + sext10(p[0] >> 20));
        submit_vertex();
        return;
    }
    execute_cold(cmd);
}

static __attribute__((noinline)) void execute_cold(uint32_t cmd) {
    const uint32_t *p = G.params;
    Mtx n;
    switch (cmd) {
    case 0x10: G.mode = p[0] & 3; break;
    case 0x11: // PUSH
        if (G.mode == 0) {
            G.proj_stack[0] = G.proj;
            G.proj_slot_serial = G.proj_serial;
            G.proj_sp = 1;
        } else if (G.mode == 3) {
            G.tex_stack[0] = G.tex;
            G.tex_sp = 1;
        } else {
            if (G.pos_sp >= 31) G.stack_error = true;
            G.pos_stack[G.pos_sp & 31] = G.pos;
            G.vec_stack[G.pos_sp & 31] = G.vec;
            G.pos_slot_serial[G.pos_sp & 31] = G.pos_serial;
            G.pos_sp = (G.pos_sp + 1) & 63;
        }
        break;
    case 0x12: { // POP
        if (G.mode == 0) {
            G.proj = G.proj_stack[0];
            G.proj_serial = G.proj_slot_serial;
            G.proj_aspect_dirty = true;
            G.proj_sp = 0;
        } else if (G.mode == 3) {
            G.tex = G.tex_stack[0];
            G.tex_sp = 0;
        } else {
            int off = (int) (p[0] << 26) >> 26;
            G.pos_sp = (G.pos_sp - off) & 63;
            if (G.pos_sp >= 31) G.stack_error = true;
            G.pos = G.pos_stack[G.pos_sp & 31];
            G.vec = G.vec_stack[G.pos_sp & 31];
            G.pos_serial = G.pos_slot_serial[G.pos_sp & 31];
        }
        G.clip_dirty = G.gpu_mtx_stale = true;
        break;
    }
    case 0x13: // STORE
        if (G.mode == 0) {
            G.proj_stack[0] = G.proj;
            G.proj_slot_serial = G.proj_serial;
        } else if (G.mode == 3) {
            G.tex_stack[0] = G.tex;
        } else {
            uint32_t i = p[0] & 31;
            if (i == 31) G.stack_error = true;
            G.pos_stack[i] = G.pos;
            G.vec_stack[i] = G.vec;
            G.pos_slot_serial[i] = G.pos_serial;
        }
        break;
    case 0x14: // RESTORE
        if (G.mode == 0) {
            G.proj = G.proj_stack[0];
            G.proj_serial = G.proj_slot_serial;
            G.proj_aspect_dirty = true;
        } else if (G.mode == 3) {
            G.tex = G.tex_stack[0];
        } else {
            uint32_t i = p[0] & 31;
            if (i == 31) G.stack_error = true;
            G.pos = G.pos_stack[i];
            G.vec = G.vec_stack[i];
            G.pos_serial = G.pos_slot_serial[i];
        }
        G.clip_dirty = G.gpu_mtx_stale = true;
        break;
    case 0x15: mtx_identity(&n); apply_load(&n); break;
    case 0x16: load_params(&n, p, 44); apply_load(&n); break;
    case 0x17: load_params(&n, p, 43); apply_load(&n); break;
    case 0x18: load_params(&n, p, 44); apply_mult(&n, false); break;
    case 0x19: load_params(&n, p, 43); apply_mult(&n, false); break;
    case 0x1a: load_params(&n, p, 33); apply_mult(&n, false); break;
    case 0x1b: // SCALE
        mtx_identity(&n);
        n.m[0] = (fx32) p[0];
        n.m[5] = (fx32) p[1];
        n.m[10] = (fx32) p[2];
        apply_mult(&n, true);
        break;
    case 0x1c: // TRANS
        mtx_identity(&n);
        n.m[12] = (fx32) p[0];
        n.m[13] = (fx32) p[1];
        n.m[14] = (fx32) p[2];
        apply_mult(&n, false);
        break;
    case 0x20: { // COLOR
        int c[3];
        rgb15((uint16_t) p[0], c);
        G.color[0] = (uint8_t) c[0];
        G.color[1] = (uint8_t) c[1];
        G.color[2] = (uint8_t) c[2];
        break;
    }
    case 0x21: // NORMAL
        if ((G.teximage >> 30) == 2) {
            int32_t n0[3] = {sext10(p[0]), sext10(p[0] >> 10), sext10(p[0] >> 20)};
            const fx32 *t = G.tex.m;
            G.ts = (int16_t) ((((int64_t) n0[0] * t[0] + (int64_t) n0[1] * t[4] + (int64_t) n0[2] * t[8]) >> 21) + G.s);
            G.tt = (int16_t) ((((int64_t) n0[0] * t[1] + (int64_t) n0[1] * t[5] + (int64_t) n0[2] * t[9]) >> 21) + G.t);
        }
        light_vertex(p[0]);
        break;
    case 0x22: // TEXCOORD
        G.s = (int16_t) p[0];
        G.t = (int16_t) (p[0] >> 16);
        if ((G.teximage >> 30) == 1) {
            const fx32 *t = G.tex.m;
            int64_t s = G.s, tt = G.t;
            // (S, T, 1/16, 1/16) times the matrix: with S and T in 1/16 texels, the translation rows count once.
            G.ts = (int16_t) ((s * t[0] + tt * t[4] + t[8] + t[12]) >> 12);
            G.tt = (int16_t) ((s * t[1] + tt * t[5] + t[9] + t[13]) >> 12);
        } else {
            G.ts = G.s;
            G.tt = G.t;
        }
        break;
    case 0x23:
        G.vx = (int16_t) p[0];
        G.vy = (int16_t) (p[0] >> 16);
        G.vz = (int16_t) p[1];
        submit_vertex();
        break;
    case 0x24: // VTX_10: 4.6 fixed
        G.vx = (int16_t) ((p[0] & 0x3ff) << 6);
        G.vy = (int16_t) (((p[0] >> 10) & 0x3ff) << 6);
        G.vz = (int16_t) (((p[0] >> 20) & 0x3ff) << 6);
        submit_vertex();
        break;
    case 0x25: G.vx = (int16_t) p[0]; G.vy = (int16_t) (p[0] >> 16); submit_vertex(); break;
    case 0x26: G.vx = (int16_t) p[0]; G.vz = (int16_t) (p[0] >> 16); submit_vertex(); break;
    case 0x27: G.vy = (int16_t) p[0]; G.vz = (int16_t) (p[0] >> 16); submit_vertex(); break;
    case 0x28: // VTX_DIFF: 10-bit signed offsets in the vertex units (4.12)
        G.vx = (int16_t) (G.vx + sext10(p[0]));
        G.vy = (int16_t) (G.vy + sext10(p[0] >> 10));
        G.vz = (int16_t) (G.vz + sext10(p[0] >> 20));
        submit_vertex();
        break;
    case 0x29: G.polyattr_pending = p[0]; break;
    case 0x2a: G.teximage = p[0]; break;
    case 0x2b: G.pltt = p[0] & 0x1fff; break;
    case 0x30: // DIF_AMB
        G.diffuse = (uint16_t) (p[0] & 0x7fff);
        G.ambient = (uint16_t) ((p[0] >> 16) & 0x7fff);
        s_light.dirty = true;
        s_light_version++;
        if (p[0] & 0x8000) {
            int c[3];
                rgb15(G.diffuse, c);
            G.color[0] = (uint8_t) c[0];
            G.color[1] = (uint8_t) c[1];
            G.color[2] = (uint8_t) c[2];
        }
        break;
    case 0x31: // SPE_EMI
        G.specular = (uint16_t) (p[0] & 0x7fff);
        G.emission = (uint16_t) ((p[0] >> 16) & 0x7fff);
        s_light.dirty = true;
        s_light_version++;
        G.use_shininess_table = (p[0] >> 15) & 1;
        break;
    case 0x32: { // LIGHT_VECTOR, transformed by the direction matrix
        int l = p[0] >> 30;
        int32_t d0[3] = {sext10(p[0]), sext10(p[0] >> 10), sext10(p[0] >> 20)};
        for (int i = 0; i < 3; i++) {
            int64_t s = 0;
            for (int k = 0; k < 3; k++) {
                s += (int64_t) d0[k] * G.vec.m[k * 4 + i];
            }
            G.light_dir[l][i] = (int32_t) (s >> 12);
        }
        // Half vector between the light and the line of sight (0, 0, -1).
        G.light_half[l][0] = G.light_dir[l][0] / 2;
        G.light_half[l][1] = G.light_dir[l][1] / 2;
        G.light_half[l][2] = (G.light_dir[l][2] - 0x200) / 2;
        s_light_version++;
        break;
    }
    case 0x33:
        G.light_color[p[0] >> 30] = (uint16_t) (p[0] & 0x7fff);
        s_light.dirty = true;
        s_light_version++;
        break;
    case 0x34:
        for (int i = 0; i < 32; i++) {
            G.shininess[i * 4 + 0] = (uint8_t) p[i];
            G.shininess[i * 4 + 1] = (uint8_t) (p[i] >> 8);
            G.shininess[i * 4 + 2] = (uint8_t) (p[i] >> 16);
            G.shininess[i * 4 + 3] = (uint8_t) (p[i] >> 24);
            s_light_version++;
        }
        break;
    case 0x40: // BEGIN_VTXS
        G.prim = (int) (p[0] & 3);
        G.strip_count = 0;
        G.strip_total = 0;
        G.polyattr = G.polyattr_pending;
        G.prim_index = 0;
        G.draw_tag = G.draw_tag * 31 + (uint32_t) G.clip.m[12] * 7 + (uint32_t) G.clip.m[13];
        break;
    case 0x41: break; // END_VTXS: nothing to do
    case 0x50: // SWAP_BUFFERS
        G.swap_pending = true;
        G.swap_vbl = G.item_vbl;
        G.swap_param = p[0] & 3;
        swap_buffers_ran();
        break;
    case 0x60: // VIEWPORT
        G.viewport[0] = (uint8_t) p[0];
        G.viewport[1] = (uint8_t) (p[0] >> 8);
        G.viewport[2] = (uint8_t) (p[0] >> 16);
        G.viewport[3] = (uint8_t) (p[0] >> 24);
        break;
    case 0x70: box_test(); break;
    case 0x71: pos_test(); break;
    case 0x72: vec_test(); break;
    default: break;
    }
}

// One parameter word for the command `cmd`: runs the command when its parameters are complete.
static void feed(uint32_t cmd, uint32_t param) {
    if (G.param_index == 0) {
        G.cmd = cmd;
    }
    G.params[G.param_index++] = param;
    if (G.param_index >= (s_param_count[cmd] ? s_param_count[cmd] : 1)) {
        G.param_index = 0;
        execute(cmd);
    }
}

static void port_write_(uint32_t addr, uint32_t value) {
    uint32_t cmd = (addr - 0x04000400) >> 2;
    if (!valid_command(cmd)) {
        return;
    }
    if (G.param_index && G.cmd != cmd) {
        G.param_index = 0; // a different command interrupts an incomplete one
    }
    feed(cmd, value);
}

// GXFIFO: a word holding up to four command ids, then their parameters. A command without parameters takes no word,
// except when the whole packed word is 0.
static void fifo_write_(uint32_t word) {
    if (G.packed_left == 0) {
        G.packed = word;
        G.packed_left = 4;
        G.param_index = 0;
        // Skip leading commands with no parameters.
        while (G.packed_left) {
            uint32_t cmd = G.packed & 0xff;
            if (s_param_count[cmd] != 0) {
                return;
            }
            if (valid_command(cmd)) {
                execute(cmd);
            }
            G.packed >>= 8;
            G.packed_left--;
            if (G.packed == 0) {
                G.packed_left = 0; // the remaining ids are NOPs
            }
        }
        return;
    }
    uint32_t cmd = G.packed & 0xff;
    G.params[G.param_index++] = word;
    if (G.param_index < s_param_count[cmd]) {
        return;
    }
    G.param_index = 0;
    execute(cmd);
    G.packed >>= 8;
    G.packed_left--;
    while (G.packed_left) {
        cmd = G.packed & 0xff;
        if (G.packed == 0) {
            G.packed_left = 0;
            break;
        }
        if (s_param_count[cmd] != 0) {
            return;
        }
        if (valid_command(cmd)) {
            execute(cmd);
        }
        G.packed >>= 8;
        G.packed_left--;
    }
}

bool g_gx3d_async;
void gx3d_worker_started(void) __attribute__((weak));

typedef struct QItem {
    uint32_t kind;
    uint32_t value;
} QItem;
#define Q_DRAW_ID 2u
#define Q_VBL 3u
#define Q_LIST 4u
#define Q_SIZE (1u << 17)
static QItem s_q[Q_SIZE];
static atomic_uint s_q_head, s_q_tail;
static atomic_int s_q_sleeping, s_q_drain_wait;
static void *s_q_wake, *s_q_done;
static bool s_q_started;

static void replay_list(uint32_t index);

static void run_item(const QItem *it) {
    if (it->kind == 0) {
        fifo_write_(it->value);
    } else if ((it->kind & 0xff) == 1) {
        feed(it->kind >> 8, it->value);
    } else if (it->kind == Q_DRAW_ID) {
        G.draw_id = it->value;
    } else if (it->kind == Q_VBL) {
        G.item_vbl = it->value;
    } else if (it->kind == Q_LIST) {
        replay_list(it->value);
    } else {
        port_write_(it->kind, it->value);
    }
}

static inline void cpu_relax(void) {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}


static void q_worker(void *arg) {
    (void) arg;
    if (gx3d_worker_started) gx3d_worker_started();
    unsigned tail = atomic_load(&s_q_tail);
    for (;;) {
        unsigned head = atomic_load_explicit(&s_q_head, memory_order_acquire);
        if (tail == head) {
            if (atomic_exchange(&s_q_drain_wait, 0)) hapi_sem_post(s_q_done);
            bool more = false;
            for (int i = 0; i < 6000 && !more; i++) {
                cpu_relax();
                more = atomic_load_explicit(&s_q_head, memory_order_acquire) != tail;
            }
            if (more) continue;
            atomic_store(&s_q_sleeping, 1);
            if (atomic_load(&s_q_head) == tail) hapi_sem_wait(s_q_wake);
            atomic_store(&s_q_sleeping, 0);
            continue;
        }
        while (tail != head) {
            run_item(&s_q[tail & (Q_SIZE - 1)]);
            tail++;
            if (!(tail & 63)) atomic_store_explicit(&s_q_tail, tail, memory_order_release);
        }
        atomic_store_explicit(&s_q_tail, tail, memory_order_release);
    }
}

static void q_start(void) {
    s_q_wake = hapi_sem_new();
    s_q_done = hapi_sem_new();
    s_q_started = true;
    hapi_thread_start(q_worker, NULL);
}

static void q_push_raw(uint32_t kind, uint32_t value);
static uint32_t s_q_draw_id = ~0u, s_q_vbl = ~0u;
static inline void q_context(void) {
    if (g_port_draw_id != s_q_draw_id) {
        s_q_draw_id = g_port_draw_id;
        q_push_raw(Q_DRAW_ID, s_q_draw_id);
    }
    if (s_vbl != s_q_vbl) {
        s_q_vbl = s_vbl;
        q_push_raw(Q_VBL, s_q_vbl);
    }
}
static void q_push(uint32_t kind, uint32_t value) {
    if (!s_q_started) q_start();
    q_context();
    q_push_raw(kind, value);
}
static void q_push_raw(uint32_t kind, uint32_t value) {
    unsigned head = atomic_load_explicit(&s_q_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&s_q_tail, memory_order_acquire) >= Q_SIZE) {
        while (head - atomic_load_explicit(&s_q_tail, memory_order_acquire) >= Q_SIZE) {
            if (atomic_exchange(&s_q_sleeping, 0)) hapi_sem_post(s_q_wake);
            hapi_sleep_ns(50000);
        }
    }
    s_q[head & (Q_SIZE - 1)] = (QItem) {kind, value};
    atomic_store_explicit(&s_q_head, head + 1, memory_order_release);
    if (atomic_load(&s_q_sleeping) && atomic_exchange(&s_q_sleeping, 0)) hapi_sem_post(s_q_wake);
}

static void q_push_block(uint32_t kind, const volatile uint32_t *src, uint32_t n, int step) {
    if (!s_q_started) q_start();
    q_context();
    while (n) {
        unsigned head = atomic_load_explicit(&s_q_head, memory_order_relaxed);
        unsigned room = Q_SIZE - (head - atomic_load_explicit(&s_q_tail, memory_order_acquire));
        if (!room) {
            if (atomic_exchange(&s_q_sleeping, 0)) hapi_sem_post(s_q_wake);
            hapi_sleep_ns(50000);
            continue;
        }
        uint32_t k = n < room ? n : room;
        for (uint32_t i = 0; i < k; i++, src += step) s_q[(head + i) & (Q_SIZE - 1)] = (QItem) {kind, *src};
        atomic_store_explicit(&s_q_head, head + k, memory_order_release);
        if (atomic_load(&s_q_sleeping) && atomic_exchange(&s_q_sleeping, 0)) hapi_sem_post(s_q_wake);
        n -= k;
    }
}

static void q_drain(void) {
    if (!s_q_started || atomic_load(&s_q_tail) == atomic_load(&s_q_head)) return;
    if (atomic_exchange(&s_q_sleeping, 0)) hapi_sem_post(s_q_wake);
    for (int i = 0; i < 20000; i++) {
        if (atomic_load_explicit(&s_q_tail, memory_order_acquire) == atomic_load(&s_q_head)) {
            return;
        }
        cpu_relax();
    }
    for (;;) {
        atomic_store(&s_q_drain_wait, 1);
        if (atomic_load(&s_q_tail) == atomic_load(&s_q_head)) break;
        hapi_sem_wait(s_q_done);
    }
}

unsigned long port_gx3d_gxstat(void) {
    q_drain();
    update_gxstat();
    return (unsigned long) (uintptr_t) DS_PTR(0x04000600);
}

void gx3d_sync_results(void) {
    q_drain();
    update_result_registers();
}

void gx3d_command(uint32_t cmd, uint32_t param) {
    if (g_gx3d_async) {
        q_push(1 | cmd << 8, param);
        return;
    }
    G.draw_id = g_port_draw_id;
    G.item_vbl = s_vbl;
    feed(cmd, param);
}
void gx3d_port_write(uint32_t addr, uint32_t value) {
    if (g_gx3d_async) {
        q_push(addr, value);
        return;
    }
    G.draw_id = g_port_draw_id;
    G.item_vbl = s_vbl;
    port_write_(addr, value);
}
typedef struct DlEntry {
    uint64_t hash;
    uint32_t n, first;
    uint32_t dec, dec_len, raw;
    uint16_t vertices;
    uint8_t kind;
    int32_t mesh;
} DlEntry;
enum { DLK_STATIC, DLK_STATIC_LIT, DLK_SKINNED, DLK_OTHER };
#define DL_ARENA_WORDS (1u << 20)
#define DL_SLOTS 8192
static uint32_t *s_dl_arena;
static uint32_t s_dl_used;
static DlEntry s_dl[DL_SLOTS];
static uint32_t s_dl_count;
static int16_t s_dl_index[DL_SLOTS * 2];

static int dl_decode(const uint32_t *w, uint32_t n, uint32_t *out, uint32_t cap) {
    uint32_t k = 0, packed = 0, left = 0, pidx = 0, params[32];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t word = w[i];
        if (left == 0) {
            packed = word;
            left = 4;
            pidx = 0;
            while (left) {
                uint32_t c = packed & 0xff;
                if (s_param_count[c] != 0) break;
                if (valid_command(c)) {
                    if (k + 1 > cap) return -1;
                    out[k++] = c;
                }
                packed >>= 8;
                left--;
                if (packed == 0) left = 0;
            }
            continue;
        }
        uint32_t c = packed & 0xff;
        params[pidx++] = word;
        if (pidx < s_param_count[c]) continue;
        if (k + 1 + pidx > cap) return -1;
        out[k++] = c | pidx << 8;
        for (uint32_t j = 0; j < pidx; j++) out[k++] = params[j];
        pidx = 0;
        packed >>= 8;
        left--;
        while (left) {
            c = packed & 0xff;
            if (packed == 0) {
                left = 0;
                break;
            }
            if (s_param_count[c] != 0) break;
            if (valid_command(c)) {
                if (k + 1 > cap) return -1;
                out[k++] = c;
            }
            packed >>= 8;
            left--;
        }
    }
    return left == 0 && pidx == 0 ? (int) k : -1;
}

#define MESH_VERTICES (1u << 18)
#define MESH_COUNT 4096
static GxMeshVertex *s_mesh_v;
static uint32_t s_mesh_used;
#define MESH_INDICES (1u << 20)
static uint16_t *s_mesh_i;
static uint32_t s_mesh_iused;
static GxMesh s_meshes[MESH_COUNT];
static uint32_t s_mesh_count, s_mesh_entries;
static struct { uint64_t hash; uint32_t n, first; int32_t id; } s_mesh_index[MESH_COUNT * 2];
bool g_gx3d_gpu_lists = true;

static atomic_uint s_mesh_gen;
const GxMesh *gx3d_mesh(uint32_t id) { return id < s_mesh_count ? &s_meshes[id] : NULL; }
uint32_t gx3d_mesh_gen(void) { return atomic_load(&s_mesh_gen); }

typedef struct MeshCorner {
    int16_t x, y, z;
    int16_t n[3];
    int16_t s, t;
    uint8_t r, g, b, color_src, st_src;
} MeshCorner;

#define MESH_DEDUP 8192
static int mesh_vertex(GxMesh *m, const MeshCorner *mc, GxMeshVertex *out, uint32_t cap, uint16_t *slots) {
    GxMeshVertex v;
    memset(&v, 0, sizeof(v));
    v.p[0] = mc->x, v.p[1] = mc->y, v.p[2] = mc->z;
    v.n[0] = mc->n[0], v.n[1] = mc->n[1], v.n[2] = mc->n[2];
    v.s = mc->s, v.t = mc->t;
    v.r = mc->r, v.g = mc->g, v.b = mc->b, v.color_src = mc->color_src;
    v.st_src = mc->st_src;
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *) &v;
    for (size_t i = 0; i < sizeof(v); i++) h = (h ^ b[i]) * 16777619u;
    uint32_t slot = h & (MESH_DEDUP - 1);
    for (;;) {
        uint16_t e = slots[slot];
        if (!e) break;
        if (memcmp(&out[e - 1], &v, sizeof(v)) == 0) return e - 1;
        slot = (slot + 1) & (MESH_DEDUP - 1);
    }
    if (m->vertex_count >= cap || m->vertex_count >= MESH_DEDUP / 2) return -1;
    out[m->vertex_count] = v;
    slots[slot] = (uint16_t) (m->vertex_count + 1);
    return (int) m->vertex_count++;
}

static bool quad_flat(const MeshCorner *const c[4]) {
    double n[2][3];
    static const int t[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int k = 0; k < 2; k++) {
        const MeshCorner *p0 = c[t[k][0]], *p1 = c[t[k][1]], *p2 = c[t[k][2]];
        double ax = p1->x - p0->x, ay = p1->y - p0->y, az = p1->z - p0->z;
        double bx = p2->x - p0->x, by = p2->y - p0->y, bz = p2->z - p0->z;
        n[k][0] = ay * bz - az * by;
        n[k][1] = az * bx - ax * bz;
        n[k][2] = ax * by - ay * bx;
    }
    double l0 = n[0][0] * n[0][0] + n[0][1] * n[0][1] + n[0][2] * n[0][2];
    double l1 = n[1][0] * n[1][0] + n[1][1] * n[1][1] + n[1][2] * n[1][2];
    if (l0 == 0 || l1 == 0) return true;
    double d = n[0][0] * n[1][0] + n[0][1] * n[1][1] + n[0][2] * n[1][2];
    return d > 0 && d * d >= 0.998 * l0 * l1;
}

static int mesh_build(const uint32_t *d, uint32_t len, GxMesh *m, GxMeshVertex *out, uint32_t cap, uint16_t *iout,
                      uint32_t icap) {
    static uint16_t slots[MESH_DEDUP];
    memset(slots, 0, sizeof(slots));
    uint32_t k = 0;
    bool begun = false, any_vertex = false;
    int prim = -1, nstrip = 0;
    uint32_t total = 0;
    MeshCorner cur = {0}, strip[4];
    int16_t vx = 0, vy = 0, vz = 0;
    uint32_t last_cmd = 0;
    memset(m, 0, sizeof(*m));
    m->last_prim = -1;
#define EMIT(a, b2, c2, d2, cnt)                                                                                       \
    do {                                                                                                               \
        const MeshCorner *cs[4] = {&(a), &(b2), &(c2), (cnt) == 4 ? &(d2) : &(c2)};                                    \
        if ((cnt) == 4 && !quad_flat(cs)) return -1;                                                                   \
        static const uint8_t tri[2][3] = {{0, 1, 2}, {0, 2, 3}};                                                       \
        for (int t_ = 0; t_ < ((cnt) == 4 ? 2 : 1); t_++) {                                                            \
            if (k + 3 > icap) return -1;                                                                               \
            for (int j_ = 0; j_ < 3; j_++) {                                                                           \
                int e_ = mesh_vertex(m, cs[tri[t_][j_]], out, cap, slots);                                             \
                if (e_ < 0) return -1;                                                                                 \
                iout[k++] = (uint16_t) e_;                                                                             \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
    for (const uint32_t *p = d, *end = d + len; p < end;) {
        uint32_t c = *p & 0xff, np = *p >> 8;
        const uint32_t *a = p + 1;
        p += 1 + np;
        last_cmd = c;
        switch (c) {
        case 0x40:
            prim = (int) (a[0] & 3);
            nstrip = 0;
            total = 0;
            begun = true;
            m->last_prim = (int8_t) prim;
            break;
        case 0x41: break;
        case 0x20:
            cur.color_src = 1;
            cur.r = (uint8_t) (a[0] & 31), cur.g = (uint8_t) ((a[0] >> 5) & 31), cur.b = (uint8_t) ((a[0] >> 10) & 31);
            m->last_color_cmd = 0x20, m->last_color_param = a[0];
            break;
        case 0x21:
            if (!begun) return -1;
            cur.color_src = 2;
            cur.n[0] = (int16_t) sext10(a[0]), cur.n[1] = (int16_t) sext10(a[0] >> 10), cur.n[2] = (int16_t) sext10(a[0] >> 20);
            m->lit = true;
            m->last_color_cmd = 0x21, m->last_color_param = a[0];
            break;
        case 0x22:
            cur.st_src = 1;
            cur.s = (int16_t) a[0], cur.t = (int16_t) (a[0] >> 16);
            m->has_texcoord = true;
            m->last_texcoord = a[0];
            break;
        case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: {
            if (!begun) return -1;
            if (!any_vertex && c != 0x23 && c != 0x24) return -1;
            any_vertex = true;
            if (c == 0x23) vx = (int16_t) a[0], vy = (int16_t) (a[0] >> 16), vz = (int16_t) a[1];
            else if (c == 0x24) {
                vx = (int16_t) ((a[0] & 0x3ff) << 6);
                vy = (int16_t) (((a[0] >> 10) & 0x3ff) << 6);
                vz = (int16_t) (((a[0] >> 20) & 0x3ff) << 6);
            } else if (c == 0x25) vx = (int16_t) a[0], vy = (int16_t) (a[0] >> 16);
            else if (c == 0x26) vx = (int16_t) a[0], vz = (int16_t) (a[0] >> 16);
            else if (c == 0x27) vy = (int16_t) a[0], vz = (int16_t) (a[0] >> 16);
            else {
                vx = (int16_t) (vx + sext10(a[0]));
                vy = (int16_t) (vy + sext10(a[0] >> 10));
                vz = (int16_t) (vz + sext10(a[0] >> 20));
            }
            MeshCorner v = cur;
            v.x = vx, v.y = vy, v.z = vz;
            strip[nstrip++] = v;
            if (prim == 0 && nstrip == 3) {
                EMIT(strip[0], strip[1], strip[2], strip[2], 3);
                nstrip = 0;
            } else if (prim == 1 && nstrip == 4) {
                EMIT(strip[0], strip[1], strip[2], strip[3], 4);
                nstrip = 0;
            } else if (prim == 2 && nstrip == 3) {
                if (total & 1) EMIT(strip[1], strip[0], strip[2], strip[2], 3);
                else EMIT(strip[0], strip[1], strip[2], strip[2], 3);
                total++;
                strip[0] = strip[1];
                strip[1] = strip[2];
                nstrip = 2;
            } else if (prim == 3 && nstrip == 4) {
                EMIT(strip[0], strip[1], strip[3], strip[2], 4);
                strip[0] = strip[2];
                strip[1] = strip[3];
                nstrip = 2;
            }
            break;
        }
        default: return -1;
        }
    }
#undef EMIT
    if (last_cmd != 0x41 || !any_vertex) return -1;
    m->last_v[0] = vx, m->last_v[1] = vy, m->last_v[2] = vz;
    m->index_count = k;
    return (int) k;
}

static int32_t mesh_find_or_build(uint64_t h, uint32_t n, uint32_t first, const uint32_t *d, uint32_t len) {
    uint32_t slot = (uint32_t) (h ^ (h >> 31)) & (MESH_COUNT * 2 - 1);
    for (;;) {
        if (!s_mesh_index[slot].n) break;
        if (s_mesh_index[slot].hash == h && s_mesh_index[slot].n == n && s_mesh_index[slot].first == first)
            return s_mesh_index[slot].id;
        slot = (slot + 1) & (MESH_COUNT * 2 - 1);
    }
    if (s_mesh_count >= MESH_COUNT || s_mesh_entries >= MESH_COUNT) return -1;
    if (!s_mesh_v) {
        static bool failed;
        if (failed) return -1;
        s_mesh_v = malloc(MESH_VERTICES * sizeof(GxMeshVertex));
        if (!s_mesh_v) {
            failed = true;
            port_log("Graphics: not enough memory for GPU meshes (slower 3D)");
            return -1;
        }
    }
    if (!s_mesh_i) s_mesh_i = malloc(MESH_INDICES * sizeof(uint16_t));
    if (!s_mesh_i) return -1;
    GxMesh m;
    int k = mesh_build(d, len, &m, &s_mesh_v[s_mesh_used], MESH_VERTICES - s_mesh_used, &s_mesh_i[s_mesh_iused],
                       MESH_INDICES - s_mesh_iused);
    int32_t id = -1;
    if (k > 0) {
        m.v = &s_mesh_v[s_mesh_used];
        m.idx = &s_mesh_i[s_mesh_iused];
        s_mesh_used += m.vertex_count;
        s_mesh_iused += (uint32_t) k;
        id = (int32_t) s_mesh_count;
        s_meshes[s_mesh_count++] = m;
    }
    s_mesh_entries++;
    s_mesh_index[slot].hash = h;
    s_mesh_index[slot].n = n;
    s_mesh_index[slot].first = first;
    s_mesh_index[slot].id = id;
    return id;
}

static void q_drain(void);

static int dl_find_or_add(const volatile uint32_t *src, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ src[i]) * 1099511628211ull;
    uint32_t slot = (uint32_t) (h ^ (h >> 29)) & (DL_SLOTS * 2 - 1);
    for (;;) {
        int e = s_dl_index[slot] - 1;
        if (e < 0) break;
        if (s_dl[e].hash == h && s_dl[e].n == n && s_dl[e].first == src[0]) return e;
        slot = (slot + 1) & (DL_SLOTS * 2 - 1);
    }
    if (!s_dl_arena) {
        static bool failed;
        if (failed) return -1;
        s_dl_arena = malloc(DL_ARENA_WORDS * 4);
        if (!s_dl_arena) {
            failed = true;
            port_log("Graphics: not enough memory for the 3D cache (slower 3D)");
            return -1;
        }
    }
    uint32_t need = n * 3 + 8;
    if (need > DL_ARENA_WORDS / 4) return -1;
    bool mesh_full = s_mesh_count + 1 >= MESH_COUNT || s_mesh_entries + 1 >= MESH_COUNT || s_mesh_used + n * 6 + 64 > MESH_VERTICES ||
                     s_mesh_iused + n * 6 + 64 > MESH_INDICES;
    if (mesh_full) {
        q_drain();
        s_mesh_count = 0;
        s_mesh_entries = 0;
        s_mesh_used = 0;
        s_mesh_iused = 0;
        memset(s_mesh_index, 0, sizeof(s_mesh_index));
        atomic_fetch_add(&s_mesh_gen, 1);
    }
    if (mesh_full || s_dl_used + need > DL_ARENA_WORDS || s_dl_count >= DL_SLOTS) {
        q_drain();
        s_dl_used = 0;
        s_dl_count = 0;
        memset(s_dl_index, 0, sizeof(s_dl_index));
        slot = (uint32_t) (h ^ (h >> 29)) & (DL_SLOTS * 2 - 1);
    }
    uint32_t raw = s_dl_used;
    for (uint32_t i = 0; i < n; i++) s_dl_arena[raw + i] = src[i];
    uint32_t dec = raw + n;
    int len = dl_decode(&s_dl_arena[raw], n, &s_dl_arena[dec], n * 2 + 8);
    if (len < 0) return -1;
    s_dl_used = dec + (uint32_t) len;
    int e = (int) s_dl_count++;
    s_dl[e] = (DlEntry) {h, n, src[0], dec, (uint32_t) len, raw, 0, 0, -1};
    s_dl[e].mesh = mesh_find_or_build(h, n, src[0], &s_dl_arena[dec], (uint32_t) len);
    bool restore = false, other_mtx = false, lit = false, light_setup = false;
    uint32_t verts = 0;
    for (const uint32_t *d = &s_dl_arena[dec], *end = d + len; d < end;) {
        uint32_t c = *d & 0xff, np = *d >> 8;
        d += 1 + np;
        if (c == 0x14) restore = true;
        else if (c >= 0x10 && c <= 0x1c) other_mtx = true;
        else if (c == 0x21) lit = true;
        else if (c == 0x32 || c == 0x34 || c == 0x50 || c == 0x60) light_setup = true;
        else if (c >= 0x23 && c <= 0x28) verts++;
    }
    s_dl[e].vertices = (uint16_t) (verts > 0xffff ? 0xffff : verts);
    s_dl[e].kind = light_setup || other_mtx ? DLK_OTHER : restore ? DLK_SKINNED : lit ? DLK_STATIC_LIT : DLK_STATIC;
    s_dl_index[slot] = (int16_t) (e + 1);
    return e;
}

static uint32_t s_draw_light_version = ~0u, s_draw_light_gen = ~0u, s_draw_light_index;

static bool record_draw(int32_t mesh_id) {
    if (mesh_id < 0 || !G.gpu_mode || !g_gx3d_gpu_lists) return false;
    const GxMesh *m = &s_meshes[mesh_id];
    uint32_t texmode = G.teximage >> 30;
    uint32_t attr = G.polyattr_pending;
    if (texmode > 1 || ((attr >> 4) & 3) == 3) return false;
    Gx3dFrame *f = building();
    if (f->draw_count >= GX_MAX_DRAWS) return false;
    uint32_t light = 0xffff;
    if (m->lit) {
        const GxLightState *ls = light_state();
        if (s_draw_light_version != s_light_version || s_draw_light_gen != G.list_gen) {
            if (f->light_count >= GX_MAX_LIGHT_STATES) return false;
            f->lights[f->light_count] = *ls;
            s_draw_light_index = f->light_count++;
            s_draw_light_version = s_light_version;
            s_draw_light_gen = G.list_gen;
        }
        light = s_draw_light_index;
    }
    gpu_matrix(f);
    GxDraw *d = &f->draws[f->draw_count++];
    d->at = f->polygon_count;
    d->mesh = (uint32_t) mesh_id;
    d->gen = atomic_load(&s_mesh_gen);
    d->mtx = G.gpu_mtx;
    d->light = (uint16_t) light;
    d->mask = (uint8_t) (attr & 15);
    d->texmode = (uint8_t) texmode;
    d->attr = attr;
    d->teximage = G.teximage;
    d->pltt = G.pltt;
    memcpy(d->viewport, G.viewport, 4);
    d->aspect = current_aspect(f);
    vec9_now(d->vec);
    const fx32 *t = G.tex.m;
    d->tm[0] = t[0], d->tm[1] = t[1], d->tm[2] = t[4], d->tm[3] = t[5];
    d->tm[4] = t[8] + t[12], d->tm[5] = t[9] + t[13];
    memcpy(d->color, G.color, 3);
    d->s = texmode == 1 ? G.ts : G.s;
    d->t = texmode == 1 ? G.tt : G.t;
    f->gpu_vertices = 1;
    G.polyattr = attr;
    G.prim = m->last_prim;
    G.strip_count = 0;
    G.strip_total = 0;
    G.prim_index = 0;
    G.vx = m->last_v[0], G.vy = m->last_v[1], G.vz = m->last_v[2];
    if (m->last_color_cmd) {
        G.params[0] = m->last_color_param;
        G.param_index = 0;
        execute(m->last_color_cmd);
    }
    if (m->has_texcoord) {
        G.params[0] = m->last_texcoord;
        G.param_index = 0;
        execute(0x22);
    }
    return true;
}

static void replay_list(uint32_t index) {
    const DlEntry *e = &s_dl[index];
    if (G.packed_left != 0 || G.param_index != 0) {
        for (uint32_t i = 0; i < e->n; i++) fifo_write_(s_dl_arena[e->raw + i]);
        return;
    }
    if (record_draw(e->mesh)) return;
    const uint32_t *d = &s_dl_arena[e->dec], *end = d + e->dec_len;
    while (d < end) {
        uint32_t c = *d & 0xff, np = *d >> 8;
        d++;
        for (uint32_t j = 0; j < np; j++) G.params[j] = d[j];
        d += np;
        G.param_index = 0;
        execute(c);
    }
}

void gx3d_fifo_write_block(const volatile uint32_t *src, uint32_t n, int step) {
    if (g_gx3d_async) {
        if (step == 1 && n >= 16) {
            int e = dl_find_or_add(src, n);
            if (e >= 0) {
                q_push(Q_LIST, (uint32_t) e);
                return;
            }
        }
        q_push_block(0, src, n, step);
        return;
    }
    for (uint32_t i = 0; i < n; i++, src += step) gx3d_fifo_write(*src);
}
void gx3d_port_write_block(uint32_t addr, const volatile uint32_t *src, uint32_t n, int step) {
    if (g_gx3d_async) {
        q_push_block(addr, src, n, step);
        return;
    }
    for (uint32_t i = 0; i < n; i++, src += step) gx3d_port_write(addr, *src);
}
void gx3d_fifo_write(uint32_t word) {
    if (g_gx3d_async) {
        q_push(0, word);
        return;
    }
    G.draw_id = g_port_draw_id;
    G.item_vbl = s_vbl;
    fifo_write_(word);
}

// ------------------------------------------------------------------------------------------------ frames
void gx3d_init(void) {
    memset(&G, 0, sizeof(G));
    memset(s_clip_cache, 0xff, sizeof(s_clip_cache));
    s_draw_light_gen = ~0u;
    s_light.dirty = true;
    s_light_version++;
    s_q_draw_id = s_q_vbl = ~0u;
    G.pos_serial = ++s_serial_counter;
    G.proj_serial = ++s_serial_counter;
    mtx_identity(&G.proj);
    G.proj_aspect_dirty = true;
    mtx_identity(&G.pos);
    mtx_identity(&G.vec);
    mtx_identity(&G.tex);
    mtx_identity(&G.clip);
    G.prim = -1;
    G.viewport[2] = 255;
    G.viewport[3] = 191;
    for (int i = 0; i < GX_LISTS; i++) {
        if (!s_lists[i]) s_lists[i] = calloc(1, sizeof(Gx3dFrame));
        s_lists[i]->vertex_count = s_lists[i]->polygon_count = s_lists[i]->matrix_count = 0;
        s_lists[i]->draw_count = s_lists[i]->light_count = 0;
        s_lists[i]->gpu_vertices = 0;
        s_lists[i]->serial = 0;
        atomic_store(&s_refs[i], 0);
    }
    G.building = 0;
    G.displayed = 1;
    G.prev_displayed = 2;
    G.gpu_mode = g_gx3d_gpu_vertices;
    update_gxstat();
}

const Gx3dFrame *gx3d_displayed(void) { return s_lists[G.displayed]; }

static void latch_render_registers(Gx3dFrame *f) {
    f->disp3dcnt = IO16(0x060);
    f->clear_color = IO32(0x350);
    f->clear_depth = IO16(0x354);
    f->clrimage_offset = IO16(0x356);
    f->fog_color = IO32(0x358);
    f->fog_offset = IO16(0x35c);
    memcpy(f->fog_table, DS_PTR(0x04000360), 32);
    memcpy(f->edge_color, DS_PTR(0x04000330), 16);
    memcpy(f->toon_table, DS_PTR(0x04000380), 64);
    f->alpha_ref = IO8(0x340) & 31;
}

// A frame the game finished without SWAP_BUFFERS (a multiplayer frame whose shared data was late): the DS keeps
// showing the previous picture. Its geometry is dropped, so that it does not pile up under the next frame's (the same
// scene twice: overflowing lists, doubled translucent polygons, model pieces the interpolation cannot pair).
void gx3d_discard(void) {
    q_drain();
    if (G.swap_pending) {
        return;
    }
    Gx3dFrame *f = building();
    f->polygon_count = 0;
    f->proj_aspect = 0;
    f->vertex_count = 0;
    f->matrix_count = 0;
    f->draw_count = 0;
    f->light_count = 0;
    G.list_gen++;
    f->gpu_vertices = 0;
    G.gpu_mtx_stale = true;
    G.draw_tag = 0;
    update_gxstat();
}

// VBlank: a list ended by SWAP_BUFFERS becomes the displayed one and a new list starts.
static void *s_list_lock;
static volatile bool s_sync_swap = true;

static void list_lock(void) {
    if (!s_list_lock) s_list_lock = hapi_mutex_new();
    hapi_mutex_lock(s_list_lock);
}

static void complete_list(void) {
    G.swap_pending = false;
    Gx3dFrame *done = building();
    done->swap_param = G.swap_param;
    latch_render_registers(done);
    static uint64_t serial;
    done->serial = ++serial;
    done->issue_vbl = G.swap_vbl;
    G.prev_displayed = G.displayed;
    G.displayed = G.building;
    for (;;) {
        int found = -1;
        for (int i = 0; i < GX_LISTS && found < 0; i++) {
            if (i != G.displayed && i != G.prev_displayed && atomic_load(&s_refs[i]) == 0) found = i;
        }
        if (found >= 0) {
            G.building = found;
            break;
        }
        hapi_mutex_unlock(s_list_lock);
        hapi_sleep_ns(100000);
        hapi_mutex_lock(s_list_lock);
    }
    Gx3dFrame *next = building();
    next->polygon_count = 0;
    next->proj_aspect = 0;
    next->vertex_count = 0;
    next->matrix_count = 0;
    next->draw_count = 0;
    next->light_count = 0;
    G.list_gen++;
    next->gpu_vertices = 0;
    G.gpu_mtx_stale = true;
    G.gpu_mode = g_gx3d_gpu_vertices;
    G.draw_tag = 0;
}

static void swap_buffers_ran(void) {
    if (!g_gx3d_async || s_sync_swap) return;
    list_lock();
    complete_list();
    hapi_mutex_unlock(s_list_lock);
}

static int s_prev_top = -1;
static int s_alternating;

const Gx3dFrame *gx3d_acquire_displayed(void) {
    int top = (IO16(0x304) >> 15) & 1;
    bool alternating = s_alternating > 0 || (s_prev_top >= 0 && top != s_prev_top);
    if (alternating && g_gx3d_async && !s_sync_swap) q_drain();
    list_lock();
    int i = G.displayed;
    if (alternating && !s_sync_swap) {
        uint32_t want = s_vbl - 1;
        const Gx3dFrame *pv = s_lists[G.prev_displayed];
        if ((int32_t) (s_lists[i]->issue_vbl - want) > 0 && pv->serial && (int32_t) (pv->issue_vbl - want) <= 0)
            i = G.prev_displayed;
    }
    atomic_fetch_add(&s_refs[i], 1);
    hapi_mutex_unlock(s_list_lock);
    return s_lists[i];
}

void gx3d_release_list(const Gx3dFrame *f) {
    for (int i = 0; i < GX_LISTS; i++) {
        if (s_lists[i] == f) {
            atomic_fetch_sub(&s_refs[i], 1);
            return;
        }
    }
}

void gx3d_copy_displayed(Gx3dFrame *dst) {
    const Gx3dFrame *f = gx3d_acquire_displayed();
    gx3d_copy_frame(dst, f);
    gx3d_release_list(f);
}

static void vblank_(void);
void gx3d_vblank(void) {
    vblank_();
    s_vbl++;
}
static void vblank_(void) {
    int top = (IO16(0x304) >> 15) & 1;
    s_alternating = s_prev_top >= 0 && top != s_prev_top ? 8 : s_alternating ? s_alternating - 1 : 0;
    s_prev_top = top;
    bool sync = !g_gx3d_async || s_alternating > 0;
    if (!sync && s_sync_swap) {
        q_drain();
        if (G.swap_pending) {
            list_lock();
            complete_list();
            hapi_mutex_unlock(s_list_lock);
        }
        s_sync_swap = false;
        return;
    }
    if (!sync) return;
    q_drain();
    s_sync_swap = true;
    if (!G.swap_pending) {
        return;
    }
    list_lock();
    complete_list();
    hapi_mutex_unlock(s_list_lock);
    update_gxstat();
}
