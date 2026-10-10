// pc_metal_msl.inc - Metal Shading Language for the GL-on-Metal shim
// (pc_gl_metal.mm). TEV is a line-by-line port of pc/shaders/default.vert and
// default.frag (the uber-shader, no specialization); keep them in step.
// Uniforms live in one block of 16-byte slots whose order matches
// kTevUniforms[] in pc_gl_metal.mm.
static const char* kPCMetalSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct TevU {
    float4 projection[4];
    float4 modelview[4];
    float4 normal_mtx[3];
    float4 texmtx_row0[2];
    float4 texmtx_row1[2];
    int4   texmtx_enable[2];
    int4   texgen_src[2];
    int4   fog_enable;
    float4 fog_params;
    float4 fog_color;
    float4 tev_prev;
    float4 tev_reg0;
    float4 tev_reg1;
    float4 tev_reg2;
    int4   num_tev_stages;
    int4   tev_color_in[3];
    int4   tev_alpha_in[3];
    int4   tev_color_op[3];
    int4   tev_alpha_op[3];
    int4   tev_tc_src[3];
    int4   tev_bsc[3];
    int4   tev_out[3];
    int4   tev_swap[3];
    int4   tev_ind_cfg[3];
    int4   tev_ind_wrap[3];
    int4   use_texture[3];
    int4   lighting_cfg0;
    int4   lighting_cfg1;
    float4 chan_color[2];
    float4 light_pos[8];
    float4 light_color[8];
    float4 kcolor[4];
    int4   tev_ksel[3];
    int4   swap_table[4];
    int4   alpha_ctrl;
    int4   alpha_refs;
    int4   num_ind_stages;
    float4 ind_scale[4];
    float4 ind_mtx_r0[3];
    float4 ind_mtx_r1[3];
    float4 modelview_r[4];   // single-pass stereo: the right eye's (pcm_stereo_right)
    float4 projection_r[4];
};

// Single-pass stereo (pc_metal_stereo.h): each eye's placement in the side-by-side target,
// clip x' = xf.x * x + xf.y * w, clip y' = xf.z * y + xf.w * w; cfg.x on, cfg.y one eye only (-1: both).
struct StereoU {
    float4 xf[2];
    int4 cfg;
};

struct TevVIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float4 color0   [[attribute(2)]];
    float2 texcoord [[attribute(3)]];
};

struct TevVOut {
    float4 position [[position]];
    float  point_size [[point_size]];
    float  clip_distance [[clip_distance]] [2];
    float4 color [[user(tev_color)]];
    float2 tc0 [[user(tev_tc0)]];
    float2 tc1 [[user(tev_tc1)]];
    float3 normal [[user(tev_normal)]];
    float  fog_z [[user(tev_fog_z)]];
};

struct TevFIn {
    float4 position [[position]];
    float4 color [[user(tev_color)]];
    float2 tc0 [[user(tev_tc0)]];
    float2 tc1 [[user(tev_tc1)]];
    float3 normal [[user(tev_normal)]];
    float  fog_z [[user(tev_fog_z)]];
};

static float4x4 mat4_of(constant float4* c) { return float4x4(c[0], c[1], c[2], c[3]); }

// Puts a clip position into its eye's half of a single-pass stereo target, clipped to that eye.
static float4 stereo_place(float4 clip, int eye, constant StereoU& st, thread float* dist) {
    dist[0] = 1.0;
    dist[1] = 1.0;
    if (st.cfg.x == 0) return clip;
    dist[0] = clip.w + clip.x;
    dist[1] = clip.w - clip.x;
    float4 x = st.xf[eye & 1];
    clip.x = x.x * clip.x + x.y * clip.w;
    clip.y = x.z * clip.y + x.w * clip.w;
    return clip;
}

