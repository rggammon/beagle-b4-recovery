/* gl-replay.c — re-execute a gl-record.c stream with no toolkit and report what reached the screen.
 *
 * Creates the same EGL window context the screensaver does (WINDOW_BIT, ES2, RGBA8888 +
 * STENCIL 8, falling back to 565 + STENCIL 8), replays every recorded call with object ids
 * remapped, then reads back the window (reliable; FBO readback is not) and prints an ASCII
 * coverage map. If the replay drops the same tiles as the screensaver it is a self-contained
 * repro and the recorded stream can be bisected (REPLAY_DROP / REPLAY_STOP).
 *
 * Env: REPLAY_LOG (default /root/glrec.txt), REPLAY_BLOB (default log with .bin),
 *      REPLAY_DROP=OP,OP,...   skip those ops (e.g. STENCILFUNC,STENCILOP)
 *      REPLAY_STOP=n           stop after n ops (bisect the stream)
 *      REPLAY_FINISH=1         glFinish at every BINDFB
 *      REPLAY_FRAMES=n         replay the recorded frames n times (default 1)
 *      REPLAY_DUMP=path        write raw RGBA window readback
 *
 *   arm-linux-gnueabihf-gcc-14 -O2 -I<ddk16 headers> gl-replay.c -L<shim> -lEGL -lGLESv2 -o gl-replay
 *   sgx-ddk14-hf-run ./gl-replay        (Pandora: soft-float build, LD_LIBRARY_PATH=/usr/lib)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef char GLchar;

static unsigned char *blobd;
static long bloblen;

/* chunks under 1 s: usleep() needs < 1e6, and nanosleep() binds to a time64 symbol the Pandora's glibc lacks */
static void sleep_us(long us)
{
	while (us > 0) {
		long c = us > 500000 ? 500000 : us;
		usleep((useconds_t)c);
		us -= c;
	}
}

/* ---- id maps: recorded id -> live id (few hundred objects, linear search is fine) ---- */
#define NK 6
enum { K_TEX, K_BUF, K_FB, K_RB, K_PROG, K_SHADER };
static struct { unsigned from, to; } map[NK][2048];
static int nmap[NK];

static unsigned mget(int k, unsigned from)
{
	if (!from)
		return 0;
	for (int i = 0; i < nmap[k]; i++)
		if (map[k][i].from == from)
			return map[k][i].to;
	return from; /* unknown: pass through */
}
static void mput(int k, unsigned from, unsigned to)
{
	if (nmap[k] < 2048) {
		map[k][nmap[k]].from = from;
		map[k][nmap[k]].to = to;
		nmap[k]++;
	}
}

/* per-program uniform location map (recorded loc -> live loc) */
static struct { unsigned prog; int rec[64], act[64], n; } ul[64];
static int nul;
static unsigned cur_prog_rec;

static int *uloc_slot(unsigned prog, int rec, int create)
{
	int i;
	for (i = 0; i < nul; i++)
		if (ul[i].prog == prog)
			break;
	if (i == nul) {
		if (!create || nul >= 64)
			return NULL;
		ul[nul].prog = prog;
		ul[nul].n = 0;
		nul++;
	}
	for (int j = 0; j < ul[i].n; j++)
		if (ul[i].rec[j] == rec)
			return &ul[i].act[j];
	if (!create || ul[i].n >= 64)
		return NULL;
	ul[i].rec[ul[i].n] = rec;
	return &ul[i].act[ul[i].n++];
}
static int uloc(int rec)
{
	int *s = uloc_slot(cur_prog_rec, rec, 0);
	return s ? *s : rec;
}

static float f32(unsigned u)
{
	union { unsigned u; float f; } x;
	x.u = u;
	return x.f;
}

/* lit pixels of the bound (default) framebuffer, coarse grid sample */
static long lit_now(int sw, int sh)
{
	unsigned char *px = malloc((size_t)sw * sh * 4);
	long lit = 0;
	glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < sh; y += 4)
		for (int x = 0; x < sw; x += 4) {
			unsigned char *p = px + ((size_t)y * sw + x) * 4;
			if (p[0] > 60 || p[1] > 60 || p[2] > 60)
				lit++;
		}
	free(px);
	return lit;
}

static const void *blob_at(long off)
{
	return (off >= 0 && off < bloblen) ? blobd + off : NULL;
}

