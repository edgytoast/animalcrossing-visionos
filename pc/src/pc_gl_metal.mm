// pc_gl_metal.mm - the OpenGL subset the PC port uses, implemented on Metal.
//
// Why: visionOS has no OpenGL. pc_gx*.c translate GX to a small, fixed slice
// of GL 3.3 (one VAO, a streamed VBO, the TEV uber-shader, texture objects,
// glReadPixels for EFB copies). Implementing that slice keeps pc_gx.c and the
// VR mod's GL code nearly unchanged while rendering straight into Metal
// textures (an IOSurface window slot, or a compositor eye).
//
// GL semantics kept: bottom-left viewport/scissor/readback origins, [-1,1]
// clip z (mapped to [0,1] in the vertex shader), per-texture sampler state,
// per-program uniform values, buffer data captured at draw time, framebuffer
// objects (a colour texture and a depth renderbuffer: the VR mod's eye and UI
// targets), separate colour/alpha blending, and the state queries the mod's
// save/restore code makes.
// Programs are recognised by their source: the TEV shader (default.frag), the
// NES blit (pc_nes_fixnes.c), and the VR mod's sky (pc_sky.cpp), UI panel
// (pc_vr.cpp) and hand markers (pc_vr_hands.cpp), each with a hand-ported MSL
// twin in pc_metal_msl.inc. Anything else draws nothing and logs once.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>
#include <unordered_map>

#include "glad/gl.h"

#include "pc_metal_msl.inc"
#include "pc_metal_stereo.h"

namespace {

// The game replaces global operator new/delete with its own heap (JKRHeap.cpp),
// which isn't up when the renderer starts and must never hold renderer state:
// every container here allocates with malloc.
template <class T>
struct MAlloc {
    using value_type = T;
    MAlloc() = default;
    template <class U>
    MAlloc(const MAlloc<U>&) {}
    T* allocate(size_t n) {
        void* p = malloc(n * sizeof(T));
        if (!p) abort();
        return (T*)p;
    }
    void deallocate(T* p, size_t) { free(p); }
    bool operator==(const MAlloc&) const { return true; }
    bool operator!=(const MAlloc&) const { return false; }
};
template <class T>
using mvec = std::vector<T, MAlloc<T>>;
using mstring = std::basic_string<char, std::char_traits<char>, MAlloc<char>>;
template <class K, class V>
using mmap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, MAlloc<std::pair<const K, V>>>;

constexpr int kFramesInFlight = 3;
constexpr size_t kRingBytes = 24u * 1024u * 1024u;
constexpr int kMaxUnits = 8;
constexpr int kMaxAttribs = 8;

enum ProgramKind { PROG_NONE = 0, PROG_TEV = 1, PROG_BLIT = 2, PROG_SKY = 3, PROG_PANEL = 4, PROG_HANDS = 5 };

struct UniformDesc {
    const char* name;
    int slot;      // 16-byte slot index into the block
    int elems;     // array length
    int kind;      // 0 = vector, 3 = mat3, 4 = mat4, -1 = sampler
};

// Order and slots must match struct TevU in pc_metal_msl.inc.
const UniformDesc kTevUniforms[] = {
    {"u_projection", 0, 1, 4},
    {"u_modelview", 4, 1, 4},
    {"u_normal_mtx", 8, 1, 3},
    {"u_texmtx_row0", 11, 2, 0},
    {"u_texmtx_row1", 13, 2, 0},
    {"u_texmtx_enable", 15, 2, 0},
    {"u_texgen_src", 17, 2, 0},
    {"u_fog_enable", 19, 1, 0},
    {"u_fog_params", 20, 1, 0},
    {"u_fog_color", 21, 1, 0},
    {"u_tev_prev", 22, 1, 0},
    {"u_tev_reg0", 23, 1, 0},
    {"u_tev_reg1", 24, 1, 0},
    {"u_tev_reg2", 25, 1, 0},
    {"u_num_tev_stages", 26, 1, 0},
    {"u_tev_color_in", 27, 3, 0},
    {"u_tev_alpha_in", 30, 3, 0},
    {"u_tev_color_op", 33, 3, 0},
    {"u_tev_alpha_op", 36, 3, 0},
    {"u_tev_tc_src", 39, 3, 0},
    {"u_tev_bsc", 42, 3, 0},
    {"u_tev_out", 45, 3, 0},
    {"u_tev_swap", 48, 3, 0},
    {"u_tev_ind_cfg", 51, 3, 0},
    {"u_tev_ind_wrap", 54, 3, 0},
    {"u_use_texture", 57, 3, 0},
    {"u_lighting_cfg0", 60, 1, 0},
    {"u_lighting_cfg1", 61, 1, 0},
    {"u_chan_color", 62, 2, 0},
    {"u_light_pos", 64, 8, 0},
    {"u_light_color", 72, 8, 0},
    {"u_kcolor", 80, 4, 0},
    {"u_tev_ksel", 84, 3, 0},
    {"u_swap_table", 87, 4, 0},
    {"u_alpha_ctrl", 91, 1, 0},
    {"u_alpha_refs", 92, 1, 0},
    {"u_num_ind_stages", 93, 1, 0},
    {"u_ind_scale", 94, 4, 0},
    {"u_ind_mtx_r0", 98, 3, 0},
    {"u_ind_mtx_r1", 101, 3, 0},
    {"u_modelview_r", 104, 1, 4},
    {"u_projection_r", 108, 1, 4},
    // samplers: fixed units (0-2 TEV, 3-6 indirect), values ignored
    {"u_texture0", -1, 1, -1},
    {"u_texture1", -1, 1, -1},
    {"u_texture2", -1, 1, -1},
    {"u_ind_tex0", -1, 1, -1},
    {"u_ind_tex1", -1, 1, -1},
    {"u_ind_tex2", -1, 1, -1},
    {"u_ind_tex3", -1, 1, -1},
};
constexpr int kTevUniformCount = sizeof(kTevUniforms) / sizeof(kTevUniforms[0]);
constexpr int kTevSlots = 112;  // + the right eye's modelview and projection (single-pass stereo)
constexpr size_t kTevBytes = kTevSlots * 16;

const UniformDesc kBlitUniforms[] = {{"tex", -1, 1, -1}};
// Slots must match SkyU / PanelU / HandsU in pc_metal_msl.inc.
const UniformDesc kSkyUniforms[] = {{"projection", 0, 1, 0}, {"eye_to_world", 1, 1, 3}, {"environment", 4, 1, 0}};
const UniformDesc kPanelUniforms[] = {
    {"u_mvp", 0, 1, 4}, {"u_half", 4, 1, 0}, {"u_center", 5, 1, 0}, {"u_tex", -1, 1, -1}};
const UniformDesc kHandsUniforms[] = {
    {"eye_from_grip", 0, 1, 4}, {"gx_projection", 4, 1, 4}, {"mirror_hand", 8, 1, 0}};
constexpr size_t kSmallUniformBytes = 9 * 16;

const UniformDesc* uniform_table(int kind, int* count) {
    switch (kind) {
        case PROG_TEV: *count = kTevUniformCount; return kTevUniforms;
        case PROG_BLIT: *count = 1; return kBlitUniforms;
        case PROG_SKY: *count = 3; return kSkyUniforms;
        case PROG_PANEL: *count = 4; return kPanelUniforms;
        case PROG_HANDS: *count = 3; return kHandsUniforms;
        default: *count = 0; return nullptr;
    }
}

struct Tex {
    bool live = false;
    void* mtl = nullptr;  // retained id<MTLTexture>
    uint64_t serial = 0;  // a new number for every Metal texture this name gets (the mirror's cache key)
    int w = 0, h = 0;
    GLenum minf = GL_NEAREST_MIPMAP_LINEAR, magf = GL_LINEAR;
    GLenum ws = GL_REPEAT, wt = GL_REPEAT;
    bool external = false;  // wraps a texture the host owns (pcm_wrap_texture): never reallocated
};

// A renderbuffer: the depth of an FBO (Depth32Float, private).
struct RBuf {
    bool live = false;
    void* mtl = nullptr;  // retained id<MTLTexture>
    int w = 0, h = 0;
};

// A framebuffer object: one colour texture (a GL texture name) and one depth renderbuffer.
struct FBuf {
    bool live = false;
    GLuint color = 0;
    GLuint depth = 0;
};

struct Buf {
    bool live = false;
    void* mtl = nullptr;  // retained id<MTLBuffer> (dedicated or ring)
    size_t off = 0;
    size_t size = 0;
};

struct Attrib {
    bool enabled = false;
    int size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    int stride = 0;
    uintptr_t offset = 0;
    GLuint buffer = 0;
};

struct VAO {
    bool live = false;
    Attrib a[kMaxAttribs];
    GLuint ebo = 0;
};

struct Shader {
    bool live = false;
    GLenum type = 0;
    mstring src;
};

struct Program {
    bool live = false;
    int kind = PROG_NONE;
    mvec<GLuint> shaders;
    uint64_t rev = 1;  // bumped by every uniform write: the encoder rebinds the block only when it moved on
    uint8_t block[kTevBytes];
};

struct State {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    id<MTLTexture> white;
    dispatch_semaphore_t inflight;

    // default framebuffer
    id<MTLTexture> color;
    id<MTLTexture> depth;
    int fbw = 0, fbh = 0;

    // the open encoder's target (the bound draw framebuffer when it opened)
    int tw = 0, th = 0;
    MTLPixelFormat tcolor_fmt = MTLPixelFormatInvalid;
    bool tdepth = false;

    // frame
    id<MTLCommandBuffer> cmd;
    id<MTLRenderCommandEncoder> enc;
    bool frame_open = false;
    int ring_index = 0;
    id<MTLBuffer> ring[kFramesInFlight];
    size_t ring_used = 0;
    bool pending_clear_color = false, pending_clear_depth = false;
    float clear_rgba[4] = {0, 0, 0, 0};
    double clear_depth = 1.0;