vertex TevVOut tev_vertex(TevVIn in [[stage_in]], constant TevU& u [[buffer(0)]],
                          constant StereoU& st [[buffer(2)]], uint iid [[instance_id]]) {
    TevVOut o;
    const int eye = st.cfg.y >= 0 ? st.cfg.y : int(iid);
    const bool right = st.cfg.x != 0 && eye == 1;
    float4 eyep = (right ? mat4_of(u.modelview_r) : mat4_of(u.modelview)) * float4(in.position, 1.0);
    float4 clip = (right ? mat4_of(u.projection_r) : mat4_of(u.projection)) * eyep;
    clip = stereo_place(clip, eye, st, o.clip_distance);
    clip.z = (clip.z + clip.w) * 0.5;   // GL clip z [-w,w] -> Metal [0,w]
    o.position = clip;
    float4 eye4 = eyep;
    o.point_size = 1.0;
    o.fog_z = -eye4.z;
    o.color = in.color0;
    float3x3 nm = float3x3(u.normal_mtx[0].xyz, u.normal_mtx[1].xyz, u.normal_mtx[2].xyz);
    float3 n = nm * in.normal;
    float nl = length(n);
    o.normal = nl > 0.0 ? n / nl : n;
    float4 tc0 = (u.texgen_src[0].x == 1) ? float4(o.normal, 1.0) : float4(in.texcoord, 0.0, 1.0);
    o.tc0 = (u.texmtx_enable[0].x != 0) ? float2(dot(u.texmtx_row0[0], tc0), dot(u.texmtx_row1[0], tc0)) : tc0.xy;
    float4 tc1 = (u.texgen_src[1].x == 1) ? float4(o.normal, 1.0) : float4(in.texcoord, 0.0, 1.0);
    o.tc1 = (u.texmtx_enable[1].x != 0) ? float2(dot(u.texmtx_row0[1], tc1), dot(u.texmtx_row1[1], tc1)) : tc1.xy;
    return o;
}

static float3 getKonstC(int sel, constant TevU& u) {
    if (sel <= 7)  return float3(float(8 - sel) / 8.0);
    if (sel <= 11) return float3(0.0);
    if (sel <= 15) return u.kcolor[sel - 12].rgb;
    int ki = (sel - 16) & 3;
    int ch = (sel - 16) >> 2;
    switch (ch) {
        case 0:  return float3(u.kcolor[ki].r);
        case 1:  return float3(u.kcolor[ki].g);
        case 2:  return float3(u.kcolor[ki].b);
        default: return float3(u.kcolor[ki].a);
    }
}

static float getKonstA(int sel, constant TevU& u) {
    if (sel <= 7)  return float(8 - sel) / 8.0;
    if (sel <= 15) return 0.0;
    int ki = (sel - 16) & 3;
    int ch = (sel - 16) >> 2;
    switch (ch) {
        case 0:  return u.kcolor[ki].r;
        case 1:  return u.kcolor[ki].g;
        case 2:  return u.kcolor[ki].b;
        default: return u.kcolor[ki].a;
    }
}

static float3 getTevC(int id, float4 prev, float4 tex, float4 ras, float4 r0, float4 r1, float4 r2, float3 konst) {
    switch (id) {
        case 0:  return prev.rgb;
        case 1:  return float3(prev.a);
        case 2:  return r0.rgb;
        case 3:  return float3(r0.a);
        case 4:  return r1.rgb;
        case 5:  return float3(r1.a);
        case 6:  return r2.rgb;
        case 7:  return float3(r2.a);
        case 8:  return tex.rgb;
        case 9:  return float3(tex.a);
        case 10: return ras.rgb;
        case 11: return float3(ras.a);
        case 12: return float3(1.0);
        case 13: return float3(0.5);
        case 14: return konst;
        default: return float3(0.0);
    }
}

static float getTevA(int id, float prev, float tex, float ras, float r0, float r1, float r2, float konst) {
    switch (id) {
        case 0:  return prev;
        case 1:  return r0;
        case 2:  return r1;
        case 3:  return r2;
        case 4:  return tex;
        case 5:  return ras;
        case 6:  return konst;
        default: return 0.0;
    }
}

static float4 applySwap(float4 v, int4 sw) {
    return float4(v[sw.x & 3], v[sw.y & 3], v[sw.z & 3], v[sw.w & 3]);
}

