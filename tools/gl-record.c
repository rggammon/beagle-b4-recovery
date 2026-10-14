/* LD_PRELOAD recorder: logs every GLES2 call the app makes (plus the shader/buffer/texture
 * data they reference) so tools/gl-replay.c can re-execute the stream with no toolkit.
 *
 * Works the way gl-fbo-trace.c does: femtovg resolves gl* through eglGetProcAddress ->
 * shim -> dlsym(RTLD_DEFAULT), which lands on these definitions first. Calls we do not
 * define pass straight to the shim and are NOT recorded; the replay prints per-op counts
 * so gaps show up as a mismatch against gl-fbo-trace's draw totals.
 *
 * Output: GLREC_LOG (default /tmp/glrec.txt) one op per line, GLREC_BLOB (default same
 * path with .bin) raw data blobs referenced by "off len" pairs. Floats are bit-exact hex.
 * Recording stops after GLREC_FRAMES (default 4) frames; a frame starts at each glClear
 * on the default framebuffer.
 *
 *   arm-linux-gnueabihf-gcc -shared -fPIC -O2 -o libgl-record.so tools/gl-record.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef long GLsizeiptr, GLintptr;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

static FILE *tf, *bf;
static long boff, frames, maxframes = 4;
static int rec = 1, unpack_align = 4, client_ptrs;
static GLuint cur_fb, cur_arr;

#define R(...) do { if (rec) fprintf(tf, __VA_ARGS__); } while (0)

static void *sym(const char *name)
{
	void *lib = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_NOLOAD);
	void *p = lib ? dlsym(lib, name) : NULL;
	return p ? p : dlsym(RTLD_NEXT, name);
}

#define REAL(ret, name, args) \
	static ret (*real) args; \
	if (!real) real = (ret (*) args)sym(#name)

static long env_long(const char *n, long d)
{
	const char *s = getenv(n);
	long v = 0;
	if (!s || !*s)
		return d;
	while (*s >= '0' && *s <= '9') /* no atol: glibc 2.38 binds it to __isoc23_strtol */
		v = v * 10 + (*s++ - '0');
	return v;
}

__attribute__((constructor))
static void init(void)
{
	const char *p = getenv("GLREC_LOG");
	const char *b = getenv("GLREC_BLOB");
	char bp[512];

	if (!p)
		p = "/tmp/glrec.txt";
	if (!b) {
		size_t n = strlen(p);
		snprintf(bp, sizeof(bp), "%.*s.bin", (int)(n > 4 && !strcmp(p + n - 4, ".txt") ? n - 4 : n), p);
		b = bp;
	}
	maxframes = env_long("GLREC_FRAMES", 4);
	tf = fopen(p, "w");
	bf = fopen(b, "wb");
	if (!tf || !bf)
		rec = 0;
	else
		setvbuf(tf, NULL, _IOFBF, 1 << 16);
}

static void finish(void)
{
	if (!rec)
		return;
	fprintf(tf, "END\n");
	fflush(tf);
	fflush(bf);
	fprintf(stderr, "gl-record: %ld frames recorded, blob bytes=%ld, client-pointer attribs=%d\n",
		frames, boff, client_ptrs);
	rec = 0;
}

__attribute__((destructor))
static void fini(void)
{
	finish();
}

static long blob(const void *p, long n)
{
	long o;
	if (!rec || !p || n <= 0)
		return -1;
	o = boff;
	fwrite(p, 1, (size_t)n, bf);
	boff += n;
	return o;
}

static unsigned fbits(float f)
{
	union { float f; unsigned u; } x;
	x.f = f;
	return x.u;
}

static int texbytes(GLenum fmt, GLenum type, int w, int h)
{
	int bpp, row;
	if (type == 0x8363 || type == 0x8033 || type == 0x8034)
		bpp = 2;
	else
		switch (fmt) {
		case 0x1908: bpp = 4; break;
		case 0x1907: bpp = 3; break;
		case 0x190A: bpp = 2; break;
		default: bpp = 1;
		}
	row = ((w * bpp + unpack_align - 1) / unpack_align) * unpack_align;
	return row * h;
}

static void unif(const char *op, GLint loc, GLsizei count, int comp, const float *v)
{
	if (!rec)
		return;
	fprintf(tf, "%s %d %d", op, loc, count);
	for (int i = 0; i < count * comp; i++)
		fprintf(tf, " %08x", fbits(v[i]));
	fputc('\n', tf);
}

