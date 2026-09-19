/* Shared namespace loader for the hard-float DDK shim (libsgxhf).
 *
 * A hard-float application links against the shim libEGL/libGLESv2 (same
 * sonames as the real DDK). Those shims are pure ABI veneers; the actual
 * rendering is done by the real soft-float TI DDK 1.6 libraries, which this
 * loader brings up in an isolated dlmopen link-map namespace.
 *
 * The one hard requirement (proven on hardware): libSGXm.so.1 — the soft-float
 * reverse interposer for the DDK's libm/libc float imports — must resolve those
 * symbols BEFORE the namespace's hard-float libc/libm. That is arranged by
 * patching the real libEGL/libGLESv2 so libSGXm is their first DT_NEEDED, which
 * places it at the front of the namespace's global search scope. Versioning the
 * interposer's symbols at GLIBC_2.4 makes them exact-version matches so they win
 * over libc's own strtod/atof/etc.
 *
 * dlmopen forbids RTLD_GLOBAL, so we keep a per-soname handle table and resolve
 * each symbol against the specific DDK library that exports it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hf-shim-loader.h"

#define MAX_LIBS 16

struct entry {
    const char *soname;   /* interned (strdup or literal) */
    void *handle;
};

static Lmid_t g_ns;
static int g_ready;
static struct entry g_libs[MAX_LIBS];
static int g_nlibs;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Absolute directory holding the real soft-float DDK. Absolute paths keep
 * dlmopen from re-finding the hard-float shim (same soname) on the app's path. */
static const char *ddk_dir(void)
{
    const char *d = getenv("SGXHF_DDK_DIR");
    return (d && *d) ? d : "/opt/sgx-ddk16/lib";
}

static void *open_ddk(Lmid_t ns, const char *soname)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", ddk_dir(), soname);
    void *h = dlmopen(ns, path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        fprintf(stderr, "libsgxhf: dlmopen %s: %s\n", path, dlerror());
    return h;
}

/* libEGL is the namespace root; libGLESv2 shares the context state, so both are
 * loaded eagerly. Their patched NEEDED pulls libSGXm in first. */
static const char *const PRELOAD[] = { "libEGL.so", "libGLESv2.so" };

static void ns_init(void)
{
    for (unsigned i = 0; i < sizeof PRELOAD / sizeof PRELOAD[0]; i++) {
        Lmid_t ns = (i == 0) ? LM_ID_NEWLM : g_ns;
        void *h = open_ddk(ns, PRELOAD[i]);
        if (!h)
            return;
        if (i == 0 && dlinfo(h, RTLD_DI_LMID, &g_ns) != 0) {
            fprintf(stderr, "libsgxhf: dlinfo: %s\n", dlerror());
            return;
        }
        g_libs[g_nlibs].soname = PRELOAD[i];
        g_libs[g_nlibs].handle = h;
        g_nlibs++;
    }
    g_ready = 1;
}

void *sgxhf_dlsym(const char *soname, const char *sym)
{
    pthread_once(&g_once, ns_init);
    if (!g_ready) {
        fprintf(stderr, "libsgxhf: namespace not initialised (needed %s)\n", sym);
        abort();
    }

    for (int i = 0; i < g_nlibs; i++)
        if (strcmp(g_libs[i].soname, soname) == 0)
            return dlsym(g_libs[i].handle, sym);

    /* A DDK library not in PRELOAD (e.g. libGLES_CM). Load it into the same
     * namespace on demand and cache the handle. */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nlibs; i++) {
        if (strcmp(g_libs[i].soname, soname) == 0) {
            pthread_mutex_unlock(&g_lock);
            return dlsym(g_libs[i].handle, sym);
        }
    }
    void *h = (g_nlibs < MAX_LIBS) ? open_ddk(g_ns, soname) : NULL;
    if (h) {
        g_libs[g_nlibs].soname = strdup(soname);
        g_libs[g_nlibs].handle = h;
        g_nlibs++;
    }
    pthread_mutex_unlock(&g_lock);
    return h ? dlsym(h, sym) : NULL;
}