static float4 tevStage(int4 cin, int cop, int4 ain, int aop, float4 prev, float4 tex, float4 ras,
                       float4 r0, float4 r1, float4 r2, float3 kc, float ka) {
    float3 ca = getTevC(cin.x, prev, tex, ras, r0, r1, r2, kc);
    float3 cb = getTevC(cin.y, prev, tex, ras, r0, r1, r2, kc);
    float3 cc = getTevC(cin.z, prev, tex, ras, r0, r1, r2, kc);
    float3 cd = getTevC(cin.w, prev, tex, ras, r0, r1, r2, kc);
    float3 blend = mix(ca, cb, cc);
    float3 cr = (cop == 1) ? (cd - blend) : (cd + blend);
    float aa = getTevA(ain.x, prev.a, tex.a, ras.a, r0.a, r1.a, r2.a, ka);
    float ab = getTevA(ain.y, prev.a, tex.a, ras.a, r0.a, r1.a, r2.a, ka);
    float ac = getTevA(ain.z, prev.a, tex.a, ras.a, r0.a, r1.a, r2.a, ka);
    float ad = getTevA(ain.w, prev.a, tex.a, ras.a, r0.a, r1.a, r2.a, ka);
    float ab2 = mix(aa, ab, ac);
    float ar = (aop == 1) ? (ad - ab2) : (ad + ab2);
    return float4(cr, ar);
}

static float4 applyBSC(float4 v, int4 bsc) {
    switch (bsc.x) { case 1: v.rgb += 0.5; break; case 2: v.rgb -= 0.5; break; default: break; }
    switch (bsc.y) { case 1: v.rgb *= 2.0; break; case 2: v.rgb *= 4.0; break; case 3: v.rgb *= 0.5; break; default: break; }
    switch (bsc.z) { case 1: v.a += 0.5; break; case 2: v.a -= 0.5; break; default: break; }
    switch (bsc.w) { case 1: v.a *= 2.0; break; case 2: v.a *= 4.0; break; case 3: v.a *= 0.5; break; default: break; }
    return v;
}

static void writeToReg(float4 val, int4 oc, thread float4& prev, thread float4& r0, thread float4& r1, thread float4& r2) {
    float3 rgb = (oc.x != 0) ? clamp(val.rgb, 0.0, 1.0) : val.rgb;
    float a = (oc.y != 0) ? clamp(val.a, 0.0, 1.0) : val.a;
    switch (oc.z) { case 0: prev.rgb = rgb; break; case 1: r0.rgb = rgb; break; case 2: r1.rgb = rgb; break; default: r2.rgb = rgb; break; }
    switch (oc.w) { case 0: prev.a = a; break; case 1: r0.a = a; break; case 2: r1.a = a; break; default: r2.a = a; break; }
}

static float2 applyIndirect(int4 cfg, int4 wrap, float2 coord, float2 prev_off, float2 tc0, float2 tc1,
                            constant TevU& u, texture2d<float> it0, sampler is0, texture2d<float> it1, sampler is1) {
    int mid = cfg.y;
    if (mid == 0) return coord;
    float3 s = (cfg.x == 0) ? it0.sample(is0, tc0).rgb : it1.sample(is1, tc1).rgb;
    int bias = cfg.z;
    float3 b = s;
    if ((bias & 1) != 0) b.x -= 0.5;
    if ((bias & 2) != 0) b.y -= 0.5;
    if ((bias & 4) != 0) b.z -= 0.5;
    float2 off = float2(0.0);
    if (mid >= 1 && mid <= 3) {
        int i = mid - 1;
        off.x = dot(u.ind_mtx_r0[i].xyz, b);
        off.y = dot(u.ind_mtx_r1[i].xyz, b);
    } else if (mid >= 5 && mid <= 7) {
        int i = mid - 5;
        off.x = dot(u.ind_mtx_r0[i].xyz, b);
    } else if (mid >= 9 && mid <= 11) {
        int i = mid - 9;
        off.y = dot(u.ind_mtx_r1[i].xyz, b);
    }
    if (wrap.z != 0) off += prev_off;
    float2 w = coord;
    if (wrap.x == 6) w.x = 0.0;
    else if (wrap.x >= 1 && wrap.x <= 5) {
        float ws = float(1 << (9 - wrap.x));
        float c = coord.x * 256.0;
        w.x = (c - ws * floor(c / ws)) / 256.0;
    }
    if (wrap.y == 6) w.y = 0.0;
    else if (wrap.y >= 1 && wrap.y <= 5) {
        float ws = float(1 << (9 - wrap.y));
        float c = coord.y * 256.0;
        w.y = (c - ws * floor(c / ws)) / 256.0;
    }
    return w + off;
}

