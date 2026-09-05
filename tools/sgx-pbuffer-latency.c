#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_OPENGL_ES_API 0x30a0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_DEVICE_EXT 0x313f
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RENDERER 0x1f01
#define GL_FRAMEBUFFER 0x8d40
#define GL_RENDERBUFFER 0x8d41
#define GL_COLOR_ATTACHMENT0 0x8ce0
#define GL_RGBA4 0x8056
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_TEXTURE_2D 0x0de1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600

#define WIDTH 1024
#define HEIGHT 600
#define ITERATIONS 120

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef void *EGLDeviceEXT;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLclampf;
typedef unsigned int GLbitfield;

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

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        fail("clock_gettime");

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    const int iterations = argc > 1 ? atoi(argv[1]) : ITERATIONS;
    const int alternate = argc <= 2 || atoi(argv[2]) != 0;
    const int use_fbo = argc <= 3 || atoi(argv[3]) != 0;
    const int use_texture = argc <= 4 || atoi(argv[4]) != 0;
    const int duration_seconds = getenv("SGX_DURATION_SECONDS") != NULL ?
                                 atoi(getenv("SGX_DURATION_SECONDS")) : 0;
    const int summary_only = getenv("SGX_SUMMARY_ONLY") != NULL &&
                             atoi(getenv("SGX_SUMMARY_ONLY")) != 0;
    void *egl_library;
    void *gles_library;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surfaces[2];
    EGLint config_count = 0;
    EGLint major = 0;
    EGLint minor = 0;
    GLuint framebuffer = 0;
    GLuint renderbuffers[2] = { 0, 0 };
    GLuint textures[2] = { 0, 0 };
    uint64_t deadline_ns = 0;
    uint64_t maximum_select_ns = 0;
    uint64_t maximum_finish_ns = 0;
    uint64_t maximum_total_ns = 0;
    uint64_t total_ns = 0;
    unsigned int frames_over_500_ms = 0;
    int frame;

    EGLDisplay (*eglGetDisplay)(void *);
    void *(*eglGetProcAddress)(const char *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglBindAPI)(unsigned int);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    EGLint (*eglGetError)(void);
    const unsigned char *(*glGetString)(GLenum);
    void (*glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    void (*glClear)(GLbitfield);
    void (*glFinish)(void);
    void (*glGenFramebuffers)(GLsizei, GLuint *);
    void (*glBindFramebuffer)(GLenum, GLuint);
    void (*glGenRenderbuffers)(GLsizei, GLuint *);
    void (*glBindRenderbuffer)(GLenum, GLuint);
    void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    GLenum (*glCheckFramebufferStatus)(GLenum);
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexParameteri)(GLenum, GLenum, int);
    void (*glTexImage2D)(GLenum, int, int, GLsizei, GLsizei, int,
                         GLenum, GLenum, const void *);
    void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, int);
    void (*glDeleteFramebuffers)(GLsizei, const GLuint *);
    void (*glDeleteRenderbuffers)(GLsizei, const GLuint *);
    void (*glDeleteTextures)(GLsizei, const GLuint *);

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    const EGLint pbuffer_attributes[] = {
        EGL_WIDTH, WIDTH,
        EGL_HEIGHT, HEIGHT,
        EGL_NONE
    };

    if (iterations <= 0)
        fail("iterations must be positive");
    if (duration_seconds < 0)
        fail("SGX_DURATION_SECONDS must not be negative");

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
    eglGetProcAddress = symbol(egl_library, "eglGetProcAddress");
    eglInitialize = symbol(egl_library, "eglInitialize");
    eglBindAPI = symbol(egl_library, "eglBindAPI");
    eglChooseConfig = symbol(egl_library, "eglChooseConfig");
    eglCreateContext = symbol(egl_library, "eglCreateContext");
    eglCreatePbufferSurface = symbol(egl_library, "eglCreatePbufferSurface");
    eglMakeCurrent = symbol(egl_library, "eglMakeCurrent");
    eglDestroySurface = symbol(egl_library, "eglDestroySurface");
    eglDestroyContext = symbol(egl_library, "eglDestroyContext");
    eglTerminate = symbol(egl_library, "eglTerminate");
    eglGetError = symbol(egl_library, "eglGetError");
    glGetString = symbol(gles_library, "glGetString");
    glClearColor = symbol(gles_library, "glClearColor");
    glClear = symbol(gles_library, "glClear");
    glFinish = symbol(gles_library, "glFinish");
    glGenFramebuffers = symbol(gles_library, "glGenFramebuffers");
    glBindFramebuffer = symbol(gles_library, "glBindFramebuffer");
    glGenRenderbuffers = symbol(gles_library, "glGenRenderbuffers");
    glBindRenderbuffer = symbol(gles_library, "glBindRenderbuffer");
    glRenderbufferStorage = symbol(gles_library, "glRenderbufferStorage");
    glFramebufferRenderbuffer = symbol(gles_library, "glFramebufferRenderbuffer");
    glCheckFramebufferStatus = symbol(gles_library, "glCheckFramebufferStatus");
    glGenTextures = symbol(gles_library, "glGenTextures");
    glBindTexture = symbol(gles_library, "glBindTexture");
    glTexParameteri = symbol(gles_library, "glTexParameteri");
    glTexImage2D = symbol(gles_library, "glTexImage2D");
    glFramebufferTexture2D = symbol(gles_library, "glFramebufferTexture2D");
    glDeleteFramebuffers = symbol(gles_library, "glDeleteFramebuffers");
    glDeleteRenderbuffers = symbol(gles_library, "glDeleteRenderbuffers");
    glDeleteTextures = symbol(gles_library, "glDeleteTextures");

    display = NULL;
    if (getenv("EGL_DRM_DEVICE") != NULL) {
        EGLBoolean (*eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *);
        const char *(*eglQueryDeviceStringEXT)(EGLDeviceEXT, EGLint);
        EGLDisplay (*eglGetPlatformDisplayEXT)(unsigned int, void *, const EGLint *);
        EGLDeviceEXT devices[16];
        EGLint device_count = 0;
        EGLint index;

        eglQueryDevicesEXT = eglGetProcAddress("eglQueryDevicesEXT");
        eglQueryDeviceStringEXT = eglGetProcAddress("eglQueryDeviceStringEXT");
        eglGetPlatformDisplayEXT = eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (eglQueryDevicesEXT == NULL || eglQueryDeviceStringEXT == NULL ||
            eglGetPlatformDisplayEXT == NULL)
            fail("EGL device extensions unavailable");
        if (!eglQueryDevicesEXT(16, devices, &device_count))
            fail("eglQueryDevicesEXT");
        for (index = 0; index < device_count; index++) {
            const char *primary = eglQueryDeviceStringEXT(devices[index], EGL_DRM_DEVICE_FILE_EXT);
            const char *render = eglQueryDeviceStringEXT(devices[index], EGL_DRM_RENDER_NODE_FILE_EXT);

            if ((primary != NULL && strcmp(primary, getenv("EGL_DRM_DEVICE")) == 0) ||
                (render != NULL && strcmp(render, getenv("EGL_DRM_DEVICE")) == 0))
                display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT,
                                                   devices[index], NULL);
        }
        if (display == NULL)
            fail("requested EGL_DRM_DEVICE not found");
    } else {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (display == NULL || !eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "FAIL: eglInitialize error=0x%x\n", eglGetError());
        return 1;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API))
        fail("eglBindAPI");
    if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) || config_count != 1)
        fail("eglChooseConfig");

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT)
        fail("eglCreateContext");

    surfaces[0] = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    surfaces[1] = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (surfaces[0] == EGL_NO_SURFACE || surfaces[1] == EGL_NO_SURFACE) {
        fprintf(stderr, "FAIL: eglCreatePbufferSurface error=0x%x\n", eglGetError());
        return 1;
    }

    if (!eglMakeCurrent(display, surfaces[0], surfaces[0], context))
        fail("initial eglMakeCurrent");

    if (use_fbo) {
        int index;

        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        if (use_texture)
            glGenTextures(2, textures);
        else
            glGenRenderbuffers(2, renderbuffers);
        for (index = 0; index < 2; index++) {
            if (use_texture) {
                glBindTexture(GL_TEXTURE_2D, textures[index]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, textures[index], 0);
            } else {
                glBindRenderbuffer(GL_RENDERBUFFER, renderbuffers[index]);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, WIDTH, HEIGHT);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_RENDERBUFFER, renderbuffers[index]);
            }
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                fail("incomplete framebuffer");
        }
    }

    printf("egl=%d.%d renderer=%s dimensions=%dx%d iterations=%d duration_seconds=%d alternate=%d fbo=%d texture=%d\n",
           major, minor, glGetString(GL_RENDERER), WIDTH, HEIGHT, iterations,
           duration_seconds, alternate, use_fbo, use_texture);
    if (!summary_only)
        puts("frame,buffer,select_us,finish_us,total_us");

    if (duration_seconds > 0)
        deadline_ns = monotonic_ns() + (uint64_t)duration_seconds * 1000000000ULL;

    for (frame = 0; duration_seconds > 0 ? monotonic_ns() < deadline_ns : frame < iterations;
         frame++) {
        const int index = alternate ? frame & 1 : 0;
        uint64_t start_ns = monotonic_ns();
        uint64_t bound_ns;
        uint64_t finished_ns;
        uint64_t select_ns;
        uint64_t finish_ns;
        uint64_t frame_ns;

        if (use_fbo) {
            if (use_texture)
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, textures[index], 0);
            else
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_RENDERBUFFER, renderbuffers[index]);
        } else if (!eglMakeCurrent(display, surfaces[index], surfaces[index], context)) {
            fail("eglMakeCurrent");
        }
        bound_ns = monotonic_ns();
        glClearColor(index ? 0.125f : 0.5f, 0.25f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        finished_ns = monotonic_ns();

        select_ns = bound_ns - start_ns;
        finish_ns = finished_ns - bound_ns;
        frame_ns = finished_ns - start_ns;
        total_ns += frame_ns;
        if (select_ns > maximum_select_ns)
            maximum_select_ns = select_ns;
        if (finish_ns > maximum_finish_ns)
            maximum_finish_ns = finish_ns;
        if (frame_ns > maximum_total_ns)
            maximum_total_ns = frame_ns;
        if (frame_ns > 500000000ULL)
            frames_over_500_ms++;

        if (!summary_only) {
            printf("%d,%d,%llu,%llu,%llu\n",
                   frame, index,
                   (unsigned long long)(select_ns / 1000ULL),
                   (unsigned long long)(finish_ns / 1000ULL),
                   (unsigned long long)(frame_ns / 1000ULL));
        }
    }

    if (summary_only) {
        printf("summary frames=%d average_total_us=%llu max_select_us=%llu max_finish_us=%llu max_total_us=%llu over_500ms=%u\n",
               frame,
               (unsigned long long)(frame > 0 ? total_ns / (uint64_t)frame / 1000ULL : 0),
               (unsigned long long)(maximum_select_ns / 1000ULL),
               (unsigned long long)(maximum_finish_ns / 1000ULL),
               (unsigned long long)(maximum_total_ns / 1000ULL),
               frames_over_500_ms);
        fflush(stdout);
    }

    fputs("cleanup gl-objects begin\n", stderr);
    fflush(stderr);
    if (use_fbo) {
        if (use_texture)
            glDeleteTextures(2, textures);
        else
            glDeleteRenderbuffers(2, renderbuffers);
        glDeleteFramebuffers(1, &framebuffer);
    }
    glFinish();
    fputs("cleanup gl-objects complete\n", stderr);
    fflush(stderr);
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
        fail("cleanup eglMakeCurrent");
    fputs("cleanup unbind complete\n", stderr);
    fflush(stderr);
    if (!eglDestroySurface(display, surfaces[1]) ||
        !eglDestroySurface(display, surfaces[0]))
        fail("eglDestroySurface");
    fputs("cleanup surfaces complete\n", stderr);
    fflush(stderr);
    if (!eglDestroyContext(display, context))
        fail("eglDestroyContext");
    fputs("cleanup context complete\n", stderr);
    fflush(stderr);
    if (!eglTerminate(display))
        fail("eglTerminate");
    fputs("cleanup egl terminate complete\n", stderr);
    fflush(stderr);
    dlclose(gles_library);
    dlclose(egl_library);
    fputs("cleanup libraries complete\n", stderr);
    fflush(stderr);

    return 0;
}
