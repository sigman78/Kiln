// examples/gl/gl_api.h — the OpenGL 4.6 core subset the GL examples call, loaded through GLFW.
// No loader library: one table of functions, one table of constants.
#pragma once

#include <kiln/core.h>

#include <cstddef>

#if defined(_WIN32) && !defined(_WIN64)
#define KILN_GLAPI __stdcall
#else
#define KILN_GLAPI
#endif

namespace kiln::glx {

using GLenum      = unsigned int;
using GLbitfield  = unsigned int;
using GLuint      = unsigned int;
using GLint       = int;
using GLsizei     = int;
using GLboolean   = unsigned char;
using GLfloat     = float;
using GLchar      = char;
using GLubyte     = unsigned char;
using GLuint64    = unsigned long long;
using GLintptr    = std::ptrdiff_t;
using GLsizeiptr  = std::ptrdiff_t;
using GLsync      = struct __GLsync*;
using GLDEBUGPROC = void(KILN_GLAPI*)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                      GLchar const* message, void const* user);

// clang-format off
inline constexpr GLenum GL_FALSE = 0, GL_TRUE = 1;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402,
                        GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
                        GL_HALF_FLOAT = 0x140B;
inline constexpr GLenum GL_RED = 0x1903, GL_RG = 0x8227, GL_RGB = 0x1907, GL_RGBA = 0x1908;
inline constexpr GLenum GL_R8 = 0x8229, GL_R16 = 0x822A, GL_RG8 = 0x822B, GL_RG16 = 0x822C,
                        GL_R16F = 0x822D, GL_R32F = 0x822E, GL_RG16F = 0x822F, GL_RG32F = 0x8230,
                        GL_RGBA8 = 0x8058, GL_RGBA16 = 0x805B, GL_SRGB8_ALPHA8 = 0x8C43,
                        GL_RGBA16F = 0x881A, GL_RGBA32F = 0x8814, GL_R8_SNORM = 0x8F94,
                        GL_RG8_SNORM = 0x8F95, GL_RGBA8_SNORM = 0x8F97, GL_R16_SNORM = 0x8F98,
                        GL_RG16_SNORM = 0x8F99, GL_RGBA16_SNORM = 0x8F9B;
inline constexpr GLenum GL_COMPRESSED_RED_RGTC1 = 0x8DBB, GL_COMPRESSED_RG_RGTC2 = 0x8DBD,
                        GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8E8C, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM = 0x8E8D,
                        GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT = 0x8E8F;
inline constexpr GLenum GL_COMPRESSED_RGB_S3TC_DXT1_EXT = 0x83F0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
                        GL_COMPRESSED_SRGB_S3TC_DXT1_EXT = 0x8C4C, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT = 0x8C4F;
inline constexpr GLenum GL_NUM_EXTENSIONS = 0x821D, GL_EXTENSIONS = 0x1F03;
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE_CUBE_MAP = 0x8513, GL_TEXTURE_2D_ARRAY = 0x8C1A;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
                        GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_TEXTURE_WRAP_R = 0x8072,
                        GL_TEXTURE_MAX_ANISOTROPY = 0x84FE;
inline constexpr GLenum GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
                        GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_NO_ERROR = 0, GL_OUT_OF_MEMORY = 0x0505;
inline constexpr GLenum GL_PIXEL_UNPACK_BUFFER = 0x88EC, GL_SHADER_STORAGE_BUFFER = 0x90D2,
                        GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT = 0x90DF;
inline constexpr GLbitfield GL_MAP_WRITE_BIT = 0x0002, GL_MAP_PERSISTENT_BIT = 0x0040,
                            GL_MAP_COHERENT_BIT = 0x0080;
inline constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_ALREADY_SIGNALED = 0x911A,
                        GL_TIMEOUT_EXPIRED = 0x911B, GL_CONDITION_SATISFIED = 0x911C, GL_WAIT_FAILED = 0x911D;
inline constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 0x0001;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                        GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71, GL_LEQUAL = 0x0203, GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F,
                        GL_DEBUG_OUTPUT = 0x92E0, GL_DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;
inline constexpr GLenum GL_DEBUG_SEVERITY_HIGH = 0x9146, GL_DEBUG_SEVERITY_MEDIUM = 0x9147,
                        GL_DEBUG_SEVERITY_NOTIFICATION = 0x826B;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100, GL_COLOR_BUFFER_BIT = 0x4000;
