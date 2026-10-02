/* LD_PRELOAD interposer: render-target / texture lifecycle tracer.
 *
 * Counts framebuffer, renderbuffer and texture creates/deletes, incomplete
 * glCheckFramebufferStatus results and GL errors after allocation calls, and
 * prints a one-line summary per second plus the first GLFBO_DETAIL events.
 * Used to see what DDK 1.4 does differently when Slint's cache-rendering-hint
 * / opacity layers render through femtovg offscreen targets.
 *
 * Resolution works like tools/gl-trace.c: femtovg resolves gl* through
 * eglGetProcAddress -> shim -> dlsym(RTLD_DEFAULT), which lands here first.
 *
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libgl-fbo-trace.so tools/gl-fbo-trace.c -ldl
 *   LD_PRELOAD=/root/libgl-fbo-trace.so GLFBO_LOG=/tmp/fbo.log <app>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;

#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

static FILE *logf;
static long detail = 60;
static double t0, next_report;
static long n_frames, fb_gen, fb_del, rb_gen, rb_del, tex_gen, tex_del;
static long teximg, rbstorage, fbtex, fbbind_nonzero, incomplete, errors;
static long draws_fbo, draws_screen;
static GLuint cur_fb;
static int finish_on_switch;
static int null_teximage;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((constructor))
static void init(void)
{
	const char *p = getenv("GLFBO_LOG");
	const char *d = getenv("GLFBO_DETAIL");

	/* GLFBO_FINISH=1: glFinish before every render-target switch */
	finish_on_switch = getenv("GLFBO_FINISH") != NULL;
	/* GLFBO_NULLTEX=1: allocate textures without initial data */
	null_teximage = getenv("GLFBO_NULLTEX") != NULL;
	logf = fopen(p ? p : "/tmp/fbo.log", "w");
	if (!logf)
		logf = stderr;
	setvbuf(logf, NULL, _IOLBF, 0);
	if (d) {
		/* no atol: glibc 2.38 headers bind it to __isoc23_strtol, absent on the board */
		detail = 0;
		while (*d >= '0' && *d <= '9')
			detail = detail * 10 + (*d++ - '0');
	}
	t0 = now();
	next_report = t0 + 1.0;
}

static void *sym(const char *name)
{
	void *lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
	void *p = lib ? dlsym(lib, name) : NULL;
	return p ? p : dlsym(RTLD_NEXT, name);
}

#define REAL(ret, name, args) \
	static ret (*real) args; \
	if (!real) real = (ret (*) args)sym(#name)

static void event(const char *fmt, long a, long b, long c, long d)
{
	if (detail-- > 0) {
		fprintf(logf, "  t=%.2f ", now() - t0);
		fprintf(logf, fmt, a, b, c, d);
		fputc('\n', logf);
	}
}

static void check_error(const char *where)
{
	REAL(GLenum, glGetError, (void));
	GLenum e;
	while (real && (e = real()) != 0) {
		errors++;
		fprintf(logf, "  t=%.2f GLERROR 0x%04x after %s\n", now() - t0, e, where);
	}
}

static void report(void)
{
	double t = now();
	if (t < next_report)
		return;
	next_report = t + 1.0;
	fprintf(logf, "t=%5.1f frames=%ld fb+%ld/-%ld(live %ld) rb+%ld/-%ld(live %ld) "
		"tex+%ld/-%ld(live %ld) teximg=%ld rbstore=%ld fbtex=%ld fbbinds=%ld "
		"draws_fbo=%ld draws_scr=%ld incomplete=%ld errors=%ld\n",
		t - t0, n_frames, fb_gen, fb_del, fb_gen - fb_del, rb_gen, rb_del,
		rb_gen - rb_del, tex_gen, tex_del, tex_gen - tex_del, teximg, rbstorage,
		fbtex, fbbind_nonzero, draws_fbo, draws_screen, incomplete, errors);
}

void glClear(GLbitfield mask)
{
	REAL(void, glClear, (GLbitfield));
	if (cur_fb == 0)
		n_frames++;
	else
		event("  clear fb=%ld mask=0x%lx%.0ld%.0ld", cur_fb, mask, 0, 0);
	report();
	real(mask);
}

void glGenFramebuffers(GLsizei n, GLuint *ids)
{
	REAL(void, glGenFramebuffers, (GLsizei, GLuint *));
	real(n, ids);
	fb_gen += n;
	event("genFB id=%ld (n=%ld)%.0ld%.0ld", n > 0 ? ids[0] : 0, n, 0, 0);
}

void glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
	REAL(void, glDeleteFramebuffers, (GLsizei, const GLuint *));
	event("delFB id=%ld (n=%ld)%.0ld%.0ld", n > 0 ? ids[0] : 0, n, 0, 0);
	fb_del += n;
	real(n, ids);
}

