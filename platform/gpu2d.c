// The DS 2D engines in software: text, affine and bitmap backgrounds, sprites, windows, colour effects, master
// brightness, VRAM display and display capture. Renders from a FrameSnapshot, one scanline at a time, at the native
// 256x192 resolution.
#include "port.h"
#include "gpu2d.h"
#include "hostapi.h"

#include <string.h>
#include <stdlib.h>

static void capture_done_in_process(void) {
    *(volatile uint8_t *) DS_PTR(0x04000067) &= 0x7f; // DISPCAPCNT: capture finished
}
void (*g_capture_done)(void) = capture_done_in_process;

// ------------------------------------------------------------------------------------------------ VRAM mapping
#define PAGE 0x4000u

static void map_pages(VramMap *m, int region, uint32_t region_offset, int bank, uint32_t size) {
    for (uint32_t off = 0; off < size; off += PAGE) {
        uint32_t page = (region_offset + off) / PAGE;
        if (page < 48) {
            m->page[region][page] = (int32_t) (g_vram_bank_offset[bank] + off);
        }
    }
}

void vram_map_build(VramMap *m, const uint8_t vramcnt[10]) {
    memset(m, 0xff, sizeof(*m));
    static const int8_t bank_of[10] = {VRAM_A, VRAM_B, VRAM_C, VRAM_D, VRAM_E, VRAM_F, VRAM_G, -1, VRAM_H, VRAM_I};
    static const uint32_t lcdc_offset[VRAM_BANK_COUNT] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                                          0x90000, 0x94000, 0x98000, 0xa0000};
    for (int i = 0; i < 10; i++) {
        int bank = bank_of[i];
        if (bank < 0) {
            continue;
        }
        uint8_t cnt = vramcnt[i];
        if (!(cnt & 0x80)) {
            continue;
        }
        uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3, size = g_vram_bank_size[bank];
        switch (bank) {
        case VRAM_A:
        case VRAM_B:
        case VRAM_C:
        case VRAM_D:
            if (bank >= VRAM_C && mst == 4) {
                map_pages(m, bank == VRAM_C ? VR_BG_B : VR_OBJ_B, 0, bank, size);
                break;
            }
            switch (mst & 3) {
            case 0: map_pages(m, VR_LCDC, lcdc_offset[bank], bank, size); break;
            case 1: map_pages(m, VR_BG_A, ofs * 0x20000, bank, size); break;
            case 2:
                if (bank <= VRAM_B) map_pages(m, VR_OBJ_A, (ofs & 1) * 0x20000, bank, size);
                break;
            case 3: map_pages(m, VR_TEX, ofs * 0x20000, bank, size); break;
            }
            break;
        case VRAM_E:
            switch (mst) {
            case 0: map_pages(m, VR_LCDC, lcdc_offset[bank], bank, size); break;
            case 1: map_pages(m, VR_BG_A, 0, bank, size); break;
            case 2: map_pages(m, VR_OBJ_A, 0, bank, size); break;
            case 3: map_pages(m, VR_TEXPAL, 0, bank, size); break;
            case 4: map_pages(m, VR_BGEXT_A, 0, bank, 0x8000); break;
            }
            break;
        case VRAM_F:
        case VRAM_G: {
            uint32_t o = (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
            switch (mst) {
            case 0: map_pages(m, VR_LCDC, lcdc_offset[bank], bank, size); break;
            case 1: map_pages(m, VR_BG_A, o, bank, size); break;
            case 2: map_pages(m, VR_OBJ_A, o, bank, size); break;
            case 3: map_pages(m, VR_TEXPAL, ((ofs & 1) + (ofs >> 1) * 4) * 0x4000, bank, size); break;
            case 4: map_pages(m, VR_BGEXT_A, (ofs & 1) * 0x4000, bank, size); break;
            case 5: map_pages(m, VR_OBJEXT_A, 0, bank, size); break;
            }
            break;
        }
        case VRAM_H:
            switch (mst & 3) {
            case 0: map_pages(m, VR_LCDC, lcdc_offset[bank], bank, size); break;
            case 1: map_pages(m, VR_BG_B, 0, bank, size); break;
            case 2: map_pages(m, VR_BGEXT_B, 0, bank, size); break;
            }
            break;
        case VRAM_I:
            switch (mst & 3) {
            case 0: map_pages(m, VR_LCDC, lcdc_offset[bank], bank, size); break;
            case 1: map_pages(m, VR_BG_B, 0x8000, bank, size); break;
            case 2: map_pages(m, VR_OBJ_B, 0, bank, size); break;
            case 3: map_pages(m, VR_OBJEXT_B, 0, bank, size); break;
            }
            break;
        }
    }
}

static inline const uint8_t *vptr(const VramMap *m, const uint8_t *vram, int region, uint32_t offset) {
    uint32_t page = offset / PAGE;
    if (page >= 48) return NULL;
    int32_t at = m->page[region][page];
    return at < 0 ? NULL : vram + at + (offset % PAGE);
}

const uint8_t *vram_ptr(const VramMap *m, const uint8_t *vram, int region, uint32_t offset) {
    return vptr(m, vram, region, offset);
}

static inline uint8_t rd8(const VramMap *m, const uint8_t *vram, int region, uint32_t offset) {
    const uint8_t *p = vptr(m, vram, region, offset);
    return p ? *p : 0;
}

static inline uint16_t rd16(const VramMap *m, const uint8_t *vram, int region, uint32_t offset) {
    const uint8_t *p = vptr(m, vram, region, offset & ~1u);
    return p ? (uint16_t) (p[0] | p[1] << 8) : 0;
}

// ------------------------------------------------------------------------------------------------ engine state
// A layer pixel: BGR555 colour in bits 0..14, bit 15 = present. 3D pixels carry their alpha and the full colour.
typedef struct Engine {
    const FrameSnapshot *f;
    const VramMap *map;
    const uint8_t *io; // this engine's registers
    const uint16_t *pal; // BG palette (256), OBJ palette follows at +256
    bool is_a;
    int bg_region, obj_region, bgext_region, objext_region;
    uint32_t dispcnt;
    // Widescreen: DS columns drawn beyond each side (the line is 256 + 2 * margin wide, DS x = index - margin).
    // Only the bottom background layer and the backdrop continue there; sprites near the side edges move out with
    // the screen edges in races (hud_shift).
    int margin;
    bool hud_shift;
    int8_t obj_shift[128]; // hud_shift: -1 / 0 / 1 margin for each sprite
    int ext_bg; // the layer drawn across the whole width, -1 none
    // A still picture as the extended layer: beyond the DS screen it shows its edge colours, blurred vertically
    // (per line, BGR555 | 0x8000 when opaque), fading a little towards the outside, instead of repeating.
    const uint16_t *side[2];
    bool hud;
    uint8_t obj_mode, obj_lo, obj_hi;
} Engine;

static inline uint16_t fade555(uint16_t c, int d, int margin) {
    if (!(c & 0x8000)) return 0;
    int f = 256 - d * 96 / margin; // down to 62% at the outer edge
    int r = (c & 31) * f >> 8, g = ((c >> 5) & 31) * f >> 8, b = ((c >> 10) & 31) * f >> 8;
    return (uint16_t) (0x8000 | r | g << 5 | b << 10);
}

static inline uint16_t io16(const Engine *e, uint32_t off) { return (uint16_t) (e->io[off] | e->io[off + 1] << 8); }
static inline uint32_t io32(const Engine *e, uint32_t off) { return io16(e, off) | (uint32_t) io16(e, off + 2) << 16; }

#define LAYER_OBJ 4
#define LAYER_BD 5

typedef struct Line { // indexed by DS x + margin
    uint16_t bg[4][GPU2D_MAX_W] __attribute__((aligned(16))); // bit 15 = opaque
    uint16_t obj[GPU2D_MAX_W] __attribute__((aligned(16)));
    uint8_t obj_prio[GPU2D_MAX_W] __attribute__((aligned(16))); // 4 = none
    uint8_t obj_alpha[GPU2D_MAX_W]; // 0xff normal, 0xfe semi-transparent, 0..15 bitmap alpha
    uint8_t obj_window[GPU2D_MAX_W];
    uint8_t obj_prios;
} Line;

// Columns a background layer covers: the whole width for the extended layer, the DS screen otherwise.
static inline void bg_span(const Engine *e, int bg, int *x0, int *x1) {
    bool ext = bg == e->ext_bg;
    *x0 = ext ? -e->margin : 0;
    *x1 = ext ? 256 + e->margin : 256;
}

static uint16_t ext_palette_color(const Engine *e, int region, uint32_t slot_offset, uint32_t index) {
    return rd16(e->map, e->f->vram, region, slot_offset + index * 2) & 0x7fff;
}

