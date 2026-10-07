// OpenGL ES 2.0 (the functions GLES1 doesn't already provide; gles1.cpp has the
// shared ones like glClear, glTexImage2D, glDrawArrays). Like GLES1, nearly
// everything is scalars or pointers into guest memory.
//
// Special cases:
//   - glShaderSource takes an array of string pointers (4-byte on the guest).
//   - "pointer" arguments of glVertexAttribPointer / gl*Pointer / glDrawElements
//     are byte offsets, not addresses, while a buffer object is bound; then
//     they're passed through unchanged.
#include <vector>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

#ifdef __ANDROID__
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#endif

// X(name, signature)
#define H32_GLES2_FUNCS(X)                                                                      \
    X(glAttachShader, void(GLuint, GLuint))                                                     \
    X(glBindAttribLocation, void(GLuint, GLuint, const GLchar*))                                \
    X(glBindBuffer, void(GLenum, GLuint))                                                       \
    X(glBindFramebuffer, void(GLenum, GLuint))                                                  \
    X(glBindRenderbuffer, void(GLenum, GLuint))                                                 \
    X(glBlendColor, void(GLfloat, GLfloat, GLfloat, GLfloat))                                   \
    X(glBlendEquation, void(GLenum))                                                            \
    X(glBlendEquationSeparate, void(GLenum, GLenum))                                            \
    X(glBlendFuncSeparate, void(GLenum, GLenum, GLenum, GLenum))                                \
    X(glBufferData, void(GLenum, GLsizeiptr, const void*, GLenum))                              \
    X(glBufferSubData, void(GLenum, GLintptr, GLsizeiptr, const void*))                         \
    X(glCheckFramebufferStatus, GLenum(GLenum))                                                 \
    X(glCompileShader, void(GLuint))                                                            \
    X(glCompressedTexSubImage2D, void(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*)) \
    X(glCopyTexSubImage2D, void(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))   \
    X(glCreateProgram, GLuint())                                                                \
    X(glCreateShader, GLuint(GLenum))                                                           \
    X(glDeleteBuffers, void(GLsizei, const GLuint*))                                            \
    X(glDeleteFramebuffers, void(GLsizei, const GLuint*))                                       \
    X(glDeleteProgram, void(GLuint))                                                            \
    X(glDeleteRenderbuffers, void(GLsizei, const GLuint*))                                      \
    X(glDeleteShader, void(GLuint))                                                             \
    X(glDepthRangef, void(GLfloat, GLfloat))                                                    \
    X(glDetachShader, void(GLuint, GLuint))                                                     \
    X(glDisableVertexAttribArray, void(GLuint))                                                 \
    X(glEnableVertexAttribArray, void(GLuint))                                                  \
    X(glFramebufferRenderbuffer, void(GLenum, GLenum, GLenum, GLuint))                          \
    X(glFramebufferTexture2D, void(GLenum, GLenum, GLenum, GLuint, GLint))                      \
    X(glGenBuffers, void(GLsizei, GLuint*))                                                     \
    X(glGenerateMipmap, void(GLenum))                                                           \
    X(glGenFramebuffers, void(GLsizei, GLuint*))                                                \
    X(glGenRenderbuffers, void(GLsizei, GLuint*))                                               \
    X(glGetActiveAttrib, void(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))     \
    X(glGetActiveUniform, void(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*))    \
    X(glGetAttachedShaders, void(GLuint, GLsizei, GLsizei*, GLuint*))                           \
    X(glGetAttribLocation, GLint(GLuint, const GLchar*))                                        \
    X(glGetBufferParameteriv, void(GLenum, GLenum, GLint*))                                     \
    X(glGetFramebufferAttachmentParameteriv, void(GLenum, GLenum, GLenum, GLint*))              \
    X(glGetProgramiv, void(GLuint, GLenum, GLint*))                                             \
    X(glGetProgramInfoLog, void(GLuint, GLsizei, GLsizei*, GLchar*))                            \
    X(glGetRenderbufferParameteriv, void(GLenum, GLenum, GLint*))                               \
    X(glGetShaderiv, void(GLuint, GLenum, GLint*))                                              \
    X(glGetShaderInfoLog, void(GLuint, GLsizei, GLsizei*, GLchar*))                             \
    X(glGetShaderPrecisionFormat, void(GLenum, GLenum, GLint*, GLint*))                         \
    X(glGetShaderSource, void(GLuint, GLsizei, GLsizei*, GLchar*))                              \
    X(glGetTexParameterfv, void(GLenum, GLenum, GLfloat*))                                      \
    X(glGetTexParameteriv, void(GLenum, GLenum, GLint*))                                        \
    X(glGetUniformfv, void(GLuint, GLint, GLfloat*))                                            \
    X(glGetUniformiv, void(GLuint, GLint, GLint*))                                              \
    X(glGetUniformLocation, GLint(GLuint, const GLchar*))                                       \
    X(glGetVertexAttribfv, void(GLuint, GLenum, GLfloat*))                                      \
    X(glGetVertexAttribiv, void(GLuint, GLenum, GLint*))                                        \
    X(glIsBuffer, GLboolean(GLuint))                                                            \
    X(glIsFramebuffer, GLboolean(GLuint))                                                       \
    X(glIsProgram, GLboolean(GLuint))                                                           \
    X(glIsRenderbuffer, GLboolean(GLuint))                                                      \
    X(glIsShader, GLboolean(GLuint))                                                            \
    X(glIsTexture, GLboolean(GLuint))                                                           \
    X(glLinkProgram, void(GLuint))                                                              \
    X(glReleaseShaderCompiler, void())                                                          \
    X(glRenderbufferStorage, void(GLenum, GLenum, GLsizei, GLsizei))                            \
    X(glSampleCoverage, void(GLfloat, GLboolean))                                               \
    X(glShaderBinary, void(GLsizei, const GLuint*, GLenum, const void*, GLsizei))               \
    X(glStencilFuncSeparate, void(GLenum, GLenum, GLint, GLuint))                               \
    X(glStencilMaskSeparate, void(GLenum, GLuint))                                              \
    X(glStencilOpSeparate, void(GLenum, GLenum, GLenum, GLenum))                                \
    X(glTexParameterfv, void(GLenum, GLenum, const GLfloat*))                                   \
    X(glUniform1f, void(GLint, GLfloat))                                                        \
    X(glUniform2f, void(GLint, GLfloat, GLfloat))                                               \
    X(glUniform3f, void(GLint, GLfloat, GLfloat, GLfloat))                                      \
    X(glUniform4f, void(GLint, GLfloat, GLfloat, GLfloat, GLfloat))                             \
    X(glUniform1i, void(GLint, GLint))                                                          \
    X(glUniform2i, void(GLint, GLint, GLint))                                                   \
    X(glUniform3i, void(GLint, GLint, GLint, GLint))                                            \
    X(glUniform4i, void(GLint, GLint, GLint, GLint, GLint))                                     \
    X(glUniform1fv, void(GLint, GLsizei, const GLfloat*))                                       \
    X(glUniform2fv, void(GLint, GLsizei, const GLfloat*))                                       \
    X(glUniform3fv, void(GLint, GLsizei, const GLfloat*))                                       \
    X(glUniform4fv, void(GLint, GLsizei, const GLfloat*))                                       \
    X(glUniform1iv, void(GLint, GLsizei, const GLint*))                                         \
    X(glUniform2iv, void(GLint, GLsizei, const GLint*))                                         \
    X(glUniform3iv, void(GLint, GLsizei, const GLint*))                                         \
    X(glUniform4iv, void(GLint, GLsizei, const GLint*))                                         \
    X(glUniformMatrix2fv, void(GLint, GLsizei, GLboolean, const GLfloat*))                      \
    X(glUniformMatrix3fv, void(GLint, GLsizei, GLboolean, const GLfloat*))                      \
    X(glUniformMatrix4fv, void(GLint, GLsizei, GLboolean, const GLfloat*))                      \
    X(glUseProgram, void(GLuint))                                                               \
    X(glValidateProgram, void(GLuint))                                                          \
    X(glVertexAttrib1f, void(GLuint, GLfloat))                                                  \
    X(glVertexAttrib2f, void(GLuint, GLfloat, GLfloat))                                         \
    X(glVertexAttrib3f, void(GLuint, GLfloat, GLfloat, GLfloat))                                \
    X(glVertexAttrib4f, void(GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                       \
    X(glVertexAttrib1fv, void(GLuint, const GLfloat*))                                          \
    X(glVertexAttrib2fv, void(GLuint, const GLfloat*))                                          \
    X(glVertexAttrib3fv, void(GLuint, const GLfloat*))                                          \
    X(glVertexAttrib4fv, void(GLuint, const GLfloat*))

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }

#ifdef __ANDROID__

// A guest "pointer" while a buffer object is bound to [binding] is an offset into it.
const void* gl_pointer(GLenum binding, uint32_t guest) {
    GLint bound = 0;
    glGetIntegerv(binding, &bound);
    if (bound) return reinterpret_cast<const void*>(uintptr_t(guest));
    return mem().ptr<const void>(guest);
}

void t_glVertexAttribPointer(GuestThread& t) {
    glVertexAttribPointer(arg(t, 0), int32_t(arg(t, 1)), arg(t, 2), GLboolean(arg(t, 3)), int32_t(arg(t, 4)),
                          gl_pointer(GL_ARRAY_BUFFER_BINDING, arg(t, 5)));
}

void t_glDrawElements(GuestThread& t) {
    glDrawElements(arg(t, 0), int32_t(arg(t, 1)), arg(t, 2), gl_pointer(GL_ELEMENT_ARRAY_BUFFER_BINDING, arg(t, 3)));
}

// GLES1 client arrays, same rule.
template <void (*Fn)(GLint, GLenum, GLsizei, const void*)>
void t_gl1_pointer(GuestThread& t) {
    Fn(int32_t(arg(t, 0)), arg(t, 1), int32_t(arg(t, 2)), gl_pointer(GL_ARRAY_BUFFER_BINDING, arg(t, 3)));
}
void t_glNormalPointer(GuestThread& t) {
    glNormalPointer(arg(t, 0), int32_t(arg(t, 1)), gl_pointer(GL_ARRAY_BUFFER_BINDING, arg(t, 2)));
}

// glShaderSource(shader, count, const char* const* strings, const GLint* lengths)
void t_glShaderSource(GuestThread& t) {
    int32_t count = int32_t(arg(t, 1));
    uint32_t strings = arg(t, 2);
    std::vector<const GLchar*> host(count > 0 ? count : 0);
    for (int32_t i = 0; i < count; i++) host[i] = mem().str(mem().read<uint32_t>(strings + i * 4));
    glShaderSource(arg(t, 0), count, host.data(), mem().ptr<const GLint>(arg(t, 3)));
}

#else  // Linux harness: no GPU

void t_gl_noop(GuestThread& t) { t.regs()[0] = 0; }
void t_gl_gen(GuestThread& t) {
    static uint32_t next = 1;
    int32_t n = int32_t(t.regs()[0]);
    for (int32_t i = 0; i < n; i++) mem().write<uint32_t>(t.regs()[1] + i * 4, next++);
}
void t_gl_create(GuestThread& t) {
    static uint32_t next = 1;
    t.regs()[0] = next++;
}
// glGet{Shader,Program}iv: report success (compile/link status, empty logs).
void t_gl_getiv(GuestThread& t) {
    uint32_t pname = arg(t, 1);
    int32_t v = (pname == 0x8B81 || pname == 0x8B82) ? 1 : 0;  // GL_COMPILE_STATUS / GL_LINK_STATUS
    if (uint32_t out = arg(t, 2)) mem().write<int32_t>(out, v);
}

#endif

}  // namespace

