/* pc_mirror.cpp - the Window view's scene mirror, captured where pc_gx hands each
 * draw to the renderer. Trevorbilt, 2026.
 *
 * The window shows the town with real depth by drawing the game's 3D world again
 * in RealityKit, from the viewer's own eyes (visionos/host/scene_mirror.h has the
 * engine-independent half and the rules). This is the engine's half: every
 * perspective batch between the top of the frame and the first 2D batch after
 * it is the world. Its vertices go to the camera's view space as the TEV vertex
 * shader would put them, and its material is reduced to "one texture T, colour =
 * T.rgb * mul + add" per vertex: the shader's per-pixel lighting, the TEV stages
 * (with T as an unknown, other textures as their average colour), the fog and the
 * vertex colours all folded into mul and add. Blending, alpha test, culling and
 * depth become the part's kind and flags; blended, depth-tested draws that don't
 * write depth are decals (shadows), lifted along the ground's normal.
 *
 * When the world ends (the HUD begins) the host snapshots the frame for the
 * window (Base, Scene, Distance); the HUD is the difference to Final.
 *
 * Built into the visionOS game (PC_MIRROR_HOST: the host's scene_mirror) and into
 * the macOS bench, where ACVR_TEST_MIRROR_DUMP=<frame> writes one frame's raw
 * capture (view space, unclipped) in the window's dump format for
 * visionos/scripts/render-mirror-dump.py: the capture checked without a window.
 *
 * Game thread only. malloc only (the game owns operator new). */
#include "pc_gx_internal.h"
#include "pc_mirror.h"
#include <dolphin/gx/GXEnum.h>

#if !defined(PC_METAL)
/* The OpenGL builds have no Metal shim to read textures from, and no window. */
extern "C" {
float g_pc_camera_focus = 400.0f;
int pc_mirror_enabled(void) { return 0; }
void pc_mirror_set_enabled(int enabled) { (void)enabled; }
void pc_mirror_begin_frame(void) {}
void pc_mirror_capture(int first, int count) { (void)first; (void)count; }
void pc_mirror_end_frame(void) {}
void pc_mirror_set_view(const float* view34) { (void)view34; }
}
#else

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(PC_MIRROR_HOST)
#include "scene_mirror.h"
#endif

extern "C" {
float g_pc_camera_focus = 400.0f;
extern int g_pc_window_w, g_pc_window_h;
}

#if defined(__APPLE__)
#include <os/log.h>
#endif

static void mir_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void mir_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ACVR] mirror: %s\n", line);
#if defined(__APPLE__)
    os_log(OS_LOG_DEFAULT, "[ACVR] mirror: %{public}s", line);
#endif
}

/* ---------------------------------------------------------------- */
/* Small malloc-backed arrays */

template <class T>
struct MArr {
    T* p = nullptr;
    size_t n = 0, cap = 0;
    void reserve(size_t c) {
        if (c <= cap) return;
        size_t nc = cap ? cap * 2 : 256;
        while (nc < c) nc *= 2;
        T* q = (T*)realloc(p, nc * sizeof(T));
        if (!q) abort();
        p = q;
        cap = nc;
    }
    void push(const T& v) {
        reserve(n + 1);
        p[n++] = v;
    }
    void clear() { n = 0; }
};

/* The draw's vertex and material, the host's types when it's there. */
#if defined(PC_MIRROR_HOST)
typedef scene_mirror::Vertex MVertex;
typedef scene_mirror::Material MMaterial;
#else
struct MVertex {
    float position[3];
    float uv[2];
    float mul[4];
    float add[4];
};
struct MMaterial {
    uint64_t texture = 0;
    uint32_t kind = 0;
    uint32_t flags = 4 | 8;
    float cutoff = 0.5f;
    float colour_base = 0, colour_alpha = 1, opacity_base = 0, opacity_alpha = 1, opacity_luma = 0;
    uint8_t wrap_s = 1, wrap_t = 1;
};
#endif

enum { KIND_OPAQUE = 0, KIND_CUTOUT = 1, KIND_BLEND = 2 };
enum { F_CULL_BACK = 1, F_CULL_FRONT = 2, F_DEPTH_WRITE = 4, F_DEPTH_TEST = 8, F_BACKGROUND = 16 };

/* ---------------------------------------------------------------- */
/* Textures: a cache from the shim's texture serial to the mirror's id and what
 * the TEV reduction needs (average colour, whether the channels repeat). */

struct TexInfo {
    uint64_t serial;
    uint64_t id;     /* 0: couldn't be read */
    float avg[4];
    uint8_t grey;    /* r == g == b everywhere */
    uint8_t a_is_r;  /* a == r everywhere (intensity formats) */
    uint8_t wrap_s, wrap_t;
    uint8_t cutout;  /* alpha all but on or off: sent with coverage-preserving mipmaps */
};

static TexInfo* s_tex = nullptr;
static size_t s_tex_cap = 0, s_tex_count = 0;
static uint8_t* s_pixels = nullptr;
static size_t s_pixels_cap = 0;

static uint64_t mir_texture_id(const uint8_t* rgba, uint32_t w, uint32_t h) {
#if defined(PC_MIRROR_HOST)
    return scene_mirror::texture_id(rgba, w, h);
#else
    uint64_t x = 1469598103934665603ull ^ ((uint64_t)w << 32 | h);
    const size_t n = (size_t)w * h * 4;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v;
        memcpy(&v, rgba + i, 4);
        x = (x ^ v) * 1099511628211ull;
    }
    return x ? x : 1;
#endif
}

/* Bench dump: the textures sent so far (id, size, pixels). */
struct DumpTex {
    uint64_t id;
    int w, h;
    uint8_t* rgba;
};
static MArr<DumpTex> s_dump_tex;
/* ACVR_TEST_MIRROR_DUMP=<frame>[,<frame>...]: one dump into ./mirror-dump, several into
 * ./mirror-dump/<frame>. s_dump_frame is the next one still to come (-1: none). */
static int s_dump_frame = -1;
static int s_dump_frames[16];
static int s_dump_count = 0, s_dump_next = 0;