// Text background.
static void draw_text_bg(const Engine *e, int bg, int y, uint16_t *out) {
    uint16_t cnt = io16(e, 0x08 + bg * 2);
    uint32_t hofs = io16(e, 0x10 + bg * 4) & 0x1ff, vofs = io16(e, 0x12 + bg * 4) & 0x1ff;
    uint32_t size = cnt >> 14;
    uint32_t w = size & 1 ? 512 : 256, h = size & 2 ? 512 : 256;
    uint32_t screen_base = ((cnt >> 8) & 31) * 0x800, char_base = ((cnt >> 2) & 15) * 0x4000;
    if (e->is_a) {
        screen_base += ((e->dispcnt >> 27) & 7) * 0x10000;
        char_base += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    bool c256 = cnt & 0x80;
    bool ext = c256 && (e->dispcnt & 0x40000000);
    uint32_t ext_slot = (uint32_t) bg;
    if (bg < 2 && (cnt & 0x2000)) {
        ext_slot += 2;
    }
    uint32_t sy = (y + vofs) & (h - 1);
    int x0, x1;
    bg_span(e, bg, &x0, &x1);
    out += e->margin;
    const uint8_t *vram = e->f->vram;
    uint32_t row_base = screen_base + (sy >= 256 ? (w == 512 ? 2 : 1) * 0x800 : 0) + ((sy & 255) / 8) * 64;
    uint32_t ty0 = sy & 7;
    const int ranges[3][2] = {{x0, x1}, {-e->margin, 0}, {256, 256 + e->margin}};
    for (int range = 0; range < 3; range++) {
    if (range == 1 && !(e->hud_shift && bg != e->ext_bg && e->margin > 0 && (out[0] & 0x8000) && (out[255] & 0x8000))) {
        break;
    }
    int x = ranges[range][0], xend = ranges[range][1];
    while (x < xend) {
        if ((x < 0 || x > 255) && e->side[0] && range == 0) {
            out[x] = fade555(e->side[x > 255][y], x < 0 ? -x : x - 255, e->margin);
            x++;
            continue;
        }
        uint32_t sx = ((uint32_t) x + hofs) & (w - 1);
        int end = x + (int) (8 - (sx & 7));
        if (end > xend) end = xend;
        if (e->side[0] && range == 0 && x <= 255 && end > 256) end = 256;
        uint32_t map_addr = row_base + (sx >= 256 ? 0x800 : 0) + ((sx & 255) / 8) * 2;
        uint16_t entry = rd16(e->map, vram, e->bg_region, map_addr);
        uint32_t ty = entry & 0x800 ? 7 - ty0 : ty0;
        uint32_t flip = entry & 0x400 ? 7 : 0;
        uint32_t tile = entry & 0x3ff;
        if (c256) {
            const uint8_t *row = vram_ptr(e->map, vram, e->bg_region, char_base + tile * 64 + ty * 8);
            const uint8_t *ext_pal =
                ext ? vram_ptr(e->map, vram, e->bgext_region, ext_slot * 0x2000 + (entry >> 12) * 512) : NULL;
            for (; x < end; x++) {
                uint32_t tx = (((uint32_t) x + hofs) & 7) ^ flip;
                uint8_t idx = row ? row[tx] : 0;
                if (!idx) continue;
                uint16_t color = ext ? (ext_pal ? (uint16_t) (ext_pal[idx * 2] | ext_pal[idx * 2 + 1] << 8) : 0)
                                     : e->pal[idx];
                out[x] = (color & 0x7fff) | 0x8000;
            }
        } else {
            const uint8_t *row = vram_ptr(e->map, vram, e->bg_region, char_base + tile * 32 + ty * 4);
            if (!row) {
                x = end;
                continue;
            }
            uint32_t bits = (uint32_t) row[0] | (uint32_t) row[1] << 8 | (uint32_t) row[2] << 16 | (uint32_t) row[3] << 24;
            if (!bits) {
                x = end;
                continue;
            }
            const uint16_t *pal = e->pal + (entry >> 12) * 16;
            if (end - x == 8) {
                if (flip) {
                    bits = __builtin_bswap32(bits);
                    bits = ((bits >> 4) & 0x0f0f0f0fu) | ((bits << 4) & 0xf0f0f0f0u);
                }
                uint16_t *o = out + x;
                for (int k = 0; k < 8; k++, bits >>= 4) {
                    uint32_t idx = bits & 15;
                    if (idx) o[k] = (pal[idx] & 0x7fff) | 0x8000;
                }
                x = end;
                continue;
            }
            for (; x < end; x++) {
                uint32_t tx = (((uint32_t) x + hofs) & 7) ^ flip;
                uint32_t idx = (bits >> (tx * 4)) & 15;
                if (!idx) continue;
                out[x] = (pal[idx] & 0x7fff) | 0x8000;
            }
        }
    }
    }
}

// Affine and extended backgrounds (BG2/BG3).
static void draw_affine_bg(const Engine *e, int bg, int y, uint16_t *out, int kind) {
    // kind: 0 affine (8-bit map), 1 extended with 16-bit map, 2 256-colour bitmap, 3 direct colour bitmap
    uint16_t cnt = io16(e, 0x08 + bg * 2);
    uint32_t base = 0x20 + (bg - 2) * 0x10;
    int32_t pa = (int16_t) io16(e, base), pb = (int16_t) io16(e, base + 2);
    int32_t pc = (int16_t) io16(e, base + 4), pd = (int16_t) io16(e, base + 6);
    int32_t rx = ((int32_t) (io32(e, base + 8) << 4)) >> 4, ry = ((int32_t) (io32(e, base + 12) << 4)) >> 4;
    rx += pb * y;
    ry += pd * y;
    bool wrap = cnt & 0x2000;
    uint32_t size = cnt >> 14;
    uint32_t w, h;
    if (kind <= 1) {
        w = h = 128u << size;
    } else {
        static const uint16_t bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[size];
        h = bh[size];
    }
    uint32_t screen_base = ((cnt >> 8) & 31) * 0x800, char_base = ((cnt >> 2) & 15) * 0x4000;
    if (e->is_a && kind <= 1) {
        screen_base += ((e->dispcnt >> 27) & 7) * 0x10000;
        char_base += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    uint32_t bitmap_base = ((cnt >> 8) & 31) * 0x4000;
    bool ext = e->dispcnt & 0x40000000;
    int x0, x1;
    bg_span(e, bg, &x0, &x1);
    out += e->margin;
    uint32_t page_a = ~0u, page_b = ~0u;
    uint32_t last_cell = ~0u, last_tile = 0;
    const uint8_t *cell_pal = NULL;
    const uint8_t *base_a = NULL, *base_b = NULL;
#define AFF_RD8(which, off)                                                                                            \
    ({                                                                                                                 \
        uint32_t o_ = (off), pg_ = o_ / PAGE;                                                                          \
        if (pg_ != page_##which) {                                                                                     \
            page_##which = pg_;                                                                                        \
            base_##which = vptr(e->map, e->f->vram, e->bg_region, pg_ * PAGE);                                         \
        }                                                                                                              \
        base_##which ? base_##which[o_ % PAGE] : (uint8_t) 0;                                                          \
    })
    for (int x = x0; x < x1; x++) {
        if ((x < 0 || x > 255) && e->side[0]) {
            out[x] = fade555(e->side[x > 255][y], x < 0 ? -x : x - 255, e->margin);
            continue;
        }
        int32_t sx = (rx + pa * x) >> 8, sy = (ry + pc * x) >> 8;
        if (wrap) {
            sx &= (int32_t) w - 1;
            sy &= (int32_t) h - 1;
        } else if (sx < 0 || sy < 0 || sx >= (int32_t) w || sy >= (int32_t) h) {
            continue;
        }
        uint16_t color;
        if (kind == 0) {
            uint32_t cell = (uint32_t) (sy / 8) * (w / 8) + (uint32_t) (sx / 8);
            if (cell != last_cell) {
                last_cell = cell;
                last_tile = AFF_RD8(a, screen_base + cell);
            }
            uint8_t tile = (uint8_t) last_tile;
            uint8_t idx = AFF_RD8(b, char_base + tile * 64 + (sy & 7) * 8 + (sx & 7));
            if (!idx) continue;
            color = e->pal[idx];
        } else if (kind == 1) {
            uint32_t cell = (uint32_t) (sy / 8) * (w / 8) + (uint32_t) (sx / 8);
            if (cell != last_cell) {
                last_cell = cell;
                last_tile = rd16(e->map, e->f->vram, e->bg_region, screen_base + cell * 2);
                if (ext) cell_pal = vptr(e->map, e->f->vram, e->bgext_region, bg * 0x2000 + (last_tile >> 12) * 512);
            }
            uint16_t entry = (uint16_t) last_tile;
            uint32_t tx = sx & 7, ty = sy & 7;
            if (entry & 0x400) tx = 7 - tx;
            if (entry & 0x800) ty = 7 - ty;
            uint8_t idx = AFF_RD8(b, char_base + (entry & 0x3ff) * 64 + ty * 8 + tx);
            if (!idx) continue;
            color = ext ? (cell_pal ? (uint16_t) ((cell_pal[idx * 2] | cell_pal[idx * 2 + 1] << 8) & 0x7fff) : 0)
                        : e->pal[idx];
        } else if (kind == 2) {
            uint8_t idx = AFF_RD8(a, bitmap_base + sy * w + sx);
            if (!idx) continue;
            color = e->pal[idx];
        } else {
            uint32_t o = bitmap_base + ((uint32_t) sy * w + (uint32_t) sx) * 2, pg = o / PAGE;
            if (pg != page_a) {
                page_a = pg;
                base_a = vptr(e->map, e->f->vram, e->bg_region, pg * PAGE);
            }
            uint16_t c = base_a ? (uint16_t) (base_a[o % PAGE] | base_a[o % PAGE + 1] << 8) : 0;
            if (!(c & 0x8000)) continue;
            color = c;
        }
        out[x] = (color & 0x7fff) | 0x8000;
    }
#undef AFF_RD8
}

// One line of a background layer, as the BG mode makes it.
static void draw_bg_line(const Engine *e, int bg, int y, uint16_t *out) {
    uint32_t mode = e->dispcnt & 7;
    if (bg < 2 || (bg == 2 && (mode == 0 || mode == 1 || mode == 3)) || (bg == 3 && mode == 0)) {
        draw_text_bg(e, bg, y, out);
    } else if ((bg == 2 && (mode == 2 || mode == 1)) || (bg == 3 && (mode == 1 || mode == 2))) {
        draw_affine_bg(e, bg, y, out, 0);
    } else {
        uint16_t cnt = io16(e, 0x08 + bg * 2);
        int kind = !(cnt & 0x80) ? 1 : (cnt & 4) ? 3 : 2;
        draw_affine_bg(e, bg, y, out, kind);
    }
}

// Sprites of one line.
static const uint8_t s_obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t s_obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// Race HUD in widescreen: sprites that touch each other form groups (a counter, a word); a group lying by a side of
// the DS screen moves out to that side of the wide screen, one across the middle (titles) stays.
static void plan_hud_shift(Engine *e) {
    const uint16_t *oam = (const uint16_t *) (e->f->oam + (e->is_a ? 0 : 0x400));
    int x0[128], x1[128], y0[128], y1[128], group[128];
    bool on[128];
    for (int i = 0; i < 128; i++) {
        const uint16_t *a = oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1];
        bool affine = a0 & 0x100;
        uint32_t shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        on[i] = (affine || !(a0 & 0x200)) && shape != 3;
        group[i] = i;
        e->obj_shift[i] = 0;
        if (!on[i]) continue;
        int w = s_obj_w[shape][size], h = s_obj_h[shape][size];
        if (affine && (a0 & 0x200)) w *= 2, h *= 2;
        int x = a1 & 0x1ff, y = a0 & 0xff;
        if (x >= 256) x -= 512;
        if (y >= 192) y -= 256;
        x0[i] = x, x1[i] = x + w, y0[i] = y, y1[i] = y + h;
        if (x1[i] <= 0 || x0[i] >= 256 || y1[i] <= 0 || y0[i] >= 192) on[i] = false; // off screen
    }
    // Groups: overlapping vertically and at most a space apart horizontally (words of a title stay together).
    for (int i = 0; i < 128; i++) {
        if (!on[i]) continue;
        for (int j = i + 1; j < 128; j++) {
            if (!on[j]) continue;
            if (y0[i] >= y1[j] || y0[j] >= y1[i]) continue;
            if (x0[i] > x1[j] + 16 || x0[j] > x1[i] + 16) continue;
            int a = i, b = j;
            while (group[a] != a) a = group[a];
            while (group[b] != b) b = group[b];
            if (a != b) group[b < a ? a : b] = b < a ? b : a;
        }
    }
    int gx0[128], gx1[128];
    for (int i = 0; i < 128; i++) gx0[i] = 1000, gx1[i] = -1000;
    for (int i = 0; i < 128; i++) {
        if (!on[i]) continue;
        int g = i;
        while (group[g] != g) g = group[g];
        group[i] = g;
        if (x0[i] < gx0[g]) gx0[g] = x0[i];
        if (x1[i] > gx1[g]) gx1[g] = x1[i];
    }
    for (int i = 0; i < 128; i++) {
        if (!on[i]) continue;
        int g = group[i], w = gx1[g] - gx0[g], cx = (gx0[g] + gx1[g]) / 2;
        if (gx1[g] >= 232 && gx0[g] >= 64) {
            e->obj_shift[i] = 1;
            continue;
        }
        if (gx0[g] <= 24 && gx1[g] <= 192) {
            e->obj_shift[i] = -1;
            continue;
        }
        if (w > 128) continue; // spans the screen
        if (cx < 72) e->obj_shift[i] = -1;
        else if (cx > 184) e->obj_shift[i] = 1;
    }
}

static void draw_objs(const Engine *e, int y, Line *line) {
    const uint16_t *oam = (const uint16_t *) (e->f->oam + (e->is_a ? 0 : 0x400));
    bool map1d = e->dispcnt & 0x10;
    uint32_t bound = map1d ? 32u << ((e->dispcnt >> 20) & 3) : 32;
    bool ext = e->dispcnt & 0x80000000u;
    for (int i = 127; i >= 0; i--) { // backwards: lower OAM indices win
        if (e->obj_mode == 1 ? i < e->obj_lo || i >= e->obj_hi : e->obj_mode == 2 && i >= e->obj_lo && i < e->obj_hi)
            continue;
        const uint16_t *a = oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1], a2 = a[2];
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue; // disabled
        uint32_t shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int w = s_obj_w[shape][size], h = s_obj_h[shape][size];
        int bw = w, bh = h; // bounding box
        if (affine && (a0 & 0x200)) {
            bw *= 2;
            bh *= 2;
        }
        int oy = a0 & 0xff;
        int dy = (y - oy) & 0xff;
        if (dy >= bh) continue;
        int ox = a1 & 0x1ff;
        if (ox >= 256) ox -= 512;
        if (e->hud_shift) ox += e->obj_shift[i] * e->margin;
        uint32_t mode = (a0 >> 10) & 3;
        uint32_t prio = (a2 >> 10) & 3;
        uint32_t tile = a2 & 0x3ff;
        bool c256 = a0 & 0x2000;
        const uint8_t *xpal = c256 && ext ? vptr(e->map, e->f->vram, e->objext_region, (a2 >> 12) * 512) : NULL;
        int bx0 = -e->margin - ox, bx1 = 256 + e->margin - ox;
        if (bx0 < 0) bx0 = 0;
        if (bx1 > bw) bx1 = bw;
        if (bx0 >= bx1) continue;
        if (!affine && mode != 3) {
            int ty = a1 & 0x2000 ? h - 1 - dy : dy;
            bool hflip = a1 & 0x1000;
            uint32_t row_in_tile = (uint32_t) (ty & 7) * (c256 ? 8 : 4);
            const uint8_t *row = NULL;
            int row_col = -1;
            uint8_t alpha = mode == 1 ? 0xfe : 0xff;
            const uint16_t *pal16 = e->pal + 256 + (a2 >> 12) * 16;
            for (int bx = bx0; bx < bx1; bx++) {
                int tx = hflip ? w - 1 - bx : bx;
                int col = tx >> 3;
                if (col != row_col) {
                    row_col = col;
                    uint32_t addr;
                    if (c256) {
                        addr = map1d ? tile * bound + ((uint32_t) (ty / 8) * (uint32_t) (w / 8) + (uint32_t) col) * 64
                                     : tile * 32 + (uint32_t) (ty / 8) * 32 * 32 + (uint32_t) col * 64;
                    } else {
                        uint32_t cell = map1d ? (uint32_t) (ty / 8) * (uint32_t) (w / 8) + (uint32_t) col
                                              : (uint32_t) (ty / 8) * 32 + (uint32_t) col;
                        addr = (map1d ? tile * bound : tile * 32) + cell * 32;
                    }
                    row = vptr(e->map, e->f->vram, e->obj_region, addr + row_in_tile);
                }
                uint8_t idx;
                uint16_t color;
                if (c256) {
                    idx = row ? row[tx & 7] : 0;
                    if (!idx) continue;
                    color = ext ? (xpal ? (uint16_t) ((xpal[idx * 2] | xpal[idx * 2 + 1] << 8) & 0x7fff) : 0)
                                : e->pal[256 + idx];
                } else {
                    uint8_t b = row ? row[(tx & 7) >> 1] : 0;
                    idx = tx & 1 ? b >> 4 : b & 15;
                    if (!idx) continue;
                    color = pal16[idx];
                }
                int lx = ox + bx + e->margin;
                if (mode == 2) {
                    line->obj_window[lx] = 1;
                    continue;
                }
                if (prio <= line->obj_prio[lx]) {
                    line->obj[lx] = (color & 0x7fff) | 0x8000;
                    line->obj_prio[lx] = (uint8_t) prio;
                    line->obj_prios |= (uint8_t) (1u << prio);
                    line->obj_alpha[lx] = alpha;
                }
            }
            continue;
        }
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint16_t *p = oam + ((a1 >> 9) & 31) * 16;
            pa = (int16_t) p[3];
            pb = (int16_t) p[7];
            pc = (int16_t) p[11];
            pd = (int16_t) p[15];
        }
        int cy = dy - bh / 2;
        int32_t accx = pa * (bx0 - bw / 2) + pb * cy, accy = pc * (bx0 - bw / 2) + pd * cy;
        for (int bx = bx0; bx < bx1; bx++, accx += pa, accy += pc) {
            int x = ox + bx;
            int lx = x + e->margin;
            int tx, ty;
            if (affine) {
                tx = (accx >> 8) + w / 2;
                ty = (accy >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = a1 & 0x1000 ? w - 1 - bx : bx;
                ty = a1 & 0x2000 ? h - 1 - dy : dy;
            }
            uint16_t color;
            uint8_t alpha = mode == 1 ? 0xfe : 0xff;
            if (mode == 3) { // bitmap OBJ
                uint32_t addr;
                if (e->dispcnt & 0x40) {
                    addr = tile * (128u << ((e->dispcnt >> 22) & 1)) + (ty * w + tx) * 2;
                } else {
                    uint32_t width = e->dispcnt & 0x20 ? 256 : 128;
                    uint32_t mask = width / 8 - 1;
                    addr = (((tile / (width / 8)) * 8 + ty) * width + (tile & mask) * 8 + tx) * 2;
                }
                uint16_t c = rd16(e->map, e->f->vram, e->obj_region, addr);
                if (!(c & 0x8000)) continue;
                color = c;
                alpha = (uint8_t) (a2 >> 12);
            } else {
                uint32_t addr;
                if (c256) {
                    uint32_t tbase = map1d ? tile * bound : tile * 32;
                    uint32_t cell = map1d ? (ty / 8) * (w / 8) + tx / 8 : (ty / 8) * 16 + tx / 8;
                    addr = map1d ? tbase + cell * 64 : tile * 32 + ((ty / 8) * 32 * 32) + (tx / 8) * 64;
                    addr += (ty & 7) * 8 + (tx & 7);
                    uint8_t idx = rd8(e->map, e->f->vram, e->obj_region, addr);
                    if (!idx) continue;
                    color = ext ? (xpal ? (uint16_t) ((xpal[idx * 2] | xpal[idx * 2 + 1] << 8) & 0x7fff) : 0)
                                : e->pal[256 + idx];
                } else {
                    uint32_t cell = map1d ? (ty / 8) * (w / 8) + tx / 8 : (ty / 8) * 32 + tx / 8;
                    addr = (map1d ? tile * bound : tile * 32) + cell * 32 + (ty & 7) * 4 + (tx & 7) / 2;
                    uint8_t b = rd8(e->map, e->f->vram, e->obj_region, addr);
                    uint8_t idx = tx & 1 ? b >> 4 : b & 15;
                    if (!idx) continue;
                    color = e->pal[256 + (a2 >> 12) * 16 + idx];
                }
            }
            if (mode == 2) {
                line->obj_window[lx] = 1;
                continue;
            }
            if (prio <= line->obj_prio[lx]) {
                line->obj[lx] = (color & 0x7fff) | 0x8000;
                line->obj_prio[lx] = (uint8_t) prio;
                line->obj_prios |= (uint8_t) (1u << prio);
                line->obj_alpha[lx] = alpha;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ composition
static inline uint32_t rgb555_to_888_calc(uint32_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | 0xff000000u;
}
static uint32_t s_rgb888[32768];
static void rgb_table_init(void) {
    if (s_rgb888[32767]) return;
    for (uint32_t c = 0; c < 32768; c++) s_rgb888[c] = rgb555_to_888_calc(c);
}
static inline uint32_t rgb555_to_888(uint16_t c) { return rgb555_to_888_calc(c & 0x7fff); }

static inline uint64_t spread3(uint32_t c) {
    return (uint64_t) (c & 0xff) | (uint64_t) ((c >> 8) & 0xff) << 16 | (uint64_t) ((c >> 16) & 0xff) << 32;
}
static inline uint32_t pack3(uint64_t v) {
    return (uint32_t) (v & 0xff) | (uint32_t) ((v >> 16) & 0xff) << 8 | (uint32_t) ((v >> 32) & 0xff) << 16 | 0xff000000u;
}
#define LANES12 0x00000fff0fff0fffull

// Blend in 8-bit per channel: (a * ea + b * eb) / 16, clamped.
static inline uint32_t blend(uint32_t a, uint32_t b, uint32_t ea, uint32_t eb) {
    uint64_t v = ((spread3(a) * ea + spread3(b) * eb) >> 4) & LANES12;
    uint64_t over = (v >> 8) & 0x0000000100010001ull;
    return pack3(v | over * 0xff);
}

static inline uint32_t brighten(uint32_t c, uint32_t ey) {
    uint64_t x = spread3(c);
    return pack3(x + ((((0x000000ff00ff00ffull - x) * ey) >> 4) & LANES12));
}

static inline uint32_t darken(uint32_t c, uint32_t ey) {
    uint64_t x = spread3(c);
    return pack3(x - (((x * ey) >> 4) & LANES12));
}

// Window mask of each pixel: bits 0..5 = layers and effects enabled there.
static void windows(const Engine *e, int y, const Line *line, uint8_t *mask) {
    uint32_t wins = (e->dispcnt >> 13) & 7;
    int width = 256 + 2 * e->margin;
    if (!wins) {
        memset(mask, 0x3f, (size_t) width);
        return;
    }
    uint16_t winin = io16(e, 0x48), winout = io16(e, 0x4a);
    for (int i = 0; i < width; i++) {
        mask[i] = winout & 0x3f;
    }
    if (wins & 4) {
        for (int i = 0; i < width; i++) {
            if (line->obj_window[i]) mask[i] = (winout >> 8) & 0x3f;
        }
    }
    for (int w = 1; w >= 0; w--) {
        if (!(wins & (1u << w))) continue;
        uint16_t hreg = io16(e, 0x40 + w * 2), vreg = io16(e, 0x44 + w * 2);
        int x1 = hreg >> 8, x2 = hreg & 0xff, y1 = vreg >> 8, y2 = vreg & 0xff;
        bool in_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (!in_y) continue;
        for (int i = 0; i < width; i++) {
            // Beyond the DS screen: as its edge columns (254 on the right: a window ending at 255 covers it).
            int x = i - e->margin;
            x = x < 0 ? 0 : x > 254 ? 254 : x;
            bool in_x = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
            if (in_x) mask[i] = (winin >> (w * 8)) & 0x3f;
        }
    }
}

// Split mode (layer3d == GPU2D_SPLIT): the 3D layer is drawn later at high resolution. `out` then receives what lies
// above the 3D layer (alpha = its opacity over what is below), `below` what lies under it (alpha 0: the 3D pixel
// replaces it; 255: it is a blend target, translucent 3D mixes with it; 1..254: also the weight of an opaque 3D pixel
// in an alpha blend).
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef uint16_t u16x4 __attribute__((vector_size(8)));
typedef uint8_t u8x4 __attribute__((vector_size(4)));
static inline u32x4 load_u16x4(const uint16_t *p) {
    u16x4 v;
    memcpy(&v, p, 8);
    return __builtin_convertvector(v, u32x4);
}
static inline uint32_t ld32a(const uint8_t *p) { return *(const uint32_t *) __builtin_assume_aligned(p, 4); }
static inline void st32a(uint8_t *p, uint32_t v) { *(uint32_t *) __builtin_assume_aligned(p, 4) = v; }
static inline u32x4 load_u8x4(const uint8_t *p) {
    uint32_t w = ld32a(p);
    u8x4 v;
    memcpy(&v, &w, 4);
    return __builtin_convertvector(v, u32x4);
}
static inline bool any4(u32x4 m) { return (m[0] | m[1] | m[2] | m[3]) != 0; }

static inline __attribute__((always_inline)) void push4(uint32_t *st0, uint32_t *st1, uint32_t *st2, uint8_t *ly0, uint8_t *ly1, uint8_t *ly2, int x,
                         u32x4 m, u32x4 v, uint8_t layer, bool split) {
    u32x4 a0, a1;
    memcpy(&a0, st0 + x, 16);
    memcpy(&a1, st1 + x, 16);
    u8x4 mb8 = __builtin_convertvector(m, u8x4);
    uint32_t mb;
    memcpy(&mb, &mb8, 4);
    uint32_t l0 = ld32a(ly0 + x), l1 = ld32a(ly1 + x);
    if (split) {
        u32x4 a2;
        memcpy(&a2, st2 + x, 16);
        a2 = (a1 & m) | (a2 & ~m);
        memcpy(st2 + x, &a2, 16);
        st32a(ly2 + x, (l1 & mb) | (ld32a(ly2 + x) & ~mb));
    }
    a1 = (a0 & m) | (a1 & ~m);
    a0 = (v & m) | (a0 & ~m);
    memcpy(st1 + x, &a1, 16);
    memcpy(st0 + x, &a0, 16);
    l1 = (l0 & mb) | (l1 & ~mb);
    l0 = ((uint32_t) layer * 0x01010101u & mb) | (l0 & ~mb);
    st32a(ly1 + x, l1);
    st32a(ly0 + x, l0);
}

static void render_engine_line(const Engine *e, int y, const uint32_t *layer3d, uint32_t *out, uint32_t *below) {
    Line line;
    int width = 256 + 2 * e->margin;
    memset(line.obj, 0, (size_t) width * sizeof(line.obj[0]));
    memset(line.obj_prio, 4, (size_t) width);
    memset(line.obj_window, 0, (size_t) width);
    line.obj_prios = 0;
    bool bg0_3d = e->is_a && (e->dispcnt & 8);
    uint8_t bg_drawn = 0;
    for (int bg = 0; bg < 4; bg++) {
        if (!(e->dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && bg0_3d) continue;
        uint16_t *b = line.bg[bg];
        memset(b, 0, (size_t) width * sizeof(b[0]));
        draw_bg_line(e, bg, y, b);
        bg_drawn |= (uint8_t) (1u << bg);
    }
    if (e->dispcnt & 0x1000) {
        draw_objs(e, y, &line);
    }
    uint8_t mask[GPU2D_MAX_W] __attribute__((aligned(16)));
    windows(e, y, &line, mask);

    uint16_t bldcnt = io16(e, 0x50), bldalpha = io16(e, 0x52);
    uint32_t effect = (bldcnt >> 6) & 3;
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(e, 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    uint32_t backdrop555 = e->pal[0] & 0x7fff;
    uint32_t prio[4];
    for (int bg = 0; bg < 4; bg++) {
        prio[bg] = io16(e, 0x08 + bg * 2) & 3;
    }
    uint8_t order[20], order_p[20];
    int order_n = 0;
    for (uint32_t p = 0; p < 4; p++) {
        order[order_n] = LAYER_OBJ;
        order_p[order_n++] = (uint8_t) p;
        for (int bg = 0; bg < 4; bg++) {
            if (prio[bg] != p || !(e->dispcnt & (0x100u << bg))) continue;
            order[order_n] = (uint8_t) bg;
            order_p[order_n++] = (uint8_t) p;
        }
    }

    bool split = layer3d == GPU2D_SPLIT && bg0_3d;
    bool windowed = (e->dispcnt >> 13) & 7;
    uint32_t st0[GPU2D_MAX_W] __attribute__((aligned(16))), st1[GPU2D_MAX_W] __attribute__((aligned(16))),
        st2[GPU2D_MAX_W] __attribute__((aligned(16)));
    uint8_t ly0[GPU2D_MAX_W] __attribute__((aligned(16))), ly1[GPU2D_MAX_W] __attribute__((aligned(16))),
        ly2[GPU2D_MAX_W] __attribute__((aligned(16)));
    for (int x = 0; x < width; x++) st0[x] = st1[x] = backdrop555;
    memset(ly0, LAYER_BD, (size_t) width);
    memset(ly1, LAYER_BD, (size_t) width);
    if (split) {
        for (int x = 0; x < width; x++) st2[x] = backdrop555;
        memset(ly2, LAYER_BD, (size_t) width);
    }
#define PUSH(x, value, layer)                                                                                          \
    do {                                                                                                               \
        if (split) {                                                                                                   \
            st2[x] = st1[x];                                                                                           \
            ly2[x] = ly1[x];                                                                                           \
        }                                                                                                              \
        st1[x] = st0[x];                                                                                               \
        ly1[x] = ly0[x];                                                                                               \
        st0[x] = (value);                                                                                              \
        ly0[x] = (uint8_t) (layer);                                                                                    \
    } while (0)
#define PUSH4(x, m, v, layer)                                                                                          \
    push4(st0, st1, st2, ly0, ly1, ly2, (x), (m), (v), (uint8_t) (layer), split)
    for (int i = order_n - 1; i >= 0; i--) {
        int layer = order[i];
        if (layer == LAYER_OBJ) {
            uint8_t p = order_p[i];
            if (!(line.obj_prios & (1u << p))) continue;
            int x = 0;
            for (; x + 4 <= width; x += 4) {
                u32x4 ov = load_u16x4(line.obj + x), pv = load_u8x4(line.obj_prio + x), mv = load_u8x4(mask + x);
                u32x4 m = (u32x4) (pv == p) & (u32x4) ((mv & 0x10) != 0) & (u32x4) ((ov & 0x8000) != 0);
                if (any4(m)) PUSH4(x, m, ov, LAYER_OBJ);
            }
            for (; x < width; x++) {
                if (line.obj_prio[x] == p && (mask[x] & 0x10) && (line.obj[x] & 0x8000)) PUSH(x, line.obj[x], LAYER_OBJ);
            }
        } else if (layer == 0 && bg0_3d) {
            if (split && !windowed) {
                memcpy(st2, st1, (size_t) width * sizeof(st2[0]));
                memcpy(ly2, ly1, (size_t) width);
                memcpy(st1, st0, (size_t) width * sizeof(st1[0]));
                memcpy(ly1, ly0, (size_t) width);
                memset(st0, 0, (size_t) width * sizeof(st0[0]));
                memset(ly0, 0, (size_t) width);
            } else if (split) {
                int x = 0;
                u32x4 zero = {0, 0, 0, 0};
                for (; x + 4 <= width; x += 4) {
                    u32x4 m = (u32x4) ((load_u8x4(mask + x) & 1) != 0);
                    if (any4(m)) PUSH4(x, m, zero, 0);
                }
                for (; x < width; x++) {
                    if (mask[x] & 1) PUSH(x, 0u, 0);
                }
            } else if (layer3d) {
                int xa = e->margin, xb = e->margin + 256;
                if (xb > width) xb = width;
                const uint32_t *row3d = layer3d + y * 256 - e->margin;
                int x = xa;
                for (; x < xb && (x & 3); x++) {
                    uint32_t c3 = row3d[x];
                    if ((c3 >> 24) && (mask[x] & 1)) PUSH(x, (c3 & 0xffffff) | 0xff000000u, 0);
                }
                for (; x + 4 <= xb; x += 4) {
                    u32x4 c3;
                    memcpy(&c3, row3d + x, 16);
                    u32x4 m = (u32x4) ((c3 >> 24) != 0) & (u32x4) ((load_u8x4(mask + x) & 1) != 0);
                    if (any4(m)) PUSH4(x, m, (c3 & 0xffffff) | 0xff000000u, 0);
                }
                for (; x < xb; x++) {
                    uint32_t c3 = row3d[x];
                    if ((c3 >> 24) && (mask[x] & 1)) PUSH(x, (c3 & 0xffffff) | 0xff000000u, 0);
                }
            }
        } else {
            if (!(bg_drawn & (1u << layer))) continue;
            const uint16_t *b = line.bg[layer];
            uint8_t bit = (uint8_t) (1u << layer);
            int x = 0;
            if (!windowed) {
                for (; x + 4 <= width; x += 4) {
                    u32x4 bv = load_u16x4(b + x);
                    u32x4 m = (u32x4) ((bv & 0x8000) != 0);
                    if (any4(m)) PUSH4(x, m, bv, layer);
                }
                for (; x < width; x++) {
                    if (b[x] & 0x8000) PUSH(x, b[x], layer);
                }
            } else {
                for (; x + 4 <= width; x += 4) {
                    u32x4 bv = load_u16x4(b + x);
                    u32x4 m = (u32x4) ((bv & 0x8000) != 0) & (u32x4) ((load_u8x4(mask + x) & bit) != 0);
                    if (any4(m)) PUSH4(x, m, bv, layer);
                }
                for (; x < width; x++) {
                    if ((b[x] & 0x8000) && (mask[x] & bit)) PUSH(x, b[x], layer);
                }
            }
        }
    }
#undef PUSH4
#undef PUSH
#define RGB(raw) ((raw) >> 31 ? (raw) : rgb555_to_888((uint16_t) (raw)))
    uint32_t backdrop = rgb555_to_888((uint16_t) backdrop555);

    if (split) {
        for (int x = 0; x < width; x++) {
            // The three topmost layers, with the 3D layer always present.
            int top[3] = {ly0[x], ly1[x], ly2[x]};
            int i3 = top[0] == 0 ? 0 : top[1] == 0 ? 1 : top[2] == 0 ? 2 : 3;
            bool effects = mask[x] & 0x20;
            uint32_t up = 0, down = backdrop & 0xffffff;
            if (i3 == 0) {
                // 3D on top: what is under it, and how they combine.
                down = RGB(st1[x]) & 0xffffff;
                bool second = (bldcnt >> (8 + top[1])) & 1;
                uint32_t w = 0;
                if (second) {
                    w = 255;
                    if (effects && (bldcnt & 1) && effect == 1) w = eva * 255 / 16 ? eva * 255 / 16 : 1;
                    if (w > 255) w = 255;
                }
                if (effects && (bldcnt & 1) && (effect == 2 || effect == 3)) {
                    up = (effect == 2 ? 0xffffffu : 0) | (evy * 255 / 16) << 24; // brighten / darken over 3D
                }
                below[x] = down | w << 24;
            } else {
                uint32_t c = RGB(st0[x]);
                bool first = (bldcnt >> top[0]) & 1;
                bool second = (bldcnt >> (8 + top[1])) & 1;
                bool semi = top[0] == LAYER_OBJ && line.obj_alpha[x] != 0xff;
                uint32_t a = 255;
                if ((semi || (effects && first && effect == 1)) && second) {
                    uint32_t ea = semi && line.obj_alpha[x] != 0xfe ? line.obj_alpha[x] + 1u : eva;
                    uint32_t eb = semi && line.obj_alpha[x] != 0xfe ? 16 - ea : evb;
                    if (top[1] == 0) {
                        a = ea * 255 / 16; // over the 3D layer: the shader blends
                        below[x] = (RGB(st2[x]) & 0xffffff) | (((bldcnt >> (8 + top[2])) & 1) ? 255u << 24 : 0);
                    } else {
                        c = blend(c, RGB(st1[x]), ea, eb);
                    }
                } else if (effects && first && effect == 2) {
                    c = brighten(c, evy);
                } else if (effects && first && effect == 3) {
                    c = darken(c, evy);
                }
                up = (c & 0xffffff) | a << 24;
                if (a == 255) below[x] = 0;
            }
            out[x] = up;
        }
        return;
    }

    for (int x = 0; x < width; x++) {
        int top0 = ly0[x], top1 = ly1[x];
        uint32_t c = RGB(st0[x]);
        uint8_t m = mask[x];
        bool effects = m & 0x20;
        bool first = (bldcnt >> top0) & 1;
        bool second = (bldcnt >> (8 + top1)) & 1;
        if (e->hud && top0 == LAYER_BD) {
            out[x] = 0;
            continue;
        }
        if (!effects && top0 != LAYER_OBJ && !(top0 == 0 && bg0_3d)) {
            out[x] = e->hud ? c | 0xff000000u : c;
            continue;
        }
        uint32_t alpha3d = 31;
        if (top0 == 0 && bg0_3d) alpha3d = st0[x] >> 31 ? layer3d[y * 256 + x - e->margin] >> 24 : 31;
        if (top0 == LAYER_OBJ && line.obj_alpha[x] != 0xff && second) {
            // Semi-transparent and bitmap sprites blend whatever the effect setting.
            uint32_t a = line.obj_alpha[x] == 0xfe ? eva : line.obj_alpha[x] + 1;
            uint32_t b = line.obj_alpha[x] == 0xfe ? evb : 16 - (line.obj_alpha[x] + 1);
            c = blend(c, RGB(st1[x]), a, b);
        } else if (top0 == 0 && bg0_3d && alpha3d < 31 && second) {
            // Translucent 3D pixels blend with the layer below using their own alpha.
            c = blend(c, RGB(st1[x]), (alpha3d + 1) / 2, 16 - (alpha3d + 1) / 2);
        } else if (effects && first) {
            if (effect == 1 && second) {
                c = blend(c, RGB(st1[x]), eva, evb);
            } else if (effect == 2) {
                c = brighten(c, evy);
            } else if (effect == 3) {
                c = darken(c, evy);
            }
        }
        out[x] = e->hud ? c | 0xff000000u : c;
    }
#undef RGB
}

static void master_brightness(uint16_t reg, uint32_t *line, int width) {
    uint32_t mode = reg >> 14, factor = reg & 31;
    if (factor > 16) factor = 16;
    if (!factor || mode == 0 || mode == 3) return;
    for (int x = 0; x < width; x++) {
        line[x] = mode == 1 ? brighten(line[x], factor) : darken(line[x], factor);
    }
}

// The layer seen behind all others (the one to continue beyond the DS screen): 2D BG number, 4 when it is the 3D
// layer, -1 when none.
static int bottom_layer(const Engine *e) {
    int best = -1, best_prio = -1;
    for (int bg = 0; bg < 4; bg++) {
        if (!(e->dispcnt & (0x100u << bg))) continue;
        int prio = io16(e, 0x08 + bg * 2) & 3;
        if (prio >= best_prio) { // the higher number is behind at equal priority
            best_prio = prio;
            best = bg == 0 && e->is_a && (e->dispcnt & 8) ? 4 : bg;
        }
    }
    return best;
}

static void engine_prepare(Engine *ep, const FrameSnapshot *f, const VramMap *map, bool is_a, int margin) {
#define e (*ep)
    e.f = f;
    e.map = map;
    e.is_a = is_a;
    e.io = f->io + (is_a ? 0 : 0x1000);
    e.pal = (const uint16_t *) (f->palette + (is_a ? 0 : 0x400));
    e.bg_region = is_a ? VR_BG_A : VR_BG_B;
    e.obj_region = is_a ? VR_OBJ_A : VR_OBJ_B;
    e.bgext_region = is_a ? VR_BGEXT_A : VR_BGEXT_B;
    e.objext_region = is_a ? VR_OBJEXT_A : VR_OBJEXT_B;
    e.dispcnt = io32(&e, 0);
    e.margin = margin;
    e.hud_shift = margin > 0 && is_a && f->scene == 2 && (e.dispcnt & 8);
    if (e.hud_shift) plan_hud_shift(&e);
    e.ext_bg = margin > 0 ? bottom_layer(&e) : -1;
    if (e.ext_bg == 4) e.ext_bg = -1;
    e.side[0] = e.side[1] = NULL;
    e.hud = false;
    e.obj_mode = 0;
    e.obj_lo = e.obj_hi = 0;
    if (e.ext_bg >= 0) {
        // A scrolling layer or one with more map beyond the screen goes on as its map does (patterns, skies); a
        // still picture made for the screen (logos, title, menu backgrounds) is mirrored at its edges instead of
        // repeating.
        static uint16_t last_hofs[2][4];
        uint16_t cnt = io16(&e, 0x08 + e.ext_bg * 2), hofs = io16(&e, 0x10 + e.ext_bg * 4);
        bool text = e.ext_bg < 2 || (e.dispcnt & 7) == 0 || ((e.dispcnt & 7) == 1 && e.ext_bg == 2) ||
                    ((e.dispcnt & 7) == 3 && e.ext_bg == 2);
        bool scrolling = hofs != last_hofs[is_a ? 0 : 1][e.ext_bg];
        last_hofs[is_a ? 0 : 1][e.ext_bg] = hofs;
        bool more_map = text ? (cnt & 0x4000) != 0 : (cnt & 0x2000) != 0; // 512 wide / affine wrap-around
        if (!(scrolling || more_map)) {
            // Its edge columns, blurred vertically.
            uint16_t edge[2][192], row[GPU2D_MAX_W];
            static uint16_t side_of[2][2][192];
            uint16_t (*side)[192] = side_of[is_a ? 0 : 1];
            int bg = e.ext_bg;
            e.ext_bg = -1; // the DS columns only
            for (int y = 0; y < 192; y++) {
                memset(row, 0, sizeof(row));
                draw_bg_line(&e, bg, y, row);
                edge[0][y] = row[margin];
                edge[1][y] = row[margin + 255];
            }
            e.ext_bg = bg;
            for (int k = 0; k < 2; k++) {
                for (int y = 0; y < 192; y++) {
                    int sum[3] = {0, 0, 0}, n = 0, opaque = 0;
                    for (int j = y - 10; j <= y + 10; j++) {
                        if (j < 0 || j > 191) continue;
                        uint16_t c = edge[k][j];
                        n++;
                        if (!(c & 0x8000)) continue;
                        opaque++;
                        sum[0] += c & 31;
                        sum[1] += (c >> 5) & 31;
                        sum[2] += (c >> 10) & 31;
                    }
                    side[k][y] = opaque * 2 < n ? 0
                                                : (uint16_t) (0x8000 | sum[0] / opaque | (sum[1] / opaque) << 5 |
                                                              (sum[2] / opaque) << 10);
                }
            }
            e.side[0] = side[0];
            e.side[1] = side[1];
        }
    }
#undef e
}

static uint32_t s_engine_a_out[256 * 192];

static void engine_layers_line(const Engine *e, int y, uint32_t *bg01, uint32_t *bg23, uint32_t *objw) {
    Line line;
    int width = 256 + 2 * e->margin;
    memset(line.obj, 0, (size_t) width * sizeof(line.obj[0]));
    memset(line.obj_prio, 4, (size_t) width);
    memset(line.obj_window, 0, (size_t) width);
    line.obj_prios = 0;
    bool bg0_3d = e->is_a && (e->dispcnt & 8);
    for (int bg = 0; bg < 4; bg++) {
        memset(line.bg[bg], 0, (size_t) width * sizeof(line.bg[bg][0]));
        if (!(e->dispcnt & (0x100u << bg))) continue;
        if (bg == 0 && bg0_3d) continue;
        draw_bg_line(e, bg, y, line.bg[bg]);
    }
    if (e->dispcnt & 0x1000) draw_objs(e, y, &line);
    uint8_t mask[GPU2D_MAX_W] __attribute__((aligned(16)));
    windows(e, y, &line, mask);
    for (int x = 0; x < width; x++) {
        bg01[x] = line.bg[0][x] | (uint32_t) line.bg[1][x] << 16;
        bg23[x] = line.bg[2][x] | (uint32_t) line.bg[3][x] << 16;
        uint32_t a = line.obj_alpha[x];
        uint32_t code = a == 0xff ? 17 : a == 0xfe ? 16 : a;
        uint32_t attr = (uint32_t) mask[x] | (uint32_t) line.obj_prio[x] << 6 | code << 9;
        objw[x] = line.obj[x] | attr << 16;
    }
}

static void engine_lines(const Engine *ep, const uint32_t *layer3d, uint32_t *out, uint32_t *below, int y0, int y1,
                         uint32_t *extra, const uint8_t *dirty) {
    const Engine e = *ep;
    const FrameSnapshot *f = e.f;
    const VramMap *map = e.map;
    bool is_a = e.is_a;
    int margin = e.margin;
    int width = 256 + 2 * margin;
    uint32_t display = (e.dispcnt >> 16) & 3;
    if (!is_a && display > 1) display = 1;
    bool forced_blank = e.dispcnt & 0x80;

    uint32_t *engine_a_out = s_engine_a_out;
    if (layer3d == GPU2D_LAYERS) {
        for (int y = y0; y < y1; y++) {
            if (dirty && !dirty[y]) continue;
            engine_layers_line(&e, y, out + y * width, below + y * width, extra + y * width);
        }
        return;
    }
    for (int y = y0; y < y1; y++) {
        if (dirty && !dirty[y]) continue;
        uint32_t *line = out + y * width;
        if (forced_blank || display == 0) {
            for (int x = 0; x < width; x++) line[x] = 0xffffffffu; // white
            if (below) memset(below + y * width, 0, (size_t) width * 4);
            continue;
        }
        if (display == 2) { // VRAM display: an LCDC bank as a 256x192 direct-colour picture
            uint32_t bank = (e.dispcnt >> 18) & 3;
            const uint8_t *src = vptr(map, f->vram, VR_LCDC, bank * 0x20000 + (uint32_t) y * 512);
            for (int x = 0; x < width; x++) {
                int dx = x - margin;
                line[x] = dx < 0 || dx > 255 ? 0xff000000u
                          : src ? rgb555_to_888((uint16_t) (src[dx * 2] | src[dx * 2 + 1] << 8))
                                : rgb555_to_888(0);
            }
        } else if (display == 3) {
            for (int x = 0; x < width; x++) line[x] = 0xff000000u;
        } else {
            render_engine_line(&e, y, layer3d, line, below ? below + y * width : NULL);
        }
        if (is_a) {
            memcpy(engine_a_out + y * 256, line + margin, 256 * 4);
        }
        if (layer3d == GPU2D_SPLIT && is_a) {
            // The compositing shader applies the master brightness to the whole screen.
            if (!(e.dispcnt & 8) || display != 1 || forced_blank) {
                for (int x = 0; x < width; x++) below[y * width + x] = 0; // no 3D on this line
                for (int x = 0; x < width; x++) line[x] |= 0xff000000u;
            }
        } else {
            master_brightness(io16(&e, 0x6c), line, width);
        }
    }
}

static void engine_finish(const Engine *e, const uint32_t *layer3d) {
    if (e->is_a && (e->f->io[0x67] & 0x80) && layer3d != GPU2D_SPLIT && layer3d != GPU2D_LAYERS) {
        gpu2d_capture(e->f, s_engine_a_out, layer3d);
    }
}

#include <pthread.h>

typedef struct Job {
    const FrameSnapshot *f;
    const VramMap *map;
    bool is_a;
    const uint32_t *layer3d;
    uint32_t *out, *below;
    int margin;
    uint32_t *extra;
} Job;

typedef struct Rows {
    const Engine *e[2];
    const Job *job[2];
    const uint8_t *dirty[2];
} Rows;

static void run_rows(void *arg, int y0, int y1) {
    const Rows *r = arg;
    for (int i = 0; i < 2; i++) {
        if (r->e[i]) {
            engine_lines(r->e[i], r->job[i]->layer3d, r->job[i]->out, r->job[i]->below, y0, y1, r->job[i]->extra,
                         r->dirty[i]);
        }
    }
}

#include <stdatomic.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
typedef SceUID Sem;
static bool sem_init_(Sem *s) { return (*s = sceKernelCreateSema("mkds_gpu2d", 0, 0, 0x7fffffff, NULL)) >= 0; }
static void sem_post_(Sem *s) { sceKernelSignalSema(*s, 1); }
static void sem_wait_(Sem *s) { sceKernelWaitSema(*s, 1, NULL); }
#else
typedef struct Sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count;
} Sem;
static bool sem_init_(Sem *s) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = 0;
    return true;
}
static void sem_post_(Sem *s) {
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}
static void sem_wait_(Sem *s) {
    pthread_mutex_lock(&s->m);
    while (!s->count) pthread_cond_wait(&s->c, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
}
#endif

#ifdef __vita__
#define WORKERS 2
#else
#define WORKERS 2
#endif
#define CHUNK_ROWS 8
#define CHUNKS (192 / CHUNK_ROWS)

static void (*s_rows_fn)(void *arg, int y0, int y1);
static void *s_rows_arg;
static atomic_uint s_next;
static atomic_int s_done;
static uint32_t s_gen;
static Sem s_wake[WORKERS], s_finished;
static int s_worker_count = -1;

void gpu2d_worker_started(int index) __attribute__((weak));
void gpu2d_worker_boost(int index, bool on) __attribute__((weak));
static atomic_int s_in_chunk[WORKERS + 1];
uint32_t g_gpu2d_boosts;
uint64_t g_gpu2d_wait_ns;

static void run_chunks(uint32_t gen, int who) {
    for (;;) {
        atomic_store(&s_in_chunk[who], 1);
        unsigned v = atomic_load(&s_next);
        if ((v >> 8) != (gen & 0xffffffu) || (v & 0xff) >= CHUNKS) break;
        if (!atomic_compare_exchange_weak(&s_next, &v, v + 1)) continue;
        int c = (int) (v & 0xff);
        s_rows_fn(s_rows_arg, c * CHUNK_ROWS, c * CHUNK_ROWS + CHUNK_ROWS);
        atomic_fetch_add(&s_done, 1);
    }
    atomic_store(&s_in_chunk[who], 0);
}

static void *worker(void *arg) {
    int index = (int) (intptr_t) arg;
    if (gpu2d_worker_started) gpu2d_worker_started(index);
    for (;;) {
        sem_wait_(&s_wake[index]);
        run_chunks(atomic_load(&s_next) >> 8, index);
        sem_post_(&s_finished);
    }
    return NULL;
}

static void start_workers(void) {
    s_worker_count = 0;
    if (!sem_init_(&s_finished)) return;
    for (int i = 0; i < WORKERS; i++) {
        if (!sem_init_(&s_wake[i])) break;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_t t;
        bool ok = pthread_create(&t, &attr, worker, (void *) (intptr_t) i) == 0;
        pthread_attr_destroy(&attr);
        if (!ok) break;
        s_worker_count++;
    }
}

void gpu2d_parallel_rows(void (*fn)(void *arg, int y0, int y1), void *arg) {
    if (s_worker_count < 0) start_workers();
    s_rows_fn = fn;
    s_rows_arg = arg;
    atomic_store(&s_done, 0);
    s_gen = (s_gen + 1) & 0xffffffu;
    atomic_store(&s_next, s_gen << 8);
    for (int i = 0; i < s_worker_count; i++) sem_post_(&s_wake[i]);
    run_chunks(s_gen, WORKERS);
    if (atomic_load(&s_done) >= CHUNKS) return;
    uint64_t t0 = hapi_now_ns();
    bool boosted[WORKERS] = {false};
    if (gpu2d_worker_boost) {
        for (int i = 0; i < s_worker_count; i++) {
            if (atomic_load(&s_in_chunk[i])) {
                gpu2d_worker_boost(i, true);
                boosted[i] = true;
            }
        }
    }
    bool any = false;
    while (atomic_load(&s_done) < CHUNKS) sem_wait_(&s_finished);
    for (int i = 0; i < s_worker_count; i++) {
        if (boosted[i]) {
            gpu2d_worker_boost(i, false);
            any = true;
        }
    }
    g_gpu2d_boosts += any;
    g_gpu2d_wait_ns += hapi_now_ns() - t0;
}

typedef struct EngineInputs {
    uint8_t io[0x70];
    uint8_t pal[0x400];
    uint8_t oam[0x400];
    uint8_t vramcnt[10];
    uint8_t master[2];
    uint8_t scene, is_a, split;
    uint16_t powcnt;
    int margin;
    const void *layer3d, *out, *below;
} EngineInputs;

uint32_t g_gpu2d_modes[3];
static EngineInputs s_last[2];
bool g_gpu2d_unchanged;
static bool s_last_valid[2];

static void inputs_of(EngineInputs *in, const Job *j, bool split) {
    memset(in, 0, sizeof(*in));
    const FrameSnapshot *f = j->f;
    memcpy(in->io, f->io + (j->is_a ? 0 : 0x1000), sizeof(in->io));
    memcpy(in->pal, f->palette + (j->is_a ? 0 : 0x400), sizeof(in->pal));
    memcpy(in->oam, f->oam + (j->is_a ? 0 : 0x400), sizeof(in->oam));
    memcpy(in->vramcnt, f->vramcnt, sizeof(in->vramcnt));
    in->scene = f->scene;
    in->is_a = j->is_a;
    in->split = split && j->is_a;
    if (j->layer3d == GPU2D_LAYERS) in->split = 2;
    in->powcnt = (uint16_t) (f->io[0x304] | f->io[0x305] << 8);
    in->margin = j->margin;
    in->layer3d = j->layer3d;
    in->out = j->out;
    in->below = j->below;
}

static bool s_skip[2];

void gpu2d_skip_engines(bool skip_a, bool skip_b) {
    s_skip[0] = skip_a;
    s_skip[1] = skip_b;
}

static uint64_t engine_vram_pages(const VramMap *map, bool is_a, bool objects) {
    static const int regions[2][2][2] = {{{VR_BG_B, VR_BGEXT_B}, {VR_OBJ_B, VR_OBJEXT_B}},
                                         {{VR_BG_A, VR_BGEXT_A}, {VR_OBJ_A, VR_OBJEXT_A}}};
    uint64_t m = 0;
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < 48; i++) {
            int32_t at = map->page[regions[is_a][objects][r]][i];
            if (at >= 0) m |= 1ull << ((uint32_t) at / PAGE);
        }
    }
    return m;
}

static uint32_t s_line_ver[2][192], s_ver_counter;
void gpu2d_line_versions(const uint32_t **engine_a, const uint32_t **engine_b) {
    *engine_a = s_line_ver[0];
    *engine_b = s_line_ver[1];
}

static void line_signatures(const Engine *e, uint32_t sig[192]) {
    const uint16_t *oam = (const uint16_t *) (e->f->oam + (e->is_a ? 0 : 0x400));
    for (int y = 0; y < 192; y++) sig[y] = 2166136261u;
    if (!(e->dispcnt & 0x1000)) return;
    for (int i = 0; i < 128; i++) {
        const uint16_t *a = oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1];
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue; // disabled
        uint32_t shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int bh = s_obj_h[shape][size];
        if (affine && (a0 & 0x200)) bh *= 2;
        uint32_t h = 2166136261u;
        h = (h ^ a0) * 16777619u;
        h = (h ^ a1) * 16777619u;
        h = (h ^ a[2]) * 16777619u;
        h = (h ^ (uint32_t) (e->hud_shift ? e->obj_shift[i] + 2 : 0)) * 16777619u;
        if (affine) {
            const uint16_t *p = oam + ((a1 >> 9) & 31) * 16;
            h = (h ^ (p[3] | (uint32_t) p[7] << 16)) * 16777619u;
            h = (h ^ (p[11] | (uint32_t) p[15] << 16)) * 16777619u;
        }
        h = (h ^ (uint32_t) i) * 16777619u;
        int oy = a0 & 0xff;
        for (int dy = 0; dy < bh; dy++) {
            int y = (oy + dy) & 0xff;
            if (y < 192) sig[y] = (sig[y] ^ h) * 16777619u;
        }
    }
}

static uint32_t s_vram_ver[2][FRAME_VRAM_PAGES];
static uint8_t s_last_oam[2][0x400];
static uint32_t s_line_sig[2][192];
static uint32_t s_prepare_sig[2];

static uint32_t prepare_signature(const Engine *e) {
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t) (e->ext_bg + 1)) * 16777619u;
    h = (h ^ (uint32_t) (e->side[0] != NULL)) * 16777619u;
    if (e->side[0]) {
        for (int k = 0; k < 2; k++) {
            for (int y = 0; y < 192; y++) h = (h ^ e->side[k][y]) * 16777619u;
        }
    }
    return h;
}

