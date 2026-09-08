/*
 * Stage 1 window-surface probe: drive a real EGL window surface through the
 * dc_nohw DisplayClass swapchain with eglSwapBuffers. No FBO, no pbuffer, no
 * DMA-BUF export, no KMS. It proves the swapchain create/swap/complete cycle
 * works; it does NOT present anything (dc_nohw is "no hardware").
 *
 * Default render is an unmistakably changing clear colour. SGX_TRIANGLE=1 adds
 * a rotating shaded triangle (shader compile/link + VBO + attrib + uniforms),
 * which also seeds the game-representative Stage 0 coverage. SGX_DEPTH=1
 * requests and exercises a depth buffer.
 *
 * Run (fresh reload + pvrsrvinit + dcnohw first; FLIPWSEGL in powervr.ini):
 *   /opt/pandora-armel/lib/ld-linux.so.3 \
 *     --library-path /opt/pandora-armel/lib:/root/s16/gl \
 *     /opt/pandora-armel/bin/sgx-window-swap [frames_per_cycle] [cycles]
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_NO_DISPLAY ((void *)0)
#define EGL_OPENGL_ES_API 0x30a0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_TEST 0x0b71
#define GL_RENDERER 0x1f01
#define GL_VERSION 0x1f02
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
#define GL_COMPILE_STATUS 0x8b81
#define GL_LINK_STATUS 0x8b82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88e4
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRIANGLES 0x0004

#define WIDTH 1024
#define HEIGHT 600
#define FRAMES_PER_CYCLE 300
#define SLOW_CALL_NS 100000000ULL

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLNativeWindowType;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef long GLsizeiptr;

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void *symbol(void *library, const char *name)
{
    void *address = dlsym(library, name);

    if (address == NULL) {
        fprintf(stderr, "FAIL: missing symbol %s\n", name);
        exit(1);
    }
    return address;
}

static uint64_t clock_ns(clockid_t clock_id)
{
    struct timespec ts;

    if (clock_gettime(clock_id, &ts) != 0)
        fail("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t monotonic_ns(void)
{
    return clock_ns(CLOCK_MONOTONIC);
}

/* EGL */
static EGLDisplay (*eglGetDisplay)(EGLNativeWindowType);
static EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*eglBindAPI)(EGLint);
static const char *(*eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*eglTerminate)(EGLDisplay);
static EGLint (*eglGetError)(void);

/* GLES2 (core) */
static const unsigned char *(*glGetString)(GLenum);
static void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*glClear)(GLbitfield);
static void (*glEnable)(GLenum);
static void (*glFinish)(void);
static void (*glDrawArrays)(GLenum, GLint, GLsizei);

/* GLES2 (shader/VBO, resolved only for SGX_TRIANGLE) */
static GLuint (*glCreateShader)(GLenum);
static void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*glCompileShader)(GLuint);
static void (*glGetShaderiv)(GLuint, GLenum, GLint *);
static void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static GLuint (*glCreateProgram)(void);
static void (*glAttachShader)(GLuint, GLuint);
static void (*glLinkProgram)(GLuint);
static void (*glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*glUseProgram)(GLuint);
static void (*glGenBuffers)(GLsizei, GLuint *);
static void (*glBindBuffer)(GLenum, GLuint);
static void (*glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static GLint (*glGetAttribLocation)(GLuint, const GLchar *);
static GLint (*glGetUniformLocation)(GLuint, const GLchar *);
static void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*glEnableVertexAttribArray)(GLuint);
static void (*glUniform1f)(GLint, GLfloat);
static void (*glDeleteBuffers)(GLsizei, const GLuint *);
static void (*glDeleteProgram)(GLuint);
static void (*glDeleteShader)(GLuint);

static const char *vertex_source =
    "attribute vec2 pos;\n"
    "uniform float angle;\n"
    "varying vec2 v;\n"
    "void main() {\n"
    "  float c = cos(angle), s = sin(angle);\n"
    "  gl_Position = vec4(pos.x*c - pos.y*s, pos.x*s + pos.y*c, 0.0, 1.0);\n"
    "  v = pos;\n"
    "}\n";