static const TexInfo* mir_texture(GLuint name) {
    int w = 0, h = 0, ws = 0, wt = 0;
    const uint64_t serial = pcm_texture_info(name, &w, &h, &ws, &wt);
    if (!serial || w <= 0 || h <= 0 || w > 2048 || h > 2048) return nullptr;
    if (s_tex_count * 2 >= s_tex_cap) {
        /* Grow (or start over when it's large: ids come back from the pixels anyway). */
        size_t ncap = s_tex_cap ? s_tex_cap * 2 : 4096;
        if (ncap > 65536) {
            ncap = s_tex_cap;
            s_tex_count = 0;
            memset(s_tex, 0, s_tex_cap * sizeof(TexInfo));
        } else {
            TexInfo* old = s_tex;
            size_t oldcap = s_tex_cap;
            s_tex = (TexInfo*)calloc(ncap, sizeof(TexInfo));
            s_tex_cap = ncap;
            s_tex_count = 0;
            for (size_t i = 0; i < oldcap; i++) {
                if (!old[i].serial) continue;
                size_t j = (size_t)(old[i].serial * 0x9E3779B97F4A7C15ull) & (s_tex_cap - 1);
                while (s_tex[j].serial) j = (j + 1) & (s_tex_cap - 1);
                s_tex[j] = old[i];
                s_tex_count++;
            }
            free(old);
        }
    }
    size_t j = (size_t)(serial * 0x9E3779B97F4A7C15ull) & (s_tex_cap - 1);
    while (s_tex[j].serial && s_tex[j].serial != serial) j = (j + 1) & (s_tex_cap - 1);
    TexInfo* t = &s_tex[j];
    /* pcm_texture_info: 0 repeat, 1 clamp, 2 mirrored; the window's: 0 clamp, 1 repeat, 2 mirror.
     * Read on every use: the game re-wraps a cached texture without a new serial. */
    static const uint8_t to_mirror_wrap[3] = {1, 0, 2};
    const uint8_t mws = to_mirror_wrap[ws >= 0 && ws <= 2 ? ws : 0];
    const uint8_t mwt = to_mirror_wrap[wt >= 0 && wt <= 2 ? wt : 0];
    if (t->serial == serial) {
        if (!t->id) return nullptr;
        t->wrap_s = mws;
        t->wrap_t = mwt;
#if defined(PC_MIRROR_HOST)
        if (!scene_mirror::knows_texture(t->id)) {
            /* Forgotten (swept, or the mirror was turned on again): send it again. */
            const size_t bytes = (size_t)w * h * 4;
            if (bytes > s_pixels_cap) {
                s_pixels = (uint8_t*)realloc(s_pixels, bytes);
                s_pixels_cap = bytes;
            }
            if (pcm_texture_read_rgba(name, s_pixels))
                scene_mirror::add_texture(t->id, s_pixels, (uint32_t)w, (uint32_t)h, true, t->cutout != 0);
        }
#endif
        return t;
    }

    /* New: read it, name it by its pixels, measure it, send it. */
    memset(t, 0, sizeof(*t));
    t->serial = serial;
    s_tex_count++;
    const size_t bytes = (size_t)w * h * 4;
    if (bytes > s_pixels_cap) {
        s_pixels = (uint8_t*)realloc(s_pixels, bytes);
        s_pixels_cap = bytes;
    }
    if (!pcm_texture_read_rgba(name, s_pixels)) return nullptr;
    double sum[4] = {0, 0, 0, 0};
    int grey = 1, a_is_r = 1;
    size_t cut = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t* px = s_pixels + i * 4;
        for (int c = 0; c < 4; c++) sum[c] += px[c];
        if (px[0] != px[1] || px[1] != px[2]) grey = 0;
        if (px[3] != px[0]) a_is_r = 0;
        if (px[3] < 16 || px[3] > 239) cut++;
    }
    const double n = (double)w * h;
    for (int c = 0; c < 4; c++) t->avg[c] = (float)(sum[c] / n / 255.0);
    t->grey = (uint8_t)grey;
    t->a_is_r = (uint8_t)a_is_r;
    t->wrap_s = mws;
    t->wrap_t = mwt;
    t->id = mir_texture_id(s_pixels, (uint32_t)w, (uint32_t)h);
    const bool cutout = cut > (size_t)(n * 0.97) && t->avg[3] < 0.97f;
    t->cutout = cutout ? 1 : 0;
#if defined(PC_MIRROR_HOST)
    if (!scene_mirror::knows_texture(t->id))
        scene_mirror::add_texture(t->id, s_pixels, (uint32_t)w, (uint32_t)h, true, cutout);
#else
    (void)cutout;
    if (s_dump_frame >= 0) {
        bool have = false;
        for (size_t k = 0; k < s_dump_tex.n; k++)
            if (s_dump_tex.p[k].id == t->id) have = true;
        if (!have) {
            DumpTex d = {t->id, w, h, (uint8_t*)malloc(bytes)};
            memcpy(d.rgba, s_pixels, bytes);
            s_dump_tex.push(d);
        }
    }
#endif
    return t;
}

/* ---------------------------------------------------------------- */
/* The TEV with the primary texture T as an unknown: each channel value is
 * k * T[src] + c (src -1: no T). */

struct Lin {
    float k, c;
    int src;
};

static inline Lin lconst(float x) { return Lin{0.0f, x, -1}; }

/* T's channel ch as a constant (its average): when a value would need two of T's
 * channels, or T twice. */
static const TexInfo* s_T = nullptr;

static inline int same_channel(int a, int b) {
    if (a == b) return 1;
    if (!s_T) return 0;
    if (s_T->grey && a < 3 && b < 3) return 1;
    if (s_T->grey && s_T->a_is_r) return 1;
    return 0;
}

static inline Lin lflatten(Lin v) {
    if (v.src < 0) return v;
    return lconst(v.k * (s_T ? s_T->avg[v.src] : 1.0f) + v.c);
}

static inline Lin ladd(Lin a, Lin b) {
    if (a.src < 0) return Lin{b.k, a.c + b.c, b.src};
    if (b.src < 0) return Lin{a.k, a.c + b.c, a.src};
    if (same_channel(a.src, b.src)) return Lin{a.k + b.k, a.c + b.c, a.src};
    b = lflatten(b);
    return Lin{a.k, a.c + b.c, a.src};
}

static inline Lin lscale(Lin a, float s) { return Lin{a.k * s, a.c * s, a.src}; }
static inline Lin lsub(Lin a, Lin b) { return ladd(a, lscale(b, -1.0f)); }

static inline Lin lmul(Lin a, Lin b) {
    if (a.src < 0) return Lin{a.c * b.k, a.c * b.c, b.src};
    if (b.src < 0) return Lin{b.c * a.k, b.c * a.c, a.src};
    /* T * T: keep the stronger, flatten the other */
    if (fabsf(a.k) >= fabsf(b.k)) b = lflatten(b); else a = lflatten(a);
    return lmul(a, b);
}

static inline Lin lclamp(Lin a) {
    if (a.src < 0) return lconst(fminf(fmaxf(a.c, 0.0f), 1.0f));
    /* T in [0, 1]: refit the line through its clamped ends */
    float v0 = fminf(fmaxf(a.c, 0.0f), 1.0f), v1 = fminf(fmaxf(a.k + a.c, 0.0f), 1.0f);
    return Lin{v1 - v0, v0, a.src};
}

struct Lin4 {
    Lin v[4];
};

static inline Lin4 l4const(const float c[4]) {
    Lin4 r;
    for (int i = 0; i < 4; i++) r.v[i] = lconst(c[i]);
    return r;
}