    // objects (index = GL name)
    mvec<Tex> textures{1};
    mvec<Buf> buffers{1};
    mvec<VAO> vaos{1};
    mvec<Shader> shaders{1};
    mvec<Program> programs{1};
    mvec<RBuf> renderbuffers{1};
    mvec<FBuf> framebuffers{1};

    // bindings
    GLuint draw_fb = 0, read_fb = 0;
    GLuint renderbuffer = 0;
    GLuint program = 0;
    GLuint vao = 0;
    GLuint array_buffer = 0;
    GLuint element_buffer_novao = 0;
    int active_unit = 0;
    GLuint unit_tex[kMaxUnits] = {0};
    int pack_alignment = 4, unpack_alignment = 4;

    // fixed-function state
    bool depth_test = false, blend = false, cull = false, scissor_test = false;
    GLenum depth_func = GL_LESS;
    bool depth_mask = true;
    bool cmask[4] = {true, true, true, true};
    GLenum cull_face = GL_BACK;
    GLenum blend_src = GL_ONE, blend_dst = GL_ZERO, blend_eq = GL_FUNC_ADD;
    GLenum blend_src_a = GL_ONE, blend_dst_a = GL_ZERO, blend_eq_a = GL_FUNC_ADD;
    int vp[4] = {0, 0, 0, 0};
    double dr_n = 0.0, dr_f = 1.0;
    int sc[4] = {0, 0, 0, 0};
    bool vp_set = false;

