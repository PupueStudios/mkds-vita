#include "port.h"
#include "gpu2d.h"
#include "tex.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct V {
    float x, y, z, w, s, t, r, g, b;
} V;

typedef struct TexEntry {
    uint32_t key, pltt;
    uint32_t *texels;
    uint32_t hash, seen, map, frame;
    uint64_t pages;
} TexEntry;

#define TEX_CACHE 512
static TexEntry s_cache[TEX_CACHE];
static int s_cache_count;
static uint32_t s_frame;
static const uint32_t *s_vram_version;
static uint32_t s_vram_newest, s_map_sig;

static const uint32_t *texture(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt) {
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 0) return NULL;
    uint32_t key = teximage & 0x3ff0ffff;
    if (fmt == 7) pltt = 0;
    for (int i = 0; i < s_cache_count; i++) {
        TexEntry *e = &s_cache[i];
        if (e->key != key || e->pltt != pltt) continue;
        if (e->frame != s_frame) {
            e->frame = s_frame;
            if (e->map != s_map_sig || tex_pages_changed(e->pages, s_vram_version, e->seen)) {
                uint32_t hash = tex_source_hash(map, vram, teximage, pltt, &e->pages);
                e->map = s_map_sig;
                e->seen = s_vram_newest;
                if (hash != e->hash) {
                    e->hash = hash;
                    if (!tex_decode(map, vram, teximage, pltt, e->texels)) return NULL;
                }
            }
        }
        return e->texels;
    }
    TexEntry *e = NULL;
    if (s_cache_count < TEX_CACHE) {
        e = &s_cache[s_cache_count];
    } else {
        for (int i = 0; i < TEX_CACHE && !e; i++) {
            if (s_cache[i].frame != s_frame) e = &s_cache[i];
        }
        if (!e) return NULL;
        free(e->texels);
        *e = s_cache[--s_cache_count];
        e = &s_cache[s_cache_count];
    }
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t *t = malloc(w * h * 4);
    if (!t) return NULL;
    if (!tex_decode(map, vram, teximage, pltt, t)) {
        free(t);
        return NULL;
    }
    s_cache_count++;
    e->key = key;
    e->pltt = pltt;
    e->texels = t;
    e->frame = s_frame;
    e->hash = tex_source_hash(map, vram, teximage, pltt, &e->pages);
    e->map = s_map_sig;
    e->seen = s_vram_newest;
    return t;
}

enum { A_Z, A_W, A_S, A_T, A_R, A_G, A_B, A_COUNT };

typedef struct Tri {
    float ox, oy;
    float l0[3], ldx[3], ldy[3];
    float a0[A_COUNT], adx[A_COUNT], ady[A_COUNT];
    int16_t minx, maxx, miny, maxy;
    const uint32_t *tex;
    uint32_t teximage;
    uint8_t alpha, mode;
    bool depth_equal, update_depth;
} Tri;

static Tri *s_tris;
static size_t s_tri_count, s_tri_cap;
static float s_depth[256 * 192];

static void add_triangle(const V *a, const V *b, const V *c, const GxPolygon *p, const uint32_t *tex,
                         bool translucent_pass) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-6f) return;
    float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
    float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
    if (maxx < 0 || maxy < 0 || minx > 256 || miny > 192) return;
    if (s_tri_count == s_tri_cap) {
        s_tri_cap = s_tri_cap ? s_tri_cap * 2 : 4096;
        s_tris = realloc(s_tris, s_tri_cap * sizeof(Tri));
    }
    Tri *t = &s_tris[s_tri_count++];
    int x0 = (int) floorf(minx), x1 = (int) ceilf(maxx), y0 = (int) floorf(miny), y1 = (int) ceilf(maxy);
    t->minx = (int16_t) (x0 < 0 ? 0 : x0);
    t->maxx = (int16_t) (x1 > 255 ? 255 : x1);
    t->miny = (int16_t) (y0 < 0 ? 0 : y0);
    t->maxy = (int16_t) (y1 > 191 ? 191 : y1);
    const V *v[3] = {a, b, c};
    float inv = 1.0f / area;
    for (int i = 0; i < 3; i++) {
        const V *p1 = v[(i + 1) % 3], *p2 = v[(i + 2) % 3];
        float dx = -(p2->y - p1->y) * inv;
        float dy = (p2->x - p1->x) * inv;
        t->l0[i] = i == 0 ? 1.0f : 0.0f;
        t->ldx[i] = dx;
        t->ldy[i] = dy;
    }
    t->ox = a->x;
    t->oy = a->y;
    float q[3][A_COUNT];
    for (int i = 0; i < 3; i++) {
        float w = v[i]->w;
        q[i][A_Z] = v[i]->z;
        q[i][A_W] = w;
        q[i][A_S] = v[i]->s * w;
        q[i][A_T] = v[i]->t * w;
        q[i][A_R] = v[i]->r * w;
        q[i][A_G] = v[i]->g * w;
        q[i][A_B] = v[i]->b * w;
    }
    for (int k = 0; k < A_COUNT; k++) {
        t->a0[k] = q[0][k];
        t->adx[k] = t->ldx[0] * q[0][k] + t->ldx[1] * q[1][k] + t->ldx[2] * q[2][k];
        t->ady[k] = t->ldy[0] * q[0][k] + t->ldy[1] * q[1][k] + t->ldy[2] * q[2][k];
    }
    uint32_t alpha = (p->attr >> 16) & 31;
    t->alpha = (uint8_t) (alpha == 0 ? 31 : alpha); // wireframe drawn solid here
    t->mode = (uint8_t) ((p->attr >> 4) & 3);
    t->depth_equal = p->attr & (1u << 14);
    t->update_depth = !translucent_pass || (p->attr & (1u << 11));
    t->tex = tex;
    t->teximage = p->teximage;
}

