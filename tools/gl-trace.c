/* LD_PRELOAD interposer: per-frame GLES2 call profiler.
 *
 * Purpose: diff femtovg's GL call stream for a CLEAN render (FVG_MODE=tilesgrect,
 * gradient plain rect) vs a STORMING render (FVG_MODE=tilesredraw, gradient
 * rounded rect). Same paint, only the shape differs, so the per-frame delta
 * (extra draws / stencil passes / geometry volume) is the storm trigger.
 *
 * Works through the hf-shim because femtovg resolves gl* via eglGetProcAddress,
 * which the shim resolves via dlsym(RTLD_DEFAULT), landing on this preloaded
 * interposer first. Each wrapped call forwards to the shim veneer via
 * dlsym(libGLESv2.so) / RTLD_NEXT.
 *
 * Frame boundary = glClear (femtovg clears once per frame). On each glClear we
 * flush the previous frame's summary and reset counters. Detailed per-call
 * logging is gated to frames [GLTRACE_FROM..GLTRACE_TO] (default 4..6) to catch
 * a steady-state frame without drowning the log.
 *
 * Build (cross, hardfloat, Bookworm sysroot):
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libgl-trace.so tools/gl-trace.c -ldl
 * Run on board:
 *   LD_PRELOAD=/root/libgl-trace.so GLTRACE_LOG=/tmp/glt.log \
 *   FVG_WINDOW=1 FVG_MODE=tilesredraw FVG_TILES=12 FVG_SECONDS=6 \
 *   LD_LIBRARY_PATH=/root/slint-libs sgx-ddk16-hf-run /root/femtovg-probe
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int   GLenum;
typedef unsigned int   GLbitfield;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef long           GLsizeiptr;

static FILE *logf = NULL;
static long total_calls = 0;
static long cap = 2000;   /* stop detailed logging after this many wrapped calls */

/* running totals (whole run) */
static long t_drawarrays, t_drawelements, t_bufferdata, t_useprogram;
static long t_bindtexture, t_teximage, t_texsubimage, t_flush;
static long t_enable, t_disable, t_stencilfunc, t_stencilop, t_blendfunc;
static long verts_arrays, verts_elems, bytes_bufferdata;

#define DETAIL (total_calls < cap)

