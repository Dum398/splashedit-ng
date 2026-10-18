#pragma once

#include <SDL3/SDL_opengl.h>

// OpenGL entry points past 1.1, loaded through SDL at startup.
#define SPLASH_GL_FUNCS(X)                                                   \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                 \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                 \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                               \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                   \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                         \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                 \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                               \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                 \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                   \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                 \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                       \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                     \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                     \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                         \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                       \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                       \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                       \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                       \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                           \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                           \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                     \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                     \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                     \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                   \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)           \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                           \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                           \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                 \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                         \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                         \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                   \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)           \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                             \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                   \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture_)

namespace gl {
#define SPLASH_GL_DECLARE(type, name) extern type name;
SPLASH_GL_FUNCS(SPLASH_GL_DECLARE)
#undef SPLASH_GL_DECLARE

bool load();
}  // namespace gl