static bool alphaTest(int comp, float val, float ref) {
    const float EPS = 0.5 / 255.0;
    switch (comp) {
        case 0:  return false;
        case 1:  return val < ref;
        case 2:  return abs(val - ref) < EPS;
        case 3:  return val <= ref;
        case 4:  return val > ref;
        case 5:  return abs(val - ref) >= EPS;
        case 6:  return val >= ref;
        default: return true;
    }
}

fragment float4 tev_fragment(TevFIn in [[stage_in]], constant TevU& u [[buffer(0)]],
                             texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]],
                             texture2d<float> t1 [[texture(1)]], sampler s1 [[sampler(1)]],
                             texture2d<float> t2 [[texture(2)]], sampler s2 [[sampler(2)]],
                             texture2d<float> it0 [[texture(3)]], sampler is0 [[sampler(3)]],
                             texture2d<float> it1 [[texture(4)]], sampler is1 [[sampler(4)]]) {
    float2 tc[2] = { in.tc0, in.tc1 };
    float2 stc[3] = { float2(0.0), float2(0.0), float2(0.0) };
    int nst = min(u.num_tev_stages.x, 3);
    {
        float2 prev_off = float2(0.0);
        for (int s = 0; s < nst; s++) {
            float2 base = tc[u.tev_tc_src[s].x & 1];
            stc[s] = base;
            if (u.num_ind_stages.x > 0 && u.tev_ind_cfg[s].y != 0) {
                stc[s] = applyIndirect(u.tev_ind_cfg[s], u.tev_ind_wrap[s], stc[s], prev_off, tc[0], tc[1],
                                       u, it0, is0, it1, is1);
                prev_off = stc[s] - base;
            }
        }
    }
    float4 texColor[3];
    texColor[0] = (u.use_texture[0].x != 0) ? t0.sample(s0, stc[0]) : float4(1.0);
    texColor[1] = (u.use_texture[1].x != 0) ? t1.sample(s1, stc[1]) : float4(1.0);
    texColor[2] = (u.use_texture[2].x != 0) ? t2.sample(s2, stc[2]) : float4(1.0);

    float4 ras;
    if (u.lighting_cfg0.w == 0) {
        ras = float4(1.0);
    } else {
        float3 matC = (u.lighting_cfg0.y != 0) ? in.color.rgb : u.chan_color[0].rgb;
        float3 ambC = (u.lighting_cfg0.z != 0) ? in.color.rgb : u.chan_color[1].rgb;
        if (u.lighting_cfg0.x != 0) {
            float3 acc = ambC;
            for (int i = 0; i < 8; i++) {
                if ((u.lighting_cfg1.z & (1 << i)) != 0) {
                    float3 L = normalize(u.light_pos[i].xyz);
                    float d = clamp(dot(in.normal, L), 0.0, 1.0);
                    acc += d * u.light_color[i].rgb;
                }
            }
            ras.rgb = matC * clamp(acc, 0.0, 1.0);
        } else {
            ras.rgb = matC;
        }
        float matA = (u.lighting_cfg1.y != 0) ? in.color.a : u.chan_color[0].a;
        ras.a = (u.lighting_cfg1.x != 0) ? matA * u.chan_color[1].a : matA;
    }

    float4 prev = u.tev_prev;
    float4 r0 = u.tev_reg0;
    float4 r1 = u.tev_reg1;
    float4 r2 = u.tev_reg2;
    for (int s = 0; s < nst; s++) {
        int4 sw = u.tev_swap[s];
        float4 sTex = applySwap(texColor[s], u.swap_table[sw.y & 3]);
        float4 sRas = applySwap(ras, u.swap_table[sw.x & 3]);
        float3 kc = getKonstC(u.tev_ksel[s].x, u);
        float ka = getKonstA(u.tev_ksel[s].y, u);
        float4 r = tevStage(u.tev_color_in[s], u.tev_color_op[s].x, u.tev_alpha_in[s], u.tev_alpha_op[s].x,
                            prev, sTex, sRas, r0, r1, r2, kc, ka);
        r = applyBSC(r, u.tev_bsc[s]);
        writeToReg(r, u.tev_out[s], prev, r0, r1, r2);
    }

    if (u.alpha_ctrl.x != 7 || u.alpha_ctrl.z != 7) {
        float ref0 = float(u.alpha_refs.x) / 255.0;
        float ref1 = float(u.alpha_refs.y) / 255.0;
        bool p0 = alphaTest(u.alpha_ctrl.x, prev.a, ref0);
        bool p1 = alphaTest(u.alpha_ctrl.z, prev.a, ref1);
        bool pass;
        switch (u.alpha_ctrl.y) {
            case 0:  pass = p0 && p1; break;
            case 1:  pass = p0 || p1; break;
            case 2:  pass = p0 != p1; break;
            default: pass = p0 == p1; break;
        }
        if (!pass) discard_fragment();
    }

    float4 outc = prev;
    if (u.fog_enable.x != 0) {
        float den = max(u.fog_params.z - u.fog_params.y, 1e-6);
        float f = clamp((in.fog_z - u.fog_params.y) / den, 0.0, 1.0);
        outc.rgb = mix(outc.rgb, u.fog_color.rgb, f);
    }
    return outc;
}

