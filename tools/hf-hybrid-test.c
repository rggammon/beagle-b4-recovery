/* Hybrid hard-float spike (option 2'): dlmopen isolation + namespace-scoped
 * libSGXm interposer + DWARF forward veneers, all in one process.
 *
 *   base namespace: this hard-float app + hard-float glibc
 *   new namespace : libSGXm (RTLD_GLOBAL, first) + the soft-float DDK + a
 *                   hard-float glibc that libSGXm's soft-float wrappers bridge
 *
 * libSGXm is loaded RTLD_GLOBAL into the new namespace BEFORE the DDK, so the
 * DDK's float-ABI libm/libc imports (sinf..., atof, strtod) bind to libSGXm's
 * soft-float wrappers instead of the namespace's hard-float libc/libm. The app's
 * own glibc (base namespace) is untouched. The app<->DDK boundary is bridged by
 * pcs("aapcs") veneers on the float-touching entry points (glClearColor,
 * glUniform4f). Everything else is pointer/int = ABI-neutral, called directly.
 *
 * PASS = clear color AND shader-uniform triangle both read back correct,
 * deterministically (the shader compiler exercises the DDK's internal libm/libc).
 *
 * Build hard-float; run with libSGXm + the DDK on LD_LIBRARY_PATH:
 *   arm-linux-gnueabihf-gcc -I<ddk headers> hf-hybrid-test.c -ldl -o hf-hybrid-test
 *   LD_LIBRARY_PATH=<dir with libSGXm.so.1>:/opt/sgx-ddk16/lib ./hf-hybrid-test
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef char GLchar;   /* DDK 1.6 GLES2 headers predate GLchar */

#define PCS __attribute__((pcs("aapcs")))
#define W 64
#define H 64

static void *g_egl, *g_gles;