static void ids(const char *op, GLsizei n, const GLuint *v)
{
	if (!rec)
		return;
	fprintf(tf, "%s %d", op, n);
	for (int i = 0; i < n; i++)
		fprintf(tf, " %u", v[i]);
	fputc('\n', tf);
}

/* ---- objects ---- */
void glGenTextures(GLsizei n, GLuint *v)
{ REAL(void, glGenTextures, (GLsizei, GLuint *)); real(n, v); ids("GENTEX", n, v); }
void glGenBuffers(GLsizei n, GLuint *v)
{ REAL(void, glGenBuffers, (GLsizei, GLuint *)); real(n, v); ids("GENBUF", n, v); }
void glGenFramebuffers(GLsizei n, GLuint *v)
{ REAL(void, glGenFramebuffers, (GLsizei, GLuint *)); real(n, v); ids("GENFB", n, v); }
void glGenRenderbuffers(GLsizei n, GLuint *v)
{ REAL(void, glGenRenderbuffers, (GLsizei, GLuint *)); real(n, v); ids("GENRB", n, v); }
void glDeleteTextures(GLsizei n, const GLuint *v)
{ REAL(void, glDeleteTextures, (GLsizei, const GLuint *)); ids("DELTEX", n, v); real(n, v); }
void glDeleteBuffers(GLsizei n, const GLuint *v)
{ REAL(void, glDeleteBuffers, (GLsizei, const GLuint *)); ids("DELBUF", n, v); real(n, v); }
void glDeleteFramebuffers(GLsizei n, const GLuint *v)
{ REAL(void, glDeleteFramebuffers, (GLsizei, const GLuint *)); ids("DELFB", n, v); real(n, v); }
void glDeleteRenderbuffers(GLsizei n, const GLuint *v)
{ REAL(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)); ids("DELRB", n, v); real(n, v); }

GLuint glCreateProgram(void)
{ REAL(GLuint, glCreateProgram, (void)); GLuint p = real(); R("CREATEPROG %u\n", p); return p; }
GLuint glCreateShader(GLenum t)
{ REAL(GLuint, glCreateShader, (GLenum)); GLuint s = real(t); R("CREATESHADER %u %u\n", t, s); return s; }
void glDeleteProgram(GLuint p)
{ REAL(void, glDeleteProgram, (GLuint)); R("DELPROG %u\n", p); real(p); }
void glDeleteShader(GLuint s)
{ REAL(void, glDeleteShader, (GLuint)); R("DELSHADER %u\n", s); real(s); }
void glAttachShader(GLuint p, GLuint s)
{ REAL(void, glAttachShader, (GLuint, GLuint)); R("ATTACH %u %u\n", p, s); real(p, s); }
void glDetachShader(GLuint p, GLuint s)
{ REAL(void, glDetachShader, (GLuint, GLuint)); R("DETACH %u %u\n", p, s); real(p, s); }
void glCompileShader(GLuint s)
{ REAL(void, glCompileShader, (GLuint)); R("COMPILE %u\n", s); real(s); }
void glLinkProgram(GLuint p)
{ REAL(void, glLinkProgram, (GLuint)); R("LINK %u\n", p); real(p); }
void glBindAttribLocation(GLuint p, GLuint i, const GLchar *n)
{ REAL(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)); R("BINDATTR %u %u %s\n", p, i, n); real(p, i, n); }
void glUseProgram(GLuint p)
{ REAL(void, glUseProgram, (GLuint)); R("USEPROG %u\n", p); real(p); }

GLint glGetUniformLocation(GLuint p, const GLchar *n)
{
	REAL(GLint, glGetUniformLocation, (GLuint, const GLchar *));
	GLint l = real(p, n);
	R("GETUNI %u %s %d\n", p, n, l);
	return l;
}

void glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len)
{
	REAL(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
	if (rec) {
		long total = 0, o;
		char *buf, *w;
		for (int i = 0; i < count; i++)
			total += (len && len[i] >= 0) ? len[i] : (long)strlen(str[i]);
		buf = w = malloc(total + 1);
		for (int i = 0; i < count; i++) {
			long l = (len && len[i] >= 0) ? len[i] : (long)strlen(str[i]);
			memcpy(w, str[i], l);
			w += l;
		}
		o = blob(buf, total);
		fprintf(tf, "SHADERSRC %u %ld %ld\n", s, o, total);
		free(buf);
	}
	real(s, count, str, len);
}

/* ---- buffers / attribs ---- */
void glBindBuffer(GLenum t, GLuint b)
{
	REAL(void, glBindBuffer, (GLenum, GLuint));
	if (t == 0x8892)
		cur_arr = b;
	R("BINDBUF %u %u\n", t, b);
	real(t, b);
}
void glBufferData(GLenum t, GLsizeiptr size, const void *d, GLenum u)
{
	REAL(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum));
	R("BUFDATA %u %ld %ld %u\n", t, (long)size, blob(d, size), u);
	real(t, size, d, u);
}
void glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void *d)
{
	REAL(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *));
	R("BUFSUB %u %ld %ld %ld\n", t, (long)off, (long)size, blob(d, size));
	real(t, off, size, d);
}
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
	REAL(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));
	if (!cur_arr)
		client_ptrs++;
	R("VAP %u %d %u %u %d %lu\n", i, size, type, norm, stride, (unsigned long)ptr);
	real(i, size, type, norm, stride, ptr);
}
void glEnableVertexAttribArray(GLuint i)
{ REAL(void, glEnableVertexAttribArray, (GLuint)); R("ENVA %u\n", i); real(i); }
void glDisableVertexAttribArray(GLuint i)
{ REAL(void, glDisableVertexAttribArray, (GLuint)); R("DISVA %u\n", i); real(i); }

/* ---- uniforms ---- */
void glUniform1i(GLint l, GLint v)
{ REAL(void, glUniform1i, (GLint, GLint)); R("UNI1I %d %d\n", l, v); real(l, v); }
void glUniform1f(GLint l, GLfloat v)
{ REAL(void, glUniform1f, (GLint, GLfloat)); R("UNI1F %d %08x\n", l, fbits(v)); real(l, v); }
void glUniform2fv(GLint l, GLsizei c, const GLfloat *v)
{ REAL(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)); unif("UNI2FV", l, c, 2, v); real(l, c, v); }
void glUniform3fv(GLint l, GLsizei c, const GLfloat *v)
{ REAL(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)); unif("UNI3FV", l, c, 3, v); real(l, c, v); }
void glUniform4fv(GLint l, GLsizei c, const GLfloat *v)
{ REAL(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)); unif("UNI4FV", l, c, 4, v); real(l, c, v); }

/* ---- textures ---- */
void glActiveTexture(GLenum t)
{ REAL(void, glActiveTexture, (GLenum)); R("ACTIVETEX %u\n", t); real(t); }
void glBindTexture(GLenum t, GLuint x)
{ REAL(void, glBindTexture, (GLenum, GLuint)); R("BINDTEX %u %u\n", t, x); real(t, x); }
void glTexParameteri(GLenum t, GLenum p, GLint v)
{ REAL(void, glTexParameteri, (GLenum, GLenum, GLint)); R("TEXPARAM %u %u %d\n", t, p, v); real(t, p, v); }
void glPixelStorei(GLenum p, GLint v)
{
	REAL(void, glPixelStorei, (GLenum, GLint));
	if (p == 0x0CF5)
		unpack_align = v;
	R("PIXSTORE %u %d\n", p, v);
	real(p, v);
}
void glTexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint border,
		  GLenum fmt, GLenum type, const void *px)
{
	REAL(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
	R("TEXIMG %u %d %d %d %d %d %u %u %ld %d\n", t, lvl, ifmt, w, h, border, fmt, type,
	  blob(px, texbytes(fmt, type, w, h)), texbytes(fmt, type, w, h));
	real(t, lvl, ifmt, w, h, border, fmt, type, px);
}
void glTexSubImage2D(GLenum t, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h,
		     GLenum fmt, GLenum type, const void *px)
{
	REAL(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *));
	R("TEXSUB %u %d %d %d %d %d %u %u %ld %d\n", t, lvl, x, y, w, h, fmt, type,
	  blob(px, texbytes(fmt, type, w, h)), texbytes(fmt, type, w, h));
	real(t, lvl, x, y, w, h, fmt, type, px);
}