// Clips a polygon to z >= -w (the near plane), in clip space.
static int clip_near(const V *in, int n, V *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const V *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            V v;
            float *pv = &v.x;
            const float *pa = &a->x, *pb = &b->x;
            for (int k = 0; k < 9; k++) pv[k] = pa[k] + (pb[k] - pa[k]) * t;
            out[m++] = v;
        }
    }
    return m;
}

static inline int floor_int(float f) {
    int i = (int) f;
    return i - (f < (float) i);
}

static inline int wrap(int u, int size, bool repeat, bool flip) {
    if (!repeat) return u < 0 ? 0 : u >= size ? size - 1 : u;
    if (!flip) return u & (size - 1);
    int p = u & (2 * size - 1);
    return p >= size ? 2 * size - 1 - p : p;
}

static void draw_triangle_rows(const Tri *t, uint32_t *out, int y0, int y1) {
    int ya = t->miny > y0 ? t->miny : y0, yb = t->maxy < y1 - 1 ? t->maxy : y1 - 1;
    const uint32_t *tex = t->tex;
    int tw = (int) TEX_WIDTH(t->teximage), th = (int) TEX_HEIGHT(t->teximage);
    bool rs = t->teximage & (1u << 16), rt = t->teximage & (1u << 17);
    bool fs = t->teximage & (1u << 18), ft = t->teximage & (1u << 19);
    uint32_t alpha = t->alpha, mode = t->mode;
    if (mode == 3) return; // shadow polygons need the stencil logic: skipped here
    for (int y = ya; y <= yb; y++) {
        float fy = (float) y + 0.5f - t->oy, fx0 = 0.5f - t->ox;
        float lo = (float) t->minx, hi = (float) t->maxx;
        for (int i = 0; i < 3; i++) {
            float c = t->l0[i] + t->ldy[i] * fy + t->ldx[i] * fx0, g = t->ldx[i];
            if (g > 0) {
                float b = -c / g;
                if (b > lo) lo = b;
            } else if (g < 0) {
                float b = -c / g;
                if (b < hi) hi = b;
            } else if (c < 0) {
                lo = 1, hi = 0;
            }
        }
        if (lo > hi) continue;
        int xa = (int) ceilf(lo), xb = (int) floorf(hi);
        if (xa < t->minx) xa = t->minx;
        if (xb > t->maxx) xb = t->maxx;
        if (xa > xb) continue;
        float a[A_COUNT];
        for (int k = 0; k < A_COUNT; k++) a[k] = t->a0[k] + t->ady[k] * fy + t->adx[k] * ((float) xa + fx0);
        float *dz = &s_depth[y * 256 + xa];
        uint32_t *o = &out[y * 256 + xa];
        for (int x = xa; x <= xb; x++, dz++, o++) {
            float z = a[A_Z], iw = a[A_W], sw = a[A_S], tw_ = a[A_T], rw = a[A_R], gw = a[A_G], bw = a[A_B];
            for (int k = 0; k < A_COUNT; k++) a[k] += t->adx[k];
            if (t->depth_equal ? fabsf(z - *dz) > 1e-4f : z >= *dz) continue;
            float k_inv = 1.0f / iw;
            uint32_t cr = (uint32_t) (rw * k_inv * (255.0f / 31.0f)), cg = (uint32_t) (gw * k_inv * (255.0f / 31.0f));
            uint32_t cb = (uint32_t) (bw * k_inv * (255.0f / 31.0f));
            if (cr > 255) cr = 255;
            if (cg > 255) cg = 255;
            if (cb > 255) cb = 255;
            uint32_t ca = alpha;
            if (tex) {
                int u = floor_int(sw * k_inv * 0.0625f), v = floor_int(tw_ * k_inv * 0.0625f);
                u = wrap(u, tw, rs, fs);
                v = wrap(v, th, rt, ft);
                uint32_t texel = tex[v * tw + u];
                uint32_t ta = texel >> 24;
                if (ta == 0) continue;
                uint32_t tr = texel & 255, tg = (texel >> 8) & 255, tb = (texel >> 16) & 255;
                if (mode == 1) { // decal
                    cr = (tr * ta + cr * (31 - ta)) / 31;
                    cg = (tg * ta + cg * (31 - ta)) / 31;
                    cb = (tb * ta + cb * (31 - ta)) / 31;
                } else {
                    cr = cr * tr / 255;
                    cg = cg * tg / 255;
                    cb = cb * tb / 255;
                    ca = ca * ta / 31;
                }
            }
            if (ca < 31 && (*o >> 24)) {
                uint32_t d = *o;
                cr = (cr * ca + (d & 255) * (31 - ca)) / 31;
                cg = (cg * ca + ((d >> 8) & 255) * (31 - ca)) / 31;
                cb = (cb * ca + ((d >> 16) & 255) * (31 - ca)) / 31;
                uint32_t da = d >> 24;
                ca = ca > da ? ca : da;
            }
            if (t->update_depth) *dz = z;
            *o = cr | cg << 8 | cb << 16 | ca << 24;
        }
    }
}

