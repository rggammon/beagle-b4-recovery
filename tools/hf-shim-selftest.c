/* Normally-linked hard-float self-test for the DDK shim set. Unlike
 * hf-hybrid-test.c (which hand-loads via dlmopen/dlsym), this calls the GL and
 * EGL entry points DIRECTLY and links against the shim libEGL/libGLESv2, exactly
 * as a real hard-float application would. A PASS proves the packaged .so form
 * works end to end: pbuffer, shader compile (float literals), float uniform,
 * geometry.
 *
 * Build hard-float against the shims:
 *   arm-linux-gnueabihf-gcc -I<ddk headers> hf-shim-selftest.c \
 *       -L<shim lib dir> -lEGL -lGLESv2 -o hf-shim-selftest
 * Run (shims on the path; the loader finds the real DDK via SGXHF_DDK_DIR):
 *   LD_LIBRARY_PATH=<shim lib dir> SGXHF_DDK_DIR=<patched ddk dir> ./hf-shim-selftest
 */
#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef char GLchar;   /* DDK 1.6 GLES2 headers predate GLchar */

#define W 64
#define H 64

static const char *VS =
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "uniform vec4 ucolor;\n"
    "void main(){ gl_FragColor = ucolor; }\n";

int main(void)
{
    const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0, n = 0;
    EGLConfig cfg;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        fprintf(stderr, "FAIL: eglInitialize\n"); return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "FAIL: config\n"); return 1;
    }
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "FAIL: surf/ctx 0x%x\n", eglGetError()); return 1;
    }
    eglMakeCurrent(dpy, surf, surf, ctx);
    printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));

    /* eglGetProcAddress must return this shim's own veneers so float-touching
     * entry points keep the hard-float ABI translation. Resolve the float
     * uniform through it and check it matches the directly-linked symbol. */
    typedef void (*genericfp)(void);
    typedef void (*u4f_t)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    u4f_t gpa_u4f = (u4f_t) eglGetProcAddress("glUniform4f");
    if (!gpa_u4f) { fprintf(stderr, "FAIL: eglGetProcAddress(glUniform4f)\n"); return 1; }
    printf("eglGetProcAddress(glUniform4f) %s directly-linked veneer\n",
           ((genericfp) gpa_u4f == (genericfp) glUniform4f) ? "==" : "!=");

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL); glCompileShader(vs);
    GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = {0}; glGetShaderInfoLog(vs, 511, NULL, l); fprintf(stderr, "FAIL: vs: %s\n", l); return 1; }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[512] = {0}; glGetShaderInfoLog(fs, 511, NULL, l); fprintf(stderr, "FAIL: fs: %s\n", l); return 1; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { fprintf(stderr, "FAIL: link\n"); return 1; }
    glUseProgram(prog);
    GLint loc = glGetUniformLocation(prog, "ucolor");
    GLint pos = glGetAttribLocation(prog, "pos");

    const GLfloat tri[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
    unsigned char center[4] = {0}, corner[4] = {0};
    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    gpa_u4f(loc, 0.9f, 0.3f, 0.1f, 1.0f);   /* via eglGetProcAddress veneer */
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);

    printf("center RGBA = %u %u %u %u  (expect ~230 77 26 255)\n", center[0], center[1], center[2], center[3]);
    printf("corner RGBA = %u %u %u %u  (expect ~26 26 26 255)\n", corner[0], corner[1], corner[2], corner[3]);
    int center_ok = center[0] > 210 && center[1] > 55 && center[1] < 100 && center[2] < 45;
    int corner_ok = corner[0] < 45 && corner[1] < 45 && corner[2] < 45;
    printf("RESULT: %s\n", (center_ok && corner_ok) ? "PASS (normally-linked hard-float via shim)"
                          : center_ok ? "PARTIAL (color ok, geometry off)"
                          : "FAIL");
    return (center_ok && corner_ok) ? 0 : 1;
}
