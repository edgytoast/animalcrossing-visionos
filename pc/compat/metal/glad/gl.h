/* glad/gl.h - Metal build shim (PC_METAL).
 *
 * The PC port renders through a small subset of OpenGL 3.3 (pc_gx*.c,
 * pc_nes_fixnes.c). visionOS has no OpenGL, so the Metal build compiles that
 * same code against this header and pc_gl_metal.m implements the subset on
 * Metal: textures, one streamed vertex buffer per VAO, the TEV uber-shader
 * (pc/shaders/ac_metal.metal, the default.vert/frag port) and the NES blit.
 * Only the calls and enums the port uses exist here. */
#ifndef PC_GL_METAL_SHIM_H
#define PC_GL_METAL_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef double GLdouble;
typedef float GLclampf;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef double GLclampd;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef void GLvoid;

typedef void (*GLADapiproc)(void);
typedef GLADapiproc (*GLADloadfunc)(const char* name);
static inline int gladLoadGL(GLADloadfunc proc) { (void)proc; return 1; }

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500

#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006

#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408

#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_MULTISAMPLE 0x809D

#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_5_6_5_REV 0x8364

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D

#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_MAX_LEVEL 0x813D

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0

#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6

/* State queries the VR mod's save/restore code makes. */
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_RANGE 0x0B70
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_VIEWPORT 0x0BA2
#define GL_POLYGON_MODE 0x0B40
#define GL_FILL 0x1B02
#define GL_BUFFER_SIZE 0x8764
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_STENCIL_TEST 0x0B90
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_COVERAGE 0x80A0
#define GL_SAMPLE_MASK 0x8E51
#define GL_DEPTH_CLAMP 0x864F
#define GL_BACK_LEFT 0x0402
#define GL_READ_BUFFER 0x0C02
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04

/* No extensions: the HD texture pack's compressed formats stay off. */
#define GLAD_GL_EXT_texture_compression_s3tc 0
#define GLAD_GL_ARB_texture_compression_bptc 0
#define GLAD_GL_KHR_texture_compression_astc_ldr 0

GLenum glGetError(void);
const GLubyte* glGetString(GLenum name);
const GLubyte* glGetStringi(GLenum name, GLuint index);
void glGetIntegerv(GLenum pname, GLint* data);
void glFlush(void);
void glFinish(void);

void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glCullFace(GLenum mode);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlendEquation(GLenum mode);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthRange(GLdouble n, GLdouble f);
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glClearDepth(GLdouble d);
void glClear(GLbitfield mask);
void glLineWidth(GLfloat width);
void glPointSize(GLfloat size);
void glPixelStorei(GLenum pname, GLint param);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);

void glGenTextures(GLsizei n, GLuint* textures);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glActiveTexture(GLenum texture);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void* pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void* pixels);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                            GLsizei height, GLint border, GLsizei imageSize, const void* data);
void glTexParameteri(GLenum target, GLenum pname, GLint param);

void glGenBuffers(GLsizei n, GLuint* buffers);
void glDeleteBuffers(GLsizei n, const GLuint* buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void glGenVertexArrays(GLsizei n, GLuint* arrays);
void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
void glBindVertexArray(GLuint array);
void glEnableVertexAttribArray(GLuint index);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                           const void* pointer);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);

GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glDeleteShader(GLuint shader);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar* name);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
void glDeleteProgram(GLuint program);
void glUseProgram(GLuint program);
GLint glGetUniformLocation(GLuint program, const GLchar* name);

void glUniform1i(GLint location, GLint v0);
void glUniform2i(GLint location, GLint v0, GLint v1);
void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2);
void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void glUniform1f(GLint location, GLfloat v0);
void glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniform1iv(GLint location, GLsizei count, const GLint* value);
void glUniform2iv(GLint location, GLsizei count, const GLint* value);
void glUniform3iv(GLint location, GLsizei count, const GLint* value);
void glUniform4iv(GLint location, GLsizei count, const GLint* value);
void glUniform1fv(GLint location, GLsizei count, const GLfloat* value);
void glUniform2fv(GLint location, GLsizei count, const GLfloat* value);
void glUniform3fv(GLint location, GLsizei count, const GLfloat* value);
void glUniform4fv(GLint location, GLsizei count, const GLfloat* value);
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);