    // encoder-applied state tracking
    bool enc_dirty = true;
    // what the open encoder has bound, so a draw sets only what changed (the VR mod's whole-town
    // frames issue thousands of draws, twice)
    void* bound_pipeline = nullptr;
    GLuint bound_block_prog = 0;
    uint64_t bound_block_rev = 0;
    void* bound_tex[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    void* bound_samp[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    bool bound_stereo_valid = false;
    float bound_stereo[12] = {0};

    // single-pass stereo (pc_metal_stereo.h)
    GLuint stereo_fb = 0;
    int stereo_eye_w = 0;
    int stereo_only = -1;
    bool applied_full = false;  // the encoder's viewport is the whole target (a stereo draw)
    NSUInteger instances = 1;

    mmap<uint64_t, id<MTLRenderPipelineState>> pipelines;
    mmap<uint64_t, id<MTLRenderPipelineState>> rect_pipelines;
    id<MTLDepthStencilState> no_depth;
    mmap<uint32_t, id<MTLDepthStencilState>> dss;
    mmap<uint32_t, id<MTLSamplerState>> samplers;
    mmap<uint64_t, int> warned;
};

State* S = nullptr;
uint64_t g_texture_serial = 0;

void warn_once(const char* what) {
    if (!S) return;
    uint64_t k = 1469598103934665603ull;
    for (const char* c = what; *c; c++) k = (k ^ (uint8_t)*c) * 1099511628211ull;
    if (S->warned.count(k)) return;
    S->warned[k] = 1;
    fprintf(stderr, "[ACVR] metal shim: %s\n", what);
}

template <typename T>
GLuint alloc_name(mvec<T>& v) {
    for (size_t i = 1; i < v.size(); i++) {
        if (!v[i].live) {
            v[i] = T();
            v[i].live = true;
            return (GLuint)i;
        }
    }
    v.emplace_back();
    v.back().live = true;
    return (GLuint)(v.size() - 1);
}

template <typename T>
T* get_obj(mvec<T>& v, GLuint name) {
    if (name == 0 || name >= v.size() || !v[name].live) return nullptr;
    return &v[name];
}

void release_ptr(void*& p) {
    if (p) {
        CFRelease(p);
        p = nullptr;
    }
}

id<MTLTexture> tex_of(GLuint name) {
    Tex* t = get_obj(S->textures, name);
    if (!t || !t->mtl) return nil;
    return (__bridge id<MTLTexture>)t->mtl;
}

// ---- frame / encoder ----

void begin_cmd_if_needed() {
    if (S->cmd) return;
    if (!S->frame_open) {
        dispatch_semaphore_wait(S->inflight, DISPATCH_TIME_FOREVER);
        S->frame_open = true;
        S->ring_index = (S->ring_index + 1) % kFramesInFlight;
        S->ring_used = 0;
    }
    S->cmd = [S->queue commandBuffer];
}

void ensure_depth() {
    if (!S->color) return;
    if (S->depth && (int)S->depth.width == S->fbw && (int)S->depth.height == S->fbh) return;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:S->fbw
                                                                                 height:S->fbh
                                                                              mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;  // the window copies it (distances)
    d.storageMode = MTLStorageModePrivate;
    S->depth = [S->device newTextureWithDescriptor:d];
}

// A new encoder (or a draw outside the GL state) starts with nothing bound.
void forget_bindings() {
    S->bound_stereo_valid = false;
    S->bound_pipeline = nullptr;
    S->bound_block_prog = 0;
    S->bound_block_rev = 0;
    for (int i = 0; i < 5; i++) S->bound_tex[i] = S->bound_samp[i] = nullptr;
}

// A framebuffer's attachments: the default framebuffer (name 0) or an FBO.
struct Target {
    id<MTLTexture> color;
    id<MTLTexture> depth;
    int w = 0, h = 0;
};

bool resolve_framebuffer(GLuint name, Target* t) {
    if (name == 0) {
        if (!S->color) return false;
        ensure_depth();
        t->color = S->color;
        t->depth = S->depth;
        t->w = S->fbw;
        t->h = S->fbh;
        return true;
    }
    FBuf* f = get_obj(S->framebuffers, name);
    if (!f) return false;
    t->color = tex_of(f->color);
    RBuf* r = get_obj(S->renderbuffers, f->depth);
    t->depth = (r && r->mtl) ? (__bridge id<MTLTexture>)r->mtl : nil;
    if (!t->color) return false;
    t->w = (int)t->color.width;
    t->h = (int)t->color.height;
    return true;
}

bool ensure_encoder() {
    if (S->enc) return true;
    Target t;
    if (!resolve_framebuffer(S->draw_fb, &t)) {
        warn_once(S->draw_fb ? "draw into an incomplete framebuffer" : "draw with no default framebuffer");
        return false;
    }
    begin_cmd_if_needed();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t.color;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (S->pending_clear_color) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor =
            MTLClearColorMake(S->clear_rgba[0], S->clear_rgba[1], S->clear_rgba[2], S->clear_rgba[3]);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }
    if (t.depth) {
        rp.depthAttachment.texture = t.depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (S->pending_clear_depth) {
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = S->clear_depth;
        } else {
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
        }
    }
    S->pending_clear_color = S->pending_clear_depth = false;
    S->tw = t.w;
    S->th = t.h;
    S->tcolor_fmt = t.color.pixelFormat;
    S->tdepth = t.depth != nil;
    S->enc = [S->cmd renderCommandEncoderWithDescriptor:rp];
    S->enc_dirty = true;
    forget_bindings();
    return S->enc != nil;
}

// A glClear waits for the next draw into its target; before the target changes
// it runs on its own (an empty pass with clear load actions).
void flush_pending_clears() {
    if (S->pending_clear_color || S->pending_clear_depth) ensure_encoder();
}

void end_encoder() {
    if (S->enc) {
        [S->enc endEncoding];
        S->enc = nil;
    }
}

// Ends the current command buffer; wait=true blocks until the GPU is done.
void submit(bool wait) {
    flush_pending_clears();
    end_encoder();
    if (!S->cmd) return;
    id<MTLCommandBuffer> c = S->cmd;
    S->cmd = nil;
    [c commit];
    if (wait) [c waitUntilCompleted];
}

// ---- streaming ----

// Copies data into this frame's ring (or a one-off buffer when it's full).
// Returns the retained buffer and offset through out params.
void stream_copy(const void* data, size_t size, id<MTLBuffer>* out_buf, size_t* out_off) {
    begin_cmd_if_needed();
    size_t aligned = (S->ring_used + 255) & ~(size_t)255;
    if (aligned + size <= kRingBytes) {
        id<MTLBuffer> r = S->ring[S->ring_index];
        if (data) memcpy((uint8_t*)r.contents + aligned, data, size);
        S->ring_used = aligned + size;
        *out_buf = r;
        *out_off = aligned;
        return;
    }
    warn_once("stream ring full; using one-off buffers");
    id<MTLBuffer> b = data ? [S->device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared]
                           : [S->device newBufferWithLength:size options:MTLResourceStorageModeShared];
    *out_buf = b;
    *out_off = 0;
}

// ---- pipeline / state objects ----

MTLBlendFactor blend_factor(GLenum f) {
    switch (f) {
        case GL_ZERO: return MTLBlendFactorZero;
        case GL_ONE: return MTLBlendFactorOne;
        case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
        case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
        case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        default: return MTLBlendFactorOne;
    }
}

MTLVertexFormat vertex_format(const Attrib& a) {
    if (a.type == GL_FLOAT) {
        switch (a.size) {
            case 1: return MTLVertexFormatFloat;
            case 2: return MTLVertexFormatFloat2;
            case 3: return MTLVertexFormatFloat3;
            default: return MTLVertexFormatFloat4;
        }
    }
    if (a.type == GL_UNSIGNED_BYTE) {
        if (a.normalized) return a.size == 4 ? MTLVertexFormatUChar4Normalized : MTLVertexFormatUChar2Normalized;
        return a.size == 4 ? MTLVertexFormatUChar4 : MTLVertexFormatUChar2;
    }
    return MTLVertexFormatFloat4;
}

uint64_t hash_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

MTLBlendOperation blend_op(GLenum eq) {
    return eq == GL_FUNC_REVERSE_SUBTRACT ? MTLBlendOperationReverseSubtract
         : eq == GL_FUNC_SUBTRACT        ? MTLBlendOperationSubtract
                                         : MTLBlendOperationAdd;
}

int attrib_count(int kind) {
    switch (kind) {
        case PROG_TEV: return 4;
        case PROG_SKY: return 0;  // gl_VertexID only
        default: return 2;
    }
}

NSString* vertex_fn(int kind) {
    switch (kind) {
        case PROG_TEV: return @"tev_vertex";
        case PROG_SKY: return @"sky_vertex";
        case PROG_PANEL: return @"panel_vertex";
        case PROG_HANDS: return @"hands_vertex";
        default: return @"blit_vertex";
    }
}

NSString* fragment_fn(int kind) {
    switch (kind) {
        case PROG_TEV: return @"tev_fragment";
        case PROG_SKY: return @"sky_fragment";
        case PROG_PANEL: return @"panel_fragment";
        case PROG_HANDS: return @"hands_fragment";
        default: return @"blit_fragment";
    }
}

id<MTLRenderPipelineState> pipeline_for(int kind, const VAO* vao) {
    uint64_t key = (uint64_t)kind;
    int nattr = attrib_count(kind);
    for (int i = 0; i < nattr; i++) {
        const Attrib& a = vao->a[i];
        key = hash_mix(key, (uint64_t)a.enabled | ((uint64_t)a.size << 1) | ((uint64_t)a.type << 4) |
                                ((uint64_t)a.normalized << 20) | ((uint64_t)a.stride << 21) |
                                ((uint64_t)a.offset << 36));
    }
    bool blend_on = S->blend;
    key = hash_mix(key, (uint64_t)blend_on | ((uint64_t)S->blend_src << 1) | ((uint64_t)S->blend_dst << 14) |
                            ((uint64_t)S->blend_eq << 27));
    key = hash_mix(key, (uint64_t)S->blend_src_a | ((uint64_t)S->blend_dst_a << 14) |
                            ((uint64_t)S->blend_eq_a << 27));
    key = hash_mix(key, (uint64_t)S->cmask[0] | ((uint64_t)S->cmask[1] << 1) | ((uint64_t)S->cmask[2] << 2) |
                            ((uint64_t)S->cmask[3] << 3) | ((uint64_t)S->tcolor_fmt << 4) |
                            ((uint64_t)S->tdepth << 40));
    auto it = S->pipelines.find(key);
    if (it != S->pipelines.end()) return it->second;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [S->library newFunctionWithName:vertex_fn(kind)];
    d.fragmentFunction = [S->library newFunctionWithName:fragment_fn(kind)];
    if (nattr > 0) {
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        int stride = 0;
        for (int i = 0; i < nattr; i++) {
            const Attrib& a = vao->a[i];
            vd.attributes[i].format = vertex_format(a);
            vd.attributes[i].offset = a.enabled ? a.offset : 0;
            vd.attributes[i].bufferIndex = 1;
            if (a.enabled && a.stride) stride = a.stride;
        }
        if (stride == 0) stride = 16;
        vd.layouts[1].stride = stride;
        vd.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;
        d.vertexDescriptor = vd;
    }
    d.colorAttachments[0].pixelFormat = S->tcolor_fmt;
    d.depthAttachmentPixelFormat = S->tdepth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
    MTLColorWriteMask wm = MTLColorWriteMaskNone;
    if (S->cmask[0]) wm |= MTLColorWriteMaskRed;
    if (S->cmask[1]) wm |= MTLColorWriteMaskGreen;
    if (S->cmask[2]) wm |= MTLColorWriteMaskBlue;
    if (S->cmask[3]) wm |= MTLColorWriteMaskAlpha;
    d.colorAttachments[0].writeMask = wm;
    if (blend_on) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].rgbBlendOperation = blend_op(S->blend_eq);
        d.colorAttachments[0].alphaBlendOperation = blend_op(S->blend_eq_a);
        d.colorAttachments[0].sourceRGBBlendFactor = blend_factor(S->blend_src);
        d.colorAttachments[0].sourceAlphaBlendFactor = blend_factor(S->blend_src_a);
        d.colorAttachments[0].destinationRGBBlendFactor = blend_factor(S->blend_dst);
        d.colorAttachments[0].destinationAlphaBlendFactor = blend_factor(S->blend_dst_a);
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> ps = [S->device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) {
        fprintf(stderr, "[ACVR] metal shim: pipeline failed: %s\n", err.localizedDescription.UTF8String);
    }
    S->pipelines[key] = ps;
    return ps;
}

MTLCompareFunction compare_fn(GLenum f) {
    switch (f) {
        case GL_NEVER: return MTLCompareFunctionNever;
        case GL_LESS: return MTLCompareFunctionLess;
        case GL_EQUAL: return MTLCompareFunctionEqual;
        case GL_LEQUAL: return MTLCompareFunctionLessEqual;
        case GL_GREATER: return MTLCompareFunctionGreater;
        case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

id<MTLDepthStencilState> depth_state() {
    // GL: with the depth test off, nothing is written to depth either.
    uint32_t key = (S->depth_test ? 1u : 0u) | ((S->depth_mask ? 1u : 0u) << 1) | ((uint32_t)S->depth_func << 2);
    auto it = S->dss.find(key);
    if (it != S->dss.end()) return it->second;
    MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
    if (S->depth_test) {
        d.depthCompareFunction = compare_fn(S->depth_func);
        d.depthWriteEnabled = S->depth_mask;
    } else {
        d.depthCompareFunction = MTLCompareFunctionAlways;
        d.depthWriteEnabled = NO;
    }
    id<MTLDepthStencilState> st = [S->device newDepthStencilStateWithDescriptor:d];
    S->dss[key] = st;
    return st;
}

MTLSamplerAddressMode address_mode(GLenum w) {
    switch (w) {
        case GL_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
        case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        default: return MTLSamplerAddressModeRepeat;
    }
}

bool is_linear(GLenum f) { return f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR; }

id<MTLSamplerState> sampler_for(const Tex* t) {
    GLenum minf = t ? t->minf : GL_NEAREST, magf = t ? t->magf : GL_NEAREST;
    GLenum ws = t ? t->ws : GL_REPEAT, wt = t ? t->wt : GL_REPEAT;
    uint32_t key = (is_linear(minf) ? 1u : 0u) | ((is_linear(magf) ? 1u : 0u) << 1) |
                   ((uint32_t)address_mode(ws) << 2) | ((uint32_t)address_mode(wt) << 6);
    auto it = S->samplers.find(key);
    if (it != S->samplers.end()) return it->second;
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.minFilter = is_linear(minf) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = is_linear(magf) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = MTLSamplerMipFilterNotMipmapped;  // only level 0 is ever uploaded
    d.sAddressMode = address_mode(ws);
    d.tAddressMode = address_mode(wt);
    id<MTLSamplerState> s = [S->device newSamplerStateWithDescriptor:d];
    S->samplers[key] = s;
    return s;
}

// stereo: a draw to both eyes of a stereo target, whose viewport is the whole target (the
// vertex shader places each eye); otherwise the GL viewport and scissor.
void apply_encoder_state(bool stereo = false) {
    if (!S->enc_dirty && S->applied_full == stereo) return;
    S->enc_dirty = false;
    S->applied_full = stereo;
    int W = S->tw, H = S->th;
    int vx = S->vp_set ? S->vp[0] : 0, vy = S->vp_set ? S->vp[1] : 0;
    int vw = S->vp_set ? S->vp[2] : W, vh = S->vp_set ? S->vp[3] : H;
    if (stereo) {
        vx = 0;
        vy = 0;
        vw = W;
        vh = H;
    }
    MTLViewport v;
    v.originX = vx;
    v.originY = H - (vy + vh);
    v.width = vw;
    v.height = vh;
    v.znear = S->dr_n;
    v.zfar = S->dr_f;
    [S->enc setViewport:v];
    MTLScissorRect r = {0, 0, (NSUInteger)W, (NSUInteger)H};
    if (S->scissor_test && !stereo) {
        int x0 = S->sc[0], y0 = H - (S->sc[1] + S->sc[3]);
        int x1 = x0 + S->sc[2], y1 = y0 + S->sc[3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        r.x = x0;
        r.y = y0;
        r.width = x1 - x0;
        r.height = y1 - y0;
    }
    [S->enc setScissorRect:r];
    static const bool clockwise = getenv("ACVR_MTL_CW") != nullptr;
    [S->enc setFrontFacingWinding:clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise];
    MTLCullMode cm = MTLCullModeNone;
    if (S->cull) cm = S->cull_face == GL_FRONT ? MTLCullModeFront : MTLCullModeBack;
    [S->enc setCullMode:cm];
    [S->enc setDepthStencilState:depth_state()];
}

bool scissor_empty() {
    if (!S->scissor_test) return false;
    return S->sc[2] <= 0 || S->sc[3] <= 0;
}

MTLPrimitiveType prim_type(GLenum mode) {
    switch (mode) {
        case GL_POINTS: return MTLPrimitiveTypePoint;
        case GL_LINES: return MTLPrimitiveTypeLine;
        case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        default: return MTLPrimitiveTypeTriangle;
    }
}

// Common draw setup. Returns false if the draw must be skipped.
bool prepare_draw(Program** out_prog, VAO** out_vao) {
    Program* p = get_obj(S->programs, S->program);
    if (!p || p->kind == PROG_NONE) {
        if (p) warn_once("draw with an unrecognised program");
        return false;
    }
    VAO* vao = get_obj(S->vaos, S->vao);
    if (!vao) {
        warn_once("draw with no VAO");
        return false;
    }
    if (S->cull && S->cull_face == GL_FRONT_AND_BACK) return false;
    if (scissor_empty()) return false;
    if (!ensure_encoder()) return false;
    const bool stereo_kind = p->kind == PROG_TEV || p->kind == PROG_SKY;
    const bool stereo = S->stereo_fb != 0 && S->draw_fb == S->stereo_fb && S->stereo_eye_w > 0 && stereo_kind;
    apply_encoder_state(stereo);
    S->instances = stereo && S->stereo_only < 0 ? 2 : 1;
    if (stereo_kind) {
        // Where each eye goes: the GL viewport (one eye's pixels) inside its half of the target.
        float st[12] = {0};
        int* cfg = (int*)&st[8];
        if (stereo) {
            const float W = (float)S->stereo_eye_w, TW = (float)S->tw, H = (float)S->th;
            const float vx = S->vp_set ? (float)S->vp[0] : 0.0f, vy = S->vp_set ? (float)S->vp[1] : 0.0f;
            const float vw = S->vp_set ? (float)S->vp[2] : W, vh = S->vp_set ? (float)S->vp[3] : H;
            for (int e = 0; e < 2; e++) {
                st[e * 4 + 0] = vw / TW;
                st[e * 4 + 1] = (2.0f * ((float)e * W + vx) + vw) / TW - 1.0f;
                st[e * 4 + 2] = vh / H;
                st[e * 4 + 3] = (2.0f * vy + vh) / H - 1.0f;
            }
            cfg[0] = 1;
            cfg[1] = S->stereo_only;
        } else {
            cfg[0] = 0;
            cfg[1] = -1;
        }
        if (!S->bound_stereo_valid || memcmp(st, S->bound_stereo, sizeof(st)) != 0) {
            [S->enc setVertexBytes:st length:sizeof(st) atIndex:2];
            memcpy(S->bound_stereo, st, sizeof(st));
            S->bound_stereo_valid = true;
        }
    }
    id<MTLRenderPipelineState> ps = pipeline_for(p->kind, vao);
    if (!ps) return false;
    if ((__bridge void*)ps != S->bound_pipeline) {
        [S->enc setRenderPipelineState:ps];
        S->bound_pipeline = (__bridge void*)ps;
    }

    // vertex buffer: all attribs of the port's VAOs share one buffer
    if (attrib_count(p->kind) > 0) {
        GLuint vb = 0;
        for (int i = 0; i < kMaxAttribs; i++) {
            if (vao->a[i].enabled) {
                vb = vao->a[i].buffer;
                break;
            }
        }
        Buf* b = get_obj(S->buffers, vb);
        if (!b || !b->mtl) {
            warn_once("draw with no vertex buffer data");
            return false;
        }
        [S->enc setVertexBuffer:(__bridge id<MTLBuffer>)b->mtl offset:b->off atIndex:1];
    }

    if (p->kind == PROG_TEV) {
        // The uniform block goes into this frame's ring once per change, bound by offset.
        if (S->bound_block_prog != S->program || S->bound_block_rev != p->rev) {
            id<MTLBuffer> rb;
            size_t off;
            stream_copy(p->block, kTevBytes, &rb, &off);
            [S->enc setVertexBuffer:rb offset:off atIndex:0];
            [S->enc setFragmentBuffer:rb offset:off atIndex:0];
            S->bound_block_prog = S->program;
            S->bound_block_rev = p->rev;
        }
        for (int u = 0; u < 5; u++) {
            Tex* t = get_obj(S->textures, S->unit_tex[u]);
            id<MTLTexture> mt = (t && t->mtl) ? (__bridge id<MTLTexture>)t->mtl : S->white;
            if ((__bridge void*)mt != S->bound_tex[u]) {
                [S->enc setFragmentTexture:mt atIndex:u];
                S->bound_tex[u] = (__bridge void*)mt;
            }
            id<MTLSamplerState> ss = sampler_for(t);
            if ((__bridge void*)ss != S->bound_samp[u]) {
                [S->enc setFragmentSamplerState:ss atIndex:u];
                S->bound_samp[u] = (__bridge void*)ss;
            }
        }
    } else {
        // The rarer programs bind everything each draw.
        S->bound_block_prog = 0;
        if (p->kind == PROG_SKY || p->kind == PROG_HANDS || p->kind == PROG_PANEL) {
            [S->enc setVertexBytes:p->block length:kSmallUniformBytes atIndex:0];
            [S->enc setFragmentBytes:p->block length:kSmallUniformBytes atIndex:0];
        }
        if (p->kind == PROG_BLIT || p->kind == PROG_PANEL) {
            Tex* t = get_obj(S->textures, S->unit_tex[0]);
            id<MTLTexture> mt = (t && t->mtl) ? (__bridge id<MTLTexture>)t->mtl : S->white;
            [S->enc setFragmentTexture:mt atIndex:0];
            [S->enc setFragmentSamplerState:sampler_for(t) atIndex:0];
            S->bound_tex[0] = (__bridge void*)mt;
            S->bound_samp[0] = nullptr;
        }
    }
    *out_prog = p;
    *out_vao = vao;
    return true;
}

void draw_fan_indices(GLint first, GLsizei count) {
    if (count < 3) return;
    mvec<uint32_t> idx;
    idx.reserve((size_t)(count - 2) * 3);
    for (GLsizei i = 1; i + 1 < count; i++) {
        idx.push_back((uint32_t)first);
        idx.push_back((uint32_t)(first + i));
        idx.push_back((uint32_t)(first + i + 1));
    }
    id<MTLBuffer> ib;
    size_t off;
    stream_copy(idx.data(), idx.size() * 4, &ib, &off);
    [S->enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                       indexCount:idx.size()
                        indexType:MTLIndexTypeUInt32
                      indexBuffer:ib
                indexBufferOffset:off
                    instanceCount:S->instances];
}

void rgba_from(const void* pixels, GLenum format, GLenum type, int w, int h, int align, mvec<uint8_t>& out) {
    out.assign((size_t)w * h * 4, 255);
    if (!pixels) return;
    const uint8_t* src = (const uint8_t*)pixels;
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        size_t row = (size_t)w * 4;
        size_t pitch = (row + align - 1) / align * align;
        for (int y = 0; y < h; y++) memcpy(&out[(size_t)y * row], src + (size_t)y * pitch, row);
    } else if (format == GL_RGB && type == GL_UNSIGNED_BYTE) {
        size_t row = (size_t)w * 3;
        size_t pitch = (row + align - 1) / align * align;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t* p = src + (size_t)y * pitch + x * 3;
                uint8_t* d = &out[((size_t)y * w + x) * 4];
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
            }
    } else if (format == GL_RGB && (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_5_6_5_REV)) {
        size_t row = (size_t)w * 2;
        size_t pitch = (row + align - 1) / align * align;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint16_t v;
                memcpy(&v, src + (size_t)y * pitch + x * 2, 2);
                int r, g, b;
                if (type == GL_UNSIGNED_SHORT_5_6_5) {
                    r = (v >> 11) & 31;
                    g = (v >> 5) & 63;
                    b = v & 31;
                } else {
                    b = (v >> 11) & 31;
                    g = (v >> 5) & 63;
                    r = v & 31;
                }
                uint8_t* d = &out[((size_t)y * w + x) * 4];
                d[0] = (uint8_t)((r << 3) | (r >> 2));
                d[1] = (uint8_t)((g << 2) | (g >> 4));
                d[2] = (uint8_t)((b << 3) | (b >> 2));
            }
    } else {
        warn_once("glTexImage2D: unsupported format/type");
    }
}

int uniform_lookup(const UniformDesc* table, int count, const char* name) {
    char base[96];
    snprintf(base, sizeof(base), "%s", name);
    int elem = 0;
    char* br = strchr(base, '[');
    if (br) {
        elem = atoi(br + 1);
        *br = '\0';
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(table[i].name, base) == 0) {
            if (elem >= table[i].elems) return -1;
            return i * 64 + elem;
        }
    }
    return -1;
}

