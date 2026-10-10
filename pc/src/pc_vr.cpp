/* pc_vr.cpp - stereo rendering, tracking and controller input for Animal
 * Crossing VR.
 *
 * From LiquidAzir's animal-crossing-vr (MIT, Copyright (c) 2026 LiquidAzir),
 * whose OpenVR backend this file replaces. Modified by Trevorbilt, 2026, for
 * Apple Vision Pro: the runtime is OpenXR on the visionOS provider
 * (visionos/openxr-provider, CompositorServices + ARKit) with its Metal
 * binding, rendered through the GL-on-Metal shim (pc_gl_metal.mm). The mod's
 * game-facing logic is unchanged: per-eye display-list replay with the head
 * pose applied to the game's view, the UI panel, the swing gesture, the tool on
 * the hand, the item presentation and the pad merge.
 *
 * Backends:
 *   - OpenXR (PC_VR_OPENXR, the visionOS app): two swapchains, one per eye,
 *     whose IOSurface textures the shim wraps as GL textures; the eye FBOs
 *     render straight into them. GPU hand-offs use the provider's MTLSharedEvent
 *     fences. Session states drive the Digital Crown pause (the game holds in
 *     pc_vr_frame_begin until a new layer arrives) and the unfocused hold.
 *   - Fake (ACVR_FAKE_VR=1, any build): a fixed head and symmetric eyes, for
 *     the macOS bench; pc_vr_mirror_to_window shows both eyes side by side.
 *
 * Spaces and conventions (see pc/VR_ARCHITECTURE.md):
 *   - Game view matrices are row-major 3x4, right-handed, -Z forward, game units.
 *   - Poses are row-major 3x4 in meters, -Z forward. OpenXR's LOCAL space (the
 *     ARKit origin) is turned into the mod's "seated" frame by a recenter: the
 *     seated origin is the head at the last recenter, yaw-aligned to it.
 *   - W = T_height * Scale * Anchor maps the game world to seated meters; per eye
 *     V_vr = (H*E)^-1 * W and the game's combined view*model position matrices
 *     are corrected by X = V_vr * V_game^-1.
 *   - Projections are kept in the GX depth convention (near -> NDC -1, far -> 0).
 *
 * Everything here runs on the game thread. No operator new (the game owns it). */
#include "pc_platform.h"
#include "pc_settings.h"
#include "pc_vr.h"
#include "pc_fp_camera.h"
#include "pc_vr_swing.h"
#include "pc_vr_menu_input.h"
#include "pc_pause_menu.h"
#include "pc_vr_hands.h"
#include "pc_sky.h"
#include "pc_mirror.h"
#if defined(PC_METAL)
#include "pc_metal_stereo.h"
#endif

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(PC_VR_OPENXR)
#include <openxr/openxr.h>
#include "vr/visionos/xr_visionos.h"
#endif
#if defined(__APPLE__)
#include <os/log.h>
#endif

#if !defined(PC_METAL)
/* The OpenGL builds have no Metal shim: the room's backdrop, fades and the
 * vignette are Metal-only. */
static inline void pcm_draw_under(const float*, const float*) {}
static inline void pcm_draw_over(const float*, const float*) {}
static inline void pcm_draw_vignette(float) {}
static inline void pcm_draw_vignette_rect(float, const float*) {}
static inline void pcm_stereo_target(unsigned, int) {}
#endif

extern "C" {
void pc_gx_draw_pending(void);
void pc_gx_flush_if_begin_complete(void);
int mEv_CheckTitleDemo(void); /* m_event.c: the title's attract loop (0: none) */
void pc_gx_restore_after_nes(void);
void pc_gx_viewport_state_invalidate(void);
void pc_gx_mark_new_pass(void);
void pc_gx_vr_reset_routing(void);
void pc_gx_dirty_colormask(void);
void pc_gx_get_clear(float* rgba, float* depth);
void pc_audio_set_paused(int paused);
extern int g_pc_target_w, g_pc_target_h;   /* pc_gx.c: current render target dims */
extern int g_pc_paused;
extern u32 pc_frame_counter;
extern u32 g_frame_limiter;

}

#if defined(PC_VISIONOS)
/* The visionOS host (visionos/host/game_host.mm), resolved at the framework's link
 * (they stay undefined in the game's prelinked object). */
extern "C" {
bool acvr_visionos_room_behind_menus_enabled(void);
void acvr_visionos_mark_session_focused(void);
bool acvr_visionos_consume_world_recenter(void);
bool acvr_visionos_quit_requested(void);
void acvr_visionos_set_vr_held(int held);
int acvr_visionos_comfort_vignette(void);
int acvr_visionos_snap_turn_degrees(void);
void acvr_visionos_note_snap_turn(int degrees);
}
#else
/* The macOS bench has no host. */
static bool acvr_visionos_room_behind_menus_enabled(void) { return false; }
static void acvr_visionos_mark_session_focused(void) {}
static bool acvr_visionos_consume_world_recenter(void) { return false; }
static bool acvr_visionos_quit_requested(void) { return false; }
static void acvr_visionos_set_vr_held(int held) { pc_audio_set_paused(held); }
static int acvr_visionos_comfort_vignette(void) { return 1; }
static int acvr_visionos_snap_turn_degrees(void) { return g_pc_settings.fp_snap_degrees; }
static void acvr_visionos_note_snap_turn(int) {}
#endif

/* ---------------------------------------------------------------- */
/* Log: [ACVR] lines on stderr and, on Apple platforms, the unified log
 * (headset bugs are diagnosed from it). */

static void pcvr_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ACVR] vr: %s\n", line);
#if defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "[ACVR] vr: %{public}s", line);
#endif
}

/* ---------------------------------------------------------------- */
/* Small row-major matrix helpers (3x4 rigid-ish, implicit [0 0 0 1]) */

typedef float M34[3][4];

static void m34_identity(M34 m) {
    memset(m, 0, sizeof(M34));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

/* out = a * b  (apply b first, then a). out may not alias a or b. */
static void m34_mul(const M34 a, const M34 b, M34 out) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]
                      + (j == 3 ? a[i][3] : 0.0f);
        }
    }
}

/* Inverse of rotation + UNIFORM scale + translation: M = [sR | t]. */
static void m34_invert_rs(const M34 m, M34 out) {
    float s2 = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
    float k = (s2 > 1e-12f) ? 1.0f / s2 : 1.0f;   /* (sR)^T / s^2 = R^T / s */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i][j] = m[j][i] * k;
    for (int i = 0; i < 3; i++)
        out[i][3] = -(out[i][0] * m[0][3] + out[i][1] * m[1][3] + out[i][2] * m[2][3]);
}

/* Inverse of a rigid transform (orthonormal rotation + translation). */
static void m34_invert_rigid(const M34 m, M34 out) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i][j] = m[j][i];
    for (int i = 0; i < 3; i++)
        out[i][3] = -(out[i][0] * m[0][3] + out[i][1] * m[1][3] + out[i][2] * m[2][3]);
}

/* Rotation from a unit quaternion (x, y, z, w) and a translation. */
static void m34_from_pose(float qx, float qy, float qz, float qw, float px, float py, float pz, M34 out) {
    out[0][0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    out[0][1] = 2.0f * (qx * qy - qz * qw);
    out[0][2] = 2.0f * (qx * qz + qy * qw);
    out[1][0] = 2.0f * (qx * qy + qz * qw);
    out[1][1] = 1.0f - 2.0f * (qx * qx + qz * qz);
    out[1][2] = 2.0f * (qy * qz - qx * qw);
    out[2][0] = 2.0f * (qx * qz - qy * qw);
    out[2][1] = 2.0f * (qy * qz + qx * qw);
    out[2][2] = 1.0f - 2.0f * (qx * qx + qy * qy);
    out[0][3] = px;
    out[1][3] = py;
    out[2][3] = pz;
}

static int m34_finite(const M34 m) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            if (!isfinite(m[r][c])) return 0;
    return 1;
}

/* ---------------------------------------------------------------- */
/* State */

typedef struct {
    GLuint fbo;
    GLuint color;      /* the GL texture the FBO renders into */
    GLuint depth_rbo;
    int w, h;
    int x;             /* left edge in the FBO: an eye of the single-pass target (both side by side) */
} PCVRTarget;

enum { PCVR_BACKEND_NONE = 0, PCVR_BACKEND_OPENXR = 1, PCVR_BACKEND_FAKE = 2 };

/* Controller state for one frame (both backends fill it). */
typedef struct {
    float mx, my, cx, cy;           /* left / right thumbstick */
    int a, b, x, y, l, r, z, start; /* GameCube buttons as the mod maps them */
    int valid;
} PCVRButtons;

static struct {
    int active;                 /* VR session live */
    int backend;
    int requested;              /* 0=off, 1=auto, 2=forced on */

    PCVRTarget eye[2];
    PCVRTarget ui;
    /* Single-pass stereo (the default; ACVR_STEREO_PASSES=2 turns it off): one target with
     * both eyes side by side (both), eye[e] its halves (same FBO, x 0 and w). The display list
     * is interpreted once and each world draw goes to both eyes (pc_metal_stereo.h). */
    int single;
    PCVRTarget both;

    int in_scene_pass;          /* inside an eye pass */
    int current_eye;
    int ui_bound;               /* ortho routing currently targets UI FBO */

    /* Tracking (seated frame) */
    M34 seated_from_local;      /* the recenter: LOCAL (ARKit origin) -> seated */
    int have_recenter;
    int recenter_requested;
    M34 head_pose;              /* H: head -> seated, meters */
    M34 eye_pose[2];            /* H*E: eye -> seated */
    M34 inv_eye_pose[2];        /* (H*E)^-1, refreshed each frame */
    float eye_fov[2][4];        /* tangents: left (<0), right, up, down (<0) */
    int have_pose;
    int head_pose_valid;        /* current frame; have_pose is the render fallback */

    /* Game view + derived correction */
    M34 game_view;              /* V: world -> game camera view, game units */
    M34 anchor_yaw;             /* last good leveled anchor (fallback) */
    int have_anchor;
    float anchor_pos[3];        /* where the anchor puts the eyes (game units) */
    /* A scripted camera's shot, held (see pcvr_update_view_correction) */
    int shot_held;
    float shot_pos[3], shot_fx, shot_fz;
    M34 view_correction[2];     /* X = V_vr * V^-1, per eye */
    float eye_projection[2][4][4]; /* GX z-convention, row-major */
    float eye_depth_range[2][2]; /* world viewport range, before UI routing */

    /* Settings */
    float world_scale;          /* meters per game unit */
    float ui_distance;          /* meters */
    float ui_size;              /* panel width, meters */
    float height_offset;        /* meters */
    float ui_dist_k;            /* smoothed panel pull-in (1.0 <-> 0.6 in FP) */

    /* UI composite GL objects */
    GLuint panel_prog;
    GLuint panel_vao, panel_vbo;
    GLint  panel_u_mvp, panel_u_tex;

    /* Input */
    PCVRButtons buttons;
    int input_ready;
    int menu_input_available;
    PCVRMenuInput menu_input;
    int recenter_latch;

    /* Motion tools: right-controller pose in seated space + derived state */
    M34 world_from_seated;      /* W^-1, updated with the view correction */
    int hand_valid;             /* pose valid this frame */
    M34 hand_world;             /* tool anchor in game-world units */
    M34 tool_local;             /* controller grip -> tool grip orientation */
    PCVRSwing swing;
    int hand_ever_valid;        /* pose has been valid at least once */
    int hand_missing_logged;    /* one-shot diagnostic for unbound pose */
    Uint32 active_since_ticks;  /* when the session went active */
    u32 flat_scene_stamp;       /* pc_frame_counter when flat-scene last stamped */
    float hand_prev[3];         /* last frame's grip position (seated m), for speed */
    double hand_prev_time;
    int hand_prev_valid;

    M34 empty_hand_pose[2];     /* grip -> seated, meters */
    int empty_hand_valid[2];
    int empty_hand_seen[2];
    int empty_hands_missing_logged;
    int empty_hands_available;  /* player draw confirmed no held item/action */
    u32 empty_hands_stamp;
    int empty_hands_render_failed;

    /* Frame bookkeeping */
    int frame_begun;            /* runtime frame begun, not yet submitted */
    int submitted_this_frame;
    int logged_first_submit;
    unsigned int frame_index;
    int world_drawn;            /* a perspective batch reached an eye this frame */
    int room_behind_menus;      /* the space opened mixed */
    float fade_black;           /* 0..1: how far the frame fades to black (UI fill) */
    Uint32 stats_ticks;
    unsigned int stats_frames, stats_see_through, stats_swings;
    int snap_noted;             /* the turning the launcher was last told */
    int aa_applied;             /* the compositor's anti-aliasing as last set (-1: not yet) */