static void render_both(const Job *ja, const Job *jb, bool split) {
    rgb_table_init();
    bool skip[2] = {s_skip[0], s_skip[1]};
    s_skip[0] = s_skip[1] = false;
    bool capture = ja->f->io[0x67] & 0x80;
    const Job *jobs[2] = {ja, jb};
    enum { NONE, LINES, FULL } mode[2];
    bool obj_vram_same[2];
    for (int i = 0; i < 2; i++) {
        const FrameSnapshot *f = jobs[i]->f;
        EngineInputs in;
        inputs_of(&in, jobs[i], split);
        bool fixed_3d = jobs[i]->layer3d != NULL && jobs[i]->layer3d != GPU2D_SPLIT && jobs[i]->layer3d != GPU2D_LAYERS;
        uint64_t bg_pages = engine_vram_pages(jobs[i]->map, jobs[i]->is_a, false);
        uint64_t obj_pages = engine_vram_pages(jobs[i]->map, jobs[i]->is_a, true);
        bool vram_same = true;
        obj_vram_same[i] = true;
        for (uint32_t p = 0; p < FRAME_VRAM_PAGES; p++) {
            if (s_vram_ver[i][p] == f->vram_version[p]) continue;
            if (bg_pages >> p & 1) vram_same = false;
            if (obj_pages >> p & 1) obj_vram_same[i] = false;
        }
        size_t oam_at = offsetof(EngineInputs, oam), oam_end = oam_at + sizeof(in.oam);
        bool rest_same = s_last_valid[i] && memcmp(&in, &s_last[i], oam_at) == 0 &&
                         memcmp((uint8_t *) &in + oam_end, (uint8_t *) &s_last[i] + oam_end, sizeof(in) - oam_end) == 0;
        bool oam_same = memcmp(in.oam, s_last_oam[i], sizeof(in.oam)) == 0;
        if (!vram_same || !rest_same || fixed_3d || (i == 0 && capture)) mode[i] = FULL;
        else mode[i] = oam_same && obj_vram_same[i] ? NONE : LINES;
        if (mode[i] != NONE && skip[i] && !(i == 0 && (capture || fixed_3d))) {
            mode[i] = NONE;
            s_last_valid[i] = false;
            continue;
        }
        if (mode[i] == NONE) continue;
        s_last[i] = in;
        s_last_valid[i] = true;
        memcpy(s_last_oam[i], in.oam, sizeof(in.oam));
        memcpy(s_vram_ver[i], f->vram_version, sizeof(s_vram_ver[i]));
    }
    g_gpu2d_unchanged = mode[0] == NONE && mode[1] == NONE;
    if (g_gpu2d_unchanged) return;
    Engine engines[2];
    uint8_t dirty[2][192];
    Rows rows = {{NULL, NULL}, {ja, jb}, {NULL, NULL}};
    for (int i = 0; i < 2; i++) {
        if (mode[i] == NONE) continue;
        engine_prepare(&engines[i], jobs[i]->f, jobs[i]->map, jobs[i]->is_a, jobs[i]->margin);
        uint32_t psig = prepare_signature(&engines[i]);
        if (psig != s_prepare_sig[i]) mode[i] = FULL;
        s_prepare_sig[i] = psig;
        uint32_t sig[192];
        line_signatures(&engines[i], sig);
        if (mode[i] == LINES) {
            int n = 0;
            if (obj_vram_same[i]) {
                for (int y = 0; y < 192; y++) n += dirty[i][y] = sig[y] != s_line_sig[i][y];
            } else {
                for (int y = 0; y < 192; y++) {
                    n += dirty[i][y] = sig[y] != s_line_sig[i][y] || sig[y] != 2166136261u ||
                                       s_line_sig[i][y] != 2166136261u;
                }
            }
            rows.dirty[i] = dirty[i];
            if (!n) {
                memcpy(s_line_sig[i], sig, sizeof(sig));
                continue;
            }
        }
        memcpy(s_line_sig[i], sig, sizeof(sig));
        rows.e[i] = &engines[i];
        s_ver_counter++;
        for (int y = 0; y < 192; y++) {
            if (mode[i] == FULL || dirty[i][y]) s_line_ver[i][y] = s_ver_counter;
        }
    }
    if (!rows.e[0] && !rows.e[1]) {
        g_gpu2d_unchanged = true;
        return;
    }
    for (int i = 0; i < 2; i++) g_gpu2d_modes[rows.e[i] ? (int) mode[i] : 0]++;
    gpu2d_parallel_rows(run_rows, &rows);
    if (rows.e[0] && mode[0] == FULL) engine_finish(&engines[0], ja->layer3d);
}

