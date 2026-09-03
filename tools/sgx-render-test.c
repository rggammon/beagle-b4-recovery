/* Self-contained offscreen EGL+GLES2 render probe for the SGX530.
 * dlopen()s the platform libEGL/libGLESv2 at runtime — no build-time GL deps.
 * Surfaceless pbuffer-less context -> FBO -> clear -> glReadPixels.
 * Prints GL_RENDERER and the read-back pixel so we can tell HW (PowerVR)
 * from software (llvmpipe/softpipe), and whether a draw actually landed.
 * Build: arm-linux-gnueabihf-gcc sgx-render-test.c -o sgx-render-test -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

/* --- minimal EGL/GLES enums --- */
#define EGL_DEFAULT_DISPLAY      ((void*)0)
#define EGL_NO_DISPLAY           ((void*)0)
#define EGL_NO_CONTEXT           ((void*)0)
#define EGL_NO_SURFACE           ((void*)0)
#define EGL_OPENGL_ES_API        0x30A0
#define EGL_VENDOR               0x3053
#define EGL_VERSION              0x3054
#define EGL_EXTENSIONS           0x3055
#define EGL_SURFACE_TYPE         0x3033
#define EGL_PBUFFER_BIT          0x0001
#define EGL_RENDERABLE_TYPE      0x3040
#define EGL_OPENGL_ES2_BIT       0x0004
#define EGL_RED_SIZE             0x3024
#define EGL_GREEN_SIZE           0x3023
#define EGL_BLUE_SIZE            0x3022
#define EGL_ALPHA_SIZE           0x3021
#define EGL_NONE                 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_DEVICE_EXT  0x313F
#define EGL_DRM_DEVICE_FILE_EXT  0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377

#define GL_VENDOR                0x1F00
#define GL_RENDERER              0x1F01
#define GL_VERSION_GL            0x1F02
#define GL_EXTENSIONS            0x1F03
#define GL_FRAMEBUFFER           0x8D40
#define GL_RENDERBUFFER          0x8D41
#define GL_COLOR_ATTACHMENT0     0x8CE0
#define GL_RGBA4                 0x8056
#define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#define GL_COLOR_BUFFER_BIT      0x00004000
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401

typedef void* EGLDisplay; typedef void* EGLContext; typedef void* EGLConfig;
typedef void* EGLSurface; typedef int EGLint; typedef unsigned int EGLBoolean;
typedef void* EGLDeviceEXT;
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint;
typedef int GLsizei; typedef float GLclampf; typedef unsigned int GLbitfield;

static void die(const char* m){ fprintf(stderr,"FAIL: %s\n", m); exit(1); }
static void* need(void* h, const char* n){ void* p=dlsym(h,n); if(!p){ fprintf(stderr,"FAIL: missing sym %s\n",n); exit(1);} return p; }

