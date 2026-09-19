/* dlmopen feasibility probe for the hard-float DDK shim (option 2).
 *
 * The decisive bet: the process's single (armhf) dynamic linker can load the
 * soft-float armel glibc into a fresh dlmopen namespace alongside the soft-float
 * DDK, so the DDK's internal libc/libm calls bind a MATCHING soft-float libc —
 * no per-symbol interposition. Both glibcs are 2.36 and the ld.so<->libc private
 * interface is float-free, so this should hold.
 *
 * This loads libGLESv2.so into LM_ID_NEWLM, reports which libc landed in the
 * namespace, and drives ABI-neutral EGL init far enough to read GL_RENDERER —
 * if the DDK initializes here, option 2 is viable.
 *
 * The DDK copies must be RPATH'd to the armel libc first, e.g.:
 *   patchelf --force-rpath --set-rpath /lib/arm-linux-gnueabi:<ddkdir> libGLESv2.so
 * Build hard-float: arm-linux-gnueabihf-gcc -I<ddk headers> hf-dlmopen-probe.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

static void *ns_sym(void *handle, const char *name)
{
    void *p = dlsym(handle, name);
    if (!p) {
        fprintf(stderr, "FAIL: missing %s: %s\n", name, dlerror());
        exit(1);
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "libGLESv2.so";

    void *ns = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
    if (!ns) {
        fprintf(stderr, "FAIL: dlmopen %s: %s\n", path, dlerror());
        return 1;
    }
    printf("dlmopen OK: %s in a new namespace\n", path);

    /* Report the namespace id and which libc/libm mapped into it. */
    Lmid_t lmid = 0;
    if (dlinfo(ns, RTLD_DI_LMID, &lmid) == 0)
        printf("namespace lmid = %ld (base = %ld)\n", (long)lmid, (long)LM_ID_BASE);
    struct link_map *lm = NULL;
    if (dlinfo(ns, RTLD_DI_LINKMAP, &lm) == 0) {
        printf("namespace objects:\n");
        for (struct link_map *p = lm; p; p = p->l_next)
            if (p->l_name && p->l_name[0])
                printf("  %s\n", p->l_name);
    }

    /* Drive EGL init through the namespace (ABI-neutral: pointers/ints only). */
    EGLDisplay (*eglGetDisplay)(void *) = ns_sym(ns, "eglGetDisplay");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = ns_sym(ns, "eglInitialize");
    EGLBoolean (*eglBindAPI)(unsigned) = ns_sym(ns, "eglBindAPI");
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = ns_sym(ns, "eglChooseConfig");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = ns_sym(ns, "eglCreateContext");
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = ns_sym(ns, "eglCreatePbufferSurface");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = ns_sym(ns, "eglMakeCurrent");
    EGLint (*eglGetError)(void) = ns_sym(ns, "eglGetError");
    const unsigned char *(*glGetString)(unsigned) = ns_sym(ns, "glGetString");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        fprintf(stderr, "FAIL: eglInitialize 0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d up in namespace\n", maj, min);
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfgattr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint pbuf[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgattr, &config, 1, &n) || n < 1) {
        fprintf(stderr, "FAIL: chooseConfig 0x%x\n", eglGetError());
        return 1;
    }
    EGLSurface surf = eglCreatePbufferSurface(dpy, config, pbuf);
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxattr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "FAIL: surface/context 0x%x\n", eglGetError());
        return 1;
    }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "FAIL: makeCurrent 0x%x\n", eglGetError());
        return 1;
    }
    printf("renderer = %s\n", (const char *)glGetString(GL_RENDERER));
    printf("RESULT: PASS (DDK initialized in a dlmopen namespace)\n");
    return 0;
}
