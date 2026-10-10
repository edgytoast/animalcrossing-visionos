/* pc_mirror.h - the Window view's scene mirror, captured at pc_gx's draws
 * (pc/src/pc_mirror.cpp). Trevorbilt, 2026. */
#ifndef PC_MIRROR_H
#define PC_MIRROR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the window wants the mirror this frame. */
int pc_mirror_enabled(void);
void pc_mirror_set_enabled(int enabled);
/* pc_gx_begin_frame: the top of the frame. */
void pc_mirror_begin_frame(void);
/* pc_gx_flush_vertices: one batch, vertex_buffer[first .. first + count), with g_gx
 * as it draws them. A 2D batch after the world ends the scene. */
void pc_mirror_capture(int first, int count);
/* Before the frame is shown: ends the scene if nothing 2D did. */
void pc_mirror_end_frame(void);
/* emu64's lookAt (row-major 3x4, world to view): the world's up for decals and the sky. */
void pc_mirror_set_view(const float* view34);

extern float g_pc_camera_focus; /* m_camera2.c: the camera's distance to what it looks at */

#ifdef __cplusplus
}
#endif
#endif