// Writes `count` array elements of `comps` components each (tightly packed in
// `src`) into the current program's block at location `loc`.
template <typename T>
void put_uniform(GLint loc, int comps, GLsizei count, const T* src) {
    if (loc < 0) return;
    Program* p = get_obj(S->programs, S->program);
    if (!p) return;
    int entries = 0;
    const UniformDesc* table = uniform_table(p->kind, &entries);
    int idx = loc / 64, elem = loc % 64;
    if (!table || idx >= entries) return;
    const UniformDesc& d = table[idx];
    if (d.slot < 0 || d.kind != 0) return;
    for (GLsizei i = 0; i < count && elem + i < d.elems; i++) {
        uint8_t* dst = p->block + (size_t)(d.slot + elem + i) * 16;
        memcpy(dst, src + (size_t)i * comps, sizeof(T) * comps);
    }
    p->rev++;
}

void put_matrix(GLint loc, int n, GLboolean transpose, const GLfloat* v) {
    if (loc < 0) return;
    Program* p = get_obj(S->programs, S->program);
    if (!p) return;
    int count = 0;
    const UniformDesc* table = uniform_table(p->kind, &count);
    int idx = loc / 64;
    if (!table || idx >= count) return;
    const UniformDesc& d = table[idx];
    if (d.kind != n) return;
    float* dst = (float*)(p->block + (size_t)d.slot * 16);
    for (int c = 0; c < n; c++)
        for (int r = 0; r < n; r++) {
            // column c, row r
            float val = transpose ? v[r * n + c] : v[c * n + r];
            dst[c * 4 + r] = val;
        }
    p->rev++;
}

}  // namespace

// ============================== GL entry points ==============================

