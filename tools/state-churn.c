/* state-churn.c — isolate GLES2 render-STATE-change cost on SGX530 DDK 1.6.
 *
 * The femtovg-vs-nanovg GL diff showed the storm tracks glUseProgram +
 * glBindTexture COUNT per frame (femtovg ~48 prog + ~97 tex/frame -> storm;
 * nanovg ~2 prog + ~3 tex/frame -> clean) far more than geometry. This repro
 * removes geometry as a factor: one tiny fixed triangle, but N draws/frame each
 * optionally preceded by a program switch and/or a texture rebind.
 *
 *   argv[1] draws        draws per frame          (default 48)
 *   argv[2] prog_switch  1 = glUseProgram(alt) before each draw   (default 1)
 *   argv[3] tex_rebind   1 = glBindTexture(alt) before each draw   (default 1)
 *   argv[4] frames       (default 60)
 *
 * Matrix to run (window/FLIPWSEGL, /etc/powervr.ini default):
 *   ./state-churn 48 0 0   -> 48 trivial draws, NO churn         (baseline)
 *   ./state-churn 48 1 0   -> 48 program switches only
 *   ./state-churn 48 0 1   -> 48 texture rebinds only
 *   ./state-churn 48 1 1   -> full churn (matches femtovg)
 *
 * Build: sgx-cc -O0 -I/usr/include/sgx-ddk16 state-churn.c -lEGL -lGLESv2 -o /root/state-churn
 */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
typedef char GLchar;  /* DDK 1.6 gl2.h predates GLchar */

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

static const char *vs_src =
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "void main(){ gl_FragColor = vec4(0.3, 0.6, 0.9, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader:\n%s\n", log); fail("compile"); }
    return s;
}

/* Distinct program objects so glUseProgram is a real switch (not a no-op). */
static GLuint mkprog(void)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) fail("link");
    return p;
}

int main(int argc, char **argv)
{
    int draws       = argc > 1 ? atoi(argv[1]) : 48;
    int prog_switch = argc > 2 ? atoi(argv[2]) : 1;
    int tex_rebind  = argc > 3 ? atoi(argv[3]) : 1;
    int frames      = argc > 4 ? atoi(argv[4]) : 60;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min, ncfg = 0, W = 0, H = 0;
    EGLConfig cfg;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) fail("eglBindAPI");
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg != 1) fail("eglChooseConfig");
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) fail("eglCreateContext");
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    if (surf == EGL_NO_SURFACE) fail("eglCreateWindowSurface");
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) fail("eglMakeCurrent");
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);

    GLuint P[2] = { mkprog(), mkprog() };
    GLuint T[2];
    unsigned char px[4] = { 255, 255, 255, 255 };
    glGenTextures(2, T);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, T[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    float tri[6] = { -0.05f, -0.05f, 0.05f, -0.05f, 0.0f, 0.05f };
    GLuint vbo; glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(P[0]);
    glBindTexture(GL_TEXTURE_2D, T[0]);

    printf("state-churn: draws=%d prog_switch=%d tex_rebind=%d frames=%d %dx%d\n",
           draws, prog_switch, tex_rebind, frames, W, H);
    fflush(stdout);

    for (int f = 0; f < frames; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int i = 0; i < draws; i++) {
            if (prog_switch) glUseProgram(P[i & 1]);
            if (tex_rebind)  glBindTexture(GL_TEXTURE_2D, T[i & 1]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        eglSwapBuffers(dpy, surf);
    }
    printf("done frames=%d\n", frames);
    return 0;
}