int main(int argc, char** argv){
    int width=argc>2 ? atoi(argv[2]) : 64;
    int height=argc>3 ? atoi(argv[3]) : 64;
    setvbuf(stdout, NULL, _IOLBF, 0);
    void* egl = dlopen("libEGL.so.1", RTLD_NOW|RTLD_GLOBAL);
    if(!egl) die(dlerror());
    void* gl  = dlopen("libGLESv2.so.2", RTLD_NOW|RTLD_GLOBAL);
    if(!gl) die(dlerror());

    EGLDisplay (*eglGetDisplay)(void*) = need(egl,"eglGetDisplay");
    EGLBoolean (*eglInitialize)(EGLDisplay,EGLint*,EGLint*) = need(egl,"eglInitialize");
    const char* (*eglQueryString)(EGLDisplay,EGLint) = need(egl,"eglQueryString");
    EGLBoolean (*eglBindAPI)(unsigned) = need(egl,"eglBindAPI");
    EGLBoolean (*eglChooseConfig)(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*) = need(egl,"eglChooseConfig");
    EGLContext (*eglCreateContext)(EGLDisplay,EGLConfig,EGLContext,const EGLint*) = need(egl,"eglCreateContext");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay,EGLSurface,EGLSurface,EGLContext) = need(egl,"eglMakeCurrent");
    EGLint (*eglGetError)(void) = need(egl,"eglGetError");
    void* (*eglGetProcAddress)(const char*) = need(egl,"eglGetProcAddress");

    const char* (*glGetString)(GLenum) = need(gl,"glGetString");
    void (*glGenFramebuffers)(GLsizei,GLuint*) = need(gl,"glGenFramebuffers");
    void (*glBindFramebuffer)(GLenum,GLuint) = need(gl,"glBindFramebuffer");
    void (*glGenRenderbuffers)(GLsizei,GLuint*) = need(gl,"glGenRenderbuffers");
    void (*glBindRenderbuffer)(GLenum,GLuint) = need(gl,"glBindRenderbuffer");
    void (*glRenderbufferStorage)(GLenum,GLenum,GLsizei,GLsizei) = need(gl,"glRenderbufferStorage");
    void (*glFramebufferRenderbuffer)(GLenum,GLenum,GLenum,GLuint) = need(gl,"glFramebufferRenderbuffer");
    GLenum (*glCheckFramebufferStatus)(GLenum) = need(gl,"glCheckFramebufferStatus");
    void (*glViewport)(GLint,GLint,GLsizei,GLsizei) = need(gl,"glViewport");
    void (*glClearColor)(GLclampf,GLclampf,GLclampf,GLclampf) = need(gl,"glClearColor");
    void (*glClear)(GLbitfield) = need(gl,"glClear");
    void (*glFinish)(void) = need(gl,"glFinish");
    void (*glReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*) = need(gl,"glReadPixels");
    GLenum (*glGetError)(void) = need(gl,"glGetError");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    const char* drm_device = getenv("EGL_DRM_DEVICE");
    if(drm_device){
        EGLBoolean (*eglQueryDevicesEXT)(EGLint,EGLDeviceEXT*,EGLint*) =
            eglGetProcAddress("eglQueryDevicesEXT");
        const char* (*eglQueryDeviceStringEXT)(EGLDeviceEXT,EGLint) =
            eglGetProcAddress("eglQueryDeviceStringEXT");
        EGLDisplay (*eglGetPlatformDisplayEXT)(unsigned int,void*,const EGLint*) =
            eglGetProcAddress("eglGetPlatformDisplayEXT");
        if(!eglQueryDevicesEXT || !eglQueryDeviceStringEXT || !eglGetPlatformDisplayEXT)
            die("EGL device extensions unavailable");
        EGLDeviceEXT devices[16]; EGLint count=0;
        if(!eglQueryDevicesEXT(16,devices,&count)) die("eglQueryDevicesEXT");
        printf("EGL devices: %d\n", count);
        for(EGLint i=0;i<count;i++){
            const char* primary=eglQueryDeviceStringEXT(devices[i],EGL_DRM_DEVICE_FILE_EXT);
            const char* render=eglQueryDeviceStringEXT(devices[i],EGL_DRM_RENDER_NODE_FILE_EXT);
            printf("  [%d] primary=%s render=%s\n", i,
                   primary ? primary : "(none)", render ? render : "(none)");
            if((primary && !strcmp(primary,drm_device)) || (render && !strcmp(render,drm_device)))
                dpy=eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT,devices[i],NULL);
        }
        if(dpy==EGL_NO_DISPLAY) die("requested EGL_DRM_DEVICE not found");
    }else{
        dpy=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if(dpy==EGL_NO_DISPLAY) die("eglGetDisplay -> NO_DISPLAY");
    printf("Initializing EGL display...\n");
    EGLint maj=0,min=0;
    if(!eglInitialize(dpy,&maj,&min)){ fprintf(stderr,"FAIL: eglInitialize err=0x%x\n",eglGetError()); return 1; }
    printf("EGL %d.%d vendor='%s' version='%s'\n", maj,min, eglQueryString(dpy,EGL_VENDOR), eglQueryString(dpy,EGL_VERSION));
    printf("EGL_EXTENSIONS = %s\n", eglQueryString(dpy,EGL_EXTENSIONS));

    if(!eglBindAPI(EGL_OPENGL_ES_API)) die("eglBindAPI");
    EGLint cfgattr[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cfg; EGLint n=0;
    if(!eglChooseConfig(dpy,cfgattr,&cfg,1,&n)||n<1){ fprintf(stderr,"FAIL: chooseConfig err=0x%x n=%d\n",eglGetError(),n); return 1; }
    EGLint ctxattr[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxattr);
    if(ctx==EGL_NO_CONTEXT){ fprintf(stderr,"FAIL: createContext err=0x%x\n",eglGetError()); return 1; }
    /* surfaceless: needs EGL_KHR_surfaceless_context */
    if(!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)){ fprintf(stderr,"FAIL: makeCurrent(surfaceless) err=0x%x\n",eglGetError()); return 1; }

    printf("GL_VENDOR   = %s\n", glGetString(GL_VENDOR));
    printf("GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("GL_VERSION  = %s\n", glGetString(GL_VERSION_GL));
    printf("GL_EXTENSIONS = %s\n", glGetString(GL_EXTENSIONS));

    GLuint fbo=0,rb=0;
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glGenRenderbuffers(1,&rb); glBindRenderbuffer(GL_RENDERBUFFER,rb);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA4,width,height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
    GLenum st=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(st!=GL_FRAMEBUFFER_COMPLETE){ fprintf(stderr,"FAIL: FBO incomplete 0x%x\n",st); return 1; }
    glViewport(0,0,width,height);
    if(argc>1){
        int frames=atoi(argv[1]);
        struct timespec start,end;
        clock_gettime(CLOCK_MONOTONIC,&start);
        for(int i=0;i<frames;i++){
            glClearColor((float)(i&1),0.25f,0.5f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();
        }
        clock_gettime(CLOCK_MONOTONIC,&end);
        double seconds=(end.tv_sec-start.tv_sec)+(end.tv_nsec-start.tv_nsec)/1000000000.0;
         printf("BENCH: %d synchronized %dx%d clears in %.3f s = %.1f fps\n",
             frames,width,height,seconds,frames/seconds);
    }
    glClearColor(0.2f,0.4f,0.6f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    unsigned char px[4]={0,0,0,0};
    glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    printf("PIXEL RGBA   = %u %u %u %u  (expect ~51 102 153 255)\n",px[0],px[1],px[2],px[3]);
    printf("glGetError   = 0x%x\n", glGetError());
    int ok = (px[0]>40&&px[0]<70) && (px[1]>90&&px[1]<115) && (px[2]>140&&px[2]<170);
    printf("RESULT: %s\n", ok? "RENDER-OK (a frame was drawn & read back)" : "RENDER-MISMATCH");
    return ok?0:1;
}