#define RES(handle, fn)                                                  \
    do {                                                                 \
        *(void **)(&fn) = dlsym(handle, #fn);                            \
        if (!fn) { fprintf(stderr, "FAIL: missing %s\n", #fn); return 1; } \
    } while (0)

static const char *VS =
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
/* VEC4POS=1 path: 4-component position straight from the client array, so the
 * vertex shader contains NO float literals — isolates "shader compiler emits bad
 * float constants" from "vertex data / raster path". */
static const char *VS_VEC4 =
    "attribute vec4 pos;\n"
    "void main(){ gl_Position = pos; }\n";
static const char *FS =
    "precision mediump float;\n"
    "uniform vec4 ucolor;\n"
    "void main(){ gl_FragColor = ucolor; }\n";
/* NOUNIFORM=1 path: hardcoded color, no glUniform4f — isolates geometry from
 * the float-uniform veneer. */
static const char *FS_FIXED =
    "precision mediump float;\n"
    "void main(){ gl_FragColor = vec4(0.9, 0.3, 0.1, 1.0); }\n";

int main(void)
{
    /* 1. Load the patchelf'd DDK (libm.so.6 -> libSGXm.so.1) into a fresh
     *    namespace. dlmopen forbids RTLD_GLOBAL with LM_ID_NEWLM, so binding is
     *    arranged via the DDK's own NEEDED list (libSGXm sits where libm was,
     *    ahead of libc) — the DDK's float libm/libc imports resolve to libSGXm's
     *    soft-float wrappers; the fresh namespace isolates it from the app glibc.
     *    libSGXm loads automatically as the patched DDK's dependency. */
    /* 1. Load the patchelf'd DDK (libm.so.6 -> libSGXm.so.1, and libc forced
     *    LAST in NEEDED) into a fresh namespace. libSGXm therefore precedes libc,
     *    so the DDK's float libm AND libc imports (sinf, strtod, atof) bind the
     *    soft-float wrappers; other libc calls fall through to libc. The fresh
     *    namespace isolates all of it from the app glibc. */
    Lmid_t ns = 0;
    g_egl = dlmopen(LM_ID_NEWLM, "libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_egl) { fprintf(stderr, "FAIL: dlmopen libEGL: %s\n", dlerror()); return 1; }
    if (dlinfo(g_egl, RTLD_DI_LMID, &ns) != 0) { fprintf(stderr, "FAIL: dlinfo\n"); return 1; }

    /* 2. Load libGLESv2 into that same namespace. */
    g_gles = dlmopen(ns, "libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_gles) { fprintf(stderr, "FAIL: dlmopen libGLESv2: %s\n", dlerror()); return 1; }

    /* 3. Resolve entry points from the namespace. Float-touching ones carry PCS. */
    EGLDisplay (*eglGetDisplay)(void *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglBindAPI)(unsigned);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*eglGetError)(void);
    RES(g_egl, eglGetDisplay); RES(g_egl, eglInitialize); RES(g_egl, eglBindAPI);
    RES(g_egl, eglChooseConfig); RES(g_egl, eglCreatePbufferSurface);
    RES(g_egl, eglCreateContext); RES(g_egl, eglMakeCurrent); RES(g_egl, eglGetError);

    const unsigned char *(*glGetString)(GLenum);
    GLuint (*glCreateShader)(GLenum);
    void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*glCompileShader)(GLuint);
    void (*glGetShaderiv)(GLuint, GLenum, GLint *);
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint (*glCreateProgram)(void);
    void (*glAttachShader)(GLuint, GLuint);
    void (*glLinkProgram)(GLuint);
    void (*glGetProgramiv)(GLuint, GLenum, GLint *);
    void (*glUseProgram)(GLuint);
    GLint (*glGetUniformLocation)(GLuint, const GLchar *);
    GLint (*glGetAttribLocation)(GLuint, const GLchar *);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf) PCS;
    void (*glClear)(GLbitfield);
    void (*glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) PCS;
    void (*glEnableVertexAttribArray)(GLuint);
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (*glDrawArrays)(GLenum, GLint, GLsizei);
    void (*glFinish)(void);
    void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    GLenum (*glGetError)(void);
    RES(g_gles, glGetString); RES(g_gles, glCreateShader); RES(g_gles, glShaderSource);
    RES(g_gles, glCompileShader); RES(g_gles, glGetShaderiv); RES(g_gles, glGetShaderInfoLog);
    RES(g_gles, glCreateProgram); RES(g_gles, glAttachShader); RES(g_gles, glLinkProgram);
    RES(g_gles, glGetProgramiv); RES(g_gles, glUseProgram); RES(g_gles, glGetUniformLocation);
    RES(g_gles, glGetAttribLocation); RES(g_gles, glViewport); RES(g_gles, glClearColor);
    RES(g_gles, glClear); RES(g_gles, glUniform4f); RES(g_gles, glEnableVertexAttribArray);
    RES(g_gles, glVertexAttribPointer); RES(g_gles, glDrawArrays); RES(g_gles, glFinish);
    RES(g_gles, glReadPixels); RES(g_gles, glGetError);

    /* 4. EGL init + pbuffer + context. */
    const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0, n = 0;
    EGLConfig cfg;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "FAIL: eglInitialize\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) { fprintf(stderr, "FAIL: config\n"); return 1; }
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) { fprintf(stderr, "FAIL: surf/ctx 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(dpy, surf, surf, ctx);
    printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));

    /* 5. Shader compile + float uniform + geometry (exercises DDK internal libm/libc). */
    int vec4pos = getenv("VEC4POS") != NULL;
    const char *vs_src = vec4pos ? VS_VEC4 : VS;
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
    GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = {0}; glGetShaderInfoLog(vs, 511, NULL, l); fprintf(stderr, "FAIL: vs: %s\n", l); return 1; }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    int no_uniform = getenv("NOUNIFORM") != NULL;
    const char *fs_src = no_uniform ? FS_FIXED : FS;
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = {0}; glGetShaderInfoLog(fs, 511, NULL, l); fprintf(stderr, "FAIL: fs: %s\n", l); return 1; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { fprintf(stderr, "FAIL: link\n"); return 1; }
    glUseProgram(prog);
    GLint loc = no_uniform ? -1 : glGetUniformLocation(prog, "ucolor");
    GLint pos = glGetAttribLocation(prog, "pos");
    printf("attrib pos=%d uniform loc=%d no_uniform=%d\n", pos, loc, no_uniform);

    const GLfloat tri2[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
    const GLfloat tri4[] = { -0.8f, -0.8f, 0.0f, 1.0f,
                             0.8f, -0.8f, 0.0f, 1.0f,
                             0.0f,  0.8f, 0.0f, 1.0f };
    unsigned char center[4] = {0}, corner[4] = {0};
    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);       /* ~26 26 26 */
    glClear(GL_COLOR_BUFFER_BIT);
    if (!no_uniform)
        glUniform4f(loc, 0.9f, 0.3f, 0.1f, 1.0f);   /* float veneer -> ~230 77 26 */
    glEnableVertexAttribArray(pos);
    if (vec4pos)
        glVertexAttribPointer(pos, 4, GL_FLOAT, GL_FALSE, 0, tri4);
    else
        glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, tri2);
    printf("err after attrib setup = 0x%x\n", glGetError());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    printf("err after drawArrays = 0x%x\n", glGetError());
    glFinish();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);

    printf("center RGBA = %u %u %u %u  (expect ~230 77 26 255)\n", center[0], center[1], center[2], center[3]);
    printf("corner RGBA = %u %u %u %u  (expect ~26 26 26 255)\n", corner[0], corner[1], corner[2], corner[3]);
    int center_ok = center[0] > 210 && center[1] > 55 && center[1] < 100 && center[2] < 45;
    int corner_ok = corner[0] < 45 && corner[1] < 45 && corner[2] < 45;
    printf("RESULT: %s\n",
           (center_ok && corner_ok) ? "PASS (hybrid: dlmopen isolation + scoped interposer + veneers)"
           : center_ok ? "PARTIAL (color ok, geometry off)"
           : "FAIL (shader/uniform path corrupted)");
    return (center_ok && corner_ok) ? 0 : 1;
}
