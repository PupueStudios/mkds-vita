#pragma once

#include "frame.h"

void gl3d_init(void);
// Renders the polygon list at (256 * wide * scale) x (192 * scale): `wide` > 1 for a list the game projected wider
// than 4:3. With `prev`, polygons found in both lists are drawn at fraction `t` from prev to frame (interpolated
// frames). Returns the colour texture (RGBA, alpha 0..1 as the DS 3D alpha).
uint32_t gl3d_render(const Gx3dFrame *frame, const Gx3dFrame *prev, float t, const FrameSnapshot *snap, int scale,
                     float wide);
bool gl3d_gpu_vertices_supported(void);
bool gl3d_meshes_supported(void);
void gl3d_use_target(int slot);
void gl3d_set_msaa(int samples);
void gl3d_capture_layer(const Gx3dFrame *frame, const FrameSnapshot *snap, uint32_t *out);
