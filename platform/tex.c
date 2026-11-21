// DS texture formats: decode a whole texture to RGBA (alpha 0..31 in the top byte), shared by the software
// rasteriser and the OpenGL renderer's texture cache.
#include "port.h"
#include "tex.h"

static inline uint32_t rgba(uint16_t c, uint32_t a) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | a << 24;
}

static inline uint8_t tex8(const VramMap *m, const uint8_t *vram, uint32_t off) {
    const uint8_t *p = vram_ptr(m, vram, VR_TEX, off & 0x7ffff);
    return p ? *p : 0;
}

static inline uint16_t tex16(const VramMap *m, const uint8_t *vram, uint32_t off) {
    return (uint16_t) (tex8(m, vram, off) | tex8(m, vram, off + 1) << 8);
}

static inline uint16_t pal16(const VramMap *m, const uint8_t *vram, uint32_t off) {
    const uint8_t *p = vram_ptr(m, vram, VR_TEXPAL, off & 0x1ffff);
    return p ? (uint16_t) (p[0] | p[1] << 8) : 0;
}

bool tex_decode(const VramMap *m, const uint8_t *vram, uint32_t teximage, uint32_t pltt, uint32_t *out) {
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 0) {
        return false;
    }
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t addr = (teximage & 0xffff) << 3;
    bool color0_transparent = teximage & (1u << 29);
    uint32_t pal = fmt == 2 ? pltt << 3 : pltt << 4;
    switch (fmt) {
    case 1: // A3I5
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t v = tex8(m, vram, addr + i);
            uint32_t a3 = v >> 5;
            out[i] = rgba(pal16(m, vram, pal + (v & 31) * 2), (a3 << 2) + (a3 >> 1));
        }
        break;
    case 2: // 4 colours
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t idx = (tex8(m, vram, addr + i / 4) >> ((i & 3) * 2)) & 3;
            out[i] = rgba(pal16(m, vram, pal + idx * 2), idx == 0 && color0_transparent ? 0 : 31);
        }
        break;
    case 3: // 16 colours
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t idx = (tex8(m, vram, addr + i / 2) >> ((i & 1) * 4)) & 15;
            out[i] = rgba(pal16(m, vram, pal + idx * 2), idx == 0 && color0_transparent ? 0 : 31);
        }
        break;
    case 4: // 256 colours
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t idx = tex8(m, vram, addr + i);
            out[i] = rgba(pal16(m, vram, pal + idx * 2), idx == 0 && color0_transparent ? 0 : 31);
        }
        break;
    case 5: { // 4x4 compressed: 2-bit texels per block, palette index data in slot 1
        uint32_t slot1 = 0x20000 + ((addr & 0x1ffff) >> 1) + (addr >= 0x40000 ? 0x10000 : 0);
        for (uint32_t by = 0; by < h / 4; by++) {
            for (uint32_t bx = 0; bx < w / 4; bx++) {
                uint32_t block = by * (w / 4) + bx;
                uint32_t bits = tex8(m, vram, addr + block * 4) | tex8(m, vram, addr + block * 4 + 1) << 8 |
                                tex8(m, vram, addr + block * 4 + 2) << 16 |
                                (uint32_t) tex8(m, vram, addr + block * 4 + 3) << 24;
                uint16_t info = tex16(m, vram, slot1 + block * 2);
                uint32_t pbase = pal + (info & 0x3fff) * 4;
                uint32_t mode = info >> 14;
                uint16_t c0 = pal16(m, vram, pbase), c1 = pal16(m, vram, pbase + 2);
                uint32_t cols[4];
                cols[0] = rgba(c0, 31);
                cols[1] = rgba(c1, 31);
                switch (mode) {
                case 0:
                    cols[2] = rgba(pal16(m, vram, pbase + 4), 31);
                    cols[3] = 0;
                    break;
                case 1: {
                    uint16_t mix = 0;
                    for (int s = 0; s < 15; s += 5) mix |= (uint16_t) ((((c0 >> s) & 31) + ((c1 >> s) & 31)) / 2 << s);
                    cols[2] = rgba(mix, 31);
                    cols[3] = 0;
                    break;
                }
                case 2:
                    cols[2] = rgba(pal16(m, vram, pbase + 4), 31);
                    cols[3] = rgba(pal16(m, vram, pbase + 6), 31);
                    break;
                default: {
                    uint16_t m1 = 0, m2 = 0;
                    for (int s = 0; s < 15; s += 5) {
                        uint32_t a = (c0 >> s) & 31, b = (c1 >> s) & 31;
                        m1 |= (uint16_t) ((a * 5 + b * 3) / 8 << s);
                        m2 |= (uint16_t) ((a * 3 + b * 5) / 8 << s);
                    }
                    cols[2] = rgba(m1, 31);
                    cols[3] = rgba(m2, 31);
                    break;
                }
                }
                for (uint32_t ty = 0; ty < 4; ty++) {
                    for (uint32_t tx = 0; tx < 4; tx++) {
                        out[(by * 4 + ty) * w + bx * 4 + tx] = cols[(bits >> ((ty * 4 + tx) * 2)) & 3];
                    }
                }
            }
        }
        break;
    }
    case 6: // A5I3
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t v = tex8(m, vram, addr + i);
            out[i] = rgba(pal16(m, vram, pal + (v & 7) * 2), v >> 3);
        }
        break;
    case 7: // direct colour
        for (uint32_t i = 0; i < w * h; i++) {
            uint16_t c = tex16(m, vram, addr + i * 2);
            out[i] = rgba(c, c & 0x8000 ? 31 : 0);
        }
        break;
    }
    return true;
}

