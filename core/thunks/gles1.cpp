// OpenGL ES 1.x. Every argument is a scalar or a pointer into guest memory,
// so the automatic wrapper handles almost all of them. Client-side vertex
// arrays (glVertexPointer & co) are read by the driver later, at draw time;
// that's safe because guest memory never moves.
//
// On the Linux harness there is no GLES1, so calls are counted (see the
// thunk stats) and answered with harmless defaults.
#include <mutex>
#include <unordered_map>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

#ifdef __ANDROID__
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/gl.h>
#include <GLES/glext.h>
#endif

// X(name, signature)
#define H32_GLES1_FUNCS(X)                                                                   \
    X(glActiveTexture, void(GLenum))                                                         \
    X(glAlphaFunc, void(GLenum, GLclampf))                                                   \
    X(glBindTexture, void(GLenum, GLuint))                                                   \
    X(glBlendFunc, void(GLenum, GLenum))                                                     \
    X(glClear, void(GLbitfield))                                                             \
    X(glClearColor, void(GLclampf, GLclampf, GLclampf, GLclampf))                            \
    X(glClearDepthf, void(GLclampf))                                                         \
    X(glClearStencil, void(GLint))                                                           \
    X(glClearColorx, void(GLclampx, GLclampx, GLclampx, GLclampx))                           \
    X(glScalex, void(GLfixed, GLfixed, GLfixed))                                             \
    X(glTranslatex, void(GLfixed, GLfixed, GLfixed))                                         \
    X(glGetTexEnviv, void(GLenum, GLenum, GLint*))                                           \
    X(glIsEnabled, GLboolean(GLenum))                                                        \
    X(glClientActiveTexture, void(GLenum))                                                   \
    X(glColor4f, void(GLfloat, GLfloat, GLfloat, GLfloat))                                   \
    X(glColor4ub, void(GLubyte, GLubyte, GLubyte, GLubyte))                                  \
    X(glColorMask, void(GLboolean, GLboolean, GLboolean, GLboolean))                         \
    X(glColorPointer, void(GLint, GLenum, GLsizei, const GLvoid*))                           \
    X(glCompressedTexImage2D, void(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const GLvoid*)) \
    X(glCopyTexImage2D, void(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint))  \
    X(glCullFace, void(GLenum))                                                              \
    X(glDeleteTextures, void(GLsizei, const GLuint*))                                        \
    X(glDepthFunc, void(GLenum))                                                             \
    X(glDepthMask, void(GLboolean))                                                          \
    X(glDisable, void(GLenum))                                                               \
    X(glDisableClientState, void(GLenum))                                                    \
    X(glDrawArrays, void(GLenum, GLint, GLsizei))                                            \
    X(glDrawElements, void(GLenum, GLsizei, GLenum, const GLvoid*))                          \
    X(glEnable, void(GLenum))                                                                \
    X(glEnableClientState, void(GLenum))                                                     \
    X(glFinish, void())                                                                      \
    X(glFlush, void())                                                                       \
    X(glFrontFace, void(GLenum))                                                             \
    X(glGenTextures, void(GLsizei, GLuint*))                                                 \
    X(glGetBooleanv, void(GLenum, GLboolean*))                                               \
    X(glGetError, GLenum())                                                                  \
    X(glGetFloatv, void(GLenum, GLfloat*))                                                   \
    X(glGetIntegerv, void(GLenum, GLint*))                                                   \
    X(glHint, void(GLenum, GLenum))                                                          \
    X(glLightModelfv, void(GLenum, const GLfloat*))                                          \
    X(glLineWidth, void(GLfloat))                                                            \
    X(glLoadIdentity, void())                                                                \
    X(glLoadMatrixf, void(const GLfloat*))                                                   \
    X(glMaterialf, void(GLenum, GLenum, GLfloat))                                            \
    X(glMaterialfv, void(GLenum, GLenum, const GLfloat*))                                    \
    X(glMatrixMode, void(GLenum))                                                            \
    X(glMultMatrixf, void(const GLfloat*))                                                   \
    X(glNormalPointer, void(GLenum, GLsizei, const GLvoid*))                                 \
    X(glOrthof, void(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat))                  \
    X(glPixelStorei, void(GLenum, GLint))                                                    \
    X(glPointSize, void(GLfloat))                                                            \
    X(glPolygonOffset, void(GLfloat, GLfloat))                                               \
    X(glPopMatrix, void())                                                                   \
    X(glPushMatrix, void())                                                                  \
    X(glReadPixels, void(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*))           \
    X(glRotatef, void(GLfloat, GLfloat, GLfloat, GLfloat))                                   \
    X(glScalef, void(GLfloat, GLfloat, GLfloat))                                             \
    X(glScissor, void(GLint, GLint, GLsizei, GLsizei))                                       \
    X(glShadeModel, void(GLenum))                                                            \
    X(glStencilFunc, void(GLenum, GLint, GLuint))                                            \
    X(glStencilMask, void(GLuint))                                                           \
    X(glStencilOp, void(GLenum, GLenum, GLenum))                                             \
    X(glTexCoordPointer, void(GLint, GLenum, GLsizei, const GLvoid*))                        \
    X(glTexEnvf, void(GLenum, GLenum, GLfloat))                                              \
    X(glTexEnvfv, void(GLenum, GLenum, const GLfloat*))                                      \
    X(glTexEnvi, void(GLenum, GLenum, GLint))                                                \
    X(glTexImage2D, void(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*)) \
    X(glTexParameterf, void(GLenum, GLenum, GLfloat))                                        \
    X(glTexParameteri, void(GLenum, GLenum, GLint))                                          \
    X(glTexParameteriv, void(GLenum, GLenum, const GLint*))                                  \
    X(glTexSubImage2D, void(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*)) \
    X(glTranslatef, void(GLfloat, GLfloat, GLfloat))                                         \
    X(glVertexPointer, void(GLint, GLenum, GLsizei, const GLvoid*))                          \
    X(glViewport, void(GLint, GLint, GLsizei, GLsizei))                                      \
    X(glDrawTexiOES, void(GLint, GLint, GLint, GLint, GLint))                                \
    X(glBindFramebufferOES, void(GLenum, GLuint))                                            \
    X(glBindRenderbufferOES, void(GLenum, GLuint))                                           \
    X(glCheckFramebufferStatusOES, GLenum(GLenum))                                           \
    X(glDeleteFramebuffersOES, void(GLsizei, const GLuint*))                                 \
    X(glDeleteRenderbuffersOES, void(GLsizei, const GLuint*))                                \
    X(glFramebufferRenderbufferOES, void(GLenum, GLenum, GLenum, GLuint))                    \
    X(glFramebufferTexture2DOES, void(GLenum, GLenum, GLenum, GLuint, GLint))                \
    X(glGenFramebuffersOES, void(GLsizei, GLuint*))                                          \
    X(glGenRenderbuffersOES, void(GLsizei, GLuint*))                                         \
    X(glRenderbufferStorageOES, void(GLenum, GLenum, GLsizei, GLsizei))

