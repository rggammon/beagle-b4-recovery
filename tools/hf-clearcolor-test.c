/* Hard-float spike: prove the DWARF-generated pcs("aapcs") veneers correct the
 * soft-float/hard-float ABI at the GLES boundary of the real DDK.
 *
 * Built HARD-FLOAT (armhf). It links the generated libEGL_hf/libGLESv2_hf shims
 * for the normal path. glClearColor is float-touching, so the shim veneer must
 * move the clear color from VFP regs (hard-float caller) to core regs (soft-float
 * DDK). The control calls the RAW soft-float glClearColor as a hard-float pointer
 * (no veneer) — its color lands in the wrong registers and reads back wrong.
 *
 * PASS = veneer readback matches the requested color AND the raw readback does not.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 64
#define HEIGHT 64

static EGLDisplay make_context(EGLContext *ctx_out, EGLSurface *surf_out)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    EGLConfig config;
    EGLint n = 0;
    const EGLint cfg[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pbuf[] = { EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE };
    const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        fprintf(stderr, "FAIL: eglInitialize 0x%x\n", eglGetError());
        exit(1);
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) { fprintf(stderr, "FAIL: bindAPI\n"); exit(1); }
    if (!eglChooseConfig(dpy, cfg, &config, 1, &n) || n < 1) {
        fprintf(stderr, "FAIL: chooseConfig 0x%x n=%d\n", eglGetError(), n);
        exit(1);
    }
    EGLSurface surf = eglCreatePbufferSurface(dpy, config, pbuf);
    if (surf == EGL_NO_SURFACE) { fprintf(stderr, "FAIL: pbuffer 0x%x\n", eglGetError()); exit(1); }
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxattr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "FAIL: context 0x%x\n", eglGetError()); exit(1); }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "FAIL: makeCurrent 0x%x\n", eglGetError()); exit(1); }
    *ctx_out = ctx;
    *surf_out = surf;
    return dpy;
}

static void clear_and_read(unsigned char px[4])
{
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(WIDTH / 2, HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

int main(void)
{
    EGLContext ctx;
    EGLSurface surf;
    EGLDisplay dpy = make_context(&ctx, &surf);
    unsigned char via_veneer[4] = {0}, via_raw[4] = {0};

    printf("renderer=%s\n", (const char *)glGetString(GL_RENDERER));

    /* Normal path: shim-provided glClearColor (DWARF pcs veneer). */
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);   /* expect ~ 51 102 153 255 */
    clear_and_read(via_veneer);

    /* Control: raw soft-float glClearColor called with the hard-float ABI. */
    void *real = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!real) { fprintf(stderr, "FAIL: dlopen real: %s\n", dlerror()); return 1; }
    void (*raw_clear)(GLclampf, GLclampf, GLclampf, GLclampf) =
        (void (*)(GLclampf, GLclampf, GLclampf, GLclampf)) dlsym(real, "glClearColor");
    raw_clear(0.2f, 0.4f, 0.6f, 1.0f);
    clear_and_read(via_raw);

    printf("veneer RGBA = %u %u %u %u  (expect ~51 102 153 255)\n",
           via_veneer[0], via_veneer[1], via_veneer[2], via_veneer[3]);
    printf("raw    RGBA = %u %u %u %u  (expect wrong)\n",
           via_raw[0], via_raw[1], via_raw[2], via_raw[3]);

    int veneer_ok = via_veneer[0] > 40 && via_veneer[0] < 62 &&
                    via_veneer[1] > 92 && via_veneer[1] < 112 &&
                    via_veneer[2] > 143 && via_veneer[2] < 163;
    int raw_wrong = !(via_raw[0] > 40 && via_raw[0] < 62 &&
                      via_raw[1] > 92 && via_raw[1] < 112 &&
                      via_raw[2] > 143 && via_raw[2] < 163);

    printf("RESULT: %s\n",
           (veneer_ok && raw_wrong) ? "PASS (veneer correct, raw wrong)"
           : veneer_ok ? "PARTIAL (veneer correct, raw not distinguishable)"
           : "FAIL (veneer did not correct the ABI)");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return veneer_ok ? 0 : 1;
}