static uint32_t region_hash(const VramMap *map, const uint8_t *vram, int region, uint32_t off, uint32_t len,
                            uint32_t mask, uint32_t h, uint64_t *pages) {
    while (len) {
        uint32_t o = off & mask;
        uint32_t chunk = 0x4000 - (o & 0x3fff);
        if (chunk > len) chunk = len;
        const uint8_t *p = vram_ptr(map, vram, region, o);
        if (p) {
            *pages |= 1ull << ((uint32_t) (p - vram) / FRAME_VRAM_PAGE);
            uint32_t i = 0;
            if (!((uintptr_t) p & 3)) {
                for (; i + 4 <= chunk; i += 4) h = (h ^ *(const uint32_t *) (p + i)) * 16777619u;
            }
            for (; i < chunk; i++) h = (h ^ p[i]) * 16777619u;
        } else {
            h = (h ^ 0xffffffffu) * 16777619u;
        }
        off += chunk;
        len -= chunk;
    }
    return h;
}

uint32_t tex_source_hash(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt, uint64_t *pages) {
    static const uint8_t bits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
    static const uint32_t pal_bytes[8] = {0, 64, 8, 32, 512, 0x10000, 16, 0};
    uint32_t fmt = (teximage >> 26) & 7, w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t addr = (teximage & 0xffff) << 3;
    *pages = 0;
    uint32_t hash = region_hash(map, vram, VR_TEX, addr, w * h * bits[fmt] / 8, 0x7ffff, 2166136261u, pages);
    if (fmt == 5) {
        uint32_t slot1 = 0x20000 + ((addr & 0x1ffff) >> 1) + (addr >= 0x40000 ? 0x10000 : 0);
        hash = region_hash(map, vram, VR_TEX, slot1, w * h / 8, 0x7ffff, hash, pages);
    }
    if (pal_bytes[fmt]) {
        uint32_t pal = fmt == 2 ? pltt << 3 : pltt << 4;
        hash = region_hash(map, vram, VR_TEXPAL, pal, pal_bytes[fmt], 0x1ffff, hash, pages);
    }
    return hash;
}

void tex_source_hash_parts(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt, bool texels,
                           uint32_t *texel_hash, uint64_t *texel_pages, bool palette, uint32_t *pal_hash,
                           uint64_t *pal_pages) {
    static const uint8_t bits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
    static const uint32_t pal_bytes[8] = {0, 64, 8, 32, 512, 0x10000, 16, 0};
    uint32_t fmt = (teximage >> 26) & 7, w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t addr = (teximage & 0xffff) << 3;
    if (texels) {
        *texel_pages = 0;
        uint32_t hash = region_hash(map, vram, VR_TEX, addr, w * h * bits[fmt] / 8, 0x7ffff, 2166136261u, texel_pages);
        if (fmt == 5) {
            uint32_t slot1 = 0x20000 + ((addr & 0x1ffff) >> 1) + (addr >= 0x40000 ? 0x10000 : 0);
            hash = region_hash(map, vram, VR_TEX, slot1, w * h / 8, 0x7ffff, hash, texel_pages);
        }
        *texel_hash = hash;
    }
    if (palette) {
        *pal_pages = 0;
        *pal_hash = 2166136261u;
        if (pal_bytes[fmt]) {
            uint32_t pal = fmt == 2 ? pltt << 3 : pltt << 4;
            *pal_hash = region_hash(map, vram, VR_TEXPAL, pal, pal_bytes[fmt], 0x1ffff, 2166136261u, pal_pages);
        }
    }
}

uint32_t tex_map_signature(const VramMap *map) {
    uint32_t h = 2166136261u;
    for (int r = VR_TEX; r <= VR_TEXPAL; r++) {
        for (int i = 0; i < 48; i++) h = (h ^ (uint32_t) map->page[r][i]) * 16777619u;
    }
    return h;
}