static float konst_c(int sel, int ch) {
    if (sel <= 7) return (float)(8 - sel) / 8.0f;
    if (sel <= 11) return 0.0f;
    if (sel <= 15) return g_gx.tev_k_colors[sel - 12][ch];
    const int ki = (sel - 16) & 3, comp = (sel - 16) >> 2;
    return g_gx.tev_k_colors[ki][comp > 3 ? 3 : comp];
}

static float konst_a(int sel) {
    if (sel <= 7) return (float)(8 - sel) / 8.0f;
    if (sel <= 15) return 0.0f;
    const int ki = (sel - 16) & 3, comp = (sel - 16) >> 2;
    return g_gx.tev_k_colors[ki][comp > 3 ? 3 : comp];
}

/* getTevC for one channel ch (0..2) */
static Lin tev_c(int id, int ch, const Lin4& prev, const Lin4& tex, const Lin4& ras, const Lin4& r0, const Lin4& r1,
                 const Lin4& r2, int ksel) {
    switch (id) {
        case 0: return prev.v[ch];
        case 1: return prev.v[3];
        case 2: return r0.v[ch];
        case 3: return r0.v[3];
        case 4: return r1.v[ch];
        case 5: return r1.v[3];
        case 6: return r2.v[ch];
        case 7: return r2.v[3];
        case 8: return tex.v[ch];
        case 9: return tex.v[3];
        case 10: return ras.v[ch];
        case 11: return ras.v[3];
        case 12: return lconst(1.0f);
        case 13: return lconst(0.5f);
        case 14: return lconst(konst_c(ksel, ch));
        default: return lconst(0.0f);
    }
}

static Lin tev_a(int id, const Lin4& prev, const Lin4& tex, const Lin4& ras, const Lin4& r0, const Lin4& r1,
                 const Lin4& r2, int ksel) {
    switch (id) {
        case 0: return prev.v[3];
        case 1: return r0.v[3];
        case 2: return r1.v[3];
        case 3: return r2.v[3];
        case 4: return tex.v[3];
        case 5: return ras.v[3];
        case 6: return lconst(konst_a(ksel));
        default: return lconst(0.0f);
    }
}

static Lin4 swap4(const Lin4& v, const PCGXTevSwapTable& t) {
    Lin4 r;
    r.v[0] = v.v[t.r & 3];
    r.v[1] = v.v[t.g & 3];
    r.v[2] = v.v[t.b & 3];
    r.v[3] = v.v[t.a & 3];
    return r;
}

static Lin bias_scale(Lin v, int bias, int scale) {
    if (bias == 1) v = ladd(v, lconst(0.5f));
    else if (bias == 2) v = ladd(v, lconst(-0.5f));
    if (scale == 1) v = lscale(v, 2.0f);
    else if (scale == 2) v = lscale(v, 4.0f);
    else if (scale == 3) v = lscale(v, 0.5f);
    return v;
}

/* ---------------------------------------------------------------- */
/* Per-frame state */

#if defined(PC_MIRROR_HOST)
static int s_enabled = 1;  /* the host's scene_mirror::enabled() says whether the window wants it */
#else
static int s_enabled = 0;  /* the bench: ACVR_TEST_MIRROR_DUMP only */
#endif
static int s_scene_started = 0, s_scene_ended = 0;
static float s_tan_x = 0.0f, s_tan_y = 0.0f, s_near = 1.0f, s_far = 1000.0f;
static float s_proj00 = 0.0f, s_proj11 = 0.0f;
static float s_view[12];
static int s_have_view = 0;
static unsigned s_frame_count = 0;
static unsigned s_draws_frame = 0, s_verts_frame = 0;
static unsigned s_draws_total = 0, s_frames_logged = 0;

/* scratch for one draw */
static MArr<MVertex> s_v;
static MArr<uint32_t> s_idx;

/* Bench dump of a whole frame (raw, view space). */
struct DumpPart {
    uint32_t first, count;
    MMaterial m;
};
static MArr<MVertex> s_dump_v;
static MArr<uint32_t> s_dump_i;
static MArr<DumpPart> s_dump_parts;

extern "C" void pc_mirror_set_enabled(int enabled) {
    if (enabled && !s_enabled) {
        /* every texture is sent again */
        s_tex_count = 0;
        if (s_tex) memset(s_tex, 0, s_tex_cap * sizeof(TexInfo));
    }
    s_enabled = enabled;
}

extern "C" int pc_mirror_enabled(void) {
#if defined(PC_MIRROR_HOST)
    return s_enabled && scene_mirror::enabled();
#else
    return s_enabled;
#endif
}

extern "C" void pc_mirror_set_view(const float* view34) {
    memcpy(s_view, view34, sizeof(s_view));
    s_have_view = 1;
}

extern "C" void pc_mirror_begin_frame(void) {
    s_scene_started = 0;
    s_scene_ended = 0;
    s_draws_frame = 0;
    s_verts_frame = 0;
    s_frame_count++;
    /* A dump frame that passed without a 3D scene (or a list out of order): on to the next. */
    while (s_dump_frame >= 0 && (int)s_frame_count > s_dump_frame)
        s_dump_frame = ++s_dump_next < s_dump_count ? s_dump_frames[s_dump_next] : -1;
#if !defined(PC_MIRROR_HOST)
    static int parsed = 0;
    if (!parsed) {
        parsed = 1;
        if (const char* d = getenv("ACVR_TEST_MIRROR_DUMP")) {
            char list[256];
            snprintf(list, sizeof(list), "%s", d);
            char* save = nullptr;
            for (char* tok = strtok_r(list, ",", &save); tok && s_dump_count < 16; tok = strtok_r(nullptr, ",", &save))
                s_dump_frames[s_dump_count++] = atoi(tok);
            s_dump_frame = s_dump_count ? s_dump_frames[0] : -1;
            s_enabled = 1;
            mir_log("bench dump of %d frame(s) from frame %d into ./mirror-dump", s_dump_count, s_dump_frame);
        }
    }
#endif
}

/* The sky: the procedural sky (pc_sky) isn't geometry, so the mirror gets a dome of
 * its colours behind everything (scene_mirror pushes a background past the level). */
extern "C" int pc_sky_environment(float* seconds, float* overcast);

