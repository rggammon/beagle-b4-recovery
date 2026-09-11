/* NanoVG vector-graphics demo for the DDK 1.6 soft-float SGX530 stack.
 *
 * Renders a small NanoVG scene into a dc_nohw EGL window (native handle 0,
 * FLIPWSEGL) and eglSwapBuffers each frame. Presentation is the in-kernel
 * paced path (dcnohw present=2); run under sgxmode, e.g.
 *
 *   sgxmode -- sgx-ddk16-run nanovg-demo 100000
 *
 * ABI: build armel/soft-float so scalar float args match the DDK.
 *   arm-linux-gnueabi-gcc-14 -O2 -DNANOVG_GLES2_IMPLEMENTATION \
 *     nanovg-demo.c nanovg.c -I<nanovg/src> -I<ddk gles2 headers> \
 *     -L<ddk lib> -lEGL -lGLESv2 -lm -o nanovg-demo
 *
 * NanoVG's GLES2 backend needs a stencil buffer for concave fills, so the EGL
 * config requests EGL_STENCIL_SIZE 8. Antialiasing uses GL_OES_standard_
 * derivatives, which SGX103 advertises.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

/* The DDK 1.6 GLES2 headers predate the GLchar typedef nanovg_gl.h expects. */
typedef char GLchar;

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NANOVG_GLES2 1
#include "nanovg.h"
#include "nanovg_gl.h"

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void draw_scene(NVGcontext *vg, int width, int height, double t)
{
    float w = (float)width;
    float h = (float)height;

    /* Panel with a vertical gradient. */
    NVGpaint bg = nvgLinearGradient(vg, 0, 0, 0, h,
                                    nvgRGBA(38, 42, 54, 255),
                                    nvgRGBA(18, 20, 28, 255));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillPaint(vg, bg);
    nvgFill(vg);

    /* Rounded rectangle with its own gradient fill. */
    float rx = w * 0.12f;
    float ry = h * 0.18f;
    float rw = w * 0.42f;
    float rh = h * 0.34f;
    NVGpaint card = nvgLinearGradient(vg, rx, ry, rx, ry + rh,
                                      nvgRGBA(90, 140, 220, 255),
                                      nvgRGBA(40, 80, 160, 255));
    nvgBeginPath(vg);
    nvgRoundedRect(vg, rx, ry, rw, rh, 16.0f);
    nvgFillPaint(vg, card);
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(230, 236, 245, 200));
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);

    /* Filled circle that pulses with time. */
    float cx = w * 0.72f;
    float cy = h * 0.36f;
    float base = (w < h ? w : h) * 0.12f;
    float radius = base * (1.0f + 0.15f * (float)sin(t * 2.0));
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, radius);
    nvgFillColor(vg, nvgRGBA(240, 180, 70, 255));
    nvgFill(vg);

    /* Animated stroked sine curve. */
    nvgBeginPath(vg);
    for (int i = 0; i <= 64; i++) {
        float px = w * 0.08f + (w * 0.84f) * (float)i / 64.0f;
        float phase = (float)i / 64.0f * 6.2831853f * 2.0f + (float)t * 2.0f;
        float py = h * 0.78f + (float)sin(phase) * h * 0.10f;
        if (i == 0)
            nvgMoveTo(vg, px, py);
        else
            nvgLineTo(vg, px, py);
    }
    nvgStrokeColor(vg, nvgRGBA(120, 220, 160, 255));
    nvgStrokeWidth(vg, 3.0f);
    nvgStroke(vg);
}

int main(int argc, char **argv)
{
    const int frames = argc > 1 ? atoi(argv[1]) : 300;
    const int duration_seconds = getenv("NVG_DURATION_SECONDS") != NULL ?
                                 atoi(getenv("NVG_DURATION_SECONDS")) : 0;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    EGLint config_count = 0;
    EGLint major = 0;
    EGLint minor = 0;
    EGLint surface_w = 0;
    EGLint surface_h = 0;
    NVGcontext *vg;
    double start;
    int frame;

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    setvbuf(stdout, NULL, _IOLBF, 0);

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor))
        fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        fail("eglBindAPI");
    if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
        config_count != 1)
        fail("eglChooseConfig (need RGBA8888 + stencil8 window config)");

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        fail("eglCreateContext");

    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    if (surface == EGL_NO_SURFACE)
        fail("eglCreateWindowSurface");
    if (!eglMakeCurrent(display, surface, surface, context))
        fail("eglMakeCurrent");

    eglQuerySurface(display, surface, EGL_WIDTH, &surface_w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &surface_h);

    printf("egl=%d.%d vendor=%s renderer=%s surface=%dx%d\n",
           major, minor, eglQueryString(display, EGL_VENDOR),
           glGetString(GL_RENDERER), surface_w, surface_h);

    vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (vg == NULL)
        fail("nvgCreateGLES2");

    start = monotonic_seconds();
    for (frame = 0; ; frame++) {
        double now = monotonic_seconds();

        if (duration_seconds > 0) {
            if (now - start >= duration_seconds)
                break;
        } else if (frame >= frames) {
            break;
        }

        glViewport(0, 0, surface_w, surface_h);
        glClearColor(0.1f, 0.11f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        nvgBeginFrame(vg, surface_w, surface_h, 1.0f);
        draw_scene(vg, surface_w, surface_h, now - start);
        nvgEndFrame(vg);

        if (!eglSwapBuffers(display, surface))
            fail("eglSwapBuffers");
    }

    printf("done frames=%d seconds=%.2f\n", frame, monotonic_seconds() - start);

    nvgDeleteGLES2(vg);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