void glBindFramebuffer(GLenum target, GLuint fb)
{
	REAL(void, glBindFramebuffer, (GLenum, GLuint));
	if (finish_on_switch && fb != cur_fb) {
		static void (*fin)(void);
		if (!fin)
			fin = (void (*)(void))sym("glFinish");
		fin();
	}
	cur_fb = fb;
	if (fb)
		fbbind_nonzero++;
	event("bindFB %ld%.0ld%.0ld%.0ld", fb, 0, 0, 0);
	real(target, fb);
}

void glBindTexture(GLenum target, GLuint tex)
{
	REAL(void, glBindTexture, (GLenum, GLuint));
	if (cur_fb)
		event("  bindTex %ld (into fb %ld)%.0ld%.0ld", tex, cur_fb, 0, 0);
	real(target, tex);
}

void glFramebufferTexture2D(GLenum target, GLenum attach, GLenum textarget, GLuint tex, GLint level)
{
	REAL(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
	fbtex++;
	event("fbTex fb=%ld tex=%ld attach=0x%lx%.0ld", cur_fb, tex, attach, 0);
	real(target, attach, textarget, tex, level);
	check_error("glFramebufferTexture2D");
}

GLenum glCheckFramebufferStatus(GLenum target)
{
	REAL(GLenum, glCheckFramebufferStatus, (GLenum));
	GLenum s = real(target);
	if (s != GL_FRAMEBUFFER_COMPLETE) {
		incomplete++;
		fprintf(logf, "  t=%.2f INCOMPLETE fb=%u status=0x%04x\n", now() - t0, cur_fb, s);
	}
	return s;
}

void glGenRenderbuffers(GLsizei n, GLuint *ids)
{
	REAL(void, glGenRenderbuffers, (GLsizei, GLuint *));
	real(n, ids);
	rb_gen += n;
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
	REAL(void, glDeleteRenderbuffers, (GLsizei, const GLuint *));
	rb_del += n;
	real(n, ids);
}

void glRenderbufferStorage(GLenum target, GLenum fmt, GLsizei w, GLsizei h)
{
	REAL(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei));
	rbstorage++;
	event("rbStorage fmt=0x%lx %ldx%ld%.0ld", fmt, w, h, 0);
	real(target, fmt, w, h);
	check_error("glRenderbufferStorage");
}

void glGenTextures(GLsizei n, GLuint *ids)
{
	REAL(void, glGenTextures, (GLsizei, GLuint *));
	real(n, ids);
	tex_gen += n;
	event("genTex id=%ld (n=%ld)%.0ld%.0ld", n > 0 ? ids[0] : 0, n, 0, 0);
}

void glDeleteTextures(GLsizei n, const GLuint *ids)
{
	REAL(void, glDeleteTextures, (GLsizei, const GLuint *));
	event("delTex id=%ld (n=%ld)%.0ld%.0ld", n > 0 ? ids[0] : 0, n, 0, 0);
	tex_del += n;
	real(n, ids);
}

void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
		  GLint border, GLenum fmt, GLenum type, const void *data)
{
	REAL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
	teximg++;
	event("texImage %ldx%ld ifmt=0x%lx data=%ld", w, h, ifmt, data != NULL);
	if (null_teximage)
		data = NULL;
	real(target, level, ifmt, w, h, border, fmt, type, data);
	check_error("glTexImage2D");
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	REAL(void, glDrawArrays, (GLenum, GLint, GLsizei));
	if (cur_fb) {
		draws_fbo++;
		event("  draw fb=%ld mode=%ld count=%ld%.0ld", cur_fb, mode, count, 0);
	} else
		draws_screen++;
	real(mode, first, count);
}