extern "C" {

GLenum glGetError(void) { return GL_NO_ERROR; }

const GLubyte* glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR: return (const GLubyte*)"Trevorbilt";
        case GL_RENDERER: return (const GLubyte*)"AC Metal shim";
        case GL_VERSION: return (const GLubyte*)"3.3 (Metal)";
        default: return (const GLubyte*)"";
    }
}
const GLubyte* glGetStringi(GLenum name, GLuint index) { (void)name; (void)index; return (const GLubyte*)""; }
void glGetIntegerv(GLenum pname, GLint* data) {
    if (!data) return;
    switch (pname) {
        case GL_CURRENT_PROGRAM: *data = (GLint)S->program; break;
        case GL_VERTEX_ARRAY_BINDING: *data = (GLint)S->vao; break;
        case GL_ARRAY_BUFFER_BINDING: *data = (GLint)S->array_buffer; break;
        case GL_DEPTH_FUNC: *data = (GLint)S->depth_func; break;
        case GL_DRAW_FRAMEBUFFER_BINDING: *data = (GLint)S->draw_fb; break;  // == GL_FRAMEBUFFER_BINDING
        case GL_READ_FRAMEBUFFER_BINDING: *data = (GLint)S->read_fb; break;
        case GL_POLYGON_MODE: data[0] = GL_FILL; break;
        case GL_VIEWPORT: {
            Target t;
            bool have = resolve_framebuffer(S->draw_fb, &t);
            data[0] = S->vp_set ? S->vp[0] : 0;
            data[1] = S->vp_set ? S->vp[1] : 0;
            data[2] = S->vp_set ? S->vp[2] : (have ? t.w : 0);
            data[3] = S->vp_set ? S->vp[3] : (have ? t.h : 0);
            break;
        }
        default: *data = 0; break;
    }
}
GLboolean glIsEnabled(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST: return S->depth_test;
        case GL_BLEND: return S->blend;
        case GL_CULL_FACE: return S->cull;
        case GL_SCISSOR_TEST: return S->scissor_test;
        default: return GL_FALSE;
    }
}
void glGetBooleanv(GLenum pname, GLboolean* data) {
    if (!data) return;
    if (pname == GL_DEPTH_WRITEMASK) {
        data[0] = S->depth_mask;
    } else if (pname == GL_COLOR_WRITEMASK) {
        for (int i = 0; i < 4; i++) data[i] = S->cmask[i];
    } else {
        data[0] = GL_FALSE;
    }
}
void glGetDoublev(GLenum pname, GLdouble* data) {
    if (!data) return;
    if (pname == GL_DEPTH_RANGE) {
        data[0] = S->dr_n;
        data[1] = S->dr_f;
    } else {
        data[0] = 0.0;
    }
}
void glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
void glReadBuffer(GLenum mode) { (void)mode; }
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    if (!params) return;
    *params = 0;
    if (pname != GL_BUFFER_SIZE) return;
    GLuint name = S->array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        VAO* v = get_obj(S->vaos, S->vao);
        name = v ? v->ebo : S->element_buffer_novao;
    }
    Buf* b = get_obj(S->buffers, name);
    if (b) *params = (GLint)b->size;
}
void glFlush(void) {}
void glFinish(void) { submit(true); }

void glEnable(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST: S->depth_test = true; break;
        case GL_BLEND: S->blend = true; break;
        case GL_CULL_FACE: S->cull = true; break;
        case GL_SCISSOR_TEST: S->scissor_test = true; break;
        default: return;
    }
    S->enc_dirty = true;
}
void glDisable(GLenum cap) {
    switch (cap) {
        case GL_DEPTH_TEST: S->depth_test = false; break;
        case GL_BLEND: S->blend = false; break;
        case GL_CULL_FACE: S->cull = false; break;
        case GL_SCISSOR_TEST: S->scissor_test = false; break;
        default: return;
    }
    S->enc_dirty = true;
}
void glDepthFunc(GLenum func) { S->depth_func = func; S->enc_dirty = true; }
void glDepthMask(GLboolean flag) { S->depth_mask = flag != 0; S->enc_dirty = true; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    S->cmask[0] = r; S->cmask[1] = g; S->cmask[2] = b; S->cmask[3] = a;
}
void glCullFace(GLenum mode) { S->cull_face = mode; S->enc_dirty = true; }
void glBlendFunc(GLenum s, GLenum d) {
    S->blend_src = S->blend_src_a = s;
    S->blend_dst = S->blend_dst_a = d;
}
void glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da) {
    S->blend_src = s;
    S->blend_dst = d;
    S->blend_src_a = sa;
    S->blend_dst_a = da;
}
void glBlendEquation(GLenum mode) { S->blend_eq = S->blend_eq_a = mode; }
void glBlendEquationSeparate(GLenum mode, GLenum mode_alpha) {
    S->blend_eq = mode;
    S->blend_eq_a = mode_alpha;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    S->vp[0] = x; S->vp[1] = y; S->vp[2] = w; S->vp[3] = h;
    S->vp_set = true;
    S->enc_dirty = true;
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    S->sc[0] = x; S->sc[1] = y; S->sc[2] = w; S->sc[3] = h;
    S->enc_dirty = true;
}
void glDepthRange(GLdouble n, GLdouble f) { S->dr_n = n; S->dr_f = f; S->enc_dirty = true; }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    S->clear_rgba[0] = r; S->clear_rgba[1] = g; S->clear_rgba[2] = b; S->clear_rgba[3] = a;
}
void glClearDepth(GLdouble d) { S->clear_depth = d; }
void glClear(GLbitfield mask) {
    // The port clears whole buffers (scissor off, masks on): a new pass with
    // clear load actions is equivalent.
    end_encoder();
    if (mask & GL_COLOR_BUFFER_BIT) S->pending_clear_color = true;
    if (mask & GL_DEPTH_BUFFER_BIT) S->pending_clear_depth = true;
}
void glLineWidth(GLfloat width) { (void)width; }
void glPointSize(GLfloat size) { (void)size; }
void glPixelStorei(GLenum pname, GLint param) {
    if (pname == GL_PACK_ALIGNMENT) S->pack_alignment = param > 0 ? param : 1;
    if (pname == GL_UNPACK_ALIGNMENT) S->unpack_alignment = param > 0 ? param : 1;
}

bool is_bgra(MTLPixelFormat f) { return f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB; }

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    if (w <= 0 || h <= 0 || !pixels || type != GL_UNSIGNED_BYTE) return;
    Target t;
    if (!resolve_framebuffer(S->read_fb, &t)) return;
    submit(true);
    int H = t.h;
    // GL rows run bottom-up from y; Metal rows top-down.
    int top = H - (y + h);
    if (top < 0 || x < 0 || x + w > t.w || top + h > H) {
        warn_once("glReadPixels out of bounds");
        return;
    }
    mvec<uint8_t> tmp((size_t)w * h * 4);
    [t.color getBytes:tmp.data() bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(x, top, w, h) mipmapLevel:0];
    bool bgra = is_bgra(t.color.pixelFormat);
    int comps = format == GL_RGB ? 3 : 4;
    size_t row = (size_t)w * comps;
    size_t pitch = (row + S->pack_alignment - 1) / S->pack_alignment * S->pack_alignment;
    uint8_t* dst = (uint8_t*)pixels;
    for (int gy = 0; gy < h; gy++) {
        const uint8_t* srow = &tmp[(size_t)(h - 1 - gy) * w * 4];
        uint8_t* drow = dst + (size_t)gy * pitch;
        for (int px = 0; px < w; px++) {
            const uint8_t* s = srow + px * 4;
            uint8_t r = bgra ? s[2] : s[0], g = s[1], b = bgra ? s[0] : s[2], a = s[3];
            drow[px * comps + 0] = r;
            drow[px * comps + 1] = g;
            drow[px * comps + 2] = b;
            if (comps == 4) drow[px * comps + 3] = a;
        }
    }
}

// ---- textures ----

void glGenTextures(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = alloc_name(S->textures);
}
void glDeleteTextures(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        Tex* t = get_obj(S->textures, names[i]);
        if (!t) continue;
        release_ptr(t->mtl);
        t->live = false;
        t->external = false;
        for (int u = 0; u < kMaxUnits; u++)
            if (S->unit_tex[u] == names[i]) S->unit_tex[u] = 0;
    }
}
void glActiveTexture(GLenum unit) {
    int u = (int)(unit - GL_TEXTURE0);
    S->active_unit = (u >= 0 && u < kMaxUnits) ? u : 0;
}
void glBindTexture(GLenum target, GLuint name) {
    (void)target;
    if (name >= S->textures.size()) {
        // GL allows binding names it never generated; create on demand
        S->textures.resize(name + 1);
    }
    if (name && !S->textures[name].live) {
        S->textures[name] = Tex();
        S->textures[name].live = true;
    }
    S->unit_tex[S->active_unit] = name;
}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const void* pixels) {
    (void)target; (void)internalformat; (void)border;
    if (level != 0) return;
    Tex* t = get_obj(S->textures, S->unit_tex[S->active_unit]);
    if (!t || t->external || w <= 0 || h <= 0) return;
    // Always a fresh texture: earlier draws in flight keep the old contents (GL semantics).
    // Storage without data is an FBO's colour (the VR mod's targets): renderable, and
    // shared so EFB copies can read it back.
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    d.usage = pixels ? MTLTextureUsageShaderRead : (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> mt = [S->device newTextureWithDescriptor:d];
    if (pixels) {
        mvec<uint8_t> rgba;
        rgba_from(pixels, format, type, w, h, S->unpack_alignment, rgba);
        [mt replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:(NSUInteger)w * 4];
    } else {
        mvec<uint8_t> zero((size_t)w * h * 4, 0);
        [mt replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:zero.data() bytesPerRow:(NSUInteger)w * 4];
    }
    release_ptr(t->mtl);
    t->mtl = (void*)CFBridgingRetain(mt);
    t->serial = ++g_texture_serial;
    t->w = w;
    t->h = h;
}
void glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum format,
                     GLenum type, const void* pixels) {
    (void)target;
    if (level != 0) return;
    Tex* t = get_obj(S->textures, S->unit_tex[S->active_unit]);
    if (!t || !t->mtl || t->external) return;
    mvec<uint8_t> rgba;
    rgba_from(pixels, format, type, w, h, S->unpack_alignment, rgba);
    // copy-on-write so in-flight draws see the old contents
    id<MTLTexture> old = (__bridge id<MTLTexture>)t->mtl;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:t->w
                                                                                 height:t->h
                                                                              mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> mt = [S->device newTextureWithDescriptor:d];
    mvec<uint8_t> all((size_t)t->w * t->h * 4);
    [old getBytes:all.data() bytesPerRow:(NSUInteger)t->w * 4 fromRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0];
    [mt replaceRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0 withBytes:all.data() bytesPerRow:(NSUInteger)t->w * 4];
    [mt replaceRegion:MTLRegionMake2D(xo, yo, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:(NSUInteger)w * 4];
    release_ptr(t->mtl);
    t->mtl = (void*)CFBridgingRetain(mt);
    t->serial = ++g_texture_serial;
}
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei w, GLsizei h, GLint border,
                            GLsizei imageSize, const void* data) {
    (void)target; (void)level; (void)internalformat; (void)w; (void)h; (void)border; (void)imageSize; (void)data;
    warn_once("compressed textures are not supported (HD pack off)");
}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    (void)target;
    Tex* t = get_obj(S->textures, S->unit_tex[S->active_unit]);
    if (!t) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: t->minf = (GLenum)param; break;
        case GL_TEXTURE_MAG_FILTER: t->magf = (GLenum)param; break;
        case GL_TEXTURE_WRAP_S: t->ws = (GLenum)param; break;
        case GL_TEXTURE_WRAP_T: t->wt = (GLenum)param; break;
        default: break;
    }
}

