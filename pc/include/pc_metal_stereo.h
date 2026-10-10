/* pc_metal_stereo.h - single-pass stereo in the GL-on-Metal shim (pc_gl_metal.mm).
 * Trevorbilt, 2026.
 *
 * The VR mod interprets each frame's display list once per eye. With single-pass
 * stereo it interprets it once: every world draw into the stereo target (both eyes
 * side by side, each eye_width wide) is one instanced draw, instance 0 the left eye
 * with the program's u_modelview / u_projection, instance 1 the right eye with the
 * matrices pcm_stereo_right gave the same program. The GL viewport stays in one
 * eye's pixels; the vertex shader places each eye in its half and clip distances
 * keep it there. Kept out of glad/gl.h so changing it rebuilds only its users. */
#ifndef PC_METAL_STEREO_H
#define PC_METAL_STEREO_H

#ifdef __cplusplus
extern "C" {
#endif

/* The framebuffer whose TEV and sky draws go to both eyes (0: off), each eye
 * eye_width pixels wide. */
void pcm_stereo_target(unsigned framebuffer, int eye_width);
/* The right eye's modelview and projection for the bound program (row-major 4x4, as
 * glUniformMatrix4fv with transpose); the left eye's are its u_modelview/u_projection. */
void pcm_stereo_right(const float* modelview44, const float* projection44);
/* Stereo-target draws for one eye only (0 or 1: the sky, drawn per eye), or both (-1). */
void pcm_stereo_eye_only(int eye);
/* The comfort vignette over a GL-pixel rectangle of the bound framebuffer (one eye). */
void pcm_draw_vignette_rect(float strength, const float rect[4]);

#ifdef __cplusplus
}
#endif

#endif /* PC_METAL_STEREO_H */