void gpu2d_render(const FrameSnapshot *f, const uint32_t *layer3d, uint32_t *top, uint32_t *bottom, int top_margin) {
    VramMap map;
    vram_map_build(&map, f->vramcnt);
    uint16_t powcnt = (uint16_t) (f->io[0x304] | f->io[0x305] << 8);
    bool a_on_top = powcnt & 0x8000;
    Job ja = {f, &map, true, layer3d, a_on_top ? top : bottom, NULL, a_on_top ? top_margin : 0};
    Job jb = {f, &map, false, NULL, a_on_top ? bottom : top, NULL, a_on_top ? 0 : top_margin};
    render_both(&ja, &jb, false);
}

static void compose_params(const FrameSnapshot *f, bool is_a, int margin, Gpu2dCompose *c) {
    const uint8_t *io = f->io + (is_a ? 0 : 0x1000);
    uint32_t dispcnt = (uint32_t) io[0] | io[1] << 8 | io[2] << 16 | (uint32_t) io[3] << 24;
    memset(c, 0, sizeof(*c));
    c->margin = margin;
    c->width = 256 + 2 * margin;
    c->bg0_3d = is_a && (dispcnt & 8);
    for (int bg = 0; bg < 4; bg++) {
        c->prio[bg] = (io[0x08 + bg * 2]) & 3;
        c->enabled[bg] = (dispcnt >> (8 + bg)) & 1;
    }
    uint16_t bldcnt = (uint16_t) (io[0x50] | io[0x51] << 8), bldalpha = (uint16_t) (io[0x52] | io[0x53] << 8);
    for (int l = 0; l < 6; l++) {
        c->first[l] = (bldcnt >> l) & 1;
        c->second[l] = (bldcnt >> (8 + l)) & 1;
    }
    c->effect = (bldcnt >> 6) & 3;
    c->eva = bldalpha & 31;
    c->evb = (bldalpha >> 8) & 31;
    c->evy = io[0x54] & 31;
    if (c->eva > 16) c->eva = 16;
    if (c->evb > 16) c->evb = 16;
    if (c->evy > 16) c->evy = 16;
    const uint8_t *pal = f->palette + (is_a ? 0 : 0x400);
    c->backdrop = (uint16_t) ((pal[0] | pal[1] << 8) & 0x7fff);
    c->bright = (uint16_t) (io[0x6c] | io[0x6d] << 8);
    uint32_t display = (dispcnt >> 16) & 3;
    if (!is_a && display > 1) display = 1;
    c->plain = display != 1 || (dispcnt & 0x80);
}

