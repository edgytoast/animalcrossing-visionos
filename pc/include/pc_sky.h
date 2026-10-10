/* Outdoor sky at infinity. All matrices use the renderer's row-major layout. */
#ifndef PC_SKY_H
#define PC_SKY_H
#ifdef __cplusplus
extern "C" {
#endif

void pc_sky_set_environment(int outdoors, float seconds, float overcast);
void pc_sky_begin_pass(void);
void pc_sky_set_view(const float* view34);
/* Returns 1 only when a sky draw was submitted. correction34 is NULL in flat mode. */
int pc_sky_draw(const float* projection44, const float* correction34);
/* Single-pass stereo: lets the sky draw again in this pass (once per eye). */
void pc_sky_allow_redraw(void);
void pc_sky_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