static void mir_add_sky(void) {
    float seconds = 0.0f, overcast = 0.0f;
    if (!pc_sky_environment(&seconds, &overcast) || !s_have_view) return;
    const float hour = seconds / 3600.0f, wet = fminf(fmaxf(overcast, 0.0f), 1.0f);
    auto smooth = [](float a, float b, float x) {
        float t = fminf(fmaxf((x - a) / (b - a), 0.0f), 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float day = smooth(5.0f, 7.5f, hour) * (1.0f - smooth(17.0f, 20.0f, hour));
    float dusk = fmaxf(1.0f - fabsf(hour - 6.0f) / 1.5f, 1.0f - fabsf(hour - 18.5f) / 1.5f);
    dusk = fmaxf(dusk, 0.0f) * (1.0f - wet);
    auto mix3 = [](const float* a, const float* b, float t, float* o) {
        for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
    };
    const float z_night[3] = {0.018f, 0.032f, 0.095f}, z_day[3] = {0.065f, 0.29f, 0.84f};
    const float h_night[3] = {0.065f, 0.10f, 0.19f}, h_day[3] = {0.65f, 0.85f, 0.96f};
    const float h_dusk[3] = {0.96f, 0.56f, 0.37f};
    float zen[3], hor[3];
    mix3(z_night, z_day, day, zen);
    mix3(h_night, h_day, day, hor);
    mix3(hor, h_dusk, dusk * 0.72f, hor);
    /* A dome around the camera in view space: the world's up from the view matrix. */
    const float up[3] = {s_view[1], s_view[5], s_view[9]};
    const float R = s_far * 0.9f;
    float e1[3] = {s_view[0], s_view[4], s_view[8]};  /* world X in view space */
    float e2[3] = {up[1] * e1[2] - up[2] * e1[1], up[2] * e1[0] - up[0] * e1[2], up[0] * e1[1] - up[1] * e1[0]};
    const int RINGS = 6, SEG = 24;
    s_v.clear();
    s_idx.clear();
    for (int r = 0; r <= RINGS; r++) {
        const float el = -0.15f + (1.6f) * (float)r / RINGS;  /* elevation, a little below the horizon to the top */
        const float t = fminf(fmaxf(sinf(el), 0.0f), 1.0f);
        const float k = powf(t, 0.48f);
        float col[3];
        mix3(hor, zen, k, col);
        for (int sgi = 0; sgi <= SEG; sgi++) {
            const float az = 6.2831853f * (float)sgi / SEG;
            MVertex v{};
            for (int i = 0; i < 3; i++)
                v.position[i] = R * (cosf(el) * (cosf(az) * e1[i] + sinf(az) * e2[i]) + sinf(el) * up[i]);
            for (int i = 0; i < 3; i++) {
                v.mul[i] = 0.0f;
                v.add[i] = col[i];
            }
            v.mul[3] = 0.0f;
            v.add[3] = 1.0f;
            s_v.push(v);
        }
    }
    for (int r = 0; r < RINGS; r++)
        for (int sgi = 0; sgi < SEG; sgi++) {
            uint32_t a = r * (SEG + 1) + sgi, b = a + 1, c = a + SEG + 1, d = c + 1;
            s_idx.push(a); s_idx.push(c); s_idx.push(b);
            s_idx.push(b); s_idx.push(c); s_idx.push(d);
        }
    MMaterial m;
    m.texture = 0;
    m.kind = KIND_OPAQUE;
    m.flags = F_BACKGROUND;
#if defined(PC_MIRROR_HOST)
    scene_mirror::Draw d;
    d.vertices = s_v.p;
    d.vertex_count = (uint32_t)s_v.n;
    d.indices = s_idx.p;
    d.index_count = (uint32_t)s_idx.n;
    d.material = m;
    scene_mirror::add_draw(d);
#else
    if ((int)s_frame_count == s_dump_frame) {
        DumpPart part{(uint32_t)s_dump_i.n, (uint32_t)s_idx.n, m};
        const uint32_t base = (uint32_t)s_dump_v.n;
        for (size_t i = 0; i < s_v.n; i++) s_dump_v.push(s_v.p[i]);
        for (size_t i = 0; i < s_idx.n; i++) s_dump_i.push(base + s_idx.p[i]);
        s_dump_parts.push(part);
    }
#endif
}

/* ---------------------------------------------------------------- */
/* The window's scale, steady through a shot.
 *
 * The window spans the camera's view a little short of its focus (scene_mirror: the glass at
 * 0.85 x focus), so the world's size in the window is 1 / (2 x 0.85 x focus x tan). Fed the
 * camera's eye-to-centre distance as it stands each frame, the world grew and shrank whenever
 * that distance changed with the camera standing still: the train intro's camera stays put
 * and looks at Rover as he walks the length of the carriage and back (focus 120 -> 420 -> 120
 * units), so the whole carriage shrank to a third and came back, the depth of everything
 * changing with it (the same picture from the camera's point, which a flat screen shows; in
 * the window's 3D, jarring to nauseating: Trevor, headset). Now the scale holds through a
 * shot and changes only:
 *   - at a cut (no scene last frame, the eye jumping, the view turning or the field of view
 *     jumping in one frame), where the picture changes anyway;
 *   - with the camera's own dolly: the part of the focus change the eye's motion made (a talk
 *     camera closing in, the C stick's zoom), so the framing follows the game's; what the
 *     looked-at point's motion made (a subject walking off, a pan to it) changes nothing.
 * A field-of-view change within a shot keeps the world's size: the glass goes deeper and the
 * camera's point farther out, so the world stays as big and only the camera moves.
 * What the hold holds is focus x tan (the glass's half-span over 0.85, in game units). */
static struct {
    int have;
    unsigned frame;  /* s_frame_count of the last scene */
    float eye[3], centre[3], fwd[3];
    float focus, tan_x;
    float span;      /* held: focus x tan_x */
} s_shot;
static int s_shot_cut = 0;
static float s_scene_view[12];  /* the view the world was drawn with (s_view at the scene's start) */

static float mir_steady_focus(float focus) {
    const float tan_x = s_tan_x > 1e-4f ? s_tan_x : 1.0f;
    if (!s_have_view || !(focus > 0.0f) || !isfinite(focus)) {
        s_shot_cut = 0;
        return focus;
    }
    /* The camera from the view (world to view, rigid): eye = -R^T t, forward = -(row 2). */
    const float* V = s_scene_view;
    float eye[3], fwd[3], centre[3];
    for (int k = 0; k < 3; k++) {
        eye[k] = -(V[0 + k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]);
        fwd[k] = -V[8 + k];
    }
    const float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (fl < 1e-6f) return focus;
    for (int k = 0; k < 3; k++) {
        fwd[k] /= fl;
        centre[k] = eye[k] + fwd[k] * focus;
    }
    /* why: 1 the first scene or none last frame, 2 the eye jumped, 3 the view turned, 4 the field of view jumped */
    int cut = !s_shot.have ? 1 : s_frame_count - s_shot.frame > 1 ? 1 : 0;
    float de2 = 0.0f, dc2 = 0.0f;
    if (!cut) {
        for (int k = 0; k < 3; k++) {
            const float de = eye[k] - s_shot.eye[k], dc = centre[k] - s_shot.centre[k];
            de2 += de * de;
            dc2 += dc * dc;
        }
        const float dot = fwd[0] * s_shot.fwd[0] + fwd[1] * s_shot.fwd[1] + fwd[2] * s_shot.fwd[2];
        const float jump = 0.25f * fmaxf(s_shot.focus, 40.0f);
        cut = de2 > jump * jump                                  ? 2
              : dot < 0.906f /* 25 degrees */                    ? 3
              : fabsf(tan_x / s_shot.tan_x - 1.0f) > 0.2f        ? 4
                                                                 : 0;
    }
    if (cut) {
        s_shot.span = focus * tan_x;
    } else if (s_shot.focus > 0.0f && de2 > 0.0f) {
        /* The eye's share of this frame's motion makes its share of the focus change. */
        const float share = de2 / (de2 + dc2);
        s_shot.span *= powf(focus / s_shot.focus, share);
    }
    s_shot.have = 1;
    s_shot.frame = s_frame_count;
    memcpy(s_shot.eye, eye, sizeof(eye));
    memcpy(s_shot.centre, centre, sizeof(centre));
    memcpy(s_shot.fwd, fwd, sizeof(fwd));
    s_shot.focus = focus;
    s_shot.tan_x = tan_x;
    /* Never far from the camera's own: a quarter to four times it (the glass keeps near the scene). */
    float steady = s_shot.span / tan_x;
    steady = fminf(fmaxf(steady, focus * 0.25f), focus * 4.0f);
    s_shot.span = steady * tan_x;
    s_shot_cut = cut;
    return steady;
}

static void mir_scene_start(void) {
    s_scene_started = 1;
    memcpy(s_scene_view, s_view, sizeof(s_scene_view));
    const float (*P)[4] = g_gx.projection_mtx;
    s_proj00 = P[0][0];
    s_proj11 = P[1][1];
    s_tan_x = P[0][0] != 0.0f ? 1.0f / fabsf(P[0][0]) : 1.0f;
    s_tan_y = P[1][1] != 0.0f ? 1.0f / fabsf(P[1][1]) : 1.0f;
    /* GX convention rows (emu64): P22 = A = n/(n-f), P23 = B = nf/(n-f), so B/A = f and
     * B/(A-1) = n. */
    const float A = P[2][2], B = P[2][3];
    if (fabsf(A) > 1e-9f && fabsf(A - 1.0f) > 1e-9f) {
        s_near = B / (A - 1.0f);
        s_far = B / A;
    }
    if (!(s_near > 0.0f)) s_near = 1.0f;
    if (!(s_far > s_near)) s_far = s_near * 1000.0f;
#if defined(PC_MIRROR_HOST)
    scene_mirror::set_units_per_metre(40.0f);  /* one ground tile (40 units) is about a metre */
    scene_mirror::set_projection(s_tan_x, s_tan_y, s_near);
#endif
    mir_add_sky();
}

/* The world is drawn: the HUD begins, or the frame ends. */
extern "C" void acvr_visionos_window_scene_end(float tan_x, float tan_y, float focus, float near_plane,
                                               float far_plane);
static void mir_scene_end(void) {
    if (s_scene_ended) return;
    s_scene_ended = 1;
    if (!s_scene_started) return;
    const float camera_focus = fmaxf(g_pc_camera_focus, s_near * 1.5f);
    const float focus = fmaxf(mir_steady_focus(camera_focus), s_near * 1.5f);
    {
        /* ACVR_TEST_CAMLOG=1: the window's scale each frame, as a 1.2 m wide window shows it. */
        static int log_on = -1;
        if (log_on < 0) log_on = getenv("ACVR_TEST_CAMLOG") != NULL;
        if (log_on) {
            const float tx = fmaxf(s_tan_x, 1e-4f);
            fprintf(stderr,
                    "[ACVR] window scale frame=%u focus=%.1f steady=%.1f tan=%.3f cut=%d mm_per_unit_before=%.3f "
                    "mm_per_unit=%.3f\n",
                    s_frame_count, camera_focus, focus, s_tan_x, s_shot_cut,
                    1200.0f / (2.0f * 0.85f * camera_focus * tx), 1200.0f / (2.0f * 0.85f * focus * tx));
        }
    }
#if defined(PC_MIRROR_HOST)
    scene_mirror::end_frame(true, focus);
    acvr_visionos_window_scene_end(s_tan_x, s_tan_y, focus, s_near, s_far);
#endif
    s_draws_total += s_draws_frame;
    if (++s_frames_logged % 600 == 1) {
        mir_log("frame %u: %u draws, %u vertices, tan %.3f x %.3f, near %.1f, focus %.1f", s_frame_count,
                s_draws_frame, s_verts_frame, s_tan_x, s_tan_y, s_near, focus);
    }
#if !defined(PC_MIRROR_HOST)
    if ((int)s_frame_count == s_dump_frame) {
        char dir[64];
        mkdir("mirror-dump", 0755);
        if (s_dump_count > 1) {
            snprintf(dir, sizeof(dir), "mirror-dump/%d", s_dump_frame);
            mkdir(dir, 0755);
        } else {
            snprintf(dir, sizeof(dir), "mirror-dump");
        }
        char path[160];
        auto half = [](float f) -> uint16_t {
            __fp16 h = (__fp16)f;
            uint16_t u;
            memcpy(&u, &h, 2);
            return u;
        };
        snprintf(path, sizeof(path), "%s/vertices.bin", dir);
        FILE* f = fopen(path, "wb");
        for (size_t i = 0; f && i < s_dump_v.n; i++) {
            const MVertex& v = s_dump_v.p[i];
            fwrite(v.position, 4, 3, f);
            fwrite(v.uv, 4, 2, f);
            uint16_t m[8];
            for (int c = 0; c < 4; c++) {
                m[c] = half(v.mul[c]);
                m[4 + c] = half(v.add[c]);
            }
            fwrite(m, 2, 8, f);
        }
        if (f) fclose(f);
        snprintf(path, sizeof(path), "%s/indices.bin", dir);
        f = fopen(path, "wb");
        if (f) {
            fwrite(s_dump_i.p, 4, s_dump_i.n, f);
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s/parts.txt", dir);
        f = fopen(path, "w");
        for (size_t i = 0; f && i < s_dump_parts.n; i++) {
            const DumpPart& p = s_dump_parts.p[i];
            fprintf(f, "%u %u %llu %u %u %g %g %g %g %g %g %u %u\n", p.first, p.count,
                    (unsigned long long)p.m.texture, p.m.kind, p.m.flags, p.m.cutoff, p.m.colour_base,
                    p.m.colour_alpha, p.m.opacity_base, p.m.opacity_alpha, p.m.opacity_luma, p.m.wrap_s, p.m.wrap_t);
        }
        if (f) fclose(f);
        snprintf(path, sizeof(path), "%s/camera.txt", dir);
        f = fopen(path, "w");
        if (f) {
            /* tan_x tan_y focus (the window's, steady) camera_focus (the game camera's own), then the
             * world's up in view space (the camera's pitch and roll) */
            fprintf(f, "%g %g %g %g %g %g %g\n", s_tan_x, s_tan_y, focus, camera_focus, s_scene_view[1],
                    s_scene_view[5], s_scene_view[9]);
            fclose(f);
        }
        for (size_t i = 0; i < s_dump_tex.n; i++) {
            snprintf(path, sizeof(path), "%s/tex-%llu-%dx%d.rgba", dir, (unsigned long long)s_dump_tex.p[i].id,
                     s_dump_tex.p[i].w, s_dump_tex.p[i].h);
            f = fopen(path, "wb");
            if (f) {
                fwrite(s_dump_tex.p[i].rgba, 1, (size_t)s_dump_tex.p[i].w * s_dump_tex.p[i].h * 4, f);
                fclose(f);
            }
        }
        mir_log("dumped frame %u into %s: %zu vertices, %zu parts, %zu textures", s_frame_count, dir, s_dump_v.n,
                s_dump_parts.n, s_dump_tex.n);
        s_dump_v.clear();
        s_dump_i.clear();
        s_dump_parts.clear();
        s_dump_frame = ++s_dump_next < s_dump_count ? s_dump_frames[s_dump_next] : -1;
    }
#endif
}

extern "C" void pc_mirror_end_frame(void) {
    if (!pc_mirror_enabled()) return;
    if (s_scene_started) {
        mir_scene_end();
    } else {
#if defined(PC_MIRROR_HOST)
        scene_mirror::end_frame(false, 0.0f);
#endif
    }
}

/* ---------------------------------------------------------------- */
/* One batch */

static inline void xf34(const float m[3][4], const float p[3], float o[3]) {
    for (int r = 0; r < 3; r++) o[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
}

static int mir_alpha_cutoff(float* cutoff) {
    /* The common cut-out: alpha > ref (or >=) AND always. Others aren't one. */
    const int c0 = g_gx.alpha_comp0, c1 = g_gx.alpha_comp1, op = g_gx.alpha_op;
    if (c0 == 7 && c1 == 7) return 0;
    if ((c0 == 4 || c0 == 6) && (c1 == 7 || op == 1 /* OR with never */)) {
        *cutoff = fmaxf((float)g_gx.alpha_ref0 / 255.0f, 0.02f);
        return 1;
    }
    if ((c1 == 4 || c1 == 6) && c0 == 7) {
        *cutoff = fmaxf((float)g_gx.alpha_ref1 / 255.0f, 0.02f);
        return 1;
    }
    *cutoff = 0.5f;
    return 1;
}

extern "C" void pc_mirror_capture(int first, int count) {
    if (count < 3 || s_scene_ended) return;
    if (g_gx.projection_type != GX_PERSPECTIVE) {
        /* the 2D layer begins: the world is done */
        if (s_scene_started) mir_scene_end();
        return;
    }
    if (!s_scene_started) mir_scene_start();
    /* Another projection (a 3D model in the HUD) isn't the world. */
    if (fabsf(g_gx.projection_mtx[0][0] - s_proj00) > 0.03f * fabsf(s_proj00) ||
        fabsf(g_gx.projection_mtx[1][1] - s_proj11) > 0.03f * fabsf(s_proj11))
        return;
    if (!g_gx.color_update_enable) return;  /* depth-only */
    if (g_gx.blend_mode == GX_BM_SUBTRACT || g_gx.blend_mode == GX_BM_LOGIC) return;
    if (g_gx.cull_mode == GX_CULL_ALL) return;
    const int prim = g_gx.current_primitive;
    if (prim != GX_TRIANGLES && prim != GX_QUADS && prim != GX_TRIANGLESTRIP && prim != GX_TRIANGLEFAN) return;

    /* --- material --- */
    MMaterial m;
    int nst = g_gx.num_tev_stages < PC_GX_MAX_TEV_STAGES ? g_gx.num_tev_stages : PC_GX_MAX_TEV_STAGES;
    if (nst < 1) nst = 1;
    int primary = -1;
    const TexInfo* stage_tex[PC_GX_MAX_TEV_STAGES] = {nullptr, nullptr, nullptr};
    for (int s = 0; s < nst; s++) {
        const int map = g_gx.tev_stages[s].tex_map;
        if (map < 0 || map >= 8 || !g_gx.gl_textures[map]) continue;
        stage_tex[s] = mir_texture(g_gx.gl_textures[map]);
        if (!stage_tex[s]) continue;
        /* a colour picture over an intensity mask */
        if (primary < 0 || (stage_tex[primary]->grey && !stage_tex[s]->grey)) primary = s;
    }
    s_T = primary >= 0 ? stage_tex[primary] : nullptr;
    if (s_T) {
        m.texture = s_T->id;
        m.wrap_s = s_T->wrap_s;
        m.wrap_t = s_T->wrap_t;
    }
    float cutoff = 0.5f;
    const int alpha_test = mir_alpha_cutoff(&cutoff);
    m.cutoff = cutoff;
    m.flags = 0;
    if (g_gx.z_compare_enable) m.flags |= F_DEPTH_TEST;
    if (g_gx.z_update_enable) m.flags |= F_DEPTH_WRITE;
    if (g_gx.cull_mode == GX_CULL_BACK) m.flags |= F_CULL_BACK;
    if (g_gx.cull_mode == GX_CULL_FRONT) m.flags |= F_CULL_FRONT;
    bool blend = g_gx.blend_mode == GX_BM_BLEND;
    if (blend) {
        const int sb = g_gx.blend_src, db = g_gx.blend_dst;
        m.kind = KIND_BLEND;
        if ((sb == GX_BL_SRCALPHA || sb == GX_BL_DSTALPHA) && (db == GX_BL_INVSRCALPHA || db == GX_BL_INVDSTALPHA)) {
            m.colour_base = 0; m.colour_alpha = 1; m.opacity_base = 0; m.opacity_alpha = 1; m.opacity_luma = 0;
        } else if (sb == GX_BL_ONE && db == GX_BL_ONE) {
            m.colour_base = 1; m.colour_alpha = 0; m.opacity_base = 0; m.opacity_alpha = 0; m.opacity_luma = 0;
        } else if (sb == GX_BL_SRCALPHA && db == GX_BL_ONE) {
            m.colour_base = 0; m.colour_alpha = 1; m.opacity_base = 0; m.opacity_alpha = 0; m.opacity_luma = 0;
        } else if (sb == GX_BL_ONE && db == GX_BL_INVSRCALPHA) {
            m.colour_base = 1; m.colour_alpha = 0; m.opacity_base = 0; m.opacity_alpha = 1; m.opacity_luma = 0;
        } else if ((sb == GX_BL_ZERO && db == GX_BL_SRCCLR) || (sb == GX_BL_DSTCLR && db == GX_BL_ZERO)) {
            /* a multiply: darken by the source's luma */
            m.colour_base = 0; m.colour_alpha = 0; m.opacity_base = 1; m.opacity_alpha = 0; m.opacity_luma = -1;
        } else if (sb == GX_BL_ZERO && db == GX_BL_ONE) {
            return;  /* draws nothing */
        } else {
            m.colour_base = 0; m.colour_alpha = 1; m.opacity_base = 0; m.opacity_alpha = 1; m.opacity_luma = 0;
        }
    } else if (alpha_test) {
        m.kind = KIND_CUTOUT;
    } else {
        m.kind = KIND_OPAQUE;
    }

    /* --- constants for the TEV --- */
    const float (*MV)[4] = g_gx.pos_mtx[g_gx.current_mtx];
    const float (*NM)[3] = g_gx.nrm_mtx[g_gx.current_mtx];
    float texrow[2][2][4];
    int texmtx_on[2], texgen_src[2];
    for (int tg = 0; tg < 2; tg++) {
        /* pc_gx.c's pc_tex_mtx_id_to_slot: GX_TEXMTX0 = 30 (in steps of 3), GX_IDENTITY = 60 */
        const int mid = g_gx.tex_gen_mtx[tg];
        int slot = -1;
        if (mid >= 0 && mid < 10) slot = mid;
        else if (mid >= 30 && mid < 60) slot = (mid - 30) / 3;
        texmtx_on[tg] = slot >= 0 && slot < 10;
        texgen_src[tg] = g_gx.tex_gen_src[tg];
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 4; c++) texrow[tg][r][c] = (r == c) ? 1.0f : 0.0f;
        if (texmtx_on[tg]) {
            const float* tm = (const float*)g_gx.tex_mtx[slot];
            memcpy(texrow[tg][0], &tm[0], sizeof(float) * 4);
            memcpy(texrow[tg][1], &tm[4], sizeof(float) * 4);
        }
    }
    const int tc_src_primary = primary >= 0 ? pc_gx_tc_src_normalize(g_gx.tev_stages[primary].tex_coord, primary) : 0;
    const int light_en = g_gx.chan_ctrl_enable[0], mat_src = g_gx.chan_ctrl_mat_src[0],
              amb_src = g_gx.chan_ctrl_amb_src[0], num_chans = g_gx.num_chans;
    const int alpha_light_en = g_gx.chan_ctrl_enable[1], alpha_mat_src = g_gx.chan_ctrl_mat_src[1];
    const int light_mask = g_gx.chan_ctrl_light_mask[0];
    float Ldir[8][3];
    for (int i = 0; i < 8; i++) {
        const float* p = g_gx.lights[i].pos;
        float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (int c = 0; c < 3; c++) Ldir[i][c] = l > 1e-8f ? p[c] / l : 0.0f;
    }
    const int fog_on = g_gx.fog_type != 0;
    const float fog_s = g_gx.fog_start, fog_e = g_gx.fog_end;

    /* --- vertices --- */
    s_v.clear();
    s_v.reserve((size_t)count);
    const PCGXVertex* src = g_gx.vertex_buffer + first;
    for (int i = 0; i < count; i++) {
        const PCGXVertex& in = src[i];
        MVertex v;
        float eye[3];
        xf34(MV, in.position, eye);
        v.position[0] = eye[0];
        v.position[1] = eye[1];
        v.position[2] = eye[2];
        float n[3];
        for (int r = 0; r < 3; r++) n[r] = NM[r][0] * in.normal[0] + NM[r][1] * in.normal[1] + NM[r][2] * in.normal[2];
        const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl > 0.0f)
            for (int r = 0; r < 3; r++) n[r] /= nl;

        /* texcoords as the vertex shader makes them (both from texcoord 0) */
        float tc[2][2];
        for (int tg = 0; tg < 2; tg++) {
            float t4[4];
            if (texgen_src[tg] == 1) {
                t4[0] = n[0]; t4[1] = n[1]; t4[2] = n[2]; t4[3] = 1.0f;
            } else {
                t4[0] = in.texcoord[0][0]; t4[1] = in.texcoord[0][1]; t4[2] = 0.0f; t4[3] = 1.0f;
            }
            if (texmtx_on[tg]) {
                tc[tg][0] = texrow[tg][0][0] * t4[0] + texrow[tg][0][1] * t4[1] + texrow[tg][0][2] * t4[2] +
                            texrow[tg][0][3] * t4[3];
                tc[tg][1] = texrow[tg][1][0] * t4[0] + texrow[tg][1][1] * t4[1] + texrow[tg][1][2] * t4[2] +
                            texrow[tg][1][3] * t4[3];
            } else {
                tc[tg][0] = t4[0];
                tc[tg][1] = t4[1];
            }
        }
        v.uv[0] = tc[tc_src_primary][0];
        v.uv[1] = tc[tc_src_primary][1];

        /* the rasterised colour (the fragment shader's lighting, per vertex) */
        const float vc[4] = {in.color0[0] / 255.0f, in.color0[1] / 255.0f, in.color0[2] / 255.0f,
                             in.color0[3] / 255.0f};
        float ras[4] = {1, 1, 1, 1};
        if (num_chans != 0) {
            const float* matC = mat_src ? vc : g_gx.chan_mat_color[0];
            const float* ambC = amb_src ? vc : g_gx.chan_amb_color[0];
            if (light_en) {
                float acc[3] = {ambC[0], ambC[1], ambC[2]};
                for (int l = 0; l < 8; l++) {
                    if (!(light_mask & (1 << l))) continue;
                    float d = n[0] * Ldir[l][0] + n[1] * Ldir[l][1] + n[2] * Ldir[l][2];
                    d = fminf(fmaxf(d, 0.0f), 1.0f);
                    for (int c = 0; c < 3; c++) acc[c] += d * g_gx.lights[l].color[c];
                }
                for (int c = 0; c < 3; c++) ras[c] = matC[c] * fminf(fmaxf(acc[c], 0.0f), 1.0f);
            } else {
                for (int c = 0; c < 3; c++) ras[c] = matC[c];
            }
            const float matA = alpha_mat_src ? vc[3] : g_gx.chan_mat_color[0][3];
            ras[3] = alpha_light_en ? matA * g_gx.chan_amb_color[0][3] : matA;
        }

        /* the TEV */
        Lin4 prev = l4const(g_gx.tev_colors[0]), r0 = l4const(g_gx.tev_colors[1]), r1 = l4const(g_gx.tev_colors[2]),
             r2 = l4const(g_gx.tev_colors[3]);
        const Lin4 ras4 = l4const(ras);
        for (int s = 0; s < nst; s++) {
            const PCGXTevStage& ts = g_gx.tev_stages[s];
            Lin4 tex;
            if (s == primary) {
                for (int c = 0; c < 4; c++) tex.v[c] = Lin{1.0f, 0.0f, c};
            } else if (stage_tex[s]) {
                tex = l4const(stage_tex[s]->avg);
            } else {
                const float one[4] = {1, 1, 1, 1};
                tex = l4const(one);
            }
            const Lin4 sTex = swap4(tex, g_gx.tev_swap_table[ts.tex_swap & 3]);
            const Lin4 sRas = swap4(ras4, g_gx.tev_swap_table[ts.ras_swap & 3]);
            Lin4 out;
            for (int c = 0; c < 3; c++) {
                const Lin a = tev_c(ts.color_a, c, prev, sTex, sRas, r0, r1, r2, ts.k_color_sel);
                const Lin b = tev_c(ts.color_b, c, prev, sTex, sRas, r0, r1, r2, ts.k_color_sel);
                const Lin cc = tev_c(ts.color_c, c, prev, sTex, sRas, r0, r1, r2, ts.k_color_sel);
                const Lin d = tev_c(ts.color_d, c, prev, sTex, sRas, r0, r1, r2, ts.k_color_sel);
                const Lin blendv = ladd(a, lmul(lsub(b, a), cc));
                Lin r = ts.color_op == 1 ? lsub(d, blendv) : ladd(d, blendv);
                r = bias_scale(r, ts.color_bias, ts.color_scale);
                if (ts.color_clamp) r = lclamp(r);
                out.v[c] = r;
            }
            {
                const Lin a = tev_a(ts.alpha_a, prev, sTex, sRas, r0, r1, r2, ts.k_alpha_sel);
                const Lin b = tev_a(ts.alpha_b, prev, sTex, sRas, r0, r1, r2, ts.k_alpha_sel);
                const Lin cc = tev_a(ts.alpha_c, prev, sTex, sRas, r0, r1, r2, ts.k_alpha_sel);
                const Lin d = tev_a(ts.alpha_d, prev, sTex, sRas, r0, r1, r2, ts.k_alpha_sel);
                const Lin blendv = ladd(a, lmul(lsub(b, a), cc));
                Lin r = ts.alpha_op == 1 ? lsub(d, blendv) : ladd(d, blendv);
                r = bias_scale(r, ts.alpha_bias, ts.alpha_scale);
                if (ts.alpha_clamp) r = lclamp(r);
                out.v[3] = r;
            }
            Lin4* dst_c = ts.color_out == 0 ? &prev : ts.color_out == 1 ? &r0 : ts.color_out == 2 ? &r1 : &r2;
            Lin4* dst_a = ts.alpha_out == 0 ? &prev : ts.alpha_out == 1 ? &r0 : ts.alpha_out == 2 ? &r1 : &r2;
            for (int c = 0; c < 3; c++) dst_c->v[c] = out.v[c];
            dst_a->v[3] = out.v[3];
        }

        /* fog, per vertex: colour * (1 - f) + fog * f */
        float f = 0.0f;
        if (fog_on) {
            const float den = fmaxf(fog_e - fog_s, 1e-6f);
            f = fminf(fmaxf((-eye[2] - fog_s) / den, 0.0f), 1.0f);
        }
        for (int c = 0; c < 4; c++) {
            Lin val = prev.v[c];
            if (c < 3 && f > 0.0f) val = ladd(lscale(val, 1.0f - f), lconst(g_gx.fog_color[c] * f));
            /* the part's T is channel c (alpha: a); another channel becomes its average */
            const int want = c;
            if (val.src >= 0 && !same_channel(val.src, want)) val = lflatten(val);
            if (val.src < 0 || !s_T) {
                v.mul[c] = 0.0f;
                v.add[c] = val.src < 0 ? val.c : val.k + val.c;
            } else {
                v.mul[c] = val.k;
                v.add[c] = val.c;
            }
        }
        s_v.push(v);
    }

    /* --- indices (quads, strips and fans to a list) --- */
    s_idx.clear();
    if (prim == GX_QUADS) {
        for (int q = 0; q + 3 < count; q += 4) {
            const uint32_t a = q, b = q + 1, c = q + 2, d = q + 3;
            s_idx.push(a); s_idx.push(b); s_idx.push(c);
            s_idx.push(a); s_idx.push(c); s_idx.push(d);
        }
    } else if (prim == GX_TRIANGLESTRIP) {
        for (int k = 0; k + 2 < count; k++) {
            if (k & 1) { s_idx.push(k + 1); s_idx.push(k); s_idx.push(k + 2); }
            else { s_idx.push(k); s_idx.push(k + 1); s_idx.push(k + 2); }
        }
    } else if (prim == GX_TRIANGLEFAN) {
        for (int k = 1; k + 1 < count; k++) { s_idx.push(0); s_idx.push(k); s_idx.push(k + 1); }
    } else {
        for (int k = 0; k + 2 < count; k += 3) { s_idx.push(k); s_idx.push(k + 1); s_idx.push(k + 2); }
    }
    if (!s_idx.n) return;
    s_draws_frame++;
    s_verts_frame += (unsigned)count;

    const bool decal = blend && (m.flags & F_DEPTH_TEST) && !(m.flags & F_DEPTH_WRITE);
#if defined(PC_MIRROR_HOST)
    scene_mirror::Draw d;
    d.vertices = s_v.p;
    d.vertex_count = (uint32_t)s_v.n;
    d.indices = s_idx.p;
    d.index_count = (uint32_t)s_idx.n;
    d.material = m;
    d.clockwise_front = false;
    d.decal = decal;
    if (decal && s_have_view) {
        d.decal_normal[0] = s_view[1];
        d.decal_normal[1] = s_view[5];
        d.decal_normal[2] = s_view[9];
    }
    {
        uint64_t h = 1469598103934665603ull;
        auto mixf = [&h](float x) {
            uint32_t u;
            memcpy(&u, &x, 4);
            h = (h ^ u) * 1099511628211ull;
        };
        for (int c = 0; c < 3; c++) mixf(src[0].position[c]);
        mixf((float)count);
        mixf(MV[0][3]);
        mixf(MV[1][3]);
        mixf(MV[2][3]);
        d.pass_key = h;
    }
    scene_mirror::add_draw(d);
#else
    (void)decal;
    if ((int)s_frame_count == s_dump_frame) {
        DumpPart part{(uint32_t)s_dump_i.n, (uint32_t)s_idx.n, m};
        const uint32_t base = (uint32_t)s_dump_v.n;
        for (size_t i = 0; i < s_v.n; i++) s_dump_v.push(s_v.p[i]);
        for (size_t i = 0; i < s_idx.n; i++) s_dump_i.push(base + s_idx.p[i]);
        s_dump_parts.push(part);
    }
#endif
}

#endif /* PC_METAL */