namespace h32 {
namespace {

#ifdef __ANDROID__

// glGetString returns driver-owned strings; give the guest stable copies.
void t_glGetString(GuestThread& t) {
    static std::mutex m;
    static std::unordered_map<uint32_t, gaddr> cache;
    uint32_t name = t.regs()[0];
    std::lock_guard lk(m);
    auto& slot = cache[name];
    if (!slot) {
        auto* s = reinterpret_cast<const char*>(glGetString(name));
        if (!s) return set_ret32(t, 0);
        slot = mem().static_string(s);
    }
    set_ret32(t, slot);
}

#else  // Linux harness: no GPU

void t_gl_noop(GuestThread& t) { t.regs()[0] = 0; }

// glGen*(n, ids): hand out increasing names.
void t_gl_gen(GuestThread& t) {
    static uint32_t next = 1;
    int32_t n = int32_t(t.regs()[0]);
    for (int32_t i = 0; i < n; i++) mem().write<uint32_t>(t.regs()[1] + i * 4, next++);
}

void t_gl_get_integerv(GuestThread& t) {
    uint32_t pname = t.regs()[0];
    int32_t v = 0;
    if (pname == 0x0D33) v = 4096;       // GL_MAX_TEXTURE_SIZE
    else if (pname == 0x84E2) v = 4;     // GL_MAX_TEXTURE_UNITS
    else if (pname == 0x8CA6) v = 0;     // GL_FRAMEBUFFER_BINDING_OES
    if (gaddr out = t.regs()[1]) mem().write<int32_t>(out, v);
}

void t_gl_fb_status(GuestThread& t) { t.regs()[0] = 0x8CD5; }  // GL_FRAMEBUFFER_COMPLETE_OES

void t_glGetString(GuestThread& t) {
    static gaddr s = mem().static_string("THUMB harness (no GPU)");
    t.regs()[0] = s;
}

#endif

}  // namespace

namespace thunks {

void register_gles1() {
#ifdef __ANDROID__
#define H32_GL_ADD(name, sig) add(#name, H32_WRAP(::name, sig));
    H32_GLES1_FUNCS(H32_GL_ADD)
#undef H32_GL_ADD
#else
#define H32_GL_ADD(name, sig) add(#name, t_gl_noop);
    H32_GLES1_FUNCS(H32_GL_ADD)
#undef H32_GL_ADD
    add("glGenTextures", t_gl_gen);
    add("glGenFramebuffersOES", t_gl_gen);
    add("glGenRenderbuffersOES", t_gl_gen);
    add("glGetIntegerv", t_gl_get_integerv);
    add("glCheckFramebufferStatusOES", t_gl_fb_status);
#endif
    add("glGetString", t_glGetString);
}

}  // namespace thunks
}  // namespace h32