    /* Comfort */
    float vignette;             /* 0..1, eased */
    int vignette_enabled;
    float last_cam[3];
    int have_last_cam;
} s_vr;

float g_pc_vr_cull_expand = 0.0f;
float g_pc_vr_cull_znear_slack = 0.0f;
/* Whole-town actor residency: buildings, villagers and props stay spawned
 * across the entire town instead of a 2x2 acre quadrant. */
int g_pc_town_residency = 0;
/* Game logic held (the headset is off or visionOS covers the game): checked by
 * Game_play_move like the pause menu, while frames keep coming. */
int g_pc_external_hold = 0;

extern "C" int pc_vr_draw_radius(void) {
    return g_pc_settings.vr_draw_radius;
}

/* GX depth convention rows: near -> -1, far -> 0 (see emu64 projection load) */
#define PC_VR_NEAR_M 0.05f
/* Far plane scales with the world so large vr_world_scale values don't clip
 * the horizon (game far is 1600 units; leave generous headroom). */
static float pcvr_far_m(void) {
    float f = 6000.0f * s_vr.world_scale;
    return f < 200.0f ? 200.0f : f;
}

static void pcvr_update_eye_projection(int eye);
static void pcvr_update_view_correction(void);

/* First person uses a life-size scale; the diorama scale returns with the
 * normal camera. Rebuilds projections (far plane derives from scale). */
extern "C" void pc_vr_set_fp_scale(int fp_active) {
    if (!s_vr.active) return;
    float want = fp_active
        ? (g_pc_settings.vr_fp_world_scale / 1000.0f)
        : (g_pc_settings.vr_world_scale / 1000.0f);
    if (want <= 0.0001f) want = fp_active ? 0.025f : 0.01f;
    if (want == s_vr.world_scale) return;
    s_vr.world_scale = want;
    pcvr_update_eye_projection(0);
    pcvr_update_eye_projection(1);
    pcvr_update_view_correction();
    pcvr_log("world scale -> %.4f m/unit (%s)", want, fp_active ? "first person" : "camera");
}

/* ---------------------------------------------------------------- */
/* GL helpers */

static int pcvr_create_depth(PCVRTarget* t) {
    glGenRenderbuffers(1, &t->depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, t->w, t->h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    return 1;
}

/* An FBO whose colour is a texture of its own (the UI layer, the fake eyes). */
static int pcvr_create_target(PCVRTarget* t, int w, int h) {
    memset(t, 0, sizeof(*t));
    t->w = w;
    t->h = h;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);

    glGenTextures(1, &t->color);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);

    pcvr_create_depth(t);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth_rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        pcvr_log("FBO %dx%d incomplete: 0x%04X", w, h, status);
        return 0;
    }
    return 1;
}

/* An eye FBO whose colour is re-attached each frame (a swapchain image). */
static int pcvr_create_eye_target(PCVRTarget* t, int w, int h) {
    memset(t, 0, sizeof(*t));
    t->w = w;
    t->h = h;
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    pcvr_create_depth(t);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

static void pcvr_attach_eye_color(PCVRTarget* t, GLuint color) {
    t->color = color;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void pcvr_destroy_target(PCVRTarget* t, int owns_color) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (owns_color && t->color) glDeleteTextures(1, &t->color);
    if (t->depth_rbo) glDeleteRenderbuffers(1, &t->depth_rbo);
    memset(t, 0, sizeof(*t));
}

/* The eye targets: their own, or the single-pass target their halves share. */
static void pcvr_destroy_eyes(void) {
    const int owns = s_vr.backend == PCVR_BACKEND_FAKE;
    if (s_vr.single) {
        pcvr_destroy_target(&s_vr.both, owns);
        memset(&s_vr.eye[0], 0, sizeof(s_vr.eye));
    } else {
        pcvr_destroy_target(&s_vr.eye[0], owns);
        pcvr_destroy_target(&s_vr.eye[1], owns);
    }
}

static const char* s_panel_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"   /* -1..1 quad */
    "layout(location=1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec2 u_half;\n"                /* panel half extents, meters */
    "uniform vec3 u_center;\n"              /* panel center, seated meters */
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec3 p = u_center + vec3(a_pos.x * u_half.x, a_pos.y * u_half.y, 0.0);\n"
    "  gl_Position = u_mvp * vec4(p, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char* s_panel_fs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  vec4 c = texture(u_tex, v_uv);\n"
    "  frag = c;\n"
    "}\n";

static GLint s_panel_u_half, s_panel_u_center;

static GLuint pcvr_compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        pcvr_log("panel shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int pcvr_create_panel_gl(void) {
    GLuint vs = pcvr_compile(GL_VERTEX_SHADER, s_panel_vs);
    GLuint fs = pcvr_compile(GL_FRAGMENT_SHADER, s_panel_fs);
    if (!vs || !fs) return 0;
    s_vr.panel_prog = glCreateProgram();
    glAttachShader(s_vr.panel_prog, vs);
    glAttachShader(s_vr.panel_prog, fs);
    glLinkProgram(s_vr.panel_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s_vr.panel_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        pcvr_log("panel shader link failed");
        return 0;
    }
    s_vr.panel_u_mvp = glGetUniformLocation(s_vr.panel_prog, "u_mvp");
    s_vr.panel_u_tex = glGetUniformLocation(s_vr.panel_prog, "u_tex");
    s_panel_u_half   = glGetUniformLocation(s_vr.panel_prog, "u_half");
    s_panel_u_center = glGetUniformLocation(s_vr.panel_prog, "u_center");

    /* Unit quad; the UI FBO is rendered with GL's bottom-left origin so
     * sampling is direct (v=0 at bottom). */
    static const float quad[] = {
        /* pos      uv */
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenVertexArrays(1, &s_vr.panel_vao);
    glGenBuffers(1, &s_vr.panel_vbo);
    glBindVertexArray(s_vr.panel_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vr.panel_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);
    return 1;
}

/* ---------------------------------------------------------------- */
/* Matrix pipeline */

static void pcvr_update_eye_projection(int eye) {
    const float* t = s_vr.eye_fov[eye];
    const float tl = t[0], tr = t[1], tu = t[2], td = t[3];
    float (*out)[4] = s_vr.eye_projection[eye];
    memset(out, 0, sizeof(float) * 16);
    out[0][0] = 2.0f / (tr - tl);
    out[0][2] = (tr + tl) / (tr - tl);
    out[1][1] = 2.0f / (tu - td);
    out[1][2] = (tu + td) / (tu - td);
    /* The GX convention used by the whole pipeline:
     * z_ndc = (z*n + n*f) / (n-f) / -z  ->  near maps to -1, far to 0. */
    const float n = PC_VR_NEAR_M, f = pcvr_far_m();
    out[2][2] = n / (n - f);
    out[2][3] = (n * f) / (n - f);
    out[3][2] = -1.0f;
}

/* Rebuild X = V_vr * V^-1 for both eyes from the current game view + pose. */
static void pcvr_update_view_correction(void) {
    /* Camera pose from V (rigid): C = -R^T t, forward = -row2 */
    const M34* V = &s_vr.game_view;
    float fx = -(*V)[2][0], fz = -(*V)[2][2];
    float len = sqrtf(fx * fx + fz * fz);

    M34 A;
    if (len > 1e-4f) {
        fx /= len; fz /= len;
        /* camera position C = -R^T t */
        float cx = -((*V)[0][0] * (*V)[0][3] + (*V)[1][0] * (*V)[1][3] + (*V)[2][0] * (*V)[2][3]);
        float cy = -((*V)[0][1] * (*V)[0][3] + (*V)[1][1] * (*V)[1][3] + (*V)[2][1] * (*V)[2][3]);
        float cz = -((*V)[0][2] * (*V)[0][3] + (*V)[1][2] * (*V)[1][3] + (*V)[2][2] * (*V)[2][3]);
        /* The game's own camera (the intro's train, events, the title): the eyes stay where the shot
         * put them, and the head looks around. Following it, the world turned around the player as
         * the camera panned after Rover walking the carriage, and slid half a metre with its sway
         * (forced motion in VR). It moves again only when it cuts or has gone far: more than 1.5 m
         * or 40 degrees from the held view, a step like a snap turn. First person follows every frame. */
        static const int follow = getenv("ACVR_VR_FOLLOW_CAMERA") != NULL;  /* test runs: the old way */
        /* Only with first person chosen, where a view that isn't first person is a scripted shot:
         * the game's own third-person camera (Left grip + Triangle) follows the player without
         * stopping, as the title's attract loop walks its camera through the town; held, either
         * stepped every 1.5 m, so they glide as before (the vignette darkens the edges). */
        if (g_pc_fp_mode && !pc_fp_view_is_active() && !follow && mEv_CheckTitleDemo() == 0) {
            const float dx = cx - s_vr.shot_pos[0], dy = cy - s_vr.shot_pos[1], dz = cz - s_vr.shot_pos[2];
            const float moved_m = sqrtf(dx * dx + dy * dy + dz * dz) * s_vr.world_scale;
            const float turned = fx * s_vr.shot_fx + fz * s_vr.shot_fz;  /* cos of the yaw between */
            if (!s_vr.shot_held || moved_m > 1.5f || turned < 0.766f /* 40 degrees */) {
                /* A step is a cut for the vignette (no edge pulse), as a snap turn is. */
                if (s_vr.shot_held) s_vr.have_last_cam = 0;
                s_vr.shot_held = 1;
                s_vr.shot_pos[0] = cx; s_vr.shot_pos[1] = cy; s_vr.shot_pos[2] = cz;
                s_vr.shot_fx = fx; s_vr.shot_fz = fz;
            }
            cx = s_vr.shot_pos[0]; cy = s_vr.shot_pos[1]; cz = s_vr.shot_pos[2];
            fx = s_vr.shot_fx; fz = s_vr.shot_fz;
        } else {
            s_vr.shot_held = 0;
        }
        s_vr.anchor_pos[0] = cx; s_vr.anchor_pos[1] = cy; s_vr.anchor_pos[2] = cz;
        /* Leveled view basis: back = (-fx, 0, -fz), up = +Y,
         * right = up x back = (bz, 0, -bx) */
        float bx = -fx, bz = -fz;
        float rx = bz, rz = -bx;
        A[0][0] = rx;  A[0][1] = 0.0f; A[0][2] = rz;
        A[1][0] = 0.0f; A[1][1] = 1.0f; A[1][2] = 0.0f;
        A[2][0] = bx;  A[2][1] = 0.0f; A[2][2] = bz;
        A[0][3] = -(A[0][0] * cx + A[0][1] * cy + A[0][2] * cz);
        A[1][3] = -(A[1][0] * cx + A[1][1] * cy + A[1][2] * cz);
        A[2][3] = -(A[2][0] * cx + A[2][1] * cy + A[2][2] * cz);
        memcpy(s_vr.anchor_yaw, A, sizeof(M34));
        s_vr.have_anchor = 1;
    } else if (s_vr.have_anchor) {
        memcpy(A, s_vr.anchor_yaw, sizeof(M34));
    } else {
        m34_identity(A);
    }

    /* W = T_height * Scale * A : world units -> seated meters */
    M34 W;
    float s = s_vr.world_scale;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            W[i][j] = A[i][j] * s;
    W[1][3] -= s_vr.height_offset;

    M34 Vinv;
    m34_invert_rigid(*V, Vinv);

    for (int eye = 0; eye < 2; eye++) {
        M34 t1, X;
        m34_mul(W, Vinv, t1);                       /* W * V^-1 */
        m34_mul(s_vr.inv_eye_pose[eye], t1, X);     /* (H*E)^-1 * W * V^-1 */
        memcpy(s_vr.view_correction[eye], X, sizeof(M34));
    }

    /* Motion tools need seated-meters -> game-world (W^-1) */
    m34_invert_rs(W, s_vr.world_from_seated);
}

extern "C" void pc_vr_notify_game_view(const float* mtx34) {
    /* A batch may still be buffered from before this view change — flush it
     * so it renders with the correction that was current when it was drawn. */
    pc_gx_flush_if_begin_complete();
    pc_sky_set_view(mtx34);
    pc_mirror_set_view(mtx34);
    if (!s_vr.active) return;
    memcpy(s_vr.game_view, mtx34, sizeof(M34));
    pcvr_update_view_correction();
}

/* The seated frame: origin at the head, turned to where it faces, level. */
static void pcvr_recenter_from(const M34 head_local) {
    float fx = -head_local[0][2], fz = -head_local[2][2];
    float len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-4f) { fx = 0.0f; fz = -1.0f; len = 1.0f; }
    fx /= len; fz /= len;
    /* local_from_seated: columns right, up, back (right = up x back); origin at the head */
    M34 local_from_seated;
    float bx = -fx, bz = -fz;
    local_from_seated[0][0] = bz;  local_from_seated[0][1] = 0.0f; local_from_seated[0][2] = bx;
    local_from_seated[1][0] = 0.0f; local_from_seated[1][1] = 1.0f; local_from_seated[1][2] = 0.0f;
    local_from_seated[2][0] = -bx; local_from_seated[2][1] = 0.0f; local_from_seated[2][2] = bz;
    local_from_seated[0][3] = head_local[0][3];
    local_from_seated[1][3] = head_local[1][3];
    local_from_seated[2][3] = head_local[2][3];
    m34_invert_rigid(local_from_seated, s_vr.seated_from_local);
    s_vr.have_recenter = 1;
    pcvr_log("recentered: head at (%.2f, %.2f, %.2f) m, facing (%.2f, %.2f)", head_local[0][3],
             head_local[1][3], head_local[2][3], fx, fz);
}

/* ================================================================ */
/* OpenXR backend */

#if defined(PC_VR_OPENXR)

enum { PCVR_SC_LEFT = 0, PCVR_SC_RIGHT = 1, PCVR_SC_COUNT = 2 };
/* Swapchains in use: one (both eyes side by side) in single-pass stereo, else one per eye. */
static int xr_sc_count(void) { return s_vr.single ? 1 : 2; }
#define PCVR_MAX_IMAGES 4

static struct {
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local, view;
    XrSessionState state;
    int running, focused, exit_requested;
    XrSwapchain swapchain[PCVR_SC_COUNT];
    uint32_t image_count[PCVR_SC_COUNT];
    GLuint image_gl[PCVR_SC_COUNT][PCVR_MAX_IMAGES];
    uint32_t acquired[PCVR_SC_COUNT];
    int have_acquired[PCVR_SC_COUNT];
    int64_t swapchain_format;
    void* release_event;           /* our MTLSharedEvent (the shim signals it) */
    uint64_t release_value;
    XrFrameState frame_state;
    XrView views[2];
    int views_valid;
    GLuint scratch[2];             /* eyes rendered when the runtime says not to show them */

    XrActionSet action_set;
    XrAction move, camera, a, b, x, y, menu, trig_l, trig_r, squeeze_l, squeeze_r, click_l, click_r;
    XrAction grip_pose, haptic;
    XrPath hand_path[2];
    XrSpace grip_space[2];
    int held;                      /* the game is held (Crown pause, unfocused) */
} s_xr;

static int xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return 1;
    pcvr_log("%s failed: %d (%s)", what, (int)r, xr_visionos_last_error());
    return 0;
}