// NES screen blit (pc_nes_fixnes.c): pos.xy in clip space, uv.
struct BlitVIn {
    float2 pos [[attribute(0)]];
    float2 uv  [[attribute(1)]];
};
struct BlitVOut {
    float4 position [[position]];
    float2 uv;
};
vertex BlitVOut blit_vertex(BlitVIn in [[stage_in]]) {
    BlitVOut o;
    o.position = float4(in.pos, 0.5, 1.0);
    o.uv = in.uv;
    return o;
}
fragment float4 blit_fragment(BlitVOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return t0.sample(s0, in.uv);
}

// The shim's own textured rectangle (glBlitFramebuffer, the room's backdrop): a
// triangle strip of 4 vertices, dst in NDC, src in texture coordinates.
struct RectU {
    float4 dst;
    float4 src;
    float4 tint;
};
vertex BlitVOut rect_vertex(uint vid [[vertex_id]], constant RectU& u [[buffer(0)]]) {
    float2 c = float2((vid & 1u) ? 1.0 : 0.0, (vid & 2u) ? 1.0 : 0.0);
    BlitVOut o;
    o.position = float4(mix(u.dst.xy, u.dst.zw, c), 0.0, 1.0);
    o.uv = mix(u.src.xy, u.src.zw, c);
    return o;
}
fragment float4 rect_fragment(BlitVOut in [[stage_in]], constant RectU& u [[buffer(0)]],
                              texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return t0.sample(s0, in.uv) * u.tint;
}

// ---- The VR mod's programs, ported from their GLSL (keep them in step) ----