bool gpu2d_render_layers(const FrameSnapshot *f, uint32_t *const top[3], uint32_t *const bottom[3], int top_margin,
                         Gpu2dCompose compose[2]) {
    uint16_t powcnt = (uint16_t) (f->io[0x304] | f->io[0x305] << 8);
    bool a_on_top = powcnt & 0x8000;
    compose_params(f, true, a_on_top ? top_margin : 0, &compose[a_on_top ? 0 : 1]);
    compose_params(f, false, a_on_top ? 0 : top_margin, &compose[a_on_top ? 1 : 0]);
    if (compose[0].plain || compose[1].plain || (f->io[0x67] & 0x80)) return false;
    VramMap map;
    vram_map_build(&map, f->vramcnt);
    uint32_t *const *la = a_on_top ? top : bottom, *const *lb = a_on_top ? bottom : top;
    Job ja = {f, &map, true, GPU2D_LAYERS, la[0], la[1], a_on_top ? top_margin : 0, la[2]};
    Job jb = {f, &map, false, GPU2D_LAYERS, lb[0], lb[1], a_on_top ? 0 : top_margin, lb[2]};
    render_both(&ja, &jb, false);
    return true;
}

typedef struct HudRows {
    const Engine *e;
    uint32_t *out;
    int rows;
    const uint8_t *used;
} HudRows;

