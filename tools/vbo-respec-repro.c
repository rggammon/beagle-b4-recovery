/* vbo-respec-repro.c — minimal repro of the DDK 1.4 "layer FBO renders black" bug.
 *
 * Pattern (from the recorded Slint/femtovg cache-rendering-hint stream, ~1900 ops reduced to this):
 *   one ARRAY_BUFFER, uploaded once with glBufferData(STREAM_DRAW), read by every draw at different offsets
 *   1. fill a CACHE fbo (texture + stencil rb) with a draw from the buffer
 *   2. [optional buffer re-specification, REPRO_RESPEC]
 *   3. draw the cache texture into a LAYER fbo (second draw from the same buffer)
 *   4. next frame: re-specify the buffer with the (smaller) window-quad data, sample the layers to the window
 * Without a respec the layers read black; with certain respecs they render. Window readback only (FBO
 * readback is unreliable on this driver).
 *
 * Env:
 *   REPRO_N=9        tiles
 *   REPRO_RESPEC=0   0 none | 1 small-then-big (different size AND content) | 2 big,big (control)
 *                    3 same size, different content | 4 size+4, same prefix | 5 size-4, same prefix
 *                    6 glBufferData(NULL,size)+SubData(big) (orphan, same size/content)
 *                    7 glBufferSubData(big) in place (same size/content)
 *   REPRO_CLIENT=0   1 = client-side arrays instead of a VBO
 *   REPRO_RB=1       attach a STENCIL_INDEX8 renderbuffer to the offscreen FBOs
 *   REPRO_BIG=28896  size of the big upload in bytes
 *   REPRO_FRAMES=3   composite frames
 *   REPRO_SWAP=1     eglSwapBuffers between the bake and the composite
 *   REPRO_BLEND=0    premultiplied blend (ONE, ONE_MINUS_SRC_ALPHA) as in the recorded stream
 *   REPRO_SCISSOR=0  scissor test around the FBO clears
 *   REPRO_CULL=0     GL_CULL_FACE (back, CCW)
 *   REPRO_SPREAD=0   1 = place each tile's quads 200 vertices apart so draws read deep into the buffer
 *                    (recorded offsets were vertex 18..1800, i.e. bytes 288..28800)
 *   REPRO_FILL2=0    1 = a second (strip) fill draw per cache, like the recorded DRAW 4 / DRAW 5 pair
 *   REPRO_SAMESIZE=0 1 = composite vertex upload padded to the bake buffer size (no size-changing glBufferData)
 *   REPRO_VSUNI=0    1 = vertex shader reads a uniform (value-neutral), like femtovg's viewSize
 *   REPRO_INSET=0    1 = cache fill inset by half a pixel on every edge, like femtovg's fills
 *   REPRO_INTERLEAVE=0  1 = recorded order: window clear first, then per tile create+clear the layer FBO and
 *                    cache FBO lazily inside the bake loop (no up-front creation, bake inside the window frame)
 *
 *   arm-linux-gnueabihf-gcc-14 -O2 -I<ddk16 headers> vbo-respec-repro.c -L<shim> -lEGL -lGLESv2 -o vbo-respec-repro
 *   Pandora: arm-linux-gnueabi-gcc (soft-float), no -D_GNU_SOURCE (glibc 2.38 __isoc23_* symbols)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef char GLchar;

static int env_int(const char *name, int def)
{
	const char *s = getenv(name);
	int v = 0, got = 0;
	if (!s || !*s)
		return def;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s++ - '0');
		got = 1;
	}
	return got ? v : def;
}

static const char *VS =
	"attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
	"void main(){ v = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *VS_UNI =
	"uniform vec2 scale; attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
	"void main(){ v = uv; gl_Position = vec4(pos * scale / scale, 0.0, 1.0); }\n";
static const char *vs_src;
/* glUniform2fv, not glUniform2f: gl-record.c only hooks the vector forms */
static const GLfloat ONE2[2] = { 1.0f, 1.0f };
static const char *FS_FILL =
	"precision mediump float; uniform vec4 col; varying vec2 v;\n"
	"void main(){ gl_FragColor = col; }\n";
