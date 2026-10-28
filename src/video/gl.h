// Minimal OpenGL 3.3 core loader (function pointers resolved via SDL).
#ifndef TSFP_GL_H
#define TSFP_GL_H

#include <cstddef>
#include <cstdint>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
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
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT 0x1902
#define GL_UNSIGNED_INT 0x1405
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define TSFP_GL_FUNCS(X)                                                                         \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                            \
    X(void, Scissor, GLint, GLint, GLsizei, GLsizei)                                             \
    X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                      \
    X(void, ClearDepth, double)                                                                   \
    X(void, Clear, GLbitfield)                                                                    \
    X(void, Enable, GLenum)                                                                       \
    X(void, Disable, GLenum)                                                                      \
    X(void, DepthFunc, GLenum)                                                                    \
    X(void, DepthMask, GLboolean)                                                                 \
    X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                               \
    X(void, BlendFunc, GLenum, GLenum)                                                            \
    X(void, BlendEquation, GLenum)                                                                \
    X(void, CullFace, GLenum)                                                                     \
    X(void, GenTextures, GLsizei, GLuint*)                                                        \
    X(void, DeleteTextures, GLsizei, const GLuint*)                                               \
    X(void, BindTexture, GLenum, GLuint)                                                          \
    X(void, ActiveTexture, GLenum)                                                                \
    X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, TexParameteri, GLenum, GLenum, GLint)                                                 \
    X(void, CopyTexSubImage2D, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)       \
    X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)                    \
    X(void, GenBuffers, GLsizei, GLuint*)                                                         \
    X(void, BindBuffer, GLenum, GLuint)                                                           \
    X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum)                                  \
    X(void, GenVertexArrays, GLsizei, GLuint*)                                                    \
    X(void, BindVertexArray, GLuint)                                                              \
    X(void, EnableVertexAttribArray, GLuint)                                                      \
    X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)          \
    X(void, DrawArrays, GLenum, GLint, GLsizei)                                                   \
    X(GLuint, CreateShader, GLenum)                                                               \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                    \
    X(void, CompileShader, GLuint)                                                                \
    X(void, GetShaderiv, GLuint, GLenum, GLint*)                                                  \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                 \
    X(GLuint, CreateProgram, void)                                                                \
    X(void, AttachShader, GLuint, GLuint)                                                         \
    X(void, LinkProgram, GLuint)                                                                  \
    X(void, GetProgramiv, GLuint, GLenum, GLint*)                                                 \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                \
    X(void, UseProgram, GLuint)                                                                   \
    X(void, DeleteShader, GLuint)                                                                 \
    X(GLint, GetUniformLocation, GLuint, const GLchar*)                                           \
    X(void, Uniform1i, GLint, GLint)                                                              \
    X(void, Uniform1iv, GLint, GLsizei, const GLint*)                                             \
    X(void, Uniform4fv, GLint, GLsizei, const GLfloat*)                                           \
    X(void, Uniform4iv, GLint, GLsizei, const GLint*)                                             \
    X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                          \
    X(void, BindAttribLocation, GLuint, GLuint, const GLchar*)                                    \
    X(void, GenFramebuffers, GLsizei, GLuint*)                                                    \
    X(void, BindFramebuffer, GLenum, GLuint)                                                      \
    X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                          \
    X(GLenum, CheckFramebufferStatus, GLenum)                                                     \
    X(void, BlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)

#define TSFP_GL_DECL(ret, name, ...) extern ret (*gl##name)(__VA_ARGS__);
TSFP_GL_FUNCS(TSFP_GL_DECL)
#undef TSFP_GL_DECL

bool gl_load();

#endif  // TSFP_GL_H