static void hud_rows(void *arg, int y0, int y1) {
    const HudRows *h = arg;
    if (y1 > h->rows) y1 = h->rows;
    for (int y = y0; y < y1; y++) {
        if (h->used && !h->used[y]) memset(h->out + y * 256, 0, 256 * 4);
        else render_engine_line(h->e, y, NULL, h->out + y * 256, NULL);
    }
}

static void sprite_lines(const Engine *e, int lo, int hi, uint8_t used[192]) {
    const uint16_t *oam = (const uint16_t *) (e->f->oam + 0x400);
    memset(used, 0, 192);
    for (int i = lo; i < hi; i++) {
        const uint16_t *a = oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1];
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        uint32_t shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        if (shape == 3) continue;
        int bh = s_obj_h[shape][size];
        if (affine && (a0 & 0x200)) bh *= 2;
        int oy = a0 & 0xff;
        for (int dy = 0; dy < bh; dy++) {
            int y = (oy + dy) & 0xff;
            if (y < 192) used[y] = 1;
        }
    }
}

int gpu2d_render_hud(const FrameSnapshot *f, uint32_t *out) {
    const uint8_t *io = f->io + 0x1000;
    uint32_t dispcnt = (uint32_t) io[0] | io[1] << 8 | io[2] << 16 | (uint32_t) io[3] << 24;
    uint32_t mode = dispcnt & 7, layers = (dispcnt >> 8) & 0x1f;
    if (((dispcnt >> 16) & 3) != 1 || !layers) return 0;
    bool map = mode == 5;
    bool icons = mode == 0 && (layers & ~1u) == 0x1c;
    bool pause = (map || icons) && (layers & 1);
    rgb_table_init();
    VramMap vmap;
    vram_map_build(&vmap, f->vramcnt);
    Engine e;
    engine_prepare(&e, f, &vmap, false, 0);
    e.hud = map || icons;
    if (icons) e.dispcnt &= ~0x800u;
    if (pause) e.dispcnt &= ~0x100u;
    uint8_t lo = f->hud_oam[0], hi = f->hud_oam[1];
    bool list = e.hud && lo <= hi && hi <= 128 && hi > 0;
    Engine sprites = e;
    sprites.dispcnt = (e.dispcnt & ~0x0f00u) | 0x1000u;
    sprites.obj_mode = 1;
    if (list) {
        e.obj_mode = 1;
        e.obj_lo = hi;
        e.obj_hi = 128;
    }
    static uint32_t s_tick, s_last_kind;
    uint32_t kind = 1u | (map ? 2u : 0) | (icons ? 4u : 0) | (pause ? 8u : 0) | (list ? 16u : 0);
    bool all = kind != s_last_kind;
    s_last_kind = kind;
    s_tick++;
    bool do_map = all || !list || (s_tick & 1), do_list = list && (all || !(s_tick & 1));
    bool round = map && list;
    if (do_map) {
        Engine m = e;
        if (round) m.dispcnt &= ~0x1000u;
        HudRows rows = {&m, out + GPU2D_HUD_ROW_MAP * 256, 192, NULL};
        gpu2d_parallel_rows(hud_rows, &rows);
        if (round) {
            static uint8_t used_map[192];
            Engine o = sprites;
            o.obj_lo = hi;
            o.obj_hi = 128;
            sprite_lines(&o, hi, 128, used_map);
            HudRows orow = {&o, out + GPU2D_HUD_ROW_MAP_OBJ * 256, 192, used_map};
            gpu2d_parallel_rows(hud_rows, &orow);
        }
    }
    if (do_list) {
        static uint8_t used_list[192], used_time[192];
        Engine l = sprites;
        l.obj_lo = lo;
        l.obj_hi = hi;
        sprite_lines(&l, lo, hi, used_list);
        HudRows lrow = {&l, out + GPU2D_HUD_ROW_LIST * 256, 192, used_list};
        gpu2d_parallel_rows(hud_rows, &lrow);
        Engine t = sprites;
        t.obj_lo = 0;
        t.obj_hi = lo;
        sprite_lines(&t, 0, lo, used_time);
        HudRows trow = {&t, out + GPU2D_HUD_ROW_TIME * 256, GPU2D_HUD_TIME_ROWS, used_time};
        gpu2d_parallel_rows(hud_rows, &trow);
    }
    if (pause && (all || do_map)) {
        Engine p = e;
        p.dispcnt = (dispcnt & ~0x1f00u) | 0x100u;
        p.obj_mode = 0;
        HudRows prow = {&p, out + GPU2D_HUD_ROW_PAUSE * 256, 192, NULL};
        gpu2d_parallel_rows(hud_rows, &prow);
    }
    if (!e.hud) return GPU2D_HUD_SHOWN | GPU2D_HUD_PICTURE;
    return GPU2D_HUD_SHOWN | (map ? GPU2D_HUD_MAP : 0) | (pause ? GPU2D_HUD_PAUSE : 0) | (list ? GPU2D_HUD_LIST : 0) |
           (round ? GPU2D_HUD_ROUND : 0);
}