// ---- buffers / VAOs ----

void glGenBuffers(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = alloc_name(S->buffers);
}
void glDeleteBuffers(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        Buf* b = get_obj(S->buffers, names[i]);
        if (!b) continue;
        release_ptr(b->mtl);
        b->live = false;
    }
}
void glBindBuffer(GLenum target, GLuint name) {
    if (target == GL_ARRAY_BUFFER) {
        S->array_buffer = name;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        VAO* v = get_obj(S->vaos, S->vao);
        if (v) v->ebo = name;
        else S->element_buffer_novao = name;
    }
}
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    GLuint name = S->array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        VAO* v = get_obj(S->vaos, S->vao);
        name = v ? v->ebo : S->element_buffer_novao;
    }
    Buf* b = get_obj(S->buffers, name);
    if (!b || size <= 0) return;
    release_ptr(b->mtl);
    b->size = (size_t)size;
    if (usage == GL_STATIC_DRAW) {
        id<MTLBuffer> mb = data ? [S->device newBufferWithBytes:data length:(NSUInteger)size options:MTLResourceStorageModeShared]
                                : [S->device newBufferWithLength:(NSUInteger)size options:MTLResourceStorageModeShared];
        b->mtl = (void*)CFBridgingRetain(mb);
        b->off = 0;
    } else {
        if (!data) {
            b->mtl = nullptr;  // allocation only; data comes with the next upload
            return;
        }
        id<MTLBuffer> rb;
        size_t off;
        stream_copy(data, (size_t)size, &rb, &off);
        b->mtl = (void*)CFBridgingRetain(rb);
        b->off = off;
    }
}
void glGenVertexArrays(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = alloc_name(S->vaos);
}
void glDeleteVertexArrays(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        VAO* v = get_obj(S->vaos, names[i]);
        if (v) v->live = false;
        if (S->vao == names[i]) S->vao = 0;
    }
}
void glBindVertexArray(GLuint name) { S->vao = name; }
void glEnableVertexAttribArray(GLuint index) {
    VAO* v = get_obj(S->vaos, S->vao);
    if (v && index < kMaxAttribs) v->a[index].enabled = true;
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                           const void* pointer) {
    VAO* v = get_obj(S->vaos, S->vao);
    if (!v || index >= kMaxAttribs) return;
    Attrib& a = v->a[index];
    a.size = size;
    a.type = type;
    a.normalized = normalized != 0;
    a.stride = stride;
    a.offset = (uintptr_t)pointer;
    a.buffer = S->array_buffer;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (count <= 0) return;
    Program* p;
    VAO* v;
    if (!prepare_draw(&p, &v)) return;
    if (mode == GL_TRIANGLE_FAN) {
        draw_fan_indices(first, count);
        return;
    }
    [S->enc drawPrimitives:prim_type(mode)
               vertexStart:(NSUInteger)first
               vertexCount:(NSUInteger)count
             instanceCount:S->instances];
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (count <= 0) return;
    Program* p;
    VAO* v;
    if (!prepare_draw(&p, &v)) return;
    Buf* ib = get_obj(S->buffers, v->ebo);
    if (!ib || !ib->mtl) {
        warn_once("glDrawElements with no index buffer");
        return;
    }
    if (mode == GL_TRIANGLE_FAN) {
        warn_once("indexed triangle fans are not supported");
        return;
    }
    MTLIndexType it = type == GL_UNSIGNED_INT ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    [S->enc drawIndexedPrimitives:prim_type(mode)
                       indexCount:(NSUInteger)count
                        indexType:it
                      indexBuffer:(__bridge id<MTLBuffer>)ib->mtl
                indexBufferOffset:ib->off + (uintptr_t)indices
                    instanceCount:S->instances];
}

// ---- shaders / programs ----

GLuint glCreateShader(GLenum type) {
    GLuint n = alloc_name(S->shaders);
    S->shaders[n].type = type;
    return n;
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strs, const GLint* lengths) {
    Shader* s = get_obj(S->shaders, shader);
    if (!s) return;
    s->src.clear();
    for (GLsizei i = 0; i < count; i++) {
        if (lengths && lengths[i] >= 0) s->src.append(strs[i], (size_t)lengths[i]);
        else s->src.append(strs[i]);
    }
}
void glCompileShader(GLuint shader) { (void)shader; }
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
    (void)shader;
    if (params) *params = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* log) {
    (void)shader;
    if (length) *length = 0;
    if (log && bufSize > 0) log[0] = '\0';
}
void glDeleteShader(GLuint shader) {
    Shader* s = get_obj(S->shaders, shader);
    if (s) s->live = false;
}
GLuint glCreateProgram(void) {
    GLuint n = alloc_name(S->programs);
    memset(S->programs[n].block, 0, kTevBytes);
    return n;
}
void glAttachShader(GLuint program, GLuint shader) {
    Program* p = get_obj(S->programs, program);
    if (p) p->shaders.push_back(shader);
}
void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name) { (void)program; (void)index; (void)name; }
void glLinkProgram(GLuint program) {
    Program* p = get_obj(S->programs, program);
    if (!p) return;
    p->kind = PROG_NONE;
    for (GLuint sh : p->shaders) {
        Shader* s = get_obj(S->shaders, sh);
        if (!s || s->type != GL_FRAGMENT_SHADER) continue;
        if (s->src.find("u_tev_color_in") != mstring::npos || s->src.find("AC_METAL_TEV") != mstring::npos)
            p->kind = PROG_TEV;
        else if (s->src.find("eye_to_world") != mstring::npos)
            p->kind = PROG_SKY;  // pc_sky.cpp
        else if (s->src.find("eye_normal") != mstring::npos)
            p->kind = PROG_HANDS;  // pc_vr_hands.cpp
        else if (s->src.find("sampler2D u_tex") != mstring::npos)
            p->kind = PROG_PANEL;  // pc_vr.cpp's UI panel
        else if (s->src.find("sampler2D tex") != mstring::npos)
            p->kind = PROG_BLIT;
    }
    if (p->kind == PROG_NONE) warn_once("unrecognised GLSL program (draws skipped)");
}
void glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
    (void)program;
    if (params) *params = pname == GL_LINK_STATUS ? GL_TRUE : 0;
}
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* log) {
    (void)program;
    if (length) *length = 0;
    if (log && bufSize > 0) log[0] = '\0';
}
void glDeleteProgram(GLuint program) {
    Program* p = get_obj(S->programs, program);
    if (p) p->live = false;
    if (S->program == program) S->program = 0;
}
void glUseProgram(GLuint program) { S->program = program; }
GLint glGetUniformLocation(GLuint program, const GLchar* name) {
    Program* p = get_obj(S->programs, program);
    if (!p || !name) return -1;
    int count = 0;
    const UniformDesc* table = uniform_table(p->kind, &count);
    return table ? uniform_lookup(table, count, name) : -1;
}

void glUniform1i(GLint loc, GLint v0) { put_uniform(loc, 1, 1, &v0); }
void glUniform2i(GLint loc, GLint v0, GLint v1) { GLint v[2] = {v0, v1}; put_uniform(loc, 2, 1, v); }
void glUniform3i(GLint loc, GLint v0, GLint v1, GLint v2) { GLint v[3] = {v0, v1, v2}; put_uniform(loc, 3, 1, v); }
void glUniform4i(GLint loc, GLint v0, GLint v1, GLint v2, GLint v3) {
    GLint v[4] = {v0, v1, v2, v3};
    put_uniform(loc, 4, 1, v);
}
void glUniform1f(GLint loc, GLfloat v0) { put_uniform(loc, 1, 1, &v0); }
void glUniform2f(GLint loc, GLfloat v0, GLfloat v1) { GLfloat v[2] = {v0, v1}; put_uniform(loc, 2, 1, v); }
void glUniform3f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2) { GLfloat v[3] = {v0, v1, v2}; put_uniform(loc, 3, 1, v); }
void glUniform4f(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    GLfloat v[4] = {v0, v1, v2, v3};
    put_uniform(loc, 4, 1, v);
}
void glUniform1iv(GLint loc, GLsizei c, const GLint* v) { put_uniform(loc, 1, c, v); }
void glUniform2iv(GLint loc, GLsizei c, const GLint* v) { put_uniform(loc, 2, c, v); }
void glUniform3iv(GLint loc, GLsizei c, const GLint* v) { put_uniform(loc, 3, c, v); }
void glUniform4iv(GLint loc, GLsizei c, const GLint* v) { put_uniform(loc, 4, c, v); }
void glUniform1fv(GLint loc, GLsizei c, const GLfloat* v) { put_uniform(loc, 1, c, v); }
void glUniform2fv(GLint loc, GLsizei c, const GLfloat* v) { put_uniform(loc, 2, c, v); }
void glUniform3fv(GLint loc, GLsizei c, const GLfloat* v) { put_uniform(loc, 3, c, v); }
void glUniform4fv(GLint loc, GLsizei c, const GLfloat* v) { put_uniform(loc, 4, c, v); }
void glUniformMatrix3fv(GLint loc, GLsizei c, GLboolean t, const GLfloat* v) { (void)c; put_matrix(loc, 3, t, v); }
void glUniformMatrix4fv(GLint loc, GLsizei c, GLboolean t, const GLfloat* v) { (void)c; put_matrix(loc, 4, t, v); }

