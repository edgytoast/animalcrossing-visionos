/* pc_test_hooks.h - env-var driven test hooks (see pc_test_hooks.c). */
#ifndef PC_TEST_HOOKS_H
#define PC_TEST_HOOKS_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a malloc'd RGBA8 copy of the last presented frame (caller frees),
 * its size, and whether rows run bottom-up (GL) or top-down (Metal). */
typedef u8* (*pc_test_capture_fn)(int* w, int* h, int* bottom_up);
extern pc_test_capture_fn g_pc_test_capture;

int  pc_test_hooks_active(void);
void pc_test_hooks_pad(u16* buttons, s8* sx, s8* sy, s8* cx, s8* cy, u8* tl, u8* tr);
void pc_test_hooks_frame(void);
/* ACVR_TEST_CAMLOG=1: one line per game camera update (Camera2_SetView). */
void pc_test_log_camera(int process, const float eye[3], const float center[3], float fovy, int first_person);

#ifdef __cplusplus
}
#endif

#endif