static const char *fragment_source =
    "precision mediump float;\n"
    "varying vec2 v;\n"
    "uniform float t;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(0.5 + 0.5*sin(t), v.x + 0.5, v.y + 0.5, 1.0);\n"
    "}\n";

static GLuint compile_shader(void *gles, GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "FAIL: shader compile: %s\n", log);
        exit(1);
    }
    (void)gles;
    return sh;
}

static void resolve_triangle_symbols(void *gles)
{
    glCreateShader = symbol(gles, "glCreateShader");
    glShaderSource = symbol(gles, "glShaderSource");
    glCompileShader = symbol(gles, "glCompileShader");
    glGetShaderiv = symbol(gles, "glGetShaderiv");
    glGetShaderInfoLog = symbol(gles, "glGetShaderInfoLog");
    glCreateProgram = symbol(gles, "glCreateProgram");
    glAttachShader = symbol(gles, "glAttachShader");
    glLinkProgram = symbol(gles, "glLinkProgram");
    glGetProgramiv = symbol(gles, "glGetProgramiv");
    glGetProgramInfoLog = symbol(gles, "glGetProgramInfoLog");
    glUseProgram = symbol(gles, "glUseProgram");
    glGenBuffers = symbol(gles, "glGenBuffers");
    glBindBuffer = symbol(gles, "glBindBuffer");
    glBufferData = symbol(gles, "glBufferData");
    glGetAttribLocation = symbol(gles, "glGetAttribLocation");
    glGetUniformLocation = symbol(gles, "glGetUniformLocation");
    glVertexAttribPointer = symbol(gles, "glVertexAttribPointer");
    glEnableVertexAttribArray = symbol(gles, "glEnableVertexAttribArray");
    glUniform1f = symbol(gles, "glUniform1f");
    glDeleteBuffers = symbol(gles, "glDeleteBuffers");
    glDeleteProgram = symbol(gles, "glDeleteProgram");
    glDeleteShader = symbol(gles, "glDeleteShader");
}