static XrPath xr_path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(s_xr.instance, s, &p);
    return p;
}

static XrAction xr_make_action(const char* name, const char* local, XrActionType type, int per_hand) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", local);
    if (per_hand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = s_xr.hand_path;
    }
    XrAction a = XR_NULL_HANDLE;
    xr_ok(xrCreateAction(s_xr.action_set, &info, &a), name);
    return a;
}

/* The Touch profile, mapped as the mod's SteamVR bindings were: the provider
 * presents PS VR2 Sense controllers and bare hands (pinches, clutches) as it. */
static int xr_create_actions(void) {
    XrActionSetCreateInfo set = {XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(set.actionSetName, sizeof(set.actionSetName), "gameplay");
    snprintf(set.localizedActionSetName, sizeof(set.localizedActionSetName), "Gameplay");
    if (!xr_ok(xrCreateActionSet(s_xr.instance, &set, &s_xr.action_set), "xrCreateActionSet")) return 0;
    s_xr.hand_path[0] = xr_path("/user/hand/left");
    s_xr.hand_path[1] = xr_path("/user/hand/right");

    s_xr.move = xr_make_action("move", "Walk", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    s_xr.camera = xr_make_action("camera", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    s_xr.a = xr_make_action("a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.b = xr_make_action("b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.x = xr_make_action("x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.y = xr_make_action("y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.menu = xr_make_action("menu", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.trig_l = xr_make_action("trigger_left", "Left trigger", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.trig_r = xr_make_action("trigger_right", "Right trigger", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.squeeze_l = xr_make_action("squeeze_left", "Left grip", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.squeeze_r = xr_make_action("squeeze_right", "Right grip", XR_ACTION_TYPE_FLOAT_INPUT, 0);
    s_xr.click_l = xr_make_action("stick_click_left", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.click_r = xr_make_action("stick_click_right", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    s_xr.grip_pose = xr_make_action("grip_pose", "Hands", XR_ACTION_TYPE_POSE_INPUT, 1);
    s_xr.haptic = xr_make_action("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);

    struct { XrAction action; const char* path; } binds[] = {
        {s_xr.move, "/user/hand/left/input/thumbstick"},
        {s_xr.camera, "/user/hand/right/input/thumbstick"},
        {s_xr.a, "/user/hand/right/input/a/click"},
        {s_xr.b, "/user/hand/right/input/b/click"},
        {s_xr.x, "/user/hand/left/input/x/click"},
        {s_xr.y, "/user/hand/left/input/y/click"},
        {s_xr.menu, "/user/hand/left/input/menu/click"},
        {s_xr.trig_l, "/user/hand/left/input/trigger/value"},
        {s_xr.trig_r, "/user/hand/right/input/trigger/value"},
        {s_xr.squeeze_l, "/user/hand/left/input/squeeze/value"},
        {s_xr.squeeze_r, "/user/hand/right/input/squeeze/value"},
        {s_xr.click_l, "/user/hand/left/input/thumbstick/click"},
        {s_xr.click_r, "/user/hand/right/input/thumbstick/click"},
        {s_xr.grip_pose, "/user/hand/left/input/grip/pose"},
        {s_xr.grip_pose, "/user/hand/right/input/grip/pose"},
        {s_xr.haptic, "/user/hand/left/output/haptic"},
        {s_xr.haptic, "/user/hand/right/output/haptic"},
    };
    XrActionSuggestedBinding sb[sizeof(binds) / sizeof(binds[0])];
    for (size_t i = 0; i < sizeof(binds) / sizeof(binds[0]); i++) {
        sb[i].action = binds[i].action;
        sb[i].binding = xr_path(binds[i].path);
    }
    XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = xr_path("/interaction_profiles/oculus/touch_controller");
    suggest.suggestedBindings = sb;
    suggest.countSuggestedBindings = (uint32_t)(sizeof(binds) / sizeof(binds[0]));
    if (!xr_ok(xrSuggestInteractionProfileBindings(s_xr.instance, &suggest), "suggest Touch bindings")) return 0;

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = s_xr.grip_pose;
        si.subactionPath = s_xr.hand_path[h];
        si.poseInActionSpace.orientation.w = 1.0f;
        xr_ok(xrCreateActionSpace(s_xr.session, &si, &s_xr.grip_space[h]), "xrCreateActionSpace(grip)");
    }
    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &s_xr.action_set;
    return xr_ok(xrAttachSessionActionSets(s_xr.session, &attach), "xrAttachSessionActionSets");
}

static int xr_bool(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(s_xr.session, &gi, &st))) return 0;
    return st.isActive && st.currentState;
}

static float xr_float(XrAction a) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(s_xr.session, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}

static void xr_vec2(XrAction a, float* x, float* y) {
    *x = *y = 0.0f;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(s_xr.session, &gi, &st)) || !st.isActive) return;
    *x = st.currentState.x;
    *y = st.currentState.y;
}

static void xr_pose_to_m34(const XrPosef& p, M34 out) {
    m34_from_pose(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x,
                  p.position.y, p.position.z, out);
}

static int xr_locate(XrSpace space, M34 local_out) {
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, s_xr.local, s_xr.frame_state.predictedDisplayTime, &loc))) return 0;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return 0;
    xr_pose_to_m34(loc.pose, local_out);
    return m34_finite(local_out);
}

static int xr_create_swapchains(int w, int h) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(s_xr.session, 0, &n, NULL);
    int64_t formats[16] = {0};
    if (n > 16) n = 16;
    xrEnumerateSwapchainFormats(s_xr.session, n, &n, formats);
    /* BGRA8 sRGB: the compositor reads our gamma-encoded bytes as sRGB, and we render
     * through a UNORM view (MTLPixelFormatBGRA8Unorm_sRGB = 81). */
    s_xr.swapchain_format = n ? formats[0] : 81;
    for (uint32_t i = 0; i < n; i++)
        if (formats[i] == 81) s_xr.swapchain_format = 81;
    for (int e = 0; e < xr_sc_count(); e++) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = s_xr.swapchain_format;
        ci.sampleCount = 1;
        ci.width = (uint32_t)(s_vr.single ? 2 * w : w);
        ci.height = (uint32_t)h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!xr_ok(xrCreateSwapchain(s_xr.session, &ci, &s_xr.swapchain[e]), "xrCreateSwapchain")) return 0;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(s_xr.swapchain[e], 0, &count, NULL);
        if (count > PCVR_MAX_IMAGES) count = PCVR_MAX_IMAGES;
        XrSwapchainImageMetalMKW images[PCVR_MAX_IMAGES];
        for (uint32_t i = 0; i < count; i++) {
            memset(&images[i], 0, sizeof(images[i]));
            images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_MKW;
        }
        if (!xr_ok(xrEnumerateSwapchainImages(s_xr.swapchain[e], count, &count,
                                              (XrSwapchainImageBaseHeader*)images),
                   "xrEnumerateSwapchainImages"))
            return 0;
        s_xr.image_count[e] = count;
        for (uint32_t i = 0; i < count; i++) {
            s_xr.image_gl[e][i] = pcm_wrap_texture(images[i].texture, 1);
            if (!s_xr.image_gl[e][i]) return 0;
        }
    }
    if (s_vr.single)
        pcvr_log("swapchain: %dx%d (both eyes side by side, single-pass stereo), format %lld, %u images", 2 * w, h,
                 (long long)s_xr.swapchain_format, s_xr.image_count[0]);
    else
        pcvr_log("swapchains: 2 x %dx%d, format %lld, %u images", w, h, (long long)s_xr.swapchain_format,
                 s_xr.image_count[0]);
    return 1;
}

static void xr_destroy(void) {
    for (int e = 0; e < PCVR_SC_COUNT; e++) {
        for (uint32_t i = 0; i < s_xr.image_count[e]; i++)
            if (s_xr.image_gl[e][i]) glDeleteTextures(1, &s_xr.image_gl[e][i]);
        if (s_xr.swapchain[e]) xrDestroySwapchain(s_xr.swapchain[e]);
    }
    if (s_xr.session) xrDestroySession(s_xr.session);
    if (s_xr.instance) xrDestroyInstance(s_xr.instance);
    memset(&s_xr, 0, sizeof(s_xr));
}

/* Creates the instance, session, spaces, swapchains and actions. Fills the eye size. */
static int xr_init(int* eye_w, int* eye_h) {
    memset(&s_xr, 0, sizeof(s_xr));
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "Animal Crossing VR");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "ACGC PC port");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!xr_ok(xrCreateInstance(&ici, &s_xr.instance), "xrCreateInstance")) return 0;

    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_ok(xrGetSystem(s_xr.instance, &sgi, &s_xr.system), "xrGetSystem")) return 0;

    uint32_t nv = 0;
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    xrEnumerateViewConfigurationViews(s_xr.instance, s_xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv,
                                      vcv);
    int w = (int)vcv[0].recommendedImageRectWidth, h = (int)vcv[0].recommendedImageRectHeight;
    if (w < 256 || w > 8192 || h < 256 || h > 8192) {
        w = 1888;
        h = 1792;
    }
    /* The Simulator reports its window (~3840x2160): halve what it costs there. */
    if (w > 2400) {
        w = (w / 2) & ~7;
        h = (h / 2) & ~7;
    }
    /* Render scale (the VR settings, next Play), capped to the runtime's largest image and, with
     * both eyes side by side, to half of Metal's 8192. */
    {
        const int scale = g_pc_settings.vr_render_scale;
        if (scale >= 50 && scale <= 150 && scale != 100) {
            w = (w * scale / 100) & ~7;
            h = (h * scale / 100) & ~7;
        }
        int max_w = vcv[0].maxImageRectWidth ? (int)vcv[0].maxImageRectWidth : 8192;
        int max_h = vcv[0].maxImageRectHeight ? (int)vcv[0].maxImageRectHeight : 8192;
        if (s_vr.single && max_w > 4096) max_w = 4096;
        if (w > max_w) w = max_w & ~7;
        if (h > max_h) h = max_h & ~7;
        pcvr_log("eye size %dx%d (render scale %d%%)", w, h, scale);
    }

    XrGraphicsBindingMetalMKW binding = {XR_TYPE_GRAPHICS_BINDING_METAL_MKW};
    binding.device = pcm_device();
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s_xr.system;
    if (!xr_ok(xrCreateSession(s_xr.instance, &sci, &s_xr.session), "xrCreateSession")) return 0;

    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!xr_ok(xrCreateReferenceSpace(s_xr.session, &rs, &s_xr.local), "LOCAL space")) return 0;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xr_ok(xrCreateReferenceSpace(s_xr.session, &rs, &s_xr.view), "VIEW space")) return 0;

    if (!xr_create_swapchains(w, h)) return 0;
    if (!xr_create_actions()) return 0;
    s_xr.release_event = pcm_new_shared_event();
    *eye_w = w;
    *eye_h = h;
    s_xr.state = XR_SESSION_STATE_UNKNOWN;
    return 1;
}