// pc_sky.cpp: a direction-only outdoor sky with painted clouds.
struct SkyU {
    float4 projection;       // P00, P11, P02, P12
    float4 eye_to_world[3];  // mat3 columns
    float4 environment;      // game seconds, overcast blend
};
struct SkyVOut {
    float4 position [[position]];
    float  clip_distance [[clip_distance]] [2];
    float2 screen [[user(sky_screen)]];
};
struct SkyFIn {
    float4 position [[position]];
    float2 screen [[user(sky_screen)]];
};
vertex SkyVOut sky_vertex(uint vid [[vertex_id]], constant StereoU& st [[buffer(2)]], uint iid [[instance_id]]) {
    SkyVOut o;
    int id = int(vid);
    o.screen = float2(float((id << 1) & 2), float(id & 2)) * 2.0 - 1.0;
    // GL z = w: the far plane, Metal (z + w) / 2 = w. Drawn per eye in a stereo target.
    float4 clip = stereo_place(float4(o.screen, 1.0, 1.0), st.cfg.y >= 0 ? st.cfg.y : int(iid), st, o.clip_distance);
    o.position = float4(clip.xy, 1.0, 1.0);
    return o;
}
static float sky_glsl_mod(float x, float y) { return x - y * floor(x / y); }
static float sky_hash(float n) { return fract(sin(n * 127.1 + 311.7) * 43758.5453); }
static float sky_oval(float2 p, float2 center, float2 size) { return length((p - center) / size); }
static float sky_cloud(float2 p, float seed) {
    p.x += 0.018 * sin(p.y * 6.0 + seed * 9.0);
    p.y += 0.008 * sin(p.x * 9.0 + seed * 15.0);
    float d = sky_oval(p, float2(0.0, -0.10), float2(1.0, 0.30));
    d = min(d, sky_oval(p, float2(-0.49, 0.04), float2(0.43, 0.39)));
    d = min(d, sky_oval(p, float2(-0.10, 0.23), float2(0.48, 0.57)));
    d = min(d, sky_oval(p, float2(0.36, 0.13 + 0.10 * seed), float2(0.46, 0.43)));
    d = min(d, sky_oval(p, float2(0.73, -0.04), float2(0.35, 0.26)));
    return 1.0 - smoothstep(0.86, 1.08, d);
}
fragment float4 sky_fragment(SkyFIn in [[stage_in]], constant SkyU& u [[buffer(0)]]) {
    const float PI = 3.14159265359;
    float3x3 eye_to_world = float3x3(u.eye_to_world[0].xyz, u.eye_to_world[1].xyz, u.eye_to_world[2].xyz);
    float3 eye = float3((in.screen + u.projection.zw) / u.projection.xy, -1.0);
    float3 ray = normalize(eye_to_world * eye);
    float h = ray.y;
    float azimuth = atan2(ray.x, ray.z) / (2.0 * PI) + 0.5;
    float hour = u.environment.x / 3600.0;
    float wet = clamp(u.environment.y, 0.0, 1.0);
    float day = smoothstep(5.0, 7.5, hour) * (1.0 - smoothstep(17.0, 20.0, hour));
    float dusk = max(1.0 - abs(hour - 6.0) / 1.5, 1.0 - abs(hour - 18.5) / 1.5);
    dusk = max(dusk, 0.0) * (1.0 - wet);
    float3 zenith = mix(float3(0.018, 0.032, 0.095), float3(0.065, 0.29, 0.84), day);
    float3 horizon = mix(float3(0.065, 0.10, 0.19), float3(0.65, 0.85, 0.96), day);
    horizon = mix(horizon, float3(0.96, 0.56, 0.37), dusk * 0.72);
    zenith = mix(zenith, mix(float3(0.045, 0.055, 0.075), float3(0.36, 0.43, 0.51), day), wet);
    horizon = mix(horizon, mix(float3(0.10, 0.12, 0.15), float3(0.65, 0.70, 0.74), day), wet);
    float3 sky = mix(horizon, zenith, pow(clamp(h, 0.0, 1.0), 0.48));
    sky = mix(sky, horizon * mix(0.55, 0.77, day), smoothstep(0.0, 0.20, -h));

    float ridge = 0.006 + 0.015 * pow(max(0.0, sin(azimuth * 2.0 * PI * 7.0 + 1.2)), 4.0)
                + 0.009 * pow(max(0.0, sin(azimuth * 2.0 * PI * 19.0)), 6.0);
    float islands = smoothstep(0.52, 0.75, sin(azimuth * 2.0 * PI * 5.0));
    float land = (1.0 - smoothstep(ridge - 0.002, ridge + 0.002, h)) * smoothstep(-0.012, 0.0, h) * islands;
    sky = mix(sky, horizon * 0.76, land * (1.0 - wet * 0.5));

    if (h > 0.015 && h < 0.87) {
        float latitude = asin(h);
        for (int row = 0; row < 3; ++row) {
            float r = float(row);
            float low = 0.055 + r * 0.22;
            float high = low + 0.045 + r * 0.11;
            float extent = 1.2 * (0.025 + r * 0.065) + 0.00001;
            if (latitude < low - extent || latitude > high + extent) continue;
            float count = 18.0 - r * 5.0;
            float x = (azimuth + u.environment.x / 86400.0) * count + r * 2.71;
            float cell = floor(x);
            for (int neighbor = -1; neighbor <= 1; ++neighbor) {
                float id = sky_glsl_mod(cell + float(neighbor), count);
                float seed = sky_hash(id + r * 39.0);
                float center = 0.5 + (seed - 0.5) * 0.32;
                float elevation = 0.055 + r * 0.22 + sky_hash(id + 93.0 + r * 17.0) * (0.045 + r * 0.11);
                float2 p = float2((x - cell - float(neighbor) - center) / (0.22 + seed * 0.14),
                                  (latitude - elevation) / (0.025 + r * 0.065));
                if (abs(p.x) < 1.4 && abs(p.y) < 1.2 && (row == 0 || seed > 0.15)) {
                    float cover = sky_cloud(p, seed) * mix(0.78, 0.96, r / 2.0);
                    float3 shade = mix(float3(0.12, 0.16, 0.25), float3(0.64, 0.76, 0.93), day);
                    float3 lit = mix(float3(0.23, 0.28, 0.38), float3(0.98, 0.98, 0.94), day);
                    lit = mix(lit, float3(0.98, 0.72, 0.57), dusk * 0.65);
                    float3 puff = mix(shade, lit, smoothstep(-0.16, 0.42, p.y));
                    puff = mix(puff, horizon * 0.94, wet * 0.85);
                    sky = mix(sky, puff, cover);
                }
            }
        }
    }
    return float4(sky, 1.0);
}