typedef struct Job {
    uint32_t *out;
    uint32_t clear_px;
} Job;

static void draw_rows(void *arg, int y0, int y1) {
    const Job *j = arg;
    for (int i = y0 * 256; i < y1 * 256; i++) {
        j->out[i] = j->clear_px;
        s_depth[i] = 1e30f;
    }
    for (size_t i = 0; i < s_tri_count; i++) {
        const Tri *t = &s_tris[i];
        if (t->maxy < y0 || t->miny >= y1) continue;
        draw_triangle_rows(t, j->out, y0, y1);
    }
}

void soft3d_render(const Gx3dFrame *frame, const FrameSnapshot *f, uint32_t *out) {
    VramMap map;
    vram_map_build(&map, f->vramcnt);
    s_frame++;
    s_vram_version = f->vram_version;
    s_vram_newest = tex_newest_version(f->vram_version);
    s_map_sig = tex_map_signature(&map);
    uint32_t clear = frame->clear_color;
    uint32_t cr = clear & 31, cg = (clear >> 5) & 31, cb = (clear >> 10) & 31, ca = (clear >> 16) & 31;
    Job job = {out, (cr * 255 / 31) | (cg * 255 / 31) << 8 | (cb * 255 / 31) << 16 | ca << 24};

    s_tri_count = 0;
    bool texturing = frame->disp3dcnt & 1;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < frame->polygon_count; i++) {
            const GxPolygon *p = &frame->polygons[i];
            if (p->hidden) continue;
            if (((p->attr >> 4) & 3) == 3) continue;
            uint32_t alpha = (p->attr >> 16) & 31;
            bool fmt_alpha = ((p->teximage >> 26) & 7) == 1 || ((p->teximage >> 26) & 7) == 6;
            bool translucent = (alpha != 0 && alpha != 31) || fmt_alpha;
            if (translucent != (pass == 1)) continue;
            V v[8], clipped[16];
            int count = p->count < 8 ? p->count : 8;
            for (int k = 0; k < count; k++) {
                const GxVertex *gv = &frame->vertices[p->first + k];
                v[k] = (V) {gv->x / 4096.0f, gv->y / 4096.0f, gv->z / 4096.0f, gv->w / 4096.0f,
                            gv->s,           gv->t,           gv->r,           gv->g,
                            gv->b};
            }
            int n = clip_near(v, count, clipped);
            if (n < 3) continue;
            const uint32_t *tex = texturing ? texture(&map, f->vram, p->teximage, p->pltt) : NULL;
            float x1 = p->viewport[0], y1 = p->viewport[1], x2 = p->viewport[2], y2 = p->viewport[3];
            for (int k = 0; k < n; k++) {
                V *c = &clipped[k];
                float w = c->w > 1e-6f ? c->w : 1e-6f;
                float nx = c->x / w, ny = c->y / w, nz = c->z / w;
                c->x = (nx + 1) * 0.5f * (x2 - x1 + 1) + x1;
                c->y = 192.0f - ((ny + 1) * 0.5f * (y2 - y1 + 1) + y1);
                c->z = nz;
                c->w = 1.0f / w;
            }
            for (int k = 1; k + 1 < n; k++) add_triangle(&clipped[0], &clipped[k], &clipped[k + 1], p, tex, pass == 1);
        }
    }
    gpu2d_parallel_rows(draw_rows, &job);
}
