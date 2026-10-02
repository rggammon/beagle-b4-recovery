/* Minimal GLES2 repro: does rendering survive re-binding a framebuffer?
 *
 * Mirrors femtovg's nested render-target pattern (Slint cache-rendering-hint
 * inside an opacity layer): bind FBO A and clear it, switch to FBO B and draw,
 * switch back to A and draw more, then read A back.  Each FBO has a texture
 * colour attachment plus a STENCIL_INDEX8 renderbuffer, as femtovg creates.
 *
 * Expected A after the sequence: left half blue (drawn after re-bind), right
 * half red (cleared before the switch).
 *
 *   arm-linux-gnueabihf-gcc-14 -O2 -I<ddk headers> fbo-reentry-test.c \
 *       -L<shim lib dir> -lEGL -lGLESv2 -o fbo-reentry-test
 *   sgx-ddk14-hf-run ./fbo-reentry-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef char GLchar;

#define W 64
#define H 64

static const char *VS =
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "uniform vec4 ucolor;\n"
    "void main(){ gl_FragColor = ucolor; }\n";

static GLuint prog;
static GLint ucolor;
static GLuint tprog;

static const char *TVS =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main(){ uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *TFS =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "void main(){ gl_FragColor = texture2D(tex, uv); }\n";

static GLuint build(const char *vsrc, const char *fsrc)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), p;
    glShaderSource(vs, 1, &vsrc, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fsrc, NULL);
    glCompileShader(fs);
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    return p;
}

/* draw texture tex over x in [x0,x1] of the current target */
static void tquad(float x0, float x1, GLuint tex)
{
    const GLfloat v[] = { x0, -1, x1, -1, x0, 1, x1, 1 };
    glViewport(0, 0, W, H);
    glUseProgram(tprog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(tprog, "tex"), 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void make_target(GLuint *fb, GLuint *tex)
{
    GLuint rb;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, *fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        printf("  fb %u INCOMPLETE\n", *fb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void clear(float r, float g, float b)
{
    glViewport(0, 0, W, H);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

static void quad(float x0, float x1, float r, float g, float b)
{
    const GLfloat v[] = { x0, -1, x1, -1, x0, 1, x1, 1 };
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glUniform4f(ucolor, r, g, b, 1.0f);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static unsigned px(GLuint fb, int x, int y)
{
    unsigned char p[4] = { 0 };
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}

static int check(const char *name, GLuint fb, unsigned left, unsigned right)
{
    unsigned l = px(fb, W / 4, H / 2), r = px(fb, 3 * W / 4, H / 2);
    int ok = l == left && r == right;
    printf("%-44s left=%06x (want %06x) right=%06x (want %06x)  %s\n",
           name, l, left, r, right, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    const EGLint cfga[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint n = 0;
    EGLConfig cfg;
    GLuint fa, ta, fb, tb, vs, fs;
    int pass = 1;

    if (!eglInitialize(dpy, NULL, NULL) || !eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "FAIL: egl init/config\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    /* FBO_WINDOW=1: render to the dc_nohw window surface instead of a pbuffer */
    const char *fw = getenv("FBO_WINDOW");
    const int window = fw && fw[0] == '1';
    const EGLint cfgw[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    if (window && (!eglChooseConfig(dpy, cfgw, &cfg, 1, &n) || n < 1)) {
        fprintf(stderr, "FAIL: window config\n");
        return 1;
    }
    EGLSurface surf = window ? eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL)
                             : eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "FAIL: surface/context 0x%x\n", eglGetError());
        return 1;
    }
    eglMakeCurrent(dpy, surf, surf, ctx);
    printf("surface=%s\n", window ? "window" : "pbuffer");
    printf("renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL);
    glCompileShader(vs);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    ucolor = glGetUniformLocation(prog, "ucolor");

    /* 1: control - clear + draw into A with no switch in between */
    make_target(&fa, &ta);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    quad(-1, 0, 0, 0, 1);
    pass &= check("1 no switch", fa, 0x0000ff, 0xff0000);

    /* 2: clear A, switch to B (clear only), back to A, draw */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    quad(-1, 0, 0, 0, 1);
    pass &= check("2 clear A, clear B, back to A + draw", fa, 0x0000ff, 0xff0000);

    /* 3: as 2 but B gets a draw too (femtovg's inner cache render) */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    quad(-1, 0, 0, 0, 1);
    pass &= check("3 clear A, draw B, back to A + draw", fa, 0x0000ff, 0xff0000);
    pass &= check("3 (B itself)", fb, 0xffff00, 0xffff00);

    /* 4: as 3 but with glFinish at each switch */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    quad(-1, 0, 0, 0, 1);
    pass &= check("4 as 3 + glFinish at switches", fa, 0x0000ff, 0xff0000);

    /* 5: A cleared, B drawn, back to A with NO further draw: is A's clear kept? */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    pass &= check("5 A cleared then left (clear kept?)", fa, 0xff0000, 0xff0000);

    tprog = build(TVS, TFS);

    /* 6: femtovg's layer pattern - back in A, draw B's TEXTURE (not a solid) */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    tquad(-1, 0, tb);
    pass &= check("6 back to A + sample B's texture", fa, 0xffff00, 0xff0000);

    /* 7: case 3 then composite A's texture onto the default surface */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    quad(-1, 0, 0, 0, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clear(0, 0, 0);
    tquad(-1, 1, ta);
    pass &= check("7 case 3, A composited to surface", 0, 0x0000ff, 0xff0000);

    /* 8: case 6 then composite A to the surface (the full Slint chain) */
    make_target(&fa, &ta);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    tquad(-1, 0, tb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clear(0, 0, 0);
    tquad(-1, 1, ta);
    pass &= check("8 case 6, A composited to surface", 0, 0xffff00, 0xff0000);

    /* 9: case 8, composite again in a second frame after a swap */
    eglSwapBuffers(dpy, surf);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clear(0, 0, 0);
    tquad(-1, 1, ta);
    pass &= check("9 case 8, A re-composited next frame", 0, 0xffff00, 0xff0000);

    /* 10: femtovg's exact order - B is created (ending in a bind of 0) while
     * A has a pending clear, then B is drawn and sampled back into A */
    make_target(&fa, &ta);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    clear(1, 0, 0);
    make_target(&fb, &tb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    clear(0, 1, 0);
    quad(-1, 1, 1, 1, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fa);
    tquad(-1, 0, tb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clear(0, 0, 0);
    tquad(-1, 1, ta);
    pass &= check("10 create B mid-A, sample B into A, composite", 0, 0xffff00, 0xff0000);
    pass &= check("10 (A itself)", fa, 0xffff00, 0xff0000);

    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return pass ? 0 : 2;
}
