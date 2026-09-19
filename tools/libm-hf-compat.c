/* Soft-float libm+libc compat lib for the hard-float DDK shim (libSGXm.so.1).
 *
 * The softfp DDK calls float-ABI libm (sinf, powf, ...) AND float-returning libc
 * (strtod, atof, ...) with the soft-float ABI (scalar floats in core registers).
 * In a hard-float process those otherwise bind the system hard-float libm/libc.
 * Instead of LD_PRELOAD (global — corrupts every caller), each DDK library is
 * patched: patchelf --replace-needed libm.so.6 libSGXm.so.1, putting libSGXm
 * ahead of libc in that library's own NEEDED list, so ONLY the DDK's float-ABI
 * references resolve here — the app and glibc are untouched.
 *
 * Each export is a soft-float (pcs("aapcs")) wrapper forwarding to the real
 * hard-float implementation via dlsym(RTLD_NEXT) (the next definition after this
 * lib). libSGXm links libm/libc normally, so both are in the dependency graph
 * before any constructor runs — no dlopen during init (which would recurse under
 * the loader lock). GCC does the VFP<->core register move.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define PCS __attribute__((pcs("aapcs")))

static void *resolve(const char *name)
{
    /* libm/libc are DT_NEEDED (linked --no-as-needed), so they are already
     * mapped: RTLD_NOLOAD just hands back the existing handle — no fresh load,
     * hence no recursive dlopen if the DDK calls us during its own init. */
    static void *hm, *hc;
    void *p = NULL;
    if (getenv("LIBSGXM_TRACE")) fprintf(stderr, "libSGXm: wrap %s\n", name);
    if (!hm) hm = dlopen("libm.so.6", RTLD_NOLOAD | RTLD_NOW);
    if (hm) p = dlsym(hm, name);
    if (!p) {
        if (!hc) hc = dlopen("libc.so.6", RTLD_NOLOAD | RTLD_NOW);
        if (hc) p = dlsym(hc, name);
    }
    if (!p) {
        fprintf(stderr, "libSGXm: real %s missing\n", name);
        abort();
    }
    return p;
}

/* float f(float) */
#define WRAP_F1(name)                                                   \
    PCS float name(float x) {                                           \
        static float (*real)(float);                                    \
        if (!real) real = (float (*)(float)) resolve(#name);            \
        return real(x);                                                 \
    }
WRAP_F1(sinf) WRAP_F1(cosf) WRAP_F1(tanf)
WRAP_F1(asinf) WRAP_F1(acosf) WRAP_F1(atanf)
WRAP_F1(expf) WRAP_F1(logf) WRAP_F1(sqrtf)
WRAP_F1(floorf) WRAP_F1(ceilf)

/* float f(float, float) */
#define WRAP_F2(name)                                                   \
    PCS float name(float a, float b) {                                  \
        static float (*real)(float, float);                             \
        if (!real) real = (float (*)(float, float)) resolve(#name);     \
        return real(a, b);                                              \
    }
WRAP_F2(fmodf) WRAP_F2(powf)

/* double f(double) */
PCS double log(double x)
{
    static double (*real)(double);
    if (!real) real = (double (*)(double)) resolve("log");
    return real(x);
}

/* double f(double, double) */
#define WRAP_D2(name)                                                   \
    PCS double name(double a, double b) {                               \
        static double (*real)(double, double);                          \
        if (!real) real = (double (*)(double, double)) resolve(#name);  \
        return real(a, b);                                              \
    }
WRAP_D2(pow) WRAP_D2(atan2)

/* float frexpf(float, int *) */
PCS float frexpf(float x, int *e)
{
    static float (*real)(float, int *);
    if (!real) real = (float (*)(float, int *)) resolve("frexpf");
    return real(x, e);
}

/* void sincosf(float, float *, float *) */
PCS void sincosf(float x, float *s, float *c)
{
    static void (*real)(float, float *, float *);
    if (!real) real = (void (*)(float, float *, float *)) resolve("sincosf");
    real(x, s, c);
}
/* --- libc: float-returning string conversions --- */
PCS double strtod(const char *nptr, char **endptr)
{
    static double (*real)(const char *, char **);
    if (!real) real = (double (*)(const char *, char **)) resolve("strtod");
    return real(nptr, endptr);
}

PCS float strtof(const char *nptr, char **endptr)
{
    static float (*real)(const char *, char **);
    if (!real) real = (float (*)(const char *, char **)) resolve("strtof");
    return real(nptr, endptr);
}

PCS double atof(const char *nptr)
{
    static double (*real)(const char *);
    if (!real) real = (double (*)(const char *)) resolve("atof");
    return real(nptr);
}