// ---- framebuffer objects (the VR mod's eye and UI targets) ----

void glGenFramebuffers(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) ids[i] = alloc_name(S->framebuffers);
}
void glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) {
        FBuf* f = get_obj(S->framebuffers, ids[i]);
        if (!f) continue;
        if (S->draw_fb == ids[i]) {
            flush_pending_clears();
            end_encoder();
            S->draw_fb = 0;
        }
        if (S->read_fb == ids[i]) S->read_fb = 0;
        f->live = false;
    }
}
void glBindFramebuffer(GLenum target, GLuint fb) {
    if (fb && !get_obj(S->framebuffers, fb)) {
        if (fb >= S->framebuffers.size()) S->framebuffers.resize(fb + 1);
        S->framebuffers[fb] = FBuf();
        S->framebuffers[fb].live = true;
    }
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        if (fb != S->draw_fb) {
            flush_pending_clears();
            end_encoder();
            S->draw_fb = fb;
            S->enc_dirty = true;
        }
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) S->read_fb = fb;
}
static FBuf* bound_fbo(GLenum target) {
    GLuint name = target == GL_READ_FRAMEBUFFER ? S->read_fb : S->draw_fb;
    return get_obj(S->framebuffers, name);
}
static void attachment_changed(GLenum target) {
    if (target != GL_READ_FRAMEBUFFER && S->draw_fb) {
        flush_pending_clears();
        end_encoder();
    }
}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    (void)textarget; (void)level;
    FBuf* f = bound_fbo(target);
    if (!f) return;
    attachment_changed(target);
    if (attachment == GL_COLOR_ATTACHMENT0) f->color = texture;
    else warn_once("glFramebufferTexture2D: only GL_COLOR_ATTACHMENT0 is supported");
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    (void)rbtarget;
    FBuf* f = bound_fbo(target);
    if (!f) return;
    attachment_changed(target);
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = rb;
    else warn_once("glFramebufferRenderbuffer: only depth attachments are supported");
}
GLenum glCheckFramebufferStatus(GLenum target) {
    FBuf* f = bound_fbo(target);
    if (!f) return GL_FRAMEBUFFER_COMPLETE;  // the default framebuffer
    Tex* t = get_obj(S->textures, f->color);
    return (t && t->mtl) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
}
void glGenRenderbuffers(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) ids[i] = alloc_name(S->renderbuffers);
}
void glDeleteRenderbuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; i++) {
        RBuf* r = get_obj(S->renderbuffers, ids[i]);
        if (!r) continue;
        release_ptr(r->mtl);
        r->live = false;
    }
}
void glBindRenderbuffer(GLenum target, GLuint rb) {
    (void)target;
    if (rb && !get_obj(S->renderbuffers, rb)) {
        if (rb >= S->renderbuffers.size()) S->renderbuffers.resize(rb + 1);
        S->renderbuffers[rb] = RBuf();
        S->renderbuffers[rb].live = true;
    }
    S->renderbuffer = rb;
}
void glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h) {
    (void)target;
    RBuf* r = get_obj(S->renderbuffers, S->renderbuffer);
    if (!r || w <= 0 || h <= 0) return;
    if (format != GL_DEPTH_COMPONENT24 && format != GL_DEPTH_COMPONENT32F && format != GL_DEPTH24_STENCIL8 &&
        format != GL_DEPTH_COMPONENT)
        warn_once("glRenderbufferStorage: only depth renderbuffers are supported");
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    release_ptr(r->mtl);
    r->mtl = (void*)CFBridgingRetain([S->device newTextureWithDescriptor:d]);
    r->w = w;
    r->h = h;
}