static int env_int(const char *n, int d)
{
	const char *s = getenv(n);
	int v = 0;
	if (!s || !*s)
		return d;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

static int dropped(const char *drop, const char *op)
{
	size_t n = strlen(op);
	const char *p = drop;
	while (p && *p) {
		if (!strncmp(p, op, n) && (p[n] == ',' || p[n] == 0))
			return 1;
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return 0;
}

static unsigned parse_ids(char *rest, unsigned *out, int max)
{
	int n = 0;
	char *tok = strtok(rest, " \n");
	unsigned cnt = tok ? (unsigned)strtoul(tok, NULL, 10) : 0;
	while ((tok = strtok(NULL, " \n")) && n < max)
		out[n++] = (unsigned)strtoul(tok, NULL, 10);
	return cnt < (unsigned)n ? cnt : (unsigned)n;
}

int main(void)
{
	const char *lp = getenv("REPLAY_LOG") ? getenv("REPLAY_LOG") : "/root/glrec.txt";
	char bp[512];
	const char *drop = getenv("REPLAY_DROP");
	long stop = env_int("REPLAY_STOP", 0), nops = 0;
	int finish_on_bind = env_int("REPLAY_FINISH", 0), reps = env_int("REPLAY_FRAMES", 1);
	int read_each = env_int("REPLAY_READ", 0), nframe = 0;
	/* REPLAY_SYNC: op applied to the FBO being left on every BINDFB: 1 glFinish, 2 glFlush,
	 * 4 1-px glReadPixels, 5 finish+readpixels, 6 flush+finish+readpixels(centre) */
	int sync_mode = env_int("REPLAY_SYNC", 0);
	unsigned cur_live_fb = 0;
	/* Chromium-bug-list probes: TEXNULL = NULL initial texture data (#494); CLIENTVA = client-side vertex
	 * arrays from a CPU shadow of the array buffer (#1); REATTACH = re-attach the colour texture on every FBO bind */
	int texnull = env_int("REPLAY_TEXNULL", 0), clientva = env_int("REPLAY_CLIENTVA", 0), reattach = env_int("REPLAY_REATTACH", 0);
	unsigned cur_abuf = 0, cur_fb_rec = 0;
	const char *shadow = NULL;
	struct { unsigned a, b, c, d; int lvl; } ftex[64];
	memset(ftex, 0, sizeof(ftex));
	/* REPLAY_DELAY_US sleeps after every op named in REPLAY_DELAY_OPS (default DRAW);
	 * REPLAY_FRAME_US sleeps after every swap (REPLAY_FRAME_US=1000000 = 1 fps) */
	long delay_us = env_int("REPLAY_DELAY_US", 0), frame_delay_us = env_int("REPLAY_FRAME_US", 0);
	const char *delay_ops = getenv("REPLAY_DELAY_OPS") ? getenv("REPLAY_DELAY_OPS") : "DRAW";
	FILE *tf, *bf;
	char *line = NULL;
	size_t cap = 0;
	long ndraw = 0, nclientptr = 0;

	if (getenv("REPLAY_BLOB"))
		snprintf(bp, sizeof(bp), "%s", getenv("REPLAY_BLOB"));
	else {
		size_t n = strlen(lp);
		snprintf(bp, sizeof(bp), "%.*s.bin", (int)(n > 4 && !strcmp(lp + n - 4, ".txt") ? n - 4 : n), lp);
	}
	tf = fopen(lp, "r");
	bf = fopen(bp, "rb");
	if (!tf || !bf) {
		fprintf(stderr, "FAIL: cannot open %s / %s\n", lp, bp);
		return 1;
	}
	fseek(bf, 0, SEEK_END);
	bloblen = ftell(bf);
	fseek(bf, 0, SEEK_SET);
	blobd = malloc(bloblen ? bloblen : 1);
	if (fread(blobd, 1, bloblen, bf) != (size_t)bloblen) {
		fprintf(stderr, "FAIL: short blob read\n");
		return 1;
	}

	/* --- EGL, as the screensaver does --- */
	setvbuf(stdout, NULL, _IONBF, 0);
	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLConfig cfg;
	EGLint n = 0, sw = 0, sh = 0, st = -1;
	const EGLint a8888[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_STENCIL_SIZE, 8, EGL_NONE };
	const EGLint a565[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_STENCIL_SIZE, 8, EGL_NONE };
	const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	if (!eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "FAIL: eglInitialize\n");
		return 1;
	}
	if ((!eglChooseConfig(dpy, a8888, &cfg, 1, &n) || n < 1) && (!eglChooseConfig(dpy, a565, &cfg, 1, &n) || n < 1)) {
		fprintf(stderr, "FAIL: no EGL config\n");
		return 1;
	}
	eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &st);
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "FAIL: context/surface 0x%x\n", eglGetError());
		return 1;
	}
	eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
	eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
	printf("replay: window %dx%d config stencil=%d renderer=%s\n", sw, sh, st, glGetString(GL_RENDERER));

	for (int rep = 0; rep < reps; rep++) {
		rewind(tf);
		nops = 0;
		int first_frame = 1;
		while (getline(&line, &cap, tf) > 0) {
			char op[24];
			char *rest;
			unsigned a, b, c, d, e;
			int i1, i2, i3, i4, i5, i6, i7, i8;
			long l1, l2;
			char name[256];

			if (sscanf(line, "%23s", op) != 1)
				continue;
			rest = line + strlen(op);
			if (!strcmp(op, "END"))
				break;
			if (stop && nops >= stop)
				break;
			nops++;
			if (!strcmp(op, "FRAME")) {
				if (!first_frame) {
					if (read_each) {
						glBindFramebuffer(GL_FRAMEBUFFER, 0);
						printf("  frame %d done at op %ld: lit(grid)=%ld\n", nframe, nops, lit_now(sw, sh));
					}
					eglSwapBuffers(dpy, surf);
					if (frame_delay_us)
						sleep_us(frame_delay_us);
				}
				first_frame = 0;
				nframe++;
				continue;
			}
			if (drop && dropped(drop, op))
				continue;

#define IDS(kind, genfn, mapped) do { \
	unsigned ids[64], live[64]; \
	unsigned cnt = parse_ids(rest, ids, 64); \
	genfn(cnt, live); \
	for (unsigned q = 0; q < cnt; q++) mput(kind, ids[q], live[q]); \
	(void)mapped; } while (0)
#define DELIDS(kind, delfn) do { \
	unsigned ids[64], live[64]; \
	unsigned cnt = parse_ids(rest, ids, 64); \
	for (unsigned q = 0; q < cnt; q++) live[q] = mget(kind, ids[q]); \
	delfn(cnt, live); } while (0)

			if (!strcmp(op, "GENTEX")) IDS(K_TEX, glGenTextures, 0);
			else if (!strcmp(op, "THRASH")) {
				/* THRASH <KiB>: write then read that much memory to evict every dirty CPU cache line (L1 32K, L2 256K) */
				static volatile unsigned char *tb;
				static long tbn;
				long kb = 0;
				sscanf(rest, "%ld", &kb);
				if (kb * 1024 > tbn) {
					tb = realloc((void *)tb, kb * 1024);
					tbn = kb * 1024;
				}
				for (long q = 0; q < kb * 1024; q += 32)
					tb[q] = (unsigned char)q;
				for (long q = 0; q < kb * 1024; q += 32)
					(void)tb[q];
			}
			else if (!strcmp(op, "GENBUF")) IDS(K_BUF, glGenBuffers, 0);
			else if (!strcmp(op, "GENFB")) IDS(K_FB, glGenFramebuffers, 0);
			else if (!strcmp(op, "GENRB")) IDS(K_RB, glGenRenderbuffers, 0);
			else if (!strcmp(op, "DELTEX")) DELIDS(K_TEX, glDeleteTextures);
			else if (!strcmp(op, "DELBUF")) DELIDS(K_BUF, glDeleteBuffers);
			else if (!strcmp(op, "DELFB")) DELIDS(K_FB, glDeleteFramebuffers);
			else if (!strcmp(op, "DELRB")) DELIDS(K_RB, glDeleteRenderbuffers);
			else if (!strcmp(op, "CREATEPROG")) { sscanf(rest, "%u", &a); mput(K_PROG, a, glCreateProgram()); }
			else if (!strcmp(op, "CREATESHADER")) { sscanf(rest, "%u %u", &a, &b); mput(K_SHADER, b, glCreateShader(a)); }
			else if (!strcmp(op, "DELPROG")) { sscanf(rest, "%u", &a); glDeleteProgram(mget(K_PROG, a)); }
			else if (!strcmp(op, "DELSHADER")) { sscanf(rest, "%u", &a); glDeleteShader(mget(K_SHADER, a)); }
			else if (!strcmp(op, "ATTACH")) { sscanf(rest, "%u %u", &a, &b); glAttachShader(mget(K_PROG, a), mget(K_SHADER, b)); }
			else if (!strcmp(op, "DETACH")) { sscanf(rest, "%u %u", &a, &b); glDetachShader(mget(K_PROG, a), mget(K_SHADER, b)); }
			else if (!strcmp(op, "COMPILE")) { sscanf(rest, "%u", &a); glCompileShader(mget(K_SHADER, a)); }
			else if (!strcmp(op, "LINK")) { sscanf(rest, "%u", &a); glLinkProgram(mget(K_PROG, a)); }
			else if (!strcmp(op, "BINDATTR")) { sscanf(rest, "%u %u %255s", &a, &b, name); glBindAttribLocation(mget(K_PROG, a), b, name); }
			else if (!strcmp(op, "USEPROG")) { sscanf(rest, "%u", &a); cur_prog_rec = a; glUseProgram(mget(K_PROG, a)); }
			else if (!strcmp(op, "GETUNI")) {
				sscanf(rest, "%u %255s %d", &a, name, &i1);
				int live = glGetUniformLocation(mget(K_PROG, a), name);
				int *s = uloc_slot(a, i1, 1);
				if (s)
					*s = live;
			}
			else if (!strcmp(op, "SHADERSRC")) {
				sscanf(rest, "%u %ld %ld", &a, &l1, &l2);
				const char *src = blob_at(l1);
				GLint len = (GLint)l2;
				if (src)
					glShaderSource(mget(K_SHADER, a), 1, &src, &len);
			}
			else if (!strcmp(op, "BINDBUF")) { sscanf(rest, "%u %u", &a, &b); if (a == 34962) cur_abuf = b; glBindBuffer(a, mget(K_BUF, b)); }
			else if (!strcmp(op, "BUFDATA")) {
				sscanf(rest, "%u %ld %ld %u", &a, &l1, &l2, &b);
				glBufferData(a, l1, blob_at(l2), b);
				if (a == 34962) {
					char *sh = calloc(1, l1 + 16);
					if (blob_at(l2))
						memcpy(sh, blob_at(l2), l1);
					shadow = sh;
				}
			}
			else if (!strcmp(op, "BUFSUB")) { long l3; sscanf(rest, "%u %ld %ld %ld", &a, &l1, &l2, &l3); glBufferSubData(a, l1, l2, blob_at(l3)); }
			else if (!strcmp(op, "VAP")) {
				unsigned long ptr;
				sscanf(rest, "%u %d %u %u %d %lu", &a, &i1, &b, &c, &i2, &ptr);
				if (clientva && cur_abuf && shadow) {
					glBindBuffer(34962, 0);
					glVertexAttribPointer(a, i1, b, (GLboolean)c, i2, shadow + ptr);
					glBindBuffer(34962, mget(K_BUF, cur_abuf));
				} else
					glVertexAttribPointer(a, i1, b, (GLboolean)c, i2, (const void *)ptr);
			}
			else if (!strcmp(op, "ENVA")) { sscanf(rest, "%u", &a); glEnableVertexAttribArray(a); }
			else if (!strcmp(op, "DISVA")) { sscanf(rest, "%u", &a); glDisableVertexAttribArray(a); }
			else if (!strcmp(op, "UNI1I")) { sscanf(rest, "%d %d", &i1, &i2); glUniform1i(uloc(i1), i2); }
			else if (!strcmp(op, "UNI1F")) { sscanf(rest, "%d %x", &i1, &a); glUniform1f(uloc(i1), f32(a)); }
			else if (!strncmp(op, "UNI", 3) && op[4] == 'F' && op[5] == 'V') {
				int comp = op[3] - '0', cnt;
				float v[64];
				char *p = rest;
				i1 = (int)strtol(p, &p, 10);
				cnt = (int)strtol(p, &p, 10);
				for (int q = 0; q < cnt * comp && q < 64; q++)
					v[q] = f32((unsigned)strtoul(p, &p, 16));
				if (comp == 2) glUniform2fv(uloc(i1), cnt, v);
				else if (comp == 3) glUniform3fv(uloc(i1), cnt, v);
				else glUniform4fv(uloc(i1), cnt, v);
			}
			else if (!strcmp(op, "ACTIVETEX")) { sscanf(rest, "%u", &a); glActiveTexture(a); }
			else if (!strcmp(op, "BINDTEX")) { sscanf(rest, "%u %u", &a, &b); glBindTexture(a, mget(K_TEX, b)); }
			else if (!strcmp(op, "TEXPARAM")) { sscanf(rest, "%u %u %d", &a, &b, &i1); glTexParameteri(a, b, i1); }
			else if (!strcmp(op, "PIXSTORE")) { sscanf(rest, "%u %d", &a, &i1); glPixelStorei(a, i1); }
			else if (!strcmp(op, "TEXIMG")) {
				sscanf(rest, "%u %d %d %d %d %d %u %u %ld %d", &a, &i1, &i2, &i3, &i4, &i5, &b, &c, &l1, &i6);
				glTexImage2D(a, i1, i2, i3, i4, i5, b, c, texnull ? NULL : blob_at(l1));
			}
			else if (!strcmp(op, "TEXSUB")) {
				sscanf(rest, "%u %d %d %d %d %d %u %u %ld %d", &a, &i1, &i2, &i3, &i4, &i5, &b, &c, &l1, &i6);
				glTexSubImage2D(a, i1, i2, i3, i4, i5, b, c, blob_at(l1));
			}
			else if (!strcmp(op, "BINDFB")) {
				sscanf(rest, "%u %u", &a, &b);
				if (finish_on_bind)
					glFinish();
				if (sync_mode && cur_live_fb && mget(K_FB, b) != cur_live_fb) {
					unsigned char one[4];
					/* sync the render target we are about to leave */
					if (sync_mode == 1 || sync_mode == 5)
						glFinish();
					if (sync_mode == 2)
						glFlush();
					if (sync_mode == 4 || sync_mode == 5)
						glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
					if (sync_mode == 6) {
						glFlush();
						glFinish();
						glReadPixels(75, 48, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
					}
				}
				cur_live_fb = mget(K_FB, b);
				cur_fb_rec = b;
				glBindFramebuffer(a, cur_live_fb);
				if (reattach && b && b % 70001 == 0 && b / 70001 < 64 && ftex[b / 70001].d)
					glFramebufferTexture2D(ftex[b / 70001].a, ftex[b / 70001].b, ftex[b / 70001].c, mget(K_TEX, ftex[b / 70001].d), ftex[b / 70001].lvl);
			}
			else if (!strcmp(op, "BINDRB")) { sscanf(rest, "%u %u", &a, &b); glBindRenderbuffer(a, mget(K_RB, b)); }
			else if (!strcmp(op, "RBSTORE")) { sscanf(rest, "%u %u %d %d", &a, &b, &i1, &i2); glRenderbufferStorage(a, b, i1, i2); }
			else if (!strcmp(op, "FBTEX")) {
				sscanf(rest, "%u %u %u %u %d", &a, &b, &c, &d, &i1);
				glFramebufferTexture2D(a, b, c, mget(K_TEX, d), i1);
				if (cur_fb_rec % 70001 == 0 && cur_fb_rec / 70001 < 64) {
					int k = cur_fb_rec / 70001;
					ftex[k].a = a; ftex[k].b = b; ftex[k].c = c; ftex[k].d = d; ftex[k].lvl = i1;
				}
			}
			else if (!strcmp(op, "FBRB")) { sscanf(rest, "%u %u %u %u", &a, &b, &c, &d); glFramebufferRenderbuffer(a, b, c, mget(K_RB, d)); }
			else if (!strcmp(op, "CHECKFB")) {
				sscanf(rest, "%u", &a);
				GLenum s = glCheckFramebufferStatus(a);
				if (s != GL_FRAMEBUFFER_COMPLETE)
					printf("  op %ld: CHECKFB status 0x%x (INCOMPLETE)\n", nops, s);
			}
			else if (!strcmp(op, "VIEWPORT")) { sscanf(rest, "%d %d %d %d", &i1, &i2, &i3, &i4); glViewport(i1, i2, i3, i4); }
			else if (!strcmp(op, "SCISSOR")) { sscanf(rest, "%d %d %d %d", &i1, &i2, &i3, &i4); glScissor(i1, i2, i3, i4); }
			else if (!strcmp(op, "ENABLE")) { sscanf(rest, "%u", &a); glEnable(a); }
			else if (!strcmp(op, "DISABLE")) { sscanf(rest, "%u", &a); glDisable(a); }
			else if (!strcmp(op, "COLORMASK")) { sscanf(rest, "%u %u %u %u", &a, &b, &c, &d); glColorMask(a, b, c, d); }
			else if (!strcmp(op, "CLEARCOLOR")) { sscanf(rest, "%x %x %x %x", &a, &b, &c, &d); glClearColor(f32(a), f32(b), f32(c), f32(d)); }
			else if (!strcmp(op, "CLEARSTENCIL")) { sscanf(rest, "%d", &i1); glClearStencil(i1); }
			else if (!strcmp(op, "CULL")) { sscanf(rest, "%u", &a); glCullFace(a); }
			else if (!strcmp(op, "FRONTFACE")) { sscanf(rest, "%u", &a); glFrontFace(a); }
			else if (!strcmp(op, "STENCILFUNC")) { sscanf(rest, "%u %d %u", &a, &i1, &b); glStencilFunc(a, i1, b); }
			else if (!strcmp(op, "STENCILMASK")) { sscanf(rest, "%u", &a); glStencilMask(a); }
			else if (!strcmp(op, "STENCILOP")) { sscanf(rest, "%u %u %u", &a, &b, &c); glStencilOp(a, b, c); }
			else if (!strcmp(op, "STENCILOPSEP")) { sscanf(rest, "%u %u %u %u", &a, &b, &c, &d); glStencilOpSeparate(a, b, c, d); }
			else if (!strcmp(op, "BLEND")) { sscanf(rest, "%u %u", &a, &b); glBlendFunc(a, b); }
			else if (!strcmp(op, "BLENDSEP")) { sscanf(rest, "%u %u %u %u", &a, &b, &c, &d); glBlendFuncSeparate(a, b, c, d); }
			else if (!strcmp(op, "CLEAR")) { sscanf(rest, "%u", &a); glClear(a); }
			else if (!strcmp(op, "DRAW")) { sscanf(rest, "%u %d %d", &a, &i1, &i2); glDrawArrays(a, i1, i2); ndraw++; }
			else printf("  op %ld: UNHANDLED %s\n", nops, op);
			if (delay_us && dropped(delay_ops, op))
				sleep_us(delay_us);
			(void)e; (void)i7; (void)i8; (void)nclientptr;
		}
	}

	{
		GLenum err, ne = 0;
		while ((err = glGetError()) != 0 && ne++ < 8)
			printf("  GL error 0x%x\n", err);
	}
	printf("replay: %ld ops, %ld draws\n", nops, ndraw);

	/* --- read back the window and print an ASCII coverage map --- */
	{
		int cols = 64, rows = 20;
		unsigned char *px = malloc((size_t)sw * sh * 4);
		long lit_total = 0;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glFinish();
		glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
		printf("window coverage (top = y high; '#' >=15%% of cell lit, '+' >=2%%, '.' none):\n");
		for (int ry = rows - 1; ry >= 0; ry--) {
			char rowbuf[128];
			for (int cx = 0; cx < cols; cx++) {
				int x0 = cx * sw / cols, x1 = (cx + 1) * sw / cols, y0 = ry * sh / rows, y1 = (ry + 1) * sh / rows;
				long lit = 0, tot = 0;
				for (int y = y0; y < y1; y++)
					for (int x = x0; x < x1; x++) {
						unsigned char *p = px + ((size_t)y * sw + x) * 4;
						tot++;
						if (p[0] > 60 || p[1] > 60 || p[2] > 60)
							lit++;
					}
				lit_total += lit;
				rowbuf[cx] = lit * 100 >= tot * 15 ? '#' : (lit * 100 >= tot * 2 ? '+' : '.');
			}
			rowbuf[cols] = 0;
			printf("  %s\n", rowbuf);
		}
		printf("lit pixels (sampled grid) = %ld\n", lit_total);
		if (getenv("REPLAY_DUMP")) {
			FILE *df = fopen(getenv("REPLAY_DUMP"), "wb");
			if (df) {
				fwrite(px, 1, (size_t)sw * sh * 4, df);
				fclose(df);
			}
		}
	}
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return 0;
}