static const char *FS_TEX =
	"precision mediump float; uniform sampler2D tex; varying vec2 v;\n"
	"void main(){ gl_FragColor = texture2D(tex, v); }\n";

static GLuint build(const char *fsrc)
{
	GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
	GLint ok = 0;
	glShaderSource(v, 1, &vs_src, NULL);
	glCompileShader(v);
	glShaderSource(f, 1, &fsrc, NULL);
	glCompileShader(f);
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "uv");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "FAIL: shader link\n");
		exit(1);
	}
	return p;
}

#define TW 150
#define TH 96

struct fbo { GLuint tex, fb, rb; };

static void make_fbo(struct fbo *t, int with_rb)
{
	void *zeros = calloc(1, TW * TH * 4);
	glGenTextures(1, &t->tex);
	glBindTexture(GL_TEXTURE_2D, t->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TW, TH, 0, GL_RGBA, GL_UNSIGNED_BYTE, zeros);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(zeros);
	glGenFramebuffers(1, &t->fb);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
	t->rb = 0;
	if (with_rb) {
		glGenRenderbuffers(1, &t->rb);
		glBindRenderbuffer(GL_RENDERBUFFER, t->rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, TW, TH);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->rb);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		printf("  FBO INCOMPLETE\n");
}

/* one vertex = x y u v (16 bytes), two attributes, like the recorded stream */
static void quad(float *v, float x0, float y0, float x1, float y1)
{
	static const float c[6][2] = { {0,0}, {1,0}, {0,1}, {0,1}, {1,0}, {1,1} };
	for (int i = 0; i < 6; i++) {
		v[i * 4 + 0] = c[i][0] ? x1 : x0;
		v[i * 4 + 1] = c[i][1] ? y1 : y0;
		v[i * 4 + 2] = (float)c[i][0];
		v[i * 4 + 3] = (float)c[i][1];
	}
}

static const float PAL[9][4] = {
	{1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}, {1,0,1,1}, {0,1,1,1}, {1,.5f,0,1}, {.5f,0,1,1}, {0,1,.5f,1}
};

static int use_vbo, bigbytes, smallbytes;
static int spread, fill2;
static int fill_at(int lead, int i) { return lead + (spread ? 200 * i : 12 * i); }
static int layer_at(int lead, int i) { return fill_at(lead, i) + (spread ? 176 : 6); }
static GLuint vbo;
static char *bigdata, *smalldata;

static void set_attribs(const char *base)
{
	glBindBuffer(GL_ARRAY_BUFFER, use_vbo ? vbo : 0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, use_vbo ? (const void *)0 : base);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, use_vbo ? (const void *)8 : base + 8);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
}

static void upload(const void *d, int n)
{
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, n, d, GL_STREAM_DRAW);
}

static void respec(int mode)
{
	if (!use_vbo || !mode)
		return;
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	switch (mode) {
	case 1: /* small (composite) data, then the original big data */
		glBufferData(GL_ARRAY_BUFFER, smallbytes, smalldata, GL_STREAM_DRAW);
		glBufferData(GL_ARRAY_BUFFER, bigbytes, bigdata, GL_STREAM_DRAW);
		break;
	case 2: /* control: same size, same content, twice */
		glBufferData(GL_ARRAY_BUFFER, bigbytes, bigdata, GL_STREAM_DRAW);
		glBufferData(GL_ARRAY_BUFFER, bigbytes, bigdata, GL_STREAM_DRAW);
		break;
	case 3: { /* same size, different content (last 16 bytes, never read by a draw) */
		char *t = malloc(bigbytes);
		memcpy(t, bigdata, bigbytes);
		for (int i = bigbytes - 16; i < bigbytes; i++)
			t[i] = (char)0x5a;
		glBufferData(GL_ARRAY_BUFFER, bigbytes, t, GL_STREAM_DRAW);
		free(t);
		break;
	}
	case 4: { /* size+4, same prefix */
		char *t = calloc(1, bigbytes + 4);
		memcpy(t, bigdata, bigbytes);
		glBufferData(GL_ARRAY_BUFFER, bigbytes + 4, t, GL_STREAM_DRAW);
		free(t);
		break;
	}
	case 5: /* size-4, same prefix */
		glBufferData(GL_ARRAY_BUFFER, bigbytes - 4, bigdata, GL_STREAM_DRAW);
		break;
	case 6: /* orphan: same size, then fill with the same content */
		glBufferData(GL_ARRAY_BUFFER, bigbytes, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, bigbytes, bigdata);
		break;
	case 7: /* in place, same content */
		glBufferSubData(GL_ARRAY_BUFFER, 0, bigbytes, bigdata);
		break;
	}
	set_attribs(NULL);
}