// A textured rectangle into the bound draw framebuffer, outside the GL state (blits,
// the room's backdrop). dst in the target's GL pixels (bottom-left origin); src in
// texture coordinates of `src` (top-left origin). blend: 0 replace, 1 "under" (the
// rectangle goes behind what's there: dst + src * (1 - dst.a)), 2 "over" (src +
// dst * (1 - src.a)); premultiplied.
static void draw_rect(id<MTLTexture> src, const float uv[4], const float dst[4], const float tint[4], bool linear,
                      int blend) {
    if (!ensure_encoder()) return;
    uint64_t key = (uint64_t)S->tcolor_fmt | ((uint64_t)S->tdepth << 32) | ((uint64_t)blend << 33);
    id<MTLRenderPipelineState> ps = nil;
    auto it = S->rect_pipelines.find(key);
    if (it != S->rect_pipelines.end()) {
        ps = it->second;
    } else {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [S->library newFunctionWithName:@"rect_vertex"];
        d.fragmentFunction = [S->library newFunctionWithName:@"rect_fragment"];
        d.colorAttachments[0].pixelFormat = S->tcolor_fmt;
        d.depthAttachmentPixelFormat = S->tdepth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
        if (blend == 1) {
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
            d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
            d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        } else if (blend == 2) {
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError* err = nil;
        ps = [S->device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!ps) fprintf(stderr, "[ACVR] metal shim: rect pipeline failed: %s\n", err.localizedDescription.UTF8String);
        S->rect_pipelines[key] = ps;
    }
    if (!ps) return;
    if (!S->no_depth) {
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        S->no_depth = [S->device newDepthStencilStateWithDescriptor:dd];
    }
    struct {
        float dst[4];
        float src[4];
        float tint[4];
    } u;
    for (int i = 0; i < 4; i++) {
        u.src[i] = uv[i];
        u.tint[i] = tint[i];
    }
    u.dst[0] = 2.0f * dst[0] / (float)S->tw - 1.0f;
    u.dst[1] = 2.0f * dst[1] / (float)S->th - 1.0f;
    u.dst[2] = 2.0f * dst[2] / (float)S->tw - 1.0f;
    u.dst[3] = 2.0f * dst[3] / (float)S->th - 1.0f;
    MTLViewport v = {0, 0, (double)S->tw, (double)S->th, 0, 1};
    [S->enc setViewport:v];
    MTLScissorRect r = {0, 0, (NSUInteger)S->tw, (NSUInteger)S->th};
    [S->enc setScissorRect:r];
    [S->enc setCullMode:MTLCullModeNone];
    [S->enc setDepthStencilState:S->no_depth];
    [S->enc setRenderPipelineState:ps];
    [S->enc setVertexBytes:&u length:sizeof(u) atIndex:0];
    [S->enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [S->enc setFragmentTexture:src ? src : S->white atIndex:0];
    Tex tmp;
    tmp.minf = tmp.magf = linear ? GL_LINEAR : GL_NEAREST;
    tmp.ws = tmp.wt = GL_CLAMP_TO_EDGE;
    [S->enc setFragmentSamplerState:sampler_for(&tmp) atIndex:0];
    [S->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    S->enc_dirty = true;  // the next GL draw sets its own viewport, scissor and depth state again
    forget_bindings();
}

void glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                       GLbitfield mask, GLenum filter) {
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    Target src;
    if (!resolve_framebuffer(S->read_fb, &src)) return;
    float uv[4] = {(float)sx0 / src.w, 1.0f - (float)sy0 / src.h, (float)sx1 / src.w, 1.0f - (float)sy1 / src.h};
    float dst[4] = {(float)dx0, (float)dy0, (float)dx1, (float)dy1};
    float one[4] = {1, 1, 1, 1};
    if (S->read_fb == S->draw_fb) {
        warn_once("glBlitFramebuffer within one framebuffer");
        return;
    }
    draw_rect(src.color, uv, dst, one, filter == GL_LINEAR, 0);
}

// ============================== host API ==============================

int pcm_init(void* device) {
    if (S) return 1;
    S = (State*)malloc(sizeof(State));
    new (S) State();
    S->device = device ? (__bridge id<MTLDevice>)device : MTLCreateSystemDefaultDevice();
    if (!S->device) {
        fprintf(stderr, "[ACVR] metal shim: no Metal device\n");
        return 0;
    }
    S->queue = [S->device newCommandQueue];
    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.mathMode = MTLMathModeSafe;  // no fast-math reassociation in TEV math
    S->library = [S->device newLibraryWithSource:[NSString stringWithUTF8String:kPCMetalSource] options:opts error:&err];
    if (!S->library) {
        fprintf(stderr, "[ACVR] metal shim: MSL compile failed: %s\n", err.localizedDescription.UTF8String);
        return 0;
    }
    for (int i = 0; i < kFramesInFlight; i++)
        S->ring[i] = [S->device newBufferWithLength:kRingBytes options:MTLResourceStorageModeShared];
    S->inflight = dispatch_semaphore_create(kFramesInFlight);
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    S->white = [S->device newTextureWithDescriptor:d];
    uint32_t w = 0xFFFFFFFFu;
    [S->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
    fprintf(stderr, "[ACVR] metal shim on %s\n", S->device.name.UTF8String);
    return 1;
}

void* pcm_make_target(int width, int height) {
    if (!S) return nullptr;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:width
                                                                                 height:height
                                                                              mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    return (void*)CFBridgingRetain([S->device newTextureWithDescriptor:d]);
}

void* pcm_device(void) { return S ? (__bridge void*)S->device : nullptr; }

void pcm_set_default_framebuffer(void* color_texture, int width, int height) {
    if (!S) return;
    id<MTLTexture> t = (__bridge id<MTLTexture>)color_texture;
    if (t == S->color && width == S->fbw && height == S->fbh) return;
    end_encoder();
    S->color = t;
    S->fbw = width;
    S->fbh = height;
    S->enc_dirty = true;
}

void pcm_present(void (*on_complete)(void* ctx), void* ctx) {
    pcm_present_ex(nullptr, nullptr, on_complete, ctx);
}

void pcm_present_ex(void (*encode)(void* command_buffer, void* color_texture, void* ctx), void* encode_ctx,
                    void (*on_complete)(void* ctx), void* ctx) {
    if (!S) return;
    if (S->pending_clear_color || S->pending_clear_depth) ensure_encoder();
    end_encoder();
    begin_cmd_if_needed();
    id<MTLCommandBuffer> c = S->cmd;
    if (encode) encode((__bridge void*)c, (__bridge void*)S->color, encode_ctx);
    S->cmd = nil;
    dispatch_semaphore_t sem = S->inflight;
    [c addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
        if (on_complete) on_complete(ctx);
        dispatch_semaphore_signal(sem);
    }];
    [c commit];
    S->frame_open = false;
}

// What the window's scene mirror needs of a texture: its serial (0: none), size and wrap modes
// (0 repeat, 1 clamp to edge, 2 mirrored repeat; pc_mirror.cpp maps them to the bridge's).
uint64_t pcm_texture_info(unsigned name, int* width, int* height, int* wrap_s, int* wrap_t) {
    if (!S) return 0;
    Tex* t = get_obj(S->textures, name);
    if (!t || !t->mtl) return 0;
    auto wrap = [](GLenum w) { return w == GL_CLAMP_TO_EDGE ? 1 : (w == GL_MIRRORED_REPEAT ? 2 : 0); };
    if (width) *width = t->w;
    if (height) *height = t->h;
    if (wrap_s) *wrap_s = wrap(t->ws);
    if (wrap_t) *wrap_t = wrap(t->wt);
    return t->serial;
}

// A texture's pixels, RGBA8, its first row first (the game's top row). False when it can't be
// read on the CPU (a private render target).
int pcm_texture_read_rgba(unsigned name, unsigned char* out) {
    if (!S || !out) return 0;
    Tex* t = get_obj(S->textures, name);
    if (!t || !t->mtl) return 0;
    id<MTLTexture> mt = (__bridge id<MTLTexture>)t->mtl;
    if (mt.storageMode == MTLStorageModePrivate) return 0;
    if (mt.usage & MTLTextureUsageRenderTarget) submit(true);  // drawn into this frame: let the GPU finish
    [mt getBytes:out bytesPerRow:(NSUInteger)t->w * 4 fromRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0];
    if (is_bgra(mt.pixelFormat)) {
        for (size_t i = 0; i < (size_t)t->w * t->h; i++) {
            unsigned char c = out[i * 4];
            out[i * 4] = out[i * 4 + 2];
            out[i * 4 + 2] = c;
        }
    }
    return 1;
}

// Runs `fn` with the frame's command buffer and the default framebuffer's colour and depth (all
// borrowed), with nothing else encoding: the window copies what's drawn so far into its IOSurfaces.
void pcm_encode_with_default_framebuffer(void (*fn)(void* command_buffer, void* color, void* depth, void* ctx),
                                         void* ctx) {
    if (!S || !fn || !S->color) return;
    flush_pending_clears();
    end_encoder();
    begin_cmd_if_needed();
    ensure_depth();
    fn((__bridge void*)S->cmd, (__bridge void*)S->color, (__bridge void*)S->depth, ctx);
}

// A GL texture name for a texture the host owns (an OpenXR swapchain image), so FBOs can
// render into it. unorm_view: render through a non-sRGB view of an sRGB texture (the
// game writes gamma-encoded colour; the compositor reads the bytes as sRGB).
unsigned pcm_wrap_texture(void* mtl_texture, int unorm_view) {
    if (!S || !mtl_texture) return 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)mtl_texture;
    if (unorm_view) {
        MTLPixelFormat f = t.pixelFormat;
        MTLPixelFormat view = f == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm
                            : f == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
                                                                 : f;
        if (view != f) t = [t newTextureViewWithPixelFormat:view];
    }
    GLuint name = alloc_name(S->textures);
    Tex& x = S->textures[name];
    x.mtl = (void*)CFBridgingRetain(t);
    x.serial = ++g_texture_serial;
    x.w = (int)t.width;
    x.h = (int)t.height;
    x.external = true;
    x.minf = x.magf = GL_LINEAR;
    x.ws = x.wt = GL_CLAMP_TO_EDGE;
    return name;
}

// The texture behind a GL texture name (borrowed), or NULL.
void* pcm_texture(unsigned name) {
    if (!S) return nullptr;
    Tex* t = get_obj(S->textures, name);
    return (t && t->mtl) ? t->mtl : nullptr;
}

// GPU waits for `event` to reach `value` before any later work in this frame (the
// compositor's last read of a swapchain image).
void pcm_encode_wait_event(void* event, uint64_t value) {
    if (!S || !event) return;
    flush_pending_clears();
    end_encoder();
    begin_cmd_if_needed();
    [S->cmd encodeWaitForEvent:(__bridge id<MTLEvent>)event value:value];
}

// `event` reaches `value` once everything encoded so far has run (the writes into a
// swapchain image are done).
void pcm_encode_signal_event(void* event, uint64_t value) {
    if (!S || !event) return;
    flush_pending_clears();
    end_encoder();
    begin_cmd_if_needed();
    [S->cmd encodeSignalEvent:(__bridge id<MTLEvent>)event value:value];
}

// Commits the work encoded so far without ending the frame (before handing images on).
void pcm_commit(void) {
    if (S) submit(false);
}

// A premultiplied colour behind what the bound draw framebuffer already holds, over the
// GL-pixel rectangle rect (x0, y0, x1, y1): a backdrop slid under a drawn UI.
void pcm_draw_under(const float rgba_premultiplied[4], const float rect[4]) {
    if (!S) return;
    float uv[4] = {0, 0, 1, 1};
    draw_rect(nil, uv, rect, rgba_premultiplied, false, 1);
}

// A premultiplied colour over the GL-pixel rectangle rect of the bound draw framebuffer.
void pcm_draw_over(const float rgba_premultiplied[4], const float rect[4]) {
    if (!S) return;
    float uv[4] = {0, 0, 1, 1};
    draw_rect(nil, uv, rect, rgba_premultiplied, false, 2);
}

void pcm_draw_vignette_rect(float strength, const float rect[4]);

// A black vignette over the whole bound draw framebuffer, `strength` 0..1 (comfort).
void pcm_draw_vignette(float strength) {
    if (!S || strength <= 0.0f) return;
    Target t;
    if (!resolve_framebuffer(S->draw_fb, &t)) return;
    float rect[4] = {0, 0, (float)t.w, (float)t.h};
    pcm_draw_vignette_rect(strength, rect);
}

// The vignette over a GL-pixel rectangle of the bound draw framebuffer (one eye of a stereo target).
void pcm_draw_vignette_rect(float strength, const float rect[4]) {
    if (!S || strength <= 0.0f) return;
    static id<MTLTexture> gradient = nil;
    if (!gradient) {
        const int n = 128;
        mvec<uint8_t> px((size_t)n * n * 4, 0);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                float dx = (x + 0.5f) / n * 2.0f - 1.0f, dy = (y + 0.5f) / n * 2.0f - 1.0f;
                float r = sqrtf(dx * dx + dy * dy);  // 1 at the edge's middle, 1.41 in the corners
                float t = (r - 0.55f) / (1.0f - 0.55f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                t = t * t * (3.0f - 2.0f * t);
                px[((size_t)y * n + x) * 4 + 3] = (uint8_t)(t * 255.0f + 0.5f);  // premultiplied black
            }
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:n
                                                                                     height:n
                                                                                  mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        gradient = [S->device newTextureWithDescriptor:d];
        [gradient replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0 withBytes:px.data() bytesPerRow:n * 4];
    }
    float uv[4] = {0, 0, 1, 1};
    float tint[4] = {strength, strength, strength, strength};
    draw_rect(gradient, uv, rect, tint, true, 2);
}

// ---- single-pass stereo (pc_metal_stereo.h) ----

void pcm_stereo_target(unsigned framebuffer, int eye_width) {
    if (!S) return;
    S->stereo_fb = framebuffer;
    S->stereo_eye_w = framebuffer ? eye_width : 0;
    S->stereo_only = -1;
    S->enc_dirty = true;
}

void pcm_stereo_eye_only(int eye) {
    if (!S) return;
    S->stereo_only = eye < 0 ? -1 : (eye & 1);
}

void pcm_stereo_right(const float* modelview44, const float* projection44) {
    if (!S) return;
    Program* p = get_obj(S->programs, S->program);
    if (!p || p->kind != PROG_TEV) return;
    float* mv = (float*)(p->block + (size_t)104 * 16);
    float* pr = (float*)(p->block + (size_t)108 * 16);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            mv[c * 4 + r] = modelview44[r * 4 + c];
            pr[c * 4 + r] = projection44[r * 4 + c];
        }
    p->rev++;
}

// A new MTLSharedEvent (retained) for the host's GPU hand-offs.
void* pcm_new_shared_event(void) {
    if (!S) return nullptr;
    return (void*)CFBridgingRetain([S->device newSharedEvent]);
}

unsigned char* pcm_read_default_framebuffer(int* width, int* height) {
    if (!S || !S->color) return nullptr;
    submit(true);
    int w = S->fbw, h = S->fbh;
    unsigned char* out = (unsigned char*)malloc((size_t)w * h * 4);
    [S->color getBytes:out bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    if (S->color.pixelFormat == MTLPixelFormatBGRA8Unorm) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            unsigned char t = out[i * 4];
            out[i * 4] = out[i * 4 + 2];
            out[i * 4 + 2] = t;
        }
    }
    *width = w;
    *height = h;
    return out;
}

}  // extern "C"