/* ---- framebuffers ---- */
void glBindFramebuffer(GLenum t, GLuint f)
{ REAL(void, glBindFramebuffer, (GLenum, GLuint)); cur_fb = f; R("BINDFB %u %u\n", t, f); real(t, f); }
void glBindRenderbuffer(GLenum t, GLuint r)
{ REAL(void, glBindRenderbuffer, (GLenum, GLuint)); R("BINDRB %u %u\n", t, r); real(t, r); }
void glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{ REAL(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)); R("RBSTORE %u %u %d %d\n", t, f, w, h); real(t, f, w, h); }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint lvl)
{ REAL(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)); R("FBTEX %u %u %u %u %d\n", t, a, tt, tex, lvl); real(t, a, tt, tex, lvl); }
void glFramebufferRenderbuffer(GLenum t, GLenum a, GLenum rt, GLuint r)
{ REAL(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)); R("FBRB %u %u %u %u\n", t, a, rt, r); real(t, a, rt, r); }
GLenum glCheckFramebufferStatus(GLenum t)
{ REAL(GLenum, glCheckFramebufferStatus, (GLenum)); R("CHECKFB %u\n", t); return real(t); }

/* ---- state ---- */
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{ REAL(void, glViewport, (GLint, GLint, GLsizei, GLsizei)); R("VIEWPORT %d %d %d %d\n", x, y, w, h); real(x, y, w, h); }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{ REAL(void, glScissor, (GLint, GLint, GLsizei, GLsizei)); R("SCISSOR %d %d %d %d\n", x, y, w, h); real(x, y, w, h); }
void glEnable(GLenum c)
{ REAL(void, glEnable, (GLenum)); R("ENABLE %u\n", c); real(c); }
void glDisable(GLenum c)
{ REAL(void, glDisable, (GLenum)); R("DISABLE %u\n", c); real(c); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ REAL(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)); R("COLORMASK %u %u %u %u\n", r, g, b, a); real(r, g, b, a); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ REAL(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)); R("CLEARCOLOR %08x %08x %08x %08x\n", fbits(r), fbits(g), fbits(b), fbits(a)); real(r, g, b, a); }
void glClearStencil(GLint s)
{ REAL(void, glClearStencil, (GLint)); R("CLEARSTENCIL %d\n", s); real(s); }
void glCullFace(GLenum m)
{ REAL(void, glCullFace, (GLenum)); R("CULL %u\n", m); real(m); }
void glFrontFace(GLenum m)
{ REAL(void, glFrontFace, (GLenum)); R("FRONTFACE %u\n", m); real(m); }
void glStencilFunc(GLenum f, GLint ref, GLuint mask)
{ REAL(void, glStencilFunc, (GLenum, GLint, GLuint)); R("STENCILFUNC %u %d %u\n", f, ref, mask); real(f, ref, mask); }
void glStencilMask(GLuint m)
{ REAL(void, glStencilMask, (GLuint)); R("STENCILMASK %u\n", m); real(m); }
void glStencilOp(GLenum a, GLenum b, GLenum c)
{ REAL(void, glStencilOp, (GLenum, GLenum, GLenum)); R("STENCILOP %u %u %u\n", a, b, c); real(a, b, c); }
void glStencilOpSeparate(GLenum face, GLenum a, GLenum b, GLenum c)
{ REAL(void, glStencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)); R("STENCILOPSEP %u %u %u %u\n", face, a, b, c); real(face, a, b, c); }
void glBlendFunc(GLenum s, GLenum d)
{ REAL(void, glBlendFunc, (GLenum, GLenum)); R("BLEND %u %u\n", s, d); real(s, d); }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d)
{ REAL(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)); R("BLENDSEP %u %u %u %u\n", a, b, c, d); real(a, b, c, d); }

void glClear(GLbitfield mask)
{
	REAL(void, glClear, (GLbitfield));
	if (rec && cur_fb == 0) {
		if (frames >= maxframes)
			finish();
		else {
			fprintf(tf, "FRAME\n");
			frames++;
		}
	}
	R("CLEAR %u\n", mask);
	real(mask);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{ REAL(void, glDrawArrays, (GLenum, GLint, GLsizei)); R("DRAW %u %d %d\n", mode, first, count); real(mode, first, count); }