static void xr_set_held(int held) {
    if (held == s_xr.held) return;
    s_xr.held = held;
    /* The host pauses the sound for any of its reasons (held, the window hidden, an
     * interruption), so one ending can't unpause over another. */
    acvr_visionos_set_vr_held(held);
    pcvr_log(held ? "held (session %d)" : "resumed (session %d)", (int)s_xr.state);
}

static void xr_poll_events(void) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s_xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
            s_xr.state = e->state;
            switch (e->state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (xr_ok(xrBeginSession(s_xr.session, &bi), "xrBeginSession")) s_xr.running = 1;
                    pcvr_log("session running");
                    break;
                }
                case XR_SESSION_STATE_FOCUSED:
                    s_xr.focused = 1;
                    acvr_visionos_mark_session_focused();
                    break;
                case XR_SESSION_STATE_VISIBLE:
                case XR_SESSION_STATE_SYNCHRONIZED:
                    s_xr.focused = 0;
                    break;
                case XR_SESSION_STATE_STOPPING:
                    s_xr.focused = 0;
                    xrEndSession(s_xr.session);
                    s_xr.running = 0;
                    pcvr_log("session stopped (the space closed): holding for a new one");
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    s_xr.focused = 0;
                    s_xr.running = 0;
                    s_xr.exit_requested = 1;
                    break;
                default:
                    break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s_xr.exit_requested = 1;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

static int pcvr_quit_requested(void) {
    return !g_pc_running || acvr_visionos_quit_requested();
}

static void xr_release_images(void);

/* Waits for and begins a runtime frame; acquires both eye images. Returns 0 when
 * the game should go on without one (quitting). Holds while the session isn't
 * running: the Digital Crown closed the space and the game waits, clock, sound and
 * all, for the next one. */
static int xr_begin_frame(void) {
    xr_poll_events();
    int stopped = 0;
    while (!s_xr.running && !s_xr.exit_requested && !pcvr_quit_requested()) {
        if (!stopped) pcvr_log("holding: the space closed");
        stopped = 1;
        xr_set_held(1);
        usleep(50000);
        xr_poll_events();
    }
    /* Back from the Digital Crown: play resumes on the pause menu, so you settle in first. */
    if (stopped && s_xr.running && !g_pc_paused) pc_pause_menu_toggle();
    if (s_xr.exit_requested && g_pc_running) {
        pcvr_log("the runtime is exiting: quitting the game");
        g_pc_running = 0;
    }
    if (!s_xr.running) return 0;

    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    s_xr.frame_state.type = XR_TYPE_FRAME_STATE;
    s_xr.frame_state.next = NULL;
    if (!xr_ok(xrWaitFrame(s_xr.session, &wi, &s_xr.frame_state), "xrWaitFrame")) return 0;
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    XrResult br = xrBeginFrame(s_xr.session, &bi);
    if (XR_FAILED(br)) {
        xr_ok(br, "xrBeginFrame");
        return 0;
    }
    /* Unfocused (headset off, visionOS over the game): frames go on, play holds; back after
     * more than a moment, play resumes on the pause menu. */
    {
        static int ever_focused = 0;
        static Uint32 unfocused_since = 0;
        if (!s_xr.focused && ever_focused && !unfocused_since) unfocused_since = SDL_GetTicks();
        if (s_xr.focused) {
            if (ever_focused && unfocused_since && SDL_GetTicks() - unfocused_since > 1500 && !g_pc_paused)
                pc_pause_menu_toggle();
            ever_focused = 1;
            unfocused_since = 0;
        }
    }
    g_pc_external_hold = !s_xr.focused;
    xr_set_held(!s_xr.focused);

    /* Views */
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s_xr.frame_state.predictedDisplayTime;
    li.space = s_xr.local;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t nv = 0;
    s_xr.views[0].type = s_xr.views[1].type = XR_TYPE_VIEW;
    s_xr.views[0].next = s_xr.views[1].next = NULL;
    s_xr.views_valid = XR_SUCCEEDED(xrLocateViews(s_xr.session, &li, &vs, 2, &nv, s_xr.views)) && nv == 2 &&
                       (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                       (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

    /* Eye images: wait for the compositor's last read before writing. */
    for (int e = 0; e < PCVR_SC_COUNT; e++) s_xr.have_acquired[e] = 0;
    for (int e = 0; e < xr_sc_count(); e++) {
        s_xr.have_acquired[e] = 0;
        if (!s_xr.frame_state.shouldRender) continue;
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        if (!xr_ok(xrAcquireSwapchainImage(s_xr.swapchain[e], &ai, &index), "xrAcquireSwapchainImage")) continue;
        XrSwapchainImageWaitInfo wti = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wti.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(s_xr.swapchain[e], &wti);
        void* event = NULL;
        uint64_t value = 0;
        xr_visionos_swapchain_image_acquire_fence(s_xr.swapchain[e], index, &event, &value);
        if (event) pcm_encode_wait_event(event, value);
        s_xr.acquired[e] = index;
        s_xr.have_acquired[e] = 1;
    }
    for (int e = 0; e < xr_sc_count(); e++) {
        PCVRTarget* t = s_vr.single ? &s_vr.both : &s_vr.eye[e];
        GLuint color;
        if (s_xr.have_acquired[e]) {
            color = s_xr.image_gl[e][s_xr.acquired[e]];
        } else {
            if (!s_xr.scratch[e]) {
                glGenTextures(1, &s_xr.scratch[e]);
                glBindTexture(GL_TEXTURE_2D, s_xr.scratch[e]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            color = s_xr.scratch[e];
        }
        pcvr_attach_eye_color(t, color);
        if (s_vr.single) s_vr.eye[0].color = s_vr.eye[1].color = color;
    }
    return 1;
}

/* Tracking and controllers for this frame (seated frame), after xr_begin_frame. */
static void xr_update_tracking(void) {
    M34 head_local;
    s_vr.head_pose_valid = s_xr.views_valid && xr_locate(s_xr.view, head_local);
    if (s_vr.head_pose_valid) {
        if (!s_vr.have_recenter || s_vr.recenter_requested) {
            pcvr_recenter_from(head_local);
            s_vr.recenter_requested = 0;
        }
        m34_mul(s_vr.seated_from_local, head_local, s_vr.head_pose);
        for (int e = 0; e < 2; e++) {
            M34 eye_local;
            xr_pose_to_m34(s_xr.views[e].pose, eye_local);
            m34_mul(s_vr.seated_from_local, eye_local, s_vr.eye_pose[e]);
            s_vr.eye_fov[e][0] = tanf(s_xr.views[e].fov.angleLeft);
            s_vr.eye_fov[e][1] = tanf(s_xr.views[e].fov.angleRight);
            s_vr.eye_fov[e][2] = tanf(s_xr.views[e].fov.angleUp);
            s_vr.eye_fov[e][3] = tanf(s_xr.views[e].fov.angleDown);
            pcvr_update_eye_projection(e);
        }
        s_vr.have_pose = 1;
    }

    /* Controllers: only while focused. */
    memset(&s_vr.buttons, 0, sizeof(s_vr.buttons));
    s_vr.input_ready = 0;
    if (s_xr.focused) {
        XrActiveActionSet active = {s_xr.action_set, XR_NULL_PATH};
        XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(s_xr.session, &si))) {
            PCVRButtons* b = &s_vr.buttons;
            xr_vec2(s_xr.move, &b->mx, &b->my);
            xr_vec2(s_xr.camera, &b->cx, &b->cy);
            b->a = xr_bool(s_xr.a) || xr_float(s_xr.trig_r) > 0.6f;
            b->b = xr_bool(s_xr.b) || xr_float(s_xr.trig_l) > 0.6f;
            b->x = xr_bool(s_xr.x);
            b->y = xr_bool(s_xr.y);
            b->l = xr_float(s_xr.squeeze_l) > 0.6f;
            b->r = xr_float(s_xr.squeeze_r) > 0.6f;
            b->z = xr_bool(s_xr.click_r);
            b->start = xr_bool(s_xr.click_l) || xr_bool(s_xr.menu);
            b->valid = 1;
            s_vr.input_ready = 1;
        }
    }
}

/* ACVR_TEST_HAND=hold|swing: a scripted hand for headless runs (the Simulator has no hands):
 * each hand held out in front, below the head and to its side, level with the head's yaw;
 * "swing" brings the right one down through a 100 degree arc every 2.5 s, fast enough
 * for the swing gesture, so a tool in hand follows it and swings. */
static double xr_display_seconds(void);
static int test_hand_pose(int hand, M34 seated_out) {
    static int mode = -1;
    if (mode < 0) {
        const char* v = getenv("ACVR_TEST_HAND");
        mode = (v == NULL || *v == 0) ? 0 : (strcmp(v, "swing") == 0 ? 2 : 1);
    }
    if (mode == 0 || !s_vr.head_pose_valid) return 0;
    float fx = -s_vr.head_pose[0][2], fz = -s_vr.head_pose[2][2];
    const float fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-4f) return 0;
    fx /= fl;
    fz /= fl;
    const float rx = -fz, rz = fx; /* right = forward x up */
    float a = 0.35f;               /* raised a little */
    if (mode == 2 && hand == (g_pc_settings.vr_tool_hand ? 0 : 1)) {  /* the tool hand swings */
        const double t = fmod(xr_display_seconds(), 2.5);
        if (t < 0.6) a = 0.35f + (float)(t / 0.6) * 0.70f;          /* wind up to ~60 deg */
        else if (t < 0.8) a = 1.05f - (float)((t - 0.6) / 0.2) * 1.75f; /* down to ~-40 deg */
        else if (t < 1.6) a = -0.70f + (float)((t - 0.8) / 0.8) * 1.05f;
    }
    const float ca = cosf(a), sa = sinf(a);
    const float side = hand == 1 ? 0.22f : -0.22f;
    /* pointing direction (yaw frame): (0, sin a, -cos a); orientation = yaw * Rx(a) */
    const float dir[3] = {fx * ca, sa, fz * ca};
    const float up[3] = {-fx * sa, ca, -fz * sa};
    for (int r = 0; r < 3; r++) {
        const float right_r = r == 0 ? rx : (r == 1 ? 0.0f : rz);
        seated_out[r][0] = right_r;
        seated_out[r][1] = up[r];
        seated_out[r][2] = -dir[r];
        const float shoulder = right_r * side + (r == 1 ? -0.35f : 0.0f);
        seated_out[r][3] = s_vr.head_pose[r][3] + shoulder + 0.40f * dir[r];
    }
    return 1;
}

static int xr_hand_pose(int hand, M34 seated_out) {
    if (test_hand_pose(hand, seated_out)) return 1;
    if (!s_vr.input_ready || !s_vr.have_recenter) return 0;
    M34 local;
    if (!xr_locate(s_xr.grip_space[hand], local)) return 0;
    m34_mul(s_vr.seated_from_local, local, seated_out);
    return 1;
}

static double xr_display_seconds(void) {
    return (double)s_xr.frame_state.predictedDisplayTime * 1e-9;
}

static void xr_release_images(void) {
    /* Our writes into both images complete at release_value; the compositor waits for it. */
    s_xr.release_value++;
    pcm_encode_signal_event(s_xr.release_event, s_xr.release_value);
    pcm_commit();
    for (int e = 0; e < xr_sc_count(); e++) {
        if (!s_xr.have_acquired[e]) continue;
        xr_visionos_swapchain_image_set_release_fence(s_xr.swapchain[e], s_xr.acquired[e], s_xr.release_event,
                                                      s_xr.release_value);
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s_xr.swapchain[e], &ri);
    }
}

/* Ends the runtime frame with the eyes as a projection layer. see_through: the
 * frame blends over the room (premultiplied alpha). */