__attribute__((constructor))
static void open_log(void)
{
    if (logf) return;
    const char *p = getenv("GLTRACE_LOG");
    if (!p) p = "/tmp/glt.log";
    logf = fopen(p, "w");
    if (!logf) logf = stderr;
    setvbuf(logf, NULL, _IOLBF, 0);
    const char *c = getenv("GLTRACE_CAP");
    if (c) { long v = 0; while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0'); if (v > 0) cap = v; }
    fprintf(logf, "# gl-trace started, detail cap=%ld calls\n", cap);
}

__attribute__((destructor))
static void dump_totals(void)
{
    if (!logf) return;
    fprintf(logf,
        "# TOTALS draws=%ld(arr=%ld elem=%ld) verts=%ld(arr=%ld elem=%ld) "
        "bufdata=%ld(%ld B) useprog=%ld bindtex=%ld teximg=%ld texsub=%ld "
        "flush=%ld en=%ld dis=%ld stencilfunc=%ld stencilop=%ld blendfunc=%ld\n",
        t_drawarrays + t_drawelements, t_drawarrays, t_drawelements,
        verts_arrays + verts_elems, verts_arrays, verts_elems,
        t_bufferdata, bytes_bufferdata, t_useprogram, t_bindtexture,
        t_teximage, t_texsubimage, t_flush, t_enable, t_disable,
        t_stencilfunc, t_stencilop, t_blendfunc);
}

static void *sym(const char *name)
{
    void *lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
    void *p = lib ? dlsym(lib, name) : NULL;
    if (!p) p = dlsym(RTLD_NEXT, name);
    return p;
}

/* Per-frame summary: at each frame boundary print the delta since the previous
 * boundary, so a cache-ON steady frame (few textured-quad draws) is visible
 * distinct from a bake frame (full fringe stream). Emitted unconditionally
 * (not gated by the detail cap) so late steady-state frames are captured. */
static long fs_frame;
static long fs_draws, fs_verts, fs_useprog, fs_bindtex, fs_teximg, fs_bufbytes;
static void frame_summary(void)
{
    long d  = (t_drawarrays + t_drawelements) - fs_draws;
    long v  = (verts_arrays + verts_elems)    - fs_verts;
    long up = t_useprogram  - fs_useprog;
    long bt = t_bindtexture - fs_bindtex;
    long ti = t_teximage    - fs_teximg;
    long bb = bytes_bufferdata - fs_bufbytes;
    fprintf(logf, "FRAME %ld draws=%ld verts=%ld useprog=%ld bindtex=%ld teximg=%ld bufbytes=%ld\n",
            fs_frame++, d, v, up, bt, ti, bb);
    fs_draws = t_drawarrays + t_drawelements;
    fs_verts = verts_arrays + verts_elems;
    fs_useprog = t_useprogram;
    fs_bindtex = t_bindtexture;
    fs_teximg = t_teximage;
    fs_bufbytes = bytes_bufferdata;
}

void glClear(GLbitfield mask)
{
    static void (*real)(GLbitfield);
    if (!real) real = (void (*)(GLbitfield))sym("glClear");
    frame_summary();
    total_calls++;
    if (DETAIL) fprintf(logf, "  glClear 0x%04x\n", mask);
    if (real) real(mask);
}

void glFlush(void)
{
    static void (*real)(void);
    if (!real) real = (void (*)(void))sym("glFlush");
    t_flush++; total_calls++;
    if (DETAIL) fprintf(logf, "-- glFlush (frame delim) --\n");
    if (real) real();
}

void glFinish(void)
{
    static void (*real)(void);
    if (!real) real = (void (*)(void))sym("glFinish");
    t_flush++; total_calls++;
    if (DETAIL) fprintf(logf, "-- glFinish (frame delim) --\n");
    if (real) real();
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static void (*real)(GLenum, GLint, GLsizei);
    if (!real) real = (void (*)(GLenum, GLint, GLsizei))sym("glDrawArrays");
    t_drawarrays++; verts_arrays += count; total_calls++;
    if (DETAIL) fprintf(logf, "  glDrawArrays mode=0x%x first=%d count=%d\n", mode, first, count);
    if (real) real(mode, first, count);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    static void (*real)(GLenum, GLsizei, GLenum, const void *);
    if (!real) real = (void (*)(GLenum, GLsizei, GLenum, const void *))sym("glDrawElements");
    t_drawelements++; verts_elems += count; total_calls++;
    if (DETAIL) fprintf(logf, "  glDrawElements mode=0x%x count=%d type=0x%x\n", mode, count, type);
    if (real) real(mode, count, type, indices);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    static void (*real)(GLenum, GLsizeiptr, const void *, GLenum);
    if (!real) real = (void (*)(GLenum, GLsizeiptr, const void *, GLenum))sym("glBufferData");
    t_bufferdata++; bytes_bufferdata += (long)size; total_calls++;
    if (DETAIL) fprintf(logf, "  glBufferData target=0x%x size=%ld usage=0x%x\n", target, (long)size, usage);
    if (real) real(target, size, data, usage);
}

void glUseProgram(GLuint program)
{
    static void (*real)(GLuint);
    if (!real) real = (void (*)(GLuint))sym("glUseProgram");
    t_useprogram++; total_calls++;
    if (DETAIL) fprintf(logf, "  glUseProgram %u\n", program);
    if (real) real(program);
}

void glBindTexture(GLenum target, GLuint texture)
{
    static void (*real)(GLenum, GLuint);
    if (!real) real = (void (*)(GLenum, GLuint))sym("glBindTexture");
    t_bindtexture++; total_calls++;
    if (DETAIL) fprintf(logf, "  glBindTexture target=0x%x tex=%u\n", target, texture);
    if (real) real(target, texture);
}

void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                  GLint border, GLenum format, GLenum type, const void *pixels)
{
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    if (!real) real = (void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))sym("glTexImage2D");
    t_teximage++; total_calls++;
    if (DETAIL) fprintf(logf, "  glTexImage2D %dx%d ifmt=0x%x fmt=0x%x\n", w, h, ifmt, format);
    if (real) real(target, level, ifmt, w, h, border, format, type, pixels);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w, GLsizei h,
                     GLenum format, GLenum type, const void *pixels)
{
    static void (*real)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    if (!real) real = (void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))sym("glTexSubImage2D");
    t_texsubimage++; total_calls++;
    if (DETAIL) fprintf(logf, "  glTexSubImage2D %dx%d fmt=0x%x\n", w, h, format);
    if (real) real(target, level, xoff, yoff, w, h, format, type, pixels);
}

void glEnable(GLenum cap)
{
    static void (*real)(GLenum);
    if (!real) real = (void (*)(GLenum))sym("glEnable");
    t_enable++; total_calls++;
    if (DETAIL) fprintf(logf, "  glEnable 0x%x%s\n", cap, cap == 0x0B90 ? " STENCIL_TEST" : "");
    if (real) real(cap);
}

void glDisable(GLenum cap)
{
    static void (*real)(GLenum);
    if (!real) real = (void (*)(GLenum))sym("glDisable");
    t_disable++; total_calls++;
    if (DETAIL) fprintf(logf, "  glDisable 0x%x\n", cap);
    if (real) real(cap);
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
    static void (*real)(GLenum, GLint, GLuint);
    if (!real) real = (void (*)(GLenum, GLint, GLuint))sym("glStencilFunc");
    t_stencilfunc++; total_calls++;
    if (DETAIL) fprintf(logf, "  glStencilFunc func=0x%x ref=%d mask=0x%x\n", func, ref, mask);
    if (real) real(func, ref, mask);
}

void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
{
    static void (*real)(GLenum, GLenum, GLenum);
    if (!real) real = (void (*)(GLenum, GLenum, GLenum))sym("glStencilOp");
    t_stencilop++; total_calls++;
    if (DETAIL) fprintf(logf, "  glStencilOp 0x%x 0x%x 0x%x\n", sfail, dpfail, dppass);
    if (real) real(sfail, dpfail, dppass);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    static void (*real)(GLenum, GLenum);
    if (!real) real = (void (*)(GLenum, GLenum))sym("glBlendFunc");
    t_blendfunc++; total_calls++;
    if (DETAIL) fprintf(logf, "  glBlendFunc 0x%x 0x%x\n", sfactor, dfactor);
    if (real) real(sfactor, dfactor);
}