int main(int argc, char **argv)
{
    const int frames_per_cycle = argc > 1 ? atoi(argv[1]) : FRAMES_PER_CYCLE;
    const int cycles = argc > 2 ? atoi(argv[2]) : 1;
    const int duration_seconds = getenv("SGX_DURATION_SECONDS") != NULL ?
                                 atoi(getenv("SGX_DURATION_SECONDS")) : 0;
    const int use_triangle = getenv("SGX_TRIANGLE") != NULL &&
                             atoi(getenv("SGX_TRIANGLE")) != 0;
    const int use_depth = getenv("SGX_DEPTH") != NULL &&
                          atoi(getenv("SGX_DEPTH")) != 0;
    const int summary_only = getenv("SGX_SUMMARY_ONLY") != NULL &&
                             atoi(getenv("SGX_SUMMARY_ONLY")) != 0;
    /* SGX_SWAP=0 = Phase 1A: prove swapchain allocation only (render + glFinish,
       no eglSwapBuffers). Default SGX_SWAP=1 = Phase 1B swap cycling. */
    const int do_swap = getenv("SGX_SWAP") == NULL ||
                        atoi(getenv("SGX_SWAP")) != 0;
    void *egl_library;
    void *gles_library;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLint major = 0, minor = 0, config_count = 0;
    EGLint surface_w = 0, surface_h = 0;
    char renderer[128] = {0};
    GLuint program = 0, vbo = 0;
    GLint loc_pos = -1, loc_angle = -1, loc_t = -1;
    int cycle;
    uint64_t total_swaps = 0;
    uint64_t total_swap_ns = 0;
    uint64_t max_swap_ns = 0;
    int over_500ms = 0;

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, use_depth ? 16 : 0,
        EGL_NONE
    };
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    if (frames_per_cycle <= 0 && duration_seconds <= 0)
        fail("frames_per_cycle must be positive (or set SGX_DURATION_SECONDS)");
    if (cycles <= 0)
        fail("cycles must be positive");

    setvbuf(stdout, NULL, _IOLBF, 0);

    egl_library = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (egl_library == NULL)
        egl_library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (egl_library == NULL)
        fail(dlerror());
    gles_library = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (gles_library == NULL)
        gles_library = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (gles_library == NULL)
        fail(dlerror());

    eglGetDisplay = symbol(egl_library, "eglGetDisplay");
    eglInitialize = symbol(egl_library, "eglInitialize");
    eglBindAPI = symbol(egl_library, "eglBindAPI");
    eglQueryString = symbol(egl_library, "eglQueryString");
    eglChooseConfig = symbol(egl_library, "eglChooseConfig");
    eglCreateContext = symbol(egl_library, "eglCreateContext");
    eglCreateWindowSurface = symbol(egl_library, "eglCreateWindowSurface");
    eglQuerySurface = symbol(egl_library, "eglQuerySurface");
    eglMakeCurrent = symbol(egl_library, "eglMakeCurrent");
    eglSwapBuffers = symbol(egl_library, "eglSwapBuffers");
    eglDestroySurface = symbol(egl_library, "eglDestroySurface");
    eglDestroyContext = symbol(egl_library, "eglDestroyContext");
    eglTerminate = symbol(egl_library, "eglTerminate");
    eglGetError = symbol(egl_library, "eglGetError");

    glGetString = symbol(gles_library, "glGetString");
    glViewport = symbol(gles_library, "glViewport");
    glClearColor = symbol(gles_library, "glClearColor");
    glClear = symbol(gles_library, "glClear");
    glEnable = symbol(gles_library, "glEnable");
    glFinish = symbol(gles_library, "glFinish");
    glDrawArrays = symbol(gles_library, "glDrawArrays");

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize error=0x%x\n", eglGetError());
        return 1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        fail("eglBindAPI");
    if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
        config_count != 1)
        fail("eglChooseConfig(window)");

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        fail("eglCreateContext");

    printf("egl=%d.%d vendor=%s version=%s triangle=%d depth=%d swap=%d cycles=%d "
           "frames_per_cycle=%d duration_seconds=%d\n",
           major, minor, eglQueryString(display, EGL_VENDOR),
           eglQueryString(display, EGL_VERSION), use_triangle, use_depth, do_swap,
           cycles, frames_per_cycle, duration_seconds);

    for (cycle = 0; cycle < cycles; cycle++) {
        EGLNativeWindowType win = (EGLNativeWindowType)(long)
            (getenv("SGX_NATIVE_WINDOW") ? atoi(getenv("SGX_NATIVE_WINDOW")) : 0);
        EGLSurface surface = eglCreateWindowSurface(display, config, win, NULL);
        uint64_t cycle_start;
        int frame;

        if (surface == EGL_NO_SURFACE) {
            fprintf(stderr, "FAIL: eglCreateWindowSurface error=0x%x\n", eglGetError());
            return 1;
        }
        if (!eglMakeCurrent(display, surface, surface, context))
            fail("eglMakeCurrent");

        eglQuerySurface(display, surface, EGL_WIDTH, &surface_w);
        eglQuerySurface(display, surface, EGL_HEIGHT, &surface_h);
        glViewport(0, 0, surface_w, surface_h);
        if (renderer[0] == 0) {
            const unsigned char *r = glGetString(GL_RENDERER);
            if (r != NULL)
                strncpy(renderer, (const char *)r, sizeof(renderer) - 1);
        }
        if (use_depth)
            glEnable(GL_DEPTH_TEST);

        if (use_triangle && program == 0) {
            const GLfloat triangle[] = {
                 0.0f,  0.6f,
                -0.6f, -0.6f,
                 0.6f, -0.6f,
            };
            GLuint vs, fs;
            GLint ok = 0;

            resolve_triangle_symbols(gles_library);
            vs = compile_shader(gles_library, GL_VERTEX_SHADER, vertex_source);
            fs = compile_shader(gles_library, GL_FRAGMENT_SHADER, fragment_source);
            program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512] = {0};
                glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
                fprintf(stderr, "FAIL: program link: %s\n", log);
                return 1;
            }
            glDeleteShader(vs);
            glDeleteShader(fs);
            loc_pos = glGetAttribLocation(program, "pos");
            loc_angle = glGetUniformLocation(program, "angle");
            loc_t = glGetUniformLocation(program, "t");
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
            glUseProgram(program);
            glVertexAttribPointer((GLuint)loc_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray((GLuint)loc_pos);
        }

        cycle_start = monotonic_ns();
        for (frame = 0; ; frame++) {
            float phase = (float)frame * 0.05f;
            uint64_t swap_start, swap_end, swap_ns;

            if (duration_seconds <= 0 && frame >= frames_per_cycle)
                break;
            if (duration_seconds > 0 &&
                monotonic_ns() - cycle_start >= (uint64_t)duration_seconds * 1000000000ULL)
                break;

            if (use_triangle) {
                glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | (use_depth ? GL_DEPTH_BUFFER_BIT : 0));
                glUniform1f(loc_angle, phase);
                glUniform1f(loc_t, phase);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            } else {
                const char *clear_env = getenv("SGX_CLEAR");
                if (clear_env != NULL) {
                    unsigned long c = strtoul(clear_env, NULL, 0);
                    glClearColor(((c >> 16) & 0xff) / 255.0f,
                                 ((c >> 8) & 0xff) / 255.0f,
                                 (c & 0xff) / 255.0f,
                                 ((c >> 24) & 0xff) / 255.0f);
                } else {
                    glClearColor(0.5f + 0.5f * (float)((frame >> 0) & 1),
                                 0.5f + 0.5f * (float)((frame >> 1) & 1),
                                 0.5f + 0.5f * (float)((frame >> 2) & 1), 1.0f);
                }
                glClear(GL_COLOR_BUFFER_BIT | (use_depth ? GL_DEPTH_BUFFER_BIT : 0));
            }

            swap_start = monotonic_ns();
            if (do_swap) {
                if (!eglSwapBuffers(display, surface)) {
                    fprintf(stderr, "FAIL: eglSwapBuffers cycle=%d frame=%d error=0x%x\n",
                            cycle, frame, eglGetError());
                    return 1;
                }
            } else {
                glFinish();
            }
            swap_end = monotonic_ns();
            swap_ns = swap_end - swap_start;

            total_swaps++;
            total_swap_ns += swap_ns;
            if (swap_ns > max_swap_ns)
                max_swap_ns = swap_ns;
            if (swap_ns > 500000000ULL)
                over_500ms++;
            if (!summary_only && swap_ns >= SLOW_CALL_NS)
                fprintf(stderr, "slow cycle=%d frame=%d swap_us=%llu\n",
                        cycle, frame, (unsigned long long)(swap_ns / 1000ULL));
        }

        glFinish();
        if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
            fail("eglMakeCurrent(release)");
        if (!eglDestroySurface(display, surface))
            fail("eglDestroySurface");
        printf("cycle=%d surface=%dx%d frames=%d\n",
               cycle, surface_w, surface_h,
               duration_seconds > 0 ? -1 : frames_per_cycle);
    }

    if (use_triangle) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        if (vbo)
            glDeleteBuffers(1, &vbo);
        if (program)
            glDeleteProgram(program);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    printf("summary cycles=%d swaps=%llu average_swap_us=%llu max_swap_us=%llu over_500ms=%d\n",
           cycles, (unsigned long long)total_swaps,
           (unsigned long long)(total_swaps ? total_swap_ns / total_swaps / 1000ULL : 0),
           (unsigned long long)(max_swap_ns / 1000ULL), over_500ms);
    printf("renderer=%s swap=%d\n", renderer, do_swap);

    if (!eglDestroyContext(display, context))
        fail("eglDestroyContext");
    if (!eglTerminate(display))
        fail("eglTerminate");
    printf("clean exit\n");
    return 0;
}