static void xr_end_frame(int see_through, float opacity) {
    xr_release_images();
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1];
    uint32_t layer_count = 0;
    if (s_xr.frame_state.shouldRender && s_xr.views_valid && s_xr.have_acquired[0] &&
        (s_vr.single || s_xr.have_acquired[1])) {
        for (int e = 0; e < 2; e++) {
            memset(&pv[e], 0, sizeof(pv[e]));
            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[e].pose = s_xr.views[e].pose;
            pv[e].fov = s_xr.views[e].fov;
            pv[e].subImage.swapchain = s_xr.swapchain[s_vr.single ? 0 : e];
            pv[e].subImage.imageRect.offset.x = s_vr.eye[e].x;
            pv[e].subImage.imageRect.extent.width = s_vr.eye[e].w;
            /* ACVR_TEST_RIGHT_EYE=1: the left view shows the right eye's image, so a one-view
             * Simulator screenshot checks the right half of the side-by-side swapchain. */
            static const int show_right = getenv("ACVR_TEST_RIGHT_EYE") != NULL;
            if (show_right && e == 0) {
                pv[0].subImage.swapchain = s_xr.swapchain[s_vr.single ? 0 : 1];
                pv[0].subImage.imageRect.offset.x = s_vr.eye[1].x;
            }
            pv[e].subImage.imageRect.extent.height = s_vr.eye[e].h;
        }
        layer.space = s_xr.local;
        layer.viewCount = 2;
        layer.views = pv;
        layer.layerFlags = see_through ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
        layer_count = 1;
    }
    xr_visionos_set_frame_environment(s_xr.session, see_through ? true : false);
    xr_visionos_set_frame_opacity(s_xr.session, opacity);
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = s_xr.frame_state.predictedDisplayTime;
    ei.environmentBlendMode = see_through ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer_count;
    ei.layers = layers;
    XrResult r = xrEndFrame(s_xr.session, &ei);
    if (XR_FAILED(r)) xr_ok(r, "xrEndFrame");
    for (int e = 0; e < PCVR_SC_COUNT; e++) s_xr.have_acquired[e] = 0;
}

static void xr_rumble(void) {
    for (int h = 0; h < 2; h++) {
        XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = s_xr.haptic;
        hi.subactionPath = s_xr.hand_path[h];
        XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = 0.8f;
        v.duration = 150000000;  /* 150 ms */
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(s_xr.session, &hi, (const XrHapticBaseHeader*)&v);
    }
}

#endif /* PC_VR_OPENXR */

/* ================================================================ */
/* Fake backend (macOS bench): a still head, symmetric eyes 6.4 cm apart. */

static void fake_update_tracking(void) {
    float yaw = 0.0f, pitch = 0.0f;
    if (const char* y = getenv("ACVR_FAKE_VR_YAW")) yaw = (float)atof(y) * (float)(PC_PI / 180.0);
    if (const char* p = getenv("ACVR_FAKE_VR_PITCH")) pitch = (float)atof(p) * (float)(PC_PI / 180.0);
    M34 head, yawm, pitchm;
    m34_identity(yawm);
    yawm[0][0] = cosf(yaw); yawm[0][2] = sinf(yaw);
    yawm[2][0] = -sinf(yaw); yawm[2][2] = cosf(yaw);
    m34_identity(pitchm);
    pitchm[1][1] = cosf(pitch); pitchm[1][2] = -sinf(pitch);
    pitchm[2][1] = sinf(pitch); pitchm[2][2] = cosf(pitch);
    m34_mul(yawm, pitchm, head);
    memcpy(s_vr.head_pose, head, sizeof(M34));
    for (int e = 0; e < 2; e++) {
        M34 eye_in_head;
        m34_identity(eye_in_head);
        eye_in_head[0][3] = e == 0 ? -0.032f : 0.032f;
        m34_mul(head, eye_in_head, s_vr.eye_pose[e]);
        s_vr.eye_fov[e][0] = -1.0f;
        s_vr.eye_fov[e][1] = 1.0f;
        s_vr.eye_fov[e][2] = 1.0f;
        s_vr.eye_fov[e][3] = -1.0f;
        pcvr_update_eye_projection(e);
    }
    s_vr.head_pose_valid = 1;
    s_vr.have_pose = 1;
    s_vr.have_recenter = 1;
    memset(&s_vr.buttons, 0, sizeof(s_vr.buttons));
    s_vr.input_ready = 1;
    s_vr.buttons.valid = 1;
}

/* ---------------------------------------------------------------- */
/* Lifecycle */

extern "C" void pc_vr_init(void) {
    memset(&s_vr, 0, sizeof(s_vr));
    s_vr.aa_applied = -1;
    s_vr.requested = g_pc_settings.vr_mode;
    s_vr.world_scale = g_pc_settings.vr_world_scale / 1000.0f;   /* stored as mm per unit */
    s_vr.ui_distance = g_pc_settings.vr_ui_distance / 100.0f;    /* stored as cm */
    s_vr.ui_size = g_pc_settings.vr_ui_size / 100.0f;            /* stored as cm */
    s_vr.height_offset = g_pc_settings.vr_height_offset / 100.0f;
    if (s_vr.world_scale <= 0.0001f) s_vr.world_scale = 0.01f;
    if (s_vr.ui_distance < 0.5f) s_vr.ui_distance = 2.0f;
    if (s_vr.ui_size < 0.5f) s_vr.ui_size = 2.4f;
    m34_identity(s_vr.game_view);
    m34_identity(s_vr.seated_from_local);
    for (int e = 0; e < 2; e++) {
        s_vr.eye_fov[e][0] = -1.0f; s_vr.eye_fov[e][1] = 1.0f;
        s_vr.eye_fov[e][2] = 1.0f;  s_vr.eye_fov[e][3] = -1.0f;
    }

    const int fake = getenv("ACVR_FAKE_VR") != NULL;
    if (s_vr.requested == 0 && !fake) return;
    {
        /* ACVR_STEREO_PASSES=2: the mod's way, the display list once per eye (the escape hatch). */
        const char* passes = getenv("ACVR_STEREO_PASSES");
#if defined(PC_METAL)
        s_vr.single = !(passes && atoi(passes) == 2);
#if defined(PC_VISIONOS)
        /* The progressive space's layered drawable: the provider's layered pass shows nothing for
         * one side-by-side image (Simulator, 2026-10-08), so a pass per eye there for now. */
        extern int acvr_visionos_layer_layered(void);
        if (s_vr.single && !fake && acvr_visionos_layer_layered()) {
            s_vr.single = 0;
            pcvr_log("progressive (layered) space: one pass per eye");
        }
#endif
#else
        (void)passes;
        s_vr.single = 0;  /* single-pass stereo is the Metal shim's */
#endif
    }

    int eye_w = 0, eye_h = 0;
#if defined(PC_VR_OPENXR)
    if (!fake) {
        pcvr_log("init: vr_mode=%d scale=%.4f (OpenXR on the visionOS provider)", s_vr.requested, s_vr.world_scale);
        if (!xr_init(&eye_w, &eye_h)) {
            pcvr_log("OpenXR unavailable - running flat");
            xr_destroy();
            return;
        }
        s_vr.backend = PCVR_BACKEND_OPENXR;
    }
#endif
    if (!s_vr.backend) {
        if (!fake) {
            pcvr_log("no VR runtime in this build - running flat (ACVR_FAKE_VR=1 for the bench)");
            return;
        }
        eye_w = eye_h = 960;
        if (const char* sz = getenv("ACVR_FAKE_VR_SIZE")) eye_w = eye_h = atoi(sz) > 64 ? atoi(sz) : 960;
        s_vr.backend = PCVR_BACKEND_FAKE;
        pcvr_log("init: fake headset, %dx%d eyes", eye_w, eye_h);
    }

    int ok;
    if (s_vr.single) {
        ok = s_vr.backend == PCVR_BACKEND_FAKE ? pcvr_create_target(&s_vr.both, 2 * eye_w, eye_h)
                                               : pcvr_create_eye_target(&s_vr.both, 2 * eye_w, eye_h);
        for (int e = 0; e < 2; e++) {
            s_vr.eye[e] = s_vr.both;
            s_vr.eye[e].w = eye_w;
            s_vr.eye[e].x = e * eye_w;
        }
        pcvr_log("single-pass stereo: one %dx%d target (ACVR_STEREO_PASSES=2 for one pass per eye)", 2 * eye_w,
                 eye_h);
    } else if (s_vr.backend == PCVR_BACKEND_FAKE) {
        ok = pcvr_create_target(&s_vr.eye[0], eye_w, eye_h) && pcvr_create_target(&s_vr.eye[1], eye_w, eye_h);
    } else {
        ok = pcvr_create_eye_target(&s_vr.eye[0], eye_w, eye_h) && pcvr_create_eye_target(&s_vr.eye[1], eye_w, eye_h);
    }
    ok = ok && pcvr_create_target(&s_vr.ui, 1280, 960) && pcvr_create_panel_gl();
    if (!ok) {
        pcvr_log("GL resource creation failed - running flat");
        pcvr_destroy_eyes();
        pcvr_destroy_target(&s_vr.ui, 1);
#if defined(PC_VR_OPENXR)
        if (s_vr.backend == PCVR_BACKEND_OPENXR) xr_destroy();
#endif
        s_vr.backend = PCVR_BACKEND_NONE;
        return;
    }

    for (int e = 0; e < 2; e++) {
        m34_identity(s_vr.eye_pose[e]);
        m34_identity(s_vr.inv_eye_pose[e]);
        pcvr_update_eye_projection(e);
        m34_identity(s_vr.view_correction[e]);
    }
    m34_identity(s_vr.head_pose);

    /* VR paces through the runtime's frame wait: the 60fps limiter would fight it. */
    g_frame_limiter = 0;

    g_pc_vr_cull_expand = 24.0f;
    g_pc_vr_cull_znear_slack = 8.0f;
    pc_full_world_enable();

    /* Controller grip -> tool orientation: tools extend along the hand matrix's
     * +Z, the grip pose points along -Z (a sword in the fist), so flip about Y,
     * then apply the user's pitch trim about X. */
    {
        float p = -(float)g_pc_settings.vr_tool_pitch * (float)(PC_PI / 180.0);
        float cp2 = cosf(p), sp2 = sinf(p);
        M34 flip, pitch;
        m34_identity(flip);
        flip[0][0] = -1.0f;
        flip[2][2] = -1.0f;
        m34_identity(pitch);
        pitch[1][1] = cp2;  pitch[1][2] = -sp2;
        pitch[2][1] = sp2;  pitch[2][2] = cp2;
        m34_mul(flip, pitch, s_vr.tool_local);
    }
    m34_identity(s_vr.world_from_seated);
    s_vr.stats_ticks = SDL_GetTicks();
    s_vr.active_since_ticks = s_vr.stats_ticks;
    s_vr.flat_scene_stamp = (u32)-1000;
    s_vr.room_behind_menus = acvr_visionos_room_behind_menus_enabled() ? 1 : 0;
    if (getenv("ACVR_FAKE_VR_ROOM")) s_vr.room_behind_menus = 1;
    s_vr.vignette_enabled = acvr_visionos_comfort_vignette();
    {
        /* The launcher's turning (smooth or a snap angle) for first person. */
        int snap = acvr_visionos_snap_turn_degrees();
        if (snap >= 0 && snap <= 90) g_pc_settings.fp_snap_degrees = snap;
        /* ...and the game's own, kept in step with the launcher (see pcvr_sync_turning). */
        s_vr.snap_noted = g_pc_settings.fp_snap_degrees;
        acvr_visionos_note_snap_turn(s_vr.snap_noted);
    }

    s_vr.active = 1;
    s_vr.current_eye = -1;
    pcvr_log("ACTIVE: eye %dx%d, world scale %.4f m/unit, room behind menus %d", eye_w, eye_h, s_vr.world_scale,
             s_vr.room_behind_menus);

    /* Starting in first person: switch to the life-size scale now */
    if (g_pc_fp_mode) {
        pc_vr_set_fp_scale(1);
    }
}

extern "C" void pc_vr_shutdown(void) {
    if (s_vr.backend != PCVR_BACKEND_NONE) {
        pc_vr_hands_shutdown();
        pcvr_destroy_eyes();
        pcvr_destroy_target(&s_vr.ui, 1);
        if (s_vr.panel_prog) glDeleteProgram(s_vr.panel_prog);
        if (s_vr.panel_vao) glDeleteVertexArrays(1, &s_vr.panel_vao);
        if (s_vr.panel_vbo) glDeleteBuffers(1, &s_vr.panel_vbo);
#if defined(PC_VR_OPENXR)
        if (s_vr.backend == PCVR_BACKEND_OPENXR) {
            if (s_vr.frame_begun) xr_end_frame(0, 1.0f);
            if (s_xr.running) xrRequestExitSession(s_xr.session);
            xr_destroy();
        }
#endif
        s_vr.backend = PCVR_BACKEND_NONE;
    }
    s_vr.active = 0;
    s_vr.menu_input_available = 0;
    pc_vr_menu_reset(&s_vr.menu_input);
}