void gpu2d_render_split(const FrameSnapshot *f, uint32_t *engine_a_above, uint32_t *engine_a_below,
                        uint32_t *engine_b, int margin_a, int margin_b) {
    VramMap map;
    vram_map_build(&map, f->vramcnt);
    Job ja = {f, &map, true, GPU2D_SPLIT, engine_a_above, engine_a_below, margin_a};
    Job jb = {f, &map, false, NULL, engine_b, NULL, margin_b};
    render_both(&ja, &jb, true);
}

// ------------------------------------------------------------------------------------------------ capture
static inline uint16_t to555(uint32_t c) {
    return (uint16_t) (((c >> 3) & 31) | ((c >> 11) & 31) << 5 | ((c >> 19) & 31) << 10);
}

void gpu2d_capture(const FrameSnapshot *f, const uint32_t *engineA, const uint32_t *layer3d) {
    uint32_t cnt = (uint32_t) f->io[0x64] | f->io[0x65] << 8 | f->io[0x66] << 16 | (uint32_t) f->io[0x67] << 24;
    uint32_t eva = cnt & 31, evb = (cnt >> 8) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    uint32_t bank = (cnt >> 16) & 3, write_ofs = ((cnt >> 18) & 3) * 0x8000;
    static const uint16_t cw[4] = {128, 256, 256, 256}, ch[4] = {128, 64, 128, 192};
    uint32_t w = cw[(cnt >> 20) & 3], h = ch[(cnt >> 20) & 3];
    bool src_3d = cnt & (1u << 24);
    uint32_t source = (cnt >> 29) & 3;
    uint32_t dispcnt = (uint32_t) f->io[0] | f->io[1] << 8 | f->io[2] << 16 | (uint32_t) f->io[3] << 24;
    uint32_t read_bank = (dispcnt >> 18) & 3, read_ofs = ((cnt >> 26) & 3) * 0x8000;
    port_vram_lock();
    uint8_t *dst = g_vram_banks + g_vram_bank_offset[bank];
    const uint8_t *srcb = f->vram + g_vram_bank_offset[read_bank];
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t a_rgba = src_3d ? (layer3d ? layer3d[y * 256 + x] : 0) : engineA[y * 256 + x];
            uint16_t a = to555(a_rgba);
            bool a_alpha = src_3d ? (a_rgba >> 24) != 0 : true;
            uint32_t roff = (read_ofs + (y * 256 + x) * 2) & 0x1ffff;
            uint16_t b = (uint16_t) (srcb[roff] | srcb[roff + 1] << 8);
            uint16_t out;
            if (source == 0) {
                out = a | (a_alpha ? 0x8000 : 0);
            } else if (source == 1) {
                out = b;
            } else {
                uint32_t r = 0;
                for (int s = 0; s < 15; s += 5) {
                    uint32_t ca = a_alpha ? (a >> s) & 31 : 0, cb = b & 0x8000 ? (b >> s) & 31 : 0;
                    uint32_t v = (ca * eva + cb * evb + 8) / 16;
                    r |= (v > 31 ? 31 : v) << s;
                }
                out = (uint16_t) (r | ((a_alpha && eva) || ((b & 0x8000) && evb) ? 0x8000 : 0));
            }
            uint32_t woff = (write_ofs + (y * 256 + x) * 2) & 0x1ffff;
            dst[woff] = (uint8_t) out;
            dst[woff + 1] = (uint8_t) (out >> 8);
        }
    }
    port_vram_bank_written((int) bank);
    port_vram_unlock();
    g_capture_done();
}

bool gpu2d_engine_a_on_top(const FrameSnapshot *f) { return (f->io[0x305] & 0x80) != 0; }
