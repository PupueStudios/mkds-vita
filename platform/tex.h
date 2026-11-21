#pragma once

#include "gpu2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEX_WIDTH(teximage) (8u << (((teximage) >> 20) & 7))
#define TEX_HEIGHT(teximage) (8u << (((teximage) >> 23) & 7))

// Decodes the texture described by TEXIMAGE_PARAM / PLTT_BASE into out (width * height RGBA, alpha 0..31 in bits
// 24..31). Returns false for "no texture".
bool tex_decode(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt, uint32_t *out);

uint32_t tex_source_hash(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt, uint64_t *pages);
void tex_source_hash_parts(const VramMap *map, const uint8_t *vram, uint32_t teximage, uint32_t pltt, bool texels,
                           uint32_t *texel_hash, uint64_t *texel_pages, bool palette, uint32_t *pal_hash,
                           uint64_t *pal_pages);
uint32_t tex_map_signature(const VramMap *map);
static inline bool tex_pages_changed(uint64_t pages, const uint32_t *version, uint32_t since) {
    for (uint32_t i = 0; pages; i++, pages >>= 1) {
        if ((pages & 1) && version[i] > since) return true;
    }
    return false;
}
static inline uint32_t tex_newest_version(const uint32_t *version) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < FRAME_VRAM_PAGES; i++) m = version[i] > m ? version[i] : m;
    return m;
}

#ifdef __cplusplus
}
#endif
