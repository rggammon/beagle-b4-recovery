/* Hard-float spike #2: push a shader-compile + float-uniform + geometry path
 * through the DWARF veneer shims, with NO libm interposer, to see whether the
 * DDK's internal soft-float libm calls actually corrupt real rendering.
 *
 * Exercises: glCreateShader/glShaderSource/glCompileShader/glLinkProgram (the
 * USSE compiler — most likely internal libm user), glUniform4f (float veneer),
 * glVertexAttribPointer (float vertex data via pointer, ABI-neutral), glDrawArrays.
 *
 * PASS = the triangle's center reads back the uniform color AND a corner reads
 * back the clear color (shader, float uniform, and geometry all correct).
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>

#define W 64
#define H 64

static const char *VS =
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "uniform vec4 ucolor;\n"
    "void main(){ gl_FragColor = ucolor; }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "FAIL: shader compile: %s\n", log);
        exit(1);
    }
    return s;
}

int main(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0, n = 0;
    EGLConfig cfg;
    const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const GLfloat tri[] = { -0.8f, -0.8f,  0.8f, -0.8f,  0.0f, 0.8f };
    unsigned char center[4] = {0}, corner[4] = {0};
    GLuint vs, fs, prog;
    GLint loc;

    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "FAIL: init\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) { fprintf(stderr, "FAIL: config\n"); return 1; }
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) { fprintf(stderr, "FAIL: surf/ctx 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(dpy, surf, surf, ctx);

    printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));

    vs = compile(GL_VERTEX_SHADER, VS);
    fs = compile(GL_FRAGMENT_SHADER, FS);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { char l[512] = {0}; glGetProgramInfoLog(prog, 511, NULL, l); fprintf(stderr, "FAIL: link: %s\n", l); return 1; }
    glUseProgram(prog);
    loc = glGetUniformLocation(prog, "ucolor");
    GLint pos = glGetAttribLocation(prog, "pos");

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);          /* ~26 26 26 */
    glClear(GL_COLOR_BUFFER_BIT);

    glUniform4f(loc, 0.9f, 0.3f, 0.1f, 1.0f);       /* float veneer -> ~230 77 26 */
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
    printf("RESULT: %s\n",
           (center_ok && corner_ok) ? "PASS (shader+uniform+geometry correct, no interposer)"
           : center_ok ? "PARTIAL (color ok, geometry off)"
           : "FAIL (shader/uniform path corrupted)");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return (center_ok && corner_ok) ? 0 : 1;
}