extern "C" int pc_vr_active(void) {
    return s_vr.active;
}

/* ---------------------------------------------------------------- */
/* Frame flow */

static void pcvr_poll_empty_hands(void) {
    for (int hand = 0; hand < 2; ++hand) {
        s_vr.empty_hand_valid[hand] = 0;
        if (!g_pc_settings.vr_empty_hands || !s_vr.head_pose_valid) continue;
#if defined(PC_VR_OPENXR)
        if (s_vr.backend == PCVR_BACKEND_OPENXR && xr_hand_pose(hand, s_vr.empty_hand_pose[hand])) {
            s_vr.empty_hand_valid[hand] = 1;
            s_vr.empty_hand_seen[hand] = 1;
        }
#endif
    }
}

/* Starts the runtime frame and samples tracking and controllers. Separate from
 * clearing the UI layer: a frame that bypasses the eye passes (NES) has already
 * drawn its UI when it gets here. */
static void pcvr_begin_runtime_frame(void) {
    if (s_vr.frame_begun) return;
#if defined(PC_VR_OPENXR)
    if (s_vr.backend == PCVR_BACKEND_OPENXR) {
        if (!xr_begin_frame()) return;
        xr_update_tracking();
        if (acvr_visionos_consume_world_recenter())
            s_vr.recenter_requested = 1;
    }
#endif
    if (s_vr.backend == PCVR_BACKEND_FAKE) fake_update_tracking();
    s_vr.frame_begun = 1;
    s_vr.submitted_this_frame = 0;
    s_vr.world_drawn = 0;
}

extern "C" void pc_vr_frame_begin(void) {
    if (!s_vr.active) return;

    pcvr_begin_runtime_frame();
    s_vr.menu_input_available = s_vr.input_ready && s_vr.head_pose_valid;
    if (!s_vr.menu_input_available) pc_vr_menu_reset(&s_vr.menu_input);

#if defined(PC_VR_OPENXR)
    /* Anti-aliasing (the VR settings, live): the compositor smooths the eye images. */
    if (s_vr.backend == PCVR_BACKEND_OPENXR && g_pc_settings.vr_anti_aliasing != s_vr.aa_applied) {
        s_vr.aa_applied = g_pc_settings.vr_anti_aliasing;
        xr_visionos_set_anti_aliasing(s_vr.aa_applied);
        static const char* names[] = {"off", "FXAA", "SMAA"};
        pcvr_log("anti-aliasing %s", names[s_vr.aa_applied % 3]);
    }
#endif

    /* The pause menu's Turning changed it: tell the launcher, so its picker and the next Play agree. */
    if (g_pc_settings.fp_snap_degrees != s_vr.snap_noted) {
        s_vr.snap_noted = g_pc_settings.fp_snap_degrees;
        acvr_visionos_note_snap_turn(s_vr.snap_noted);
    }

    /* X+Y chord: recenter the seated origin */
    {
        int chord = s_vr.buttons.x && s_vr.buttons.y;
        if (chord && !s_vr.recenter_latch) {
            s_vr.recenter_requested = 1;
            s_vr.recenter_latch = 1;
        } else if (!chord) {
            s_vr.recenter_latch = 0;
        }
    }

    /* (H*E)^-1 per eye, then refresh the correction with last frame's view
     * (the view notify updates it again as soon as the game loads one). */
    for (int e = 0; e < 2; e++) m34_invert_rigid(s_vr.eye_pose[e], s_vr.inv_eye_pose[e]);
    pcvr_update_view_correction();

    /* --- Motion tools: right-controller pose + swing gesture --- */
    s_vr.hand_valid = 0;
    float hand_speed = -1.0f;
#if defined(PC_VR_OPENXR)
    if (s_vr.backend == PCVR_BACKEND_OPENXR) {
        M34 hand_seated, t1;
        /* The tool hand (the VR settings' Tool hand: right by default, or left). A change of hand
         * starts the swing speed afresh, or the jump between hands would read as a swing. */
        static int s_tool_hand = -1;
        if (g_pc_settings.vr_tool_hand != s_tool_hand) {
            s_tool_hand = g_pc_settings.vr_tool_hand;
            s_vr.hand_prev_valid = 0;
            pcvr_log("tool hand: %s", s_tool_hand ? "left" : "right");
        }
        if (xr_hand_pose(s_tool_hand ? 0 : 1, hand_seated)) {
            m34_mul(s_vr.world_from_seated, hand_seated, t1); /* seated -> game world */
            m34_mul(t1, s_vr.tool_local, s_vr.hand_world);    /* grip orientation */
            /* W^-1 left a 1/world_scale in the basis; the matrix this
             * replaces (right_hand_mtx) carries the player's 0.01 actor
             * scale that tool models are authored for. Rebase the 3x3 to
             * 0.01 (translation stays in world units). */
            float k = 0.01f * s_vr.world_scale;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    s_vr.hand_world[r][c] *= k;
            s_vr.hand_valid = 1;
            s_vr.hand_ever_valid = 1;

            /* Speed by finite difference over display times (the provider
             * reports no velocities). */
            const double now = xr_display_seconds();
            if (s_vr.hand_prev_valid && now > s_vr.hand_prev_time + 1e-4 && now < s_vr.hand_prev_time + 0.1) {
                float dx = hand_seated[0][3] - s_vr.hand_prev[0];
                float dy = hand_seated[1][3] - s_vr.hand_prev[1];
                float dz = hand_seated[2][3] - s_vr.hand_prev[2];
                hand_speed = sqrtf(dx * dx + dy * dy + dz * dz) / (float)(now - s_vr.hand_prev_time);
            } else {
                hand_speed = 0.0f;
            }
            s_vr.hand_prev[0] = hand_seated[0][3];
            s_vr.hand_prev[1] = hand_seated[1][3];
            s_vr.hand_prev[2] = hand_seated[2][3];
            s_vr.hand_prev_time = now;
            s_vr.hand_prev_valid = 1;
        } else {
            s_vr.hand_prev_valid = 0;
        }
    }
#endif
    /* Always update, including tracking loss: a partial gesture or pending
     * virtual A must not survive a pause, submenu, or invalid pose. */
    pc_vr_swing_update(&s_vr.swing, SDL_GetTicks(), hand_speed,
        s_vr.hand_valid && hand_speed >= 0.0f && pc_vr_tool_input_allowed() &&
        g_pc_settings.vr_motion_swing && pc_fp_swingable_equipped() && !s_vr.buttons.l);
    if (s_vr.swing.pulse == 2) s_vr.stats_swings++;  /* a swing just fired the tool (virtual A) */

    pcvr_poll_empty_hands();

    /* Stats: one line every 10 s. */
    {
        Uint32 now_ticks = SDL_GetTicks();
        s_vr.stats_frames++;
        if (now_ticks - s_vr.stats_ticks >= 10000) {
            pcvr_log("stats: %.1f fps, %u see-through frames, head %s, tool hand (%s) %s, fp %d, %u tool swings, "
                     "eye %dx%d, %s",
                     s_vr.stats_frames * 1000.0f / (float)(now_ticks - s_vr.stats_ticks), s_vr.stats_see_through,
                     s_vr.head_pose_valid ? "tracked" : "lost", g_pc_settings.vr_tool_hand ? "left" : "right",
                     s_vr.hand_valid ? "tracked" : "none",
                     pc_fp_view_is_active(), s_vr.stats_swings, s_vr.eye[0].w, s_vr.eye[0].h,
                     s_vr.single ? "single-pass stereo" : "a pass per eye");
            s_vr.stats_frames = 0;
            s_vr.stats_see_through = 0;
            s_vr.stats_swings = 0;
            s_vr.stats_ticks = now_ticks;
        }
    }

    /* Clear the UI layer (transparent) */
    pc_gx_draw_pending();
    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.ui.fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    s_vr.frame_index++;
}

extern "C" void pc_vr_begin_eye(int eye) {
    if (!s_vr.active) return;
    pc_gx_draw_pending();
    pc_sky_begin_pass();
    s_vr.in_scene_pass = 1;
    s_vr.current_eye = eye;
    s_vr.ui_bound = 0;
    s_vr.eye_depth_range[eye][0] = 0.0f;
    s_vr.eye_depth_range[eye][1] = 1.0f;
    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
    g_pc_target_w = s_vr.eye[eye].w;
    g_pc_target_h = s_vr.eye[eye].h;
    glDisable(GL_SCISSOR_TEST);
    /* Single-pass: the viewport is in one eye's pixels; the shim puts each eye in its half. */
    glViewport(0, 0, s_vr.eye[eye].w, s_vr.eye[eye].h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    /* Use the game's current copy-clear color for the sky (both eyes at once in single-pass). */
    float rgba[4], depth;
    pc_gx_get_clear(rgba, &depth);
    glClearColor(rgba[0], rgba[1], rgba[2], 1.0f);
    glClearDepth(depth);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pcm_stereo_target(s_vr.single ? s_vr.both.fbo : 0, s_vr.eye[0].w);
    /* Per-eye matrices differ: every uniform group must re-upload to every
     * shader program this pass, and ortho routing starts on the eye FBO. */
    pc_gx_vr_reset_routing();
    pc_gx_mark_new_pass();
}

extern "C" int pc_vr_current_eye(void) {
    return s_vr.in_scene_pass ? s_vr.current_eye : -1;
}

extern "C" int pc_vr_in_scene_pass(void) {
    return s_vr.active && s_vr.in_scene_pass;
}

/* pc_gx: a perspective batch is going into an eye (the world is on screen). */
extern "C" void pc_vr_note_world_draw(void) {
    if (s_vr.in_scene_pass && !s_vr.ui_bound) s_vr.world_drawn = 1;
}

/* pc_gx: a full-panel black fill with this alpha went into the UI (a fade). */
extern "C" void pc_vr_note_ui_fade(float alpha) {
    if (alpha > s_vr.fade_black) s_vr.fade_black = alpha;
}

extern "C" const float* pc_vr_eye_projection(void) {
    return &s_vr.eye_projection[s_vr.current_eye == 1 ? 1 : 0][0][0];
}

extern "C" const float* pc_vr_view_correction(void) {
    return &s_vr.view_correction[s_vr.current_eye == 1 ? 1 : 0][0][0];
}

/* Single-pass stereo: this scene pass draws both eyes (pc_gx gives each draw the right
 * eye's matrices too, and draws the sky once per eye). */
extern "C" int pc_vr_single_pass(void) {
    return s_vr.active && s_vr.single && s_vr.in_scene_pass;
}

extern "C" int pc_vr_scene_passes(void) {
    return s_vr.single ? 1 : 2;
}

extern "C" const float* pc_vr_eye_projection_of(int eye) {
    return &s_vr.eye_projection[eye == 1 ? 1 : 0][0][0];
}

extern "C" const float* pc_vr_view_correction_of(int eye) {
    return &s_vr.view_correction[eye == 1 ? 1 : 0][0][0];
}

extern "C" void pc_vr_set_scene_depth_range(float near_depth, float far_depth) {
    if (!s_vr.active || !s_vr.in_scene_pass || s_vr.ui_bound ||
        s_vr.current_eye < 0 || s_vr.current_eye > 1 ||
        !isfinite(near_depth) || !isfinite(far_depth)) return;
    if (s_vr.eye_depth_range[s_vr.current_eye][0] == near_depth &&
        s_vr.eye_depth_range[s_vr.current_eye][1] == far_depth) return;
    /* Match glDepthRange's clamping, including emu64's 1022/1023 far value. */
    for (int e = 0; e < 2; e++) {
        if (!s_vr.single && e != s_vr.current_eye) continue;
        s_vr.eye_depth_range[e][0] = fmaxf(0.0f, fminf(1.0f, near_depth));
        s_vr.eye_depth_range[e][1] = fmaxf(0.0f, fminf(1.0f, far_depth));
    }
}

extern "C" unsigned int pc_vr_bind_ui_target(int ui) {
    if (!s_vr.active) return 0;
    if (ui) {
        s_vr.ui_bound = 1;
        glBindFramebuffer(GL_FRAMEBUFFER, s_vr.ui.fbo);
        g_pc_target_w = s_vr.ui.w;
        g_pc_target_h = s_vr.ui.h;
        return s_vr.ui.fbo;
    }
    s_vr.ui_bound = 0;
    if (s_vr.in_scene_pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[s_vr.current_eye].fbo);
        g_pc_target_w = s_vr.eye[s_vr.current_eye].w;
        g_pc_target_h = s_vr.eye[s_vr.current_eye].h;
        return s_vr.eye[s_vr.current_eye].fbo;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_pc_target_w = g_pc_window_w;
    g_pc_target_h = g_pc_window_h;
    return 0;
}

extern "C" int pc_vr_skip_ui_draws(void) {
    return s_vr.active && s_vr.in_scene_pass && s_vr.current_eye == 1;
}

extern "C" float pc_vr_world_scale(void) {
    return s_vr.world_scale;
}

extern "C" int pc_vr_tool_input_allowed(void) {
    return s_vr.active && s_vr.head_pose_valid && !g_pc_paused && !g_pc_external_hold &&
        pc_fp_view_is_active() && !pc_vr_flat_scene_active() && !pc_fp_in_talk();
}

/* Yaw of the headset gaze relative to the FP anchor, in binary-angle units.
 * Positive = turned left, matching the game's yaw sense. Lets movement (and
 * tool-target facing at the action boundary) follow where you're LOOKING, not just
 * where the stick-turned anchor points. Head turned left by d: RotY(+d),
 * so H[0][2]=sin d, H[2][2]=cos d -> atan2 recovers d. */
extern "C" float pc_vr_head_yaw_offset_bang(void) {
    if (!s_vr.active || !s_vr.have_pose) return 0.0f;
    return atan2f(s_vr.head_pose[0][2], s_vr.head_pose[2][2])
         * (65536.0f / (float)(2.0 * PC_PI));
}

extern "C" void pc_vr_set_empty_hands_available(int available) {
    s_vr.empty_hands_available = available != 0;
    s_vr.empty_hands_stamp = pc_frame_counter;
}

static int pcvr_empty_hands_visible(void) {
    return g_pc_settings.vr_empty_hands && pc_vr_tool_input_allowed() &&
        s_vr.empty_hands_available && s_vr.empty_hands_stamp == pc_frame_counter;
}

/* Flat-scene: frame-stamped like the FP active flag so scene teardown can
 * never leave it latched (m_play stamps it every play frame). */
extern "C" void pc_vr_set_flat_scene(int on) {
    s_vr.flat_scene_stamp = on ? pc_frame_counter : (u32)(pc_frame_counter - 1000u);
}

extern "C" int pc_vr_flat_scene_active(void) {
    /* The PC pause menu gates Game_play_move (no stamping) but drawing
     * continues — freeze the answer while paused, like the FP flag, or a
     * paused submenu re-renders its 3D items with VR matrices. */
    static int s_last;
    if (!g_pc_paused && !g_pc_external_hold) {
        s_last = (u32)(pc_frame_counter - s_vr.flat_scene_stamp) <= 1u;
    }
    return s_vr.active && s_last;
}

/* After both world passes, before the UI panel. The hands use the same eye
 * projection/depth buffers as the world; their grip poses are already in
 * seated meters, so neither world scale nor the tool alignment applies. */
static void pcvr_draw_empty_hands(void) {
    if (!pcvr_empty_hands_visible() || s_vr.empty_hands_render_failed) return;
    if (!s_vr.empty_hand_valid[0] && !s_vr.empty_hand_valid[1]) return;

    GLint draw_fbo, read_fbo, viewport[4];
    GLdouble depth_range[2];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetDoublev(GL_DEPTH_RANGE, depth_range);
    for (int eye = 0; eye < 2; ++eye) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_vr.eye[eye].fbo);
        glViewport(s_vr.eye[eye].x, 0, s_vr.eye[eye].w, s_vr.eye[eye].h);
        glDepthRange(s_vr.eye_depth_range[eye][0], s_vr.eye_depth_range[eye][1]);
        for (int hand = 0; hand < 2; ++hand) {
            if (!s_vr.empty_hand_valid[hand]) continue;
            M34 eye_from_grip;
            m34_mul(s_vr.inv_eye_pose[eye], s_vr.empty_hand_pose[hand], eye_from_grip);
            if (!m34_finite(eye_from_grip)) continue;
            if (!pc_vr_hands_draw(&eye_from_grip[0][0], &s_vr.eye_projection[eye][0][0], hand)) {
                s_vr.empty_hands_render_failed = 1;
                pcvr_log("optional empty-hand renderer unavailable; continuing without hands");
                break;
            }
        }
        if (s_vr.empty_hands_render_failed) break;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glDepthRange(depth_range[0], depth_range[1]);
}