namespace thunks {

void register_gles2() {
#ifdef __ANDROID__
#define H32_GL_ADD(name, sig) add(#name, H32_WRAP(::name, sig));
    H32_GLES2_FUNCS(H32_GL_ADD)
#undef H32_GL_ADD
    add("glShaderSource", t_glShaderSource);
    add("glVertexAttribPointer", t_glVertexAttribPointer);
    add("glDrawElements", t_glDrawElements);
    add("glVertexPointer", t_gl1_pointer<::glVertexPointer>);
    add("glColorPointer", t_gl1_pointer<::glColorPointer>);
    add("glTexCoordPointer", t_gl1_pointer<::glTexCoordPointer>);
    add("glNormalPointer", t_glNormalPointer);
#else
#define H32_GL_ADD(name, sig) add(#name, t_gl_noop);
    H32_GLES2_FUNCS(H32_GL_ADD)
#undef H32_GL_ADD
    add("glShaderSource", t_gl_noop);
    add("glVertexAttribPointer", t_gl_noop);
    for (const char* n : {"glGenBuffers", "glGenFramebuffers", "glGenRenderbuffers"}) add(n, t_gl_gen);
    for (const char* n : {"glCreateProgram", "glCreateShader"}) add(n, t_gl_create);
    for (const char* n : {"glGetShaderiv", "glGetProgramiv"}) add(n, t_gl_getiv);
#endif
}

}  // namespace thunks
}  // namespace h32