/* Framebuffer objects: a colour texture plus a depth renderbuffer (the VR mod's
 * eye and UI targets). */
void glGenFramebuffers(GLsizei n, GLuint* ids);
void glDeleteFramebuffers(GLsizei n, const GLuint* ids);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
GLenum glCheckFramebufferStatus(GLenum target);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void glGenRenderbuffers(GLsizei n, GLuint* ids);
void glDeleteRenderbuffers(GLsizei n, const GLuint* ids);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer);
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                       GLint dstY1, GLbitfield mask, GLenum filter);
void glReadBuffer(GLenum mode);

void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
GLboolean glIsEnabled(GLenum cap);
void glGetBooleanv(GLenum pname, GLboolean* data);
void glGetDoublev(GLenum pname, GLdouble* data);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
void glPolygonMode(GLenum face, GLenum mode);

/* Host side of the shim (pc_gl_metal.mm): textures the host owns, GPU fences. */
unsigned pcm_wrap_texture(void* mtl_texture, int unorm_view);
void* pcm_texture(unsigned name);
void pcm_encode_wait_event(void* event, uint64_t value);
void pcm_encode_signal_event(void* event, uint64_t value);
void pcm_commit(void);
void pcm_draw_under(const float rgba_premultiplied[4], const float rect[4]);
void pcm_draw_over(const float rgba_premultiplied[4], const float rect[4]);
void pcm_draw_vignette(float strength);
void* pcm_new_shared_event(void);
void* pcm_device(void);
uint64_t pcm_texture_info(unsigned name, int* width, int* height, int* wrap_s, int* wrap_t);
int pcm_texture_read_rgba(unsigned name, unsigned char* out);
void pcm_encode_with_default_framebuffer(void (*fn)(void* command_buffer, void* color, void* depth, void* ctx),
                                         void* ctx);

/* The port calls some uploads through glad2's prefixed names. */
#define glad_glUniform1f glUniform1f
#define glad_glUniform1i glUniform1i
#define glad_glUniform1iv glUniform1iv
#define glad_glUniform2f glUniform2f
#define glad_glUniform2i glUniform2i
#define glad_glUniform2iv glUniform2iv
#define glad_glUniform3f glUniform3f
#define glad_glUniform3fv glUniform3fv
#define glad_glUniform3i glUniform3i
#define glad_glUniform3iv glUniform3iv
#define glad_glUniform4f glUniform4f
#define glad_glUniform4fv glUniform4fv
#define glad_glUniform4i glUniform4i
#define glad_glUniform4iv glUniform4iv
#define glad_glUniformMatrix3fv glUniformMatrix3fv
#define glad_glUniformMatrix4fv glUniformMatrix4fv

/* ---- Host side (pc_gl_metal.m) ---- */

/* device: an id<MTLDevice> (bridged), or NULL for the system default. */
int  pcm_init(void* device);
void* pcm_device(void);
/* A retained BGRA8 render target (shared storage) for offscreen runs. */
void* pcm_make_target(int width, int height);
/* The "default framebuffer": an id<MTLTexture> (BGRA8Unorm or RGBA8Unorm),
 * e.g. an IOSurface-backed window slot. The shim makes the depth buffer. */
void pcm_set_default_framebuffer(void* color_texture, int width, int height);
/* Ends the frame: encodes any pending clear, commits. on_complete runs on a
 * Metal completion thread once the GPU has finished the frame. */
void pcm_present(void (*on_complete)(void* ctx), void* ctx);
/* Same, with a chance to encode more work (copies of the finished frame) into
 * the frame's command buffer (id<MTLCommandBuffer>) before it commits. */
void pcm_present_ex(void (*encode)(void* command_buffer, void* color_texture, void* ctx), void* encode_ctx,
                    void (*on_complete)(void* ctx), void* ctx);
/* Copy of the default framebuffer as top-down RGBA8 (waits for the GPU). */
unsigned char* pcm_read_default_framebuffer(int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif /* PC_GL_METAL_SHIM_H */