int main(void)
{
	int N = env_int("REPRO_N", 9), respec_mode = env_int("REPRO_RESPEC", 0), with_rb = env_int("REPRO_RB", 1);
	int frames = env_int("REPRO_FRAMES", 3), do_swap = env_int("REPRO_SWAP", 1);
	const int lead = 3; /* unused leading vertices, like the recorded buffer */
	int sw = 0, sh = 0, bad = 0;
	EGLConfig cfg;
	EGLint n = 0;
	const EGLint a8888[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_STENCIL_SIZE, 8, EGL_NONE };
	const EGLint a565[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_STENCIL_SIZE, 8, EGL_NONE };
	const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct fbo cache[9], layer[9];

	if (N > 9)
		N = 9;
	use_vbo = !env_int("REPRO_CLIENT", 0);
	spread = env_int("REPRO_SPREAD", 0);
	fill2 = env_int("REPRO_FILL2", 0);
	bigbytes = env_int("REPRO_BIG", 28896);
	smallbytes = (6 * (N + 1)) * 16;
	if (bigbytes < (layer_at(lead, N - 1) + 6) * 16 + 16)
		bigbytes = (layer_at(lead, N - 1) + 6) * 16 + 16;
	if (env_int("REPRO_SAMESIZE", 0))
		smallbytes = bigbytes; /* composite upload no longer changes the buffer size */

	setvbuf(stdout, NULL, _IONBF, 0);
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "FAIL: eglInitialize\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	if ((!eglChooseConfig(dpy, a8888, &cfg, 1, &n) || n < 1) && (!eglChooseConfig(dpy, a565, &cfg, 1, &n) || n < 1)) {
		fprintf(stderr, "FAIL: no EGL config\n");
		return 1;
	}
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "FAIL: context/surface 0x%x\n", eglGetError());
		return 1;
	}
	eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
	printf("repro: window %dx%d N=%d vbo=%d respec=%d rb=%d big=%d small=%d frames=%d swap=%d renderer=%s\n",
	       sw, sh, N, use_vbo, respec_mode, with_rb, bigbytes, smallbytes, frames, do_swap, glGetString(GL_RENDERER));

	vs_src = env_int("REPRO_VSUNI", 0) ? VS_UNI : VS;
	GLuint pfill = build(FS_FILL), ptex = build(FS_TEX);
	GLint ucol = glGetUniformLocation(pfill, "col"), utex = glGetUniformLocation(ptex, "tex");
	GLint sfill = glGetUniformLocation(pfill, "scale"), stex = glGetUniformLocation(ptex, "scale");

	/* vertex data: tile i fill quad at vertex lead+12i, layer quad at lead+12i+6; window quads in the small buffer */
	bigdata = calloc(1, bigbytes);
	smalldata = calloc(1, smallbytes);
	const float ix = env_int("REPRO_INSET", 0) ? 1.0f / TW : 0.0f, iy = env_int("REPRO_INSET", 0) ? 1.0f / TH : 0.0f;
	for (int i = 0; i < N; i++) {
		quad((float *)bigdata + fill_at(lead, i) * 4, -1 + ix, -1 + iy, 1 - ix, 1 - iy);
		quad((float *)bigdata + layer_at(lead, i) * 4, -1, -1, 1, 1);
		int col = i % 3, row = i / 3;
		float px0 = 20 + col * 160.0f, py0 = 20 + row * 106.0f;
		quad((float *)smalldata + 6 * (i + 1) * 4, 2 * px0 / sw - 1, 2 * py0 / sh - 1,
		     2 * (px0 + TW) / sw - 1, 2 * (py0 + TH) / sh - 1);
	}
	if (use_vbo)
		glGenBuffers(1, &vbo);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	if (env_int("REPRO_BLEND", 0)) {
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	if (env_int("REPRO_CULL", 0)) {
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glFrontFace(GL_CCW);
	}
	const int use_scissor = env_int("REPRO_SCISSOR", 0);

	const int interleave = env_int("REPRO_INTERLEAVE", 0);
	if (!interleave) {
		for (int i = 0; i < N; i++) {
			make_fbo(&cache[i], with_rb);
			make_fbo(&layer[i], with_rb);
		}
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, sw, sh);
		glClearColor(0.05f, 0.05f, 0.08f, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}

	/* ---- bake pass ---- */
	if (use_vbo)
		upload(bigdata, bigbytes);
	set_attribs(bigdata);
	for (int i = 0; i < N; i++) {
		if (interleave) {
			make_fbo(&layer[i], with_rb);
			glViewport(0, 0, TW, TH);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			make_fbo(&cache[i], with_rb);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, cache[i].fb);
		glViewport(0, 0, TW, TH);
		glClearColor(0, 0, 0, 0);
		if (use_scissor) {
			glEnable(GL_SCISSOR_TEST);
			glScissor(0, 0, TW, TH);
		}
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		if (use_scissor)
			glDisable(GL_SCISSOR_TEST);
		glUseProgram(pfill);
		glUniform2fv(sfill, 1, ONE2);
		glUniform4fv(ucol, 1, PAL[i]);
		set_attribs(bigdata);
		glDrawArrays(GL_TRIANGLES, fill_at(lead, i), 6);
		if (fill2)
			glDrawArrays(GL_TRIANGLE_STRIP, fill_at(lead, i), 4);

		respec(respec_mode);

		glBindFramebuffer(GL_FRAMEBUFFER, layer[i].fb);
		glViewport(0, 0, TW, TH);
		if (!interleave)
			glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glUseProgram(ptex);
		glUniform2fv(stex, 1, ONE2);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, cache[i].tex);
		glUniform1i(utex, 0);
		set_attribs(bigdata);
		glDrawArrays(GL_TRIANGLES, layer_at(lead, i), 6);
	}

	/* ---- composite frames: window samples the layers; buffer re-specified with the small data ---- */
	for (int f = 0; f < frames; f++) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, sw, sh);
		glClearColor(0.05f, 0.05f, 0.08f, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		if (f == 0 && do_swap) {
			eglSwapBuffers(dpy, surf);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		if (use_vbo)
			upload(smalldata, smallbytes);
		glUseProgram(ptex);
		glUniform2fv(stex, 1, ONE2);
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(utex, 0);
		set_attribs(smalldata);
		for (int i = 0; i < N; i++) {
			glBindTexture(GL_TEXTURE_2D, layer[i].tex);
			glDrawArrays(GL_TRIANGLES, 6 * (i + 1), 6);
		}
		if (f + 1 < frames)
			eglSwapBuffers(dpy, surf);
	}

	for (int i = 0; i < N; i++) {
		unsigned char p[4] = { 0 };
		int col = i % 3, row = i / 3;
		glReadPixels(20 + col * 160 + TW / 2, 20 + row * 106 + TH / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
		if (p[0] < 80 && p[1] < 80 && p[2] < 80)
			bad++;
		printf("  tile %d centre=%02x%02x%02x %s\n", i, p[0], p[1], p[2], (p[0] < 80 && p[1] < 80 && p[2] < 80) ? "BLACK" : "lit");
	}
	printf("RESULT: %d/%d tiles lit%s\n", N - bad, N, bad ? "  => BLACK LAYERS" : "");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return bad ? 2 : 0;
}