extern "C" void pc_vr_end_scene_passes(void) {
    if (!s_vr.active) return;
    pc_gx_draw_pending();
    pcm_stereo_target(0, 0);  /* what follows (hands, panel, vignette) is drawn per eye */
    pcvr_draw_empty_hands();
    s_vr.in_scene_pass = 0;
    s_vr.current_eye = -1;
    s_vr.ui_bound = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_pc_target_w = g_pc_window_w;
    g_pc_target_h = g_pc_window_h;
}

/* The panel's model-view-projection for one eye: P_eye * (H*E)^-1, the panel's
 * vertices already in seated meters. */
static void pcvr_panel_mvp(int eye, float m44[4][4]) {
    const M34* v = &s_vr.inv_eye_pose[eye];
    float tmp[4][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            tmp[i][j] = (*v)[i][j];
    tmp[3][0] = 0.0f; tmp[3][1] = 0.0f; tmp[3][2] = 0.0f; tmp[3][3] = 1.0f;

    const float (*P)[4] = s_vr.eye_projection[eye];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m44[i][j] = P[i][0] * tmp[0][j] + P[i][1] * tmp[1][j]
                      + P[i][2] * tmp[2][j] + P[i][3] * tmp[3][j];
}

/* The panel's corners in the eye's GL pixels (for the backdrop behind it). */
static void pcvr_panel_rect(int eye, float half_w, float half_h, const float center[3], float rect[4]) {
    float m44[4][4];
    pcvr_panel_mvp(eye, m44);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int c = 0; c < 4; c++) {
        float p[4] = {center[0] + ((c & 1) ? half_w : -half_w), center[1] + ((c & 2) ? half_h : -half_h), center[2],
                      1.0f};
        float clip[4];
        for (int i = 0; i < 4; i++)
            clip[i] = m44[i][0] * p[0] + m44[i][1] * p[1] + m44[i][2] * p[2] + m44[i][3] * p[3];
        if (clip[3] < 1e-4f) clip[3] = 1e-4f;
        float sx = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)s_vr.eye[eye].w;
        float sy = (clip[1] / clip[3] * 0.5f + 0.5f) * (float)s_vr.eye[eye].h;
        x0 = fminf(x0, sx); y0 = fminf(y0, sy);
        x1 = fmaxf(x1, sx); y1 = fmaxf(y1, sy);
    }
    /* In the eye's half of a single-pass target, kept inside it. */
    const float ex = (float)s_vr.eye[eye].x, ew = (float)s_vr.eye[eye].w;
    rect[0] = ex + fmaxf(0.0f, fminf(ew, x0)); rect[1] = y0;
    rect[2] = ex + fmaxf(0.0f, fminf(ew, x1)); rect[3] = y1;
}