// pc_vr.cpp's UI panel: the UI target as a quad in the seated frame.
struct PanelU {
    float4 mvp[4];
    float4 half_extent;  // u_half
    float4 center;       // u_center
};
struct PanelVIn {
    float2 pos [[attribute(0)]];
    float2 uv  [[attribute(1)]];
};
vertex BlitVOut panel_vertex(PanelVIn in [[stage_in]], constant PanelU& u [[buffer(0)]]) {
    float3 p = u.center.xyz + float3(in.pos.x * u.half_extent.x, in.pos.y * u.half_extent.y, 0.0);
    float4 clip = mat4_of(u.mvp) * float4(p, 1.0);
    clip.z = (clip.z + clip.w) * 0.5;
    BlitVOut o;
    o.position = clip;
    o.uv = float2(in.uv.x, 1.0 - in.uv.y);  // a render target: GL's v = 0 is its bottom row
    return o;
}
fragment float4 panel_fragment(BlitVOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]]) {
    return t0.sample(s0, in.uv);
}

// pc_vr_hands.cpp: white controller-grip markers.
struct HandsU {
    float4 eye_from_grip[4];
    float4 gx_projection[4];
    float4 mirror_hand;
};
struct HandsVIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
};
struct HandsVOut {
    float4 position [[position]];
    float3 eye_normal;
};
vertex HandsVOut hands_vertex(HandsVIn in [[stage_in]], constant HandsU& u [[buffer(0)]]) {
    float3 mirror = float3(u.mirror_hand.x, 1.0, 1.0);
    float4x4 e = mat4_of(u.eye_from_grip);
    float4 clip = mat4_of(u.gx_projection) * e * float4(in.position * mirror, 1.0);
    clip.z = 2.0 * clip.z + clip.w;     // GX [-1, 0] -> GL [-1, 1]
    clip.z = (clip.z + clip.w) * 0.5;   // GL -> Metal
    HandsVOut o;
    o.position = clip;
    o.eye_normal = float3x3(e[0].xyz, e[1].xyz, e[2].xyz) * (in.normal * mirror);
    return o;
}
fragment float4 hands_fragment(HandsVOut in [[stage_in]]) {
    float3 light = normalize(float3(-0.35, 0.75, 0.60));
    float shade = 0.82 + 0.18 * max(dot(normalize(in.eye_normal), light), 0.0);
    return float4(float3(shade), 1.0);
}
)MSL";
