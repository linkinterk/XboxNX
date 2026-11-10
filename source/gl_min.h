/*
 * XboxNX — lo mínimo de OpenGL (core 3.3) que usa gpu.c, cargado con
 * eglGetProcAddress: igual en la Switch (mesa/nouveau) y en la PC (mesa),
 * sin depender de glad ni de las cabeceras de GL de cada sistema.
 */
#ifndef XBOXNX_GL_MIN_H
#define XBOXNX_GL_MIN_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_POINTS 0x0000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_CLAMP 0x864F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_FUNC_ADD 0x8006

/* (función, tipo de retorno, argumentos) */
#define GL_FUNCS(X)                                                                                         \
    X(glGetError, GLenum, (void))                                                                           \
    X(glGetString, const GLubyte *, (GLenum))                                                              \
    X(glViewport, void, (GLint, GLint, GLsizei, GLsizei))                                                   \
    X(glScissor, void, (GLint, GLint, GLsizei, GLsizei))                                                    \
    X(glEnable, void, (GLenum))                                                                             \
    X(glDisable, void, (GLenum))                                                                            \
    X(glClear, void, (GLbitfield))                                                                          \
    X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                                             \
    X(glClearDepth, void, (GLdouble))                                                                       \
    X(glClearStencil, void, (GLint))                                                                        \
    X(glColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean))                                      \
    X(glDepthMask, void, (GLboolean))                                                                       \
    X(glDepthFunc, void, (GLenum))                                                                          \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum))                                          \
    X(glBlendEquation, void, (GLenum))                                                                      \
    X(glBlendColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                                             \
    X(glCullFace, void, (GLenum))                                                                           \
    X(glFrontFace, void, (GLenum))                                                                          \
    X(glPixelStorei, void, (GLenum, GLint))                                                                 \
    X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))                         \
    X(glGenTextures, void, (GLsizei, GLuint *))                                                             \
    X(glDeleteTextures, void, (GLsizei, const GLuint *))                                                    \
    X(glBindTexture, void, (GLenum, GLuint))                                                                \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))    \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(glTexParameteri, void, (GLenum, GLenum, GLint))                                                       \
    X(glActiveTexture, void, (GLenum))                                                                      \
    X(glGenFramebuffers, void, (GLsizei, GLuint *))                                                         \
    X(glDeleteFramebuffers, void, (GLsizei, const GLuint *))                                                \
    X(glBindFramebuffer, void, (GLenum, GLuint))                                                            \
    X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))                                \
    X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint))                                    \
    X(glCheckFramebufferStatus, GLenum, (GLenum))                                                           \
    X(glGenRenderbuffers, void, (GLsizei, GLuint *))                                                        \
    X(glDeleteRenderbuffers, void, (GLsizei, const GLuint *))                                               \
    X(glBindRenderbuffer, void, (GLenum, GLuint))                                                           \
    X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei))                                      \
    X(glCreateShader, GLuint, (GLenum))                                                                     \
    X(glDeleteShader, void, (GLuint))                                                                       \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *))                        \
    X(glCompileShader, void, (GLuint))                                                                      \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *))                                                       \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))                                     \
    X(glCreateProgram, GLuint, (void))                                                                      \
    X(glAttachShader, void, (GLuint, GLuint))                                                               \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *))                                         \
    X(glLinkProgram, void, (GLuint))                                                                        \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *))                                                      \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))                                    \
    X(glUseProgram, void, (GLuint))                                                                         \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *))                                                \
    X(glUniform1i, void, (GLint, GLint))                                                                    \
    X(glUniform1iv, void, (GLint, GLsizei, const GLint *))                                                  \
    X(glUniform1uiv, void, (GLint, GLsizei, const GLuint *))                                                \
    X(glUniform1ui, void, (GLint, GLuint))                                                                  \
    X(glUniform1f, void, (GLint, GLfloat))                                                                  \
    X(glUniform2fv, void, (GLint, GLsizei, const GLfloat *))                                                \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *))                                                \
    X(glGenVertexArrays, void, (GLsizei, GLuint *))                                                         \
    X(glBindVertexArray, void, (GLuint))                                                                    \
    X(glGenBuffers, void, (GLsizei, GLuint *))                                                              \
    X(glBindBuffer, void, (GLenum, GLuint))                                                                 \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum))                                       \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))               \
    X(glEnableVertexAttribArray, void, (GLuint))                                                            \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei))                                                         \
    X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *))                                        \
    X(glFinish, void, (void))                                                                               \
    X(glFlush, void, (void))

#define GL_DECL(name, ret, args) extern ret(*name) args;
GL_FUNCS(GL_DECL)
#undef GL_DECL

/* Carga todas las funciones; false si falta alguna (nombre en *missing). */
int gl_load(void *(*getproc)(const char *), const char **missing);

#endif