/* Draw the UI texture as a panel into one eye FBO */
static void pcvr_draw_panel(int eye, int see_through) {
    float m44[4][4];
    pcvr_panel_mvp(eye, m44);

    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
    glViewport(s_vr.eye[eye].x, 0, s_vr.eye[eye].w, s_vr.eye[eye].h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    /* UI content is rendered over transparent black (premultiplied) */
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(s_vr.panel_prog);
    glUniformMatrix4fv(s_vr.panel_u_mvp, 1, GL_TRUE, &m44[0][0]);
    /* First person: the panel (and its apparent size) pulls in so dialogue
     * doesn't sit stereo-behind a villager standing a meter away. dist_k is
     * smoothed once per frame in compose (both eyes must agree). */
    float dist_k = s_vr.ui_dist_k;
    float half_w = s_vr.ui_size * 0.5f * dist_k;
    float half_h = half_w * 0.75f; /* 4:3 */
    float center[3] = {0.0f, -0.05f * dist_k, -s_vr.ui_distance * dist_k};
    glUniform2f(s_panel_u_half, half_w, half_h);
    glUniform3f(s_panel_u_center, center[0], center[1], center[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_vr.ui.color);
    glUniform1i(s_vr.panel_u_tex, 0);
    glBindVertexArray(s_vr.panel_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* The room around a menu: a dark backdrop slid under the panel, so the menu
     * doesn't look ghostly over the room (premultiplied (13,11,18,235)). */
    if (see_through) {
        float rect[4];
        pcvr_panel_rect(eye, half_w, half_h, center, rect);
        const float backdrop[4] = {13.0f / 255.0f, 11.0f / 255.0f, 18.0f / 255.0f, 235.0f / 255.0f};
        pcm_draw_under(backdrop, rect);
    }
}

/* Comfort: darken the edges of both eyes while the view moves on its own (the
 * stick turns or walks it, the camera scrolls between acres). */
static void pcvr_draw_vignette(int eye) {
    if (s_vr.vignette < 0.02f) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
    const float r[4] = {(float)s_vr.eye[eye].x, 0.0f, (float)(s_vr.eye[eye].x + s_vr.eye[eye].w),
                        (float)s_vr.eye[eye].h};
    pcm_draw_vignette_rect(0.92f * s_vr.vignette, r);
}

/* Drives the vignette from how fast the game camera moved this frame. */
static void pcvr_update_vignette(void) {
    if (!s_vr.vignette_enabled || !s_vr.have_anchor || g_pc_paused || pc_vr_flat_scene_active()) {
        s_vr.vignette = 0.0f;
        s_vr.have_last_cam = 0;
        return;
    }
    /* The eyes' motion (a held shot's camera moving moves nothing) */
    float c[3] = {s_vr.anchor_pos[0], s_vr.anchor_pos[1], s_vr.anchor_pos[2]};
    float target = 0.0f;
    if (s_vr.have_last_cam) {
        float dx = c[0] - s_vr.last_cam[0], dz = c[2] - s_vr.last_cam[2];
        float speed_m = sqrtf(dx * dx + dz * dz) * s_vr.world_scale;  /* meters this frame */
        /* Walking speed (~0.05 m a frame) up: ease in; a cut (> 2 m) is no motion. */
        if (speed_m > 0.02f && speed_m < 2.0f) target = fminf(1.0f, (speed_m - 0.02f) / 0.06f);
    }
    if (fabsf(s_vr.buttons.cx) > 0.3f && g_pc_settings.fp_snap_degrees == 0) target = 1.0f;
    memcpy(s_vr.last_cam, c, sizeof(c));
    s_vr.have_last_cam = 1;
    s_vr.vignette += (target - s_vr.vignette) * (target > s_vr.vignette ? 0.35f : 0.12f);
}

static void pcvr_compose(int see_through) {
    /* Smooth the FP panel pull-in once per frame (shared by both eyes) so
     * conversation-phase changes ease instead of popping */
    {
        float target = pc_fp_view_is_active() ? 0.6f : 1.0f;
        if (s_vr.ui_dist_k <= 0.0f) s_vr.ui_dist_k = 1.0f;
        s_vr.ui_dist_k += (target - s_vr.ui_dist_k) * 0.2f;
        if (fabsf(target - s_vr.ui_dist_k) < 0.01f) s_vr.ui_dist_k = target;
    }
    pc_gx_draw_pending();
    if (see_through) {
        /* No world on screen: the eyes are the room (transparent) with the panel. */
        for (int eye = 0; eye < (s_vr.single ? 1 : 2); eye++) {  /* single-pass: one clear, both */
            glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    } else {
        pcvr_update_vignette();
        {
            /* A fade to black darkens the world around the panel too. In the mixed space the
             * frame's opacity also falls with it (pc_vr_compose_and_submit), so the darkened
             * world (premultiplied: colour and alpha both down) crosses into the room. */
            if (s_vr.fade_black > 0.01f) {
                for (int eye = 0; eye < 2; eye++) {
                    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
                    float r[4] = {(float)s_vr.eye[eye].x, 0, (float)(s_vr.eye[eye].x + s_vr.eye[eye].w),
                                  (float)s_vr.eye[eye].h};
                    const float black[4] = {0.0f, 0.0f, 0.0f, s_vr.fade_black};
                    pcm_draw_over(black, r);
                }
            }
        }
        for (int eye = 0; eye < 2; eye++) pcvr_draw_vignette(eye);
    }
    pcvr_draw_panel(0, see_through);
    pcvr_draw_panel(1, see_through);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Whether this frame shows the room: with the option on, any frame without the
 * world in it (menus, the inventory, the title's menus, loading). */
static int pcvr_frame_see_through(void) {
    if (!s_vr.room_behind_menus) return 0;
    return !s_vr.world_drawn || pc_vr_flat_scene_active();
}

extern "C" void pc_vr_compose_and_submit(void) {
    if (!s_vr.active) return;
    if (!s_vr.frame_begun) pcvr_begin_runtime_frame();
    {
        /* ACVR_TEST_CAMLOG=1: where the eyes are anchored each frame (game units, yaw in degrees). */
        static int log_on = -1;
        if (log_on < 0) log_on = getenv("ACVR_TEST_CAMLOG") != NULL;
        if (log_on && s_vr.have_anchor)
            fprintf(stderr, "[ACVR] vr anchor frame=%u fp=%d held=%d pos=(%.1f,%.1f,%.1f) yaw=%.1f\n", s_vr.frame_index,
                    pc_fp_view_is_active(), s_vr.shot_held, s_vr.anchor_pos[0], s_vr.anchor_pos[1], s_vr.anchor_pos[2],
                    atan2f(-s_vr.anchor_yaw[2][0], -s_vr.anchor_yaw[2][2]) * (float)(180.0 / PC_PI));
    }
    const int see_through = pcvr_frame_see_through();
    if (see_through) s_vr.stats_see_through++;
    pcvr_compose(see_through);

#if defined(PC_VR_OPENXR)
    if (s_vr.backend == PCVR_BACKEND_OPENXR && s_vr.frame_begun) {
        /* Mixed space: a fade to black crosses the world into the room. */
        float opacity = 1.0f;
        if (s_vr.room_behind_menus && !see_through) opacity = 1.0f - s_vr.fade_black;
        xr_end_frame(see_through, opacity);
    }
#endif
    if (!s_vr.logged_first_submit) {
        s_vr.logged_first_submit = 1;
        pcvr_log("first frame submitted (see-through %d)", see_through);
    }
    s_vr.frame_begun = 0;
    s_vr.submitted_this_frame = 1;
    /* The fade the game built into this frame's display list (fbdemo_fade_draw runs
     * before the frame begins); the next frame reports its own. */
    s_vr.fade_black = 0.0f;

    /* Leave GL in the state pc_gx expects */
    pc_gx_restore_after_nes();
    glEnable(GL_DEPTH_TEST);
}

/* Frames that bypass graph_task_set00 (NES minigames, boot screens) never
 * run the eye-pass loop. The compositor must still get a frame — build a
 * panel-only frame from the UI FBO (which the NES renderer draws into) over
 * cleared eye buffers. */
extern "C" void pc_vr_ensure_submitted(void) {
    if (!s_vr.active || s_vr.submitted_this_frame) {
        s_vr.submitted_this_frame = 0;
        return;
    }
    pcvr_begin_runtime_frame();
    pc_gx_draw_pending();
    for (int eye = 0; eye < (s_vr.single ? 1 : 2); eye++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_vr.eye[eye].fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepth(1.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_vr.world_drawn = 0;
    pc_vr_compose_and_submit();
    s_vr.submitted_this_frame = 0;
}

/* The bench's view of a fake headset: both eyes side by side in the window. On
 * the headset nobody sees the window. */
extern "C" void pc_vr_mirror_to_window(void) {
    if (!s_vr.active || s_vr.backend != PCVR_BACKEND_FAKE) return;
    int ww = g_pc_window_w, wh = g_pc_window_h;
    int ew = s_vr.eye[0].w, eh = s_vr.eye[0].h;
    if (ww <= 0 || wh <= 0 || ew <= 0 || eh <= 0) return;
    float scale = (float)(ww / 2) / (float)ew;
    if ((float)eh * scale > (float)wh) scale = (float)wh / (float)eh;
    int dw = (int)(ew * scale), dh = (int)(eh * scale);
    int dy = (wh - dh) / 2;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, ww, wh);
    glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    for (int e = 0; e < 2; e++) {
        int dx = e * (ww / 2) + (ww / 2 - dw) / 2;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_vr.eye[e].fbo);
        const int sx = s_vr.eye[e].x;
        glBlitFramebuffer(sx, 0, sx + ew, eh, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    pc_gx_viewport_state_invalidate();
}

/* ---------------------------------------------------------------- */
/* Input merge (called from PADRead) */

static int pcvr_menu_input_available(void) {
    return s_vr.active && s_vr.input_ready && s_vr.menu_input_available && s_vr.head_pose_valid;
}

extern "C" void pc_vr_merge_pad(unsigned short* buttons,
                                signed char* stickX, signed char* stickY,
                                signed char* cstickX, signed char* cstickY,
                                unsigned char* triggerL, unsigned char* triggerR) {
    /* dolphin/pad.h values (avoid header dependency in C++ TU) */
    enum {
        BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008,
        BTN_Z = 0x0010, BTN_R = 0x0020, BTN_L = 0x0040,
        BTN_A = 0x0100, BTN_B = 0x0200, BTN_X = 0x0400, BTN_Y = 0x0800,
        BTN_START = 0x1000
    };
    if (!s_vr.active) return;

    /* Held (unfocused, or the Crown pause): no input at all. */
    if (g_pc_external_hold) {
        *buttons = 0; *stickX = *stickY = *cstickX = *cstickY = 0;
        *triggerL = *triggerR = 0;
        pc_vr_swing_cancel(&s_vr.swing);
        return;
    }

    PCVRMenuSample in = {0};
    in.active = s_vr.active;
    in.available = pcvr_menu_input_available();
    in.tick = SDL_GetTicks();
    in.frame = pc_frame_counter;
    if (in.available) {
        const PCVRButtons* b = &s_vr.buttons;
        in.mx = b->mx; in.my = b->my; in.cx = b->cx; in.cy = b->cy;
        in.a = b->a; in.b = b->b; in.x = b->x; in.y = b->y;
        in.l = b->l; in.r = b->r; in.start = b->start; in.z = b->z;
    }
    in.neutral = !in.a && !in.b && !in.x && !in.y && !in.l && !in.r && !in.start && !in.z &&
        fabsf(in.mx) <= 0.12f && fabsf(in.my) <= 0.12f &&
        fabsf(in.cx) <= 0.12f && fabsf(in.cy) <= 0.12f && !*buttons &&
        abs((int)*stickX) <= 12 && abs((int)*stickY) <= 12 &&
        abs((int)*cstickX) <= 12 && abs((int)*cstickY) <= 12 && *triggerL <= 12 && *triggerR <= 12;
    unsigned int clicks = 0;
    if (pc_vr_menu_filter(&s_vr.menu_input, &in, g_pc_paused,
                          pc_pause_menu_open_vr_settings, pc_pause_menu_vr_input, &clicks)) {
        pc_vr_swing_cancel(&s_vr.swing);
        s_vr.menu_input.y_prev = in.y;
        *buttons = 0; *stickX = *stickY = *cstickX = *cstickY = 0;
        *triggerL = *triggerR = 0;
        return;
    }
    if (!in.available) return; // inactive flat mode keeps its ordinary pad input
    float mx = in.mx, my = in.my, cx = in.cx, cy = in.cy;
    int l_held = in.l;

    if (l_held) {
        /* Left grip held: left stick becomes the D-pad (tool switching) */
        if (my > 0.5f)  *buttons |= BTN_UP;
        if (my < -0.5f) *buttons |= BTN_DOWN;
        if (mx < -0.5f) *buttons |= BTN_LEFT;
        if (mx > 0.5f)  *buttons |= BTN_RIGHT;
    } else if (fabsf(mx) > 0.12f || fabsf(my) > 0.12f) {
        int sx = (int)(mx * 100.0f), sy = (int)(my * 100.0f);
        if (sx > 100) sx = 100; if (sx < -100) sx = -100;
        if (sy > 100) sy = 100; if (sy < -100) sy = -100;
        *stickX = (signed char)sx;
        *stickY = (signed char)sy;
    }
    if (fabsf(cx) > 0.12f || fabsf(cy) > 0.12f) {
        int sx = (int)(cx * 100.0f), sy = (int)(cy * 100.0f);
        if (sx > 100) sx = 100; if (sx < -100) sx = -100;
        if (sy > 100) sy = 100; if (sy < -100) sy = -100;
        *cstickX = (signed char)sx;
        *cstickY = (signed char)sy;
    }

    /* Left grip + Y: toggle first person. Requires Y to RISE while the grip
     * is already held (holding Y then squeezing grip must not fire), and X
     * must be up (X+Y is the recenter chord). Y is swallowed while gripped.
     * Not while the pause menu is open — the camera is frozen there. */
    int y_now = in.y;
    if (l_held && y_now && !s_vr.menu_input.y_prev && !in.x && !g_pc_paused) {
        pc_fp_toggle();
    }
    s_vr.menu_input.y_prev = y_now;

    /* Motion swing: inject the tool-use press (ORed with real A) */
    /* Selecting a tool must not also use the old tool from a pending gesture.
     * Real A is still merged independently below. */
    if (!pc_vr_tool_input_allowed() || l_held) pc_vr_swing_cancel(&s_vr.swing);
    if (s_vr.swing.pulse > 0) *buttons |= BTN_A;

    if (in.a) *buttons |= BTN_A;
    if (in.b) *buttons |= BTN_B;
    if (in.x && !in.y) *buttons |= BTN_X;
    if (y_now && !l_held && !in.x) *buttons |= BTN_Y;
    if ((clicks & 2u)) *buttons |= BTN_Z;
    if ((clicks & 1u)) *buttons |= BTN_START;
    if (l_held) { *buttons |= BTN_L; *triggerL = 255; }
    if (in.r) { *buttons |= BTN_R; *triggerR = 255; }
}

/* Tool anchor for the item draw: row-major 3x4 (MtxF top rows), game-world
 * units. Returns 0 when the animated hand should be used instead. */
extern "C" int pc_vr_hand_tool_mtx(float out[12]) {
    if (!s_vr.active || !s_vr.hand_valid || !g_pc_settings.vr_tool_on_hand)
        return 0;
    if (!pc_fp_view_is_active())
        return 0;
    memcpy(out, s_vr.hand_world, sizeof(float) * 12);
    return 1;
}

extern "C" int pc_vr_item_presentation_mtx(float out[12]) {
    if (!out || !s_vr.active || !s_vr.head_pose_valid || !s_vr.have_anchor ||
        !pc_fp_view_is_active() || pc_vr_flat_scene_active() || g_pc_paused ||
        !isfinite(s_vr.world_scale) || s_vr.world_scale <= 0.0001f)
        return 0;

    /* CPU actor drawing uses the most recent tracking sample, like tools.
     * W^-1 * H places the head in game world; its basis contains 1/scale.
     * Offset in physical meters, then normalize only the orientation so the
     * caller retains the original item size and animation. */
    M34 head_world, result;
    m34_mul(s_vr.world_from_seated, s_vr.head_pose, head_world);
    for (int row = 0; row < 3; ++row) {
        result[row][3] = head_world[row][3] - 0.14f * head_world[row][1]
                                             - 0.90f * head_world[row][2];
        for (int col = 0; col < 3; ++col)
            result[row][col] = head_world[row][col] * s_vr.world_scale;
        for (int col = 0; col < 4; ++col)
            if (!isfinite(result[row][col])) return 0;
    }
    memcpy(out, result, sizeof(result));
    return 1;
}

extern "C" void pc_vr_rumble(int on) {
    if (!s_vr.active || !s_vr.input_ready || !on) return;
#if defined(PC_VR_OPENXR)
    if (s_vr.backend == PCVR_BACKEND_OPENXR) xr_rumble();
#endif
}

/* ---------------------------------------------------------------- */
/* NES integration: draw the NES frame onto the UI panel in VR */

static GLint s_nes_saved_viewport[4];

extern "C" void pc_vr_nes_begin_draw(void) {
    if (!s_vr.active) return;
    glGetIntegerv(GL_VIEWPORT, s_nes_saved_viewport);
    glBindFramebuffer(GL_FRAMEBUFFER, s_vr.ui.fbo);
    /* The game DL path may have left alpha writes masked off; the panel
     * needs coverage alpha from the NES quad. */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_pc_target_w = s_vr.ui.w;
    g_pc_target_h = s_vr.ui.h;
}

extern "C" void pc_vr_nes_end_draw(void) {
    if (!s_vr.active) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(s_nes_saved_viewport[0], s_nes_saved_viewport[1],
               (GLsizei)s_nes_saved_viewport[2], (GLsizei)s_nes_saved_viewport[3]);
    g_pc_target_w = g_pc_window_w;
    g_pc_target_h = g_pc_window_h;
    /* We touched the color mask behind the dirty system */
    pc_gx_dirty_colormask();
}
