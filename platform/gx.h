// What one displayed frame needs: the 3D polygon list with its rendering state, and the 2D engines' registers and
// memories. Built by gx3d.c (polygons) and frame.c (snapshot), consumed by the renderers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX_MAX_POLYGONS 8192 // culled ones included (see GxPolygon.hidden)
#define GX_MAX_VERTICES 32768

typedef struct GxVertex {
    int32_t x, y, z, w; // clip space, 20.12 fixed point (before the viewport transform)
    int16_t s, t;       // texture coordinates, 12.4 texels
    uint8_t r, g, b;    // 0..31 (lit and clamped)
    uint8_t pad;
} GxVertex;

typedef struct GxLightState {
    int32_t emi9[3];
    int32_t dir[4][3], half[4][3];
    int32_t spe_lc[4][3], dif_lc[4][3], amb[4][3];
    uint8_t use_table;
    uint8_t shininess[128];
} GxLightState;
#define GX_MAX_LIGHT_STATES 64
#define GX_MAX_VECS 4096

typedef struct GxMeshVertex {
    int16_t p[4];
    int16_t n[4];
    int16_t s, t;
    uint8_t r, g, b, color_src;
    uint8_t st_src, pad[3];
} GxMeshVertex;

typedef struct GxMesh {
    uint32_t vertex_count, index_count;
    const GxMeshVertex *v;
    const uint16_t *idx;
    bool lit;
    uint32_t last_color_cmd, last_color_param;
    bool has_texcoord;
    uint32_t last_texcoord;
    int16_t last_v[3];
    int8_t last_prim;
} GxMesh;

const GxMesh *gx3d_mesh(uint32_t id);
uint32_t gx3d_mesh_gen(void);

typedef struct GxDraw {
    uint32_t at;
    uint32_t mesh, gen;
    uint32_t mtx;
    uint16_t light;
    uint8_t mask, texmode;
    uint32_t attr, teximage, pltt;
    uint8_t viewport[4];
    int32_t aspect;
    int32_t vec[9];
    int32_t tm[6];
    uint8_t color[3];
    int16_t s, t;
} GxDraw;
#define GX_MAX_DRAWS 2048

typedef struct GxPolygon {
    uint32_t first;      // index of the first vertex
    uint8_t count;       // 3 or 4
    uint8_t viewport[4]; // x1, y1, x2, y2 (DS screen coordinates, y up from the bottom as in VIEWPORT)
    uint8_t front;       // 1 when the polygon faces the viewer
    uint8_t hidden;      // culled or outside the view: not drawn, kept so that models have the same polygons every
                         // frame (frame interpolation pairs them up)
    uint32_t attr;       // POLYGON_ATTR latched at BEGIN_VTXS
    uint32_t teximage;   // TEXIMAGE_PARAM
    uint32_t pltt;       // PLTT_BASE
    uint32_t tag;        // place in its vertex block (BEGIN_VTXS), counting culled ones: identity for interpolation
    int32_t aspect;      // aspect ratio of its projection (20.12): 4:3 views keep their shape on a wide screen
    uint32_t obj;        // what the game was drawing (g_port_draw_id): identity of the object for interpolation
} GxPolygon;

#define GX_MAX_MATRICES 8192

typedef struct Gx3dFrame {
    GxVertex vertices[GX_MAX_VERTICES];
    GxPolygon polygons[GX_MAX_POLYGONS];
    float matrices[GX_MAX_MATRICES][16];
    GxDraw draws[GX_MAX_DRAWS];
    GxLightState lights[GX_MAX_LIGHT_STATES];
    uint32_t vertex_count, polygon_count;
    uint32_t matrix_count;
    uint32_t draw_count, light_count;
    uint8_t gpu_vertices;
    uint32_t swap_param; // SWAP_BUFFERS: bit 0 manual translucent sort, bit 1 W-buffering
    // Rendering registers latched at the swap.
    uint16_t disp3dcnt;
    uint32_t clear_color;
    uint16_t clear_depth;
    uint16_t clrimage_offset;
    uint32_t fog_color;
    uint16_t fog_offset;
    uint8_t fog_table[32];
    uint16_t edge_color[8];
    uint16_t toon_table[32];
    uint8_t alpha_ref;
    int32_t proj_aspect; // aspect ratio of the projection of the first polygon, 20.12 (0: none)
    uint64_t serial; // increases with every list
    uint32_t issue_vbl;
} Gx3dFrame;

static inline void gx3d_copy_frame(Gx3dFrame *d, const Gx3dFrame *g) {
    __builtin_memcpy(d->vertices, g->vertices, sizeof(GxVertex) * g->vertex_count);
    __builtin_memcpy(d->polygons, g->polygons, sizeof(GxPolygon) * g->polygon_count);
    if (g->gpu_vertices) {
        __builtin_memcpy(d->matrices, g->matrices, sizeof(g->matrices[0]) * g->matrix_count);
        __builtin_memcpy(d->draws, g->draws, sizeof(g->draws[0]) * g->draw_count);
        __builtin_memcpy(d->lights, g->lights, sizeof(g->lights[0]) * g->light_count);
    }
    __SIZE_TYPE__ header = __builtin_offsetof(Gx3dFrame, vertex_count);
    __builtin_memcpy((unsigned char *) d + header, (const unsigned char *) g + header, sizeof(Gx3dFrame) - header);
}

void gx3d_copy_displayed(Gx3dFrame *dst);
const Gx3dFrame *gx3d_acquire_displayed(void);
void gx3d_release_list(const Gx3dFrame *f);

// The 3D list being displayed (swapped in at the last VBlank).
const Gx3dFrame *gx3d_displayed(void);

#ifdef __cplusplus
}
#endif