inline constexpr GLenum GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
                        GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00,
                        GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

#define KILN_GL_FUNCTIONS(X)                                                                                   \
    X(void, glFinish, ())                                                                                      \
    X(void, glClear, (GLbitfield mask))                                                                        \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                        \
    X(void, glEnable, (GLenum cap))                                                                            \
    X(void, glDisable, (GLenum cap))                                                                           \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                              \
    X(void, glDepthFunc, (GLenum func))                                                                        \
    X(void, glDepthMask, (GLboolean flag))                                                                     \
    X(GLubyte const*, glGetString, (GLenum name))                                                              \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                                        \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data))    \
    X(void, glDebugMessageCallback, (GLDEBUGPROC callback, void const* user))                                  \
    X(void, glCreateBuffers, (GLsizei n, GLuint* buffers))                                                     \
    X(void, glNamedBufferStorage, (GLuint buffer, GLsizeiptr size, void const* data, GLbitfield flags))        \
    X(void*, glMapNamedBufferRange, (GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access))    \
    X(GLboolean, glUnmapNamedBuffer, (GLuint buffer))                                                          \
    X(void, glDeleteBuffers, (GLsizei n, GLuint const* buffers))                                               \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                                      \
    X(void, glCopyNamedBufferSubData,                                                                          \
      (GLuint src, GLuint dst, GLintptr srcOffset, GLintptr dstOffset, GLsizeiptr size))                       \
    X(void, glCreateTextures, (GLenum target, GLsizei n, GLuint* textures))                                    \
    X(void, glTextureStorage2D, (GLuint texture, GLsizei levels, GLenum format, GLsizei w, GLsizei h))         \
    X(void, glTextureStorage3D,                                                                                \
      (GLuint texture, GLsizei levels, GLenum format, GLsizei w, GLsizei h, GLsizei d))                        \
    X(void, glTextureSubImage2D,                                                                               \
      (GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,        \
       void const* pixels))                                                                                    \
    X(void, glTextureSubImage3D,                                                                               \
      (GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, \
       GLenum type, void const* pixels))                                                                       \
    X(void, glCompressedTextureSubImage2D,                                                                     \
      (GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size,       \
       void const* data))                                                                                      \
    X(void, glCompressedTextureSubImage3D,                                                                     \
      (GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, \
       GLsizei size, void const* data))                                                                        \
    X(void, glGetTextureSubImage,                                                                              \
      (GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, \
       GLenum type, GLsizei size, void* data))                                                                 \
    X(void, glGetCompressedTextureSubImage,                                                                    \
      (GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLsizei size,  \
       void* data))                                                                                            \
    X(GLubyte const*, glGetStringi, (GLenum name, GLuint index))                                               \
    X(void, glDeleteTextures, (GLsizei n, GLuint const* textures))                                             \
    X(void, glBindTextureUnit, (GLuint unit, GLuint texture))                                                  \
    X(void, glCreateSamplers, (GLsizei n, GLuint* samplers))                                                   \
    X(void, glSamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                                  \
    X(void, glSamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))                                \
    X(void, glBindSampler, (GLuint unit, GLuint sampler))                                                      \
    X(void, glDeleteSamplers, (GLsizei n, GLuint const* samplers))                                             \
    X(GLsync, glFenceSync, (GLenum condition, GLbitfield flags))                                               \
    X(GLenum, glClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                             \
    X(void, glDeleteSync, (GLsync sync))                                                                       \
    X(void, glCreateVertexArrays, (GLsizei n, GLuint* arrays))                                                 \
    X(void, glDeleteVertexArrays, (GLsizei n, GLuint const* arrays))                                           \
    X(void, glBindVertexArray, (GLuint array))                                                                 \
    X(void, glVertexArrayVertexBuffer,                                                                         \
      (GLuint vao, GLuint binding, GLuint buffer, GLintptr offset, GLsizei stride))                            \
    X(void, glVertexArrayElementBuffer, (GLuint vao, GLuint buffer))                                           \
    X(void, glVertexArrayAttribFormat,                                                                         \
      (GLuint vao, GLuint attrib, GLint size, GLenum type, GLboolean normalized, GLuint offset))               \
    X(void, glVertexArrayAttribBinding, (GLuint vao, GLuint attrib, GLuint binding))                           \
    X(void, glEnableVertexArrayAttrib, (GLuint vao, GLuint attrib))                                            \
    X(void, glVertexAttrib4f, (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                      \
    X(GLuint, glCreateShader, (GLenum type))                                                                   \
    X(void, glShaderSource, (GLuint shader, GLsizei count, GLchar const* const* text, GLint const* length))    \
    X(void, glCompileShader, (GLuint shader))                                                                  \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* value))                                        \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei* length, GLchar* log))                   \
    X(void, glDeleteShader, (GLuint shader))                                                                   \
    X(GLuint, glCreateProgram, ())                                                                             \
    X(void, glAttachShader, (GLuint program, GLuint shader))                                                   \
    X(void, glLinkProgram, (GLuint program))                                                                   \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* value))                                      \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei size, GLsizei* length, GLchar* log))                 \
    X(void, glDeleteProgram, (GLuint program))                                                                 \
    X(void, glUseProgram, (GLuint program))                                                                    \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, GLfloat const* value))    \
    X(void, glUniform3f, (GLint location, GLfloat x, GLfloat y, GLfloat z))                                    \
    X(void, glUniform1f, (GLint location, GLfloat x))                                                          \
    X(void, glUniform1ui, (GLint location, GLuint x))                                                          \
    X(void, glUniform1uiv, (GLint location, GLsizei count, GLuint const* value))                               \
    X(void, glUniform4fv, (GLint location, GLsizei count, GLfloat const* value))                               \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))                                    \
    X(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)) \
    X(void, glGetIntegerv, (GLenum pname, GLint* data))                                                        \
    X(GLenum, glGetError, ())                                                                                  \
    X(void, glDrawElementsBaseVertex,                                                                          \
      (GLenum mode, GLsizei count, GLenum type, void const* indices, GLint baseVertex))                        \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                                           \
    X(void, glCreateFramebuffers, (GLsizei n, GLuint* framebuffers))                                           \
    X(void, glDeleteFramebuffers, (GLsizei n, GLuint const* framebuffers))                                     \
    X(void, glCreateRenderbuffers, (GLsizei n, GLuint* renderbuffers))                                         \
    X(void, glDeleteRenderbuffers, (GLsizei n, GLuint const* renderbuffers))                                   \
    X(void, glNamedRenderbufferStorage, (GLuint renderbuffer, GLenum format, GLsizei w, GLsizei h))            \
    X(void, glNamedFramebufferRenderbuffer,                                                                    \
      (GLuint framebuffer, GLenum attachment, GLenum target, GLuint renderbuffer))                             \
    X(GLenum, glCheckNamedFramebufferStatus, (GLuint framebuffer, GLenum target))                              \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                                            \
    X(void, glNamedFramebufferReadBuffer, (GLuint framebuffer, GLenum mode))                                   \
    X(void, glBlitNamedFramebuffer,                                                                            \
      (GLuint src, GLuint dst, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,    \
       GLint dy1, GLbitfield mask, GLenum filter))
// clang-format on

// ARB_bindless_texture (not core): loaded by load_gl_bindless().
#define KILN_GL_BINDLESS_FUNCTIONS(X)                                                                        \
    X(GLuint64, glGetTextureSamplerHandleARB, (GLuint texture, GLuint sampler))                              \
    X(void, glMakeTextureHandleResidentARB, (GLuint64 handle))                                               \
    X(void, glMakeTextureHandleNonResidentARB, (GLuint64 handle))

#define KILN_GL_DECLARE(ret, name, params)                                                                   \
    using PFN_##name = ret(KILN_GLAPI*) params;                                                              \
    extern PFN_##name name;
KILN_GL_FUNCTIONS(KILN_GL_DECLARE)
KILN_GL_BINDLESS_FUNCTIONS(KILN_GL_DECLARE)
#undef KILN_GL_DECLARE

/// Loads every function above for the current context. False (and logs the first missing one)
/// if the context is older than 4.6 core.
[[nodiscard]] bool load_gl() noexcept;
/// Loads ARB_bindless_texture; false if the driver does not offer it.
[[nodiscard]] bool load_gl_bindless() noexcept;

} // namespace kiln::glx
