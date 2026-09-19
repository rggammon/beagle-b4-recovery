/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kmsmode - set a custom DVI modeline and animate a vertically-scrolling
 * grating to expose a source-vs-panel refresh beat (the "ripple"). Raw DRM
 * UAPI ioctls, libc only (builds hard-float or armel; no libdrm, no DDK).
 *
 *   kmsmode [stock|fix|forced|vbl] [seconds] [static]
 *     stock   vtotal 619 -> 59.81  Hz  (reproduces the current ripple)
 *     fix     vtotal 617 -> 59.995 Hz  (candidate exact-60 timing)
 *     forced  use the connector's currently-forced mode (whatever uEnv set)
 *     vbl     measure the true refresh via WAIT_VBLANK (no animation)
 *     static  hold one grating frame (no flips) to test unmoving-line stability
 *
 * stock/fix share pixel clock 43.833 MHz, htotal 1184, 32-px hsync (within the
 * OMAP3 DISPC <=64-px limit); only two lines of vertical back porch differ, so
 * this is a clean A/B for the 0.19 Hz-under-60 hypothesis.
 *
 * The animation page-flips one buffer per vblank, so the printed fps equals the
 * real scanout refresh. If the panel free-runs, the scrolling grating shows a
 * torn line rolling through about once every 1/|60-fps| seconds; if the panel
 * genlocks to the source, both stock and fix scroll cleanly.
 *
 * Usage: kmsmode [stock|fix|forced] [seconds]   (default: fix 20)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

static int g_fd = -1;
static volatile sig_atomic_t g_stop = 0;

static void on_int(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static int xioctl(unsigned long req, void *arg, const char *name)
{
	int r = ioctl(g_fd, req, arg);
	if (r)
		fprintf(stderr, "FAIL %s: %s\n", name, strerror(errno));
	return r;
}

/* Wide 16-px horizontal bands scrolling STEP px/frame make a horizontal tear
 * seam readable: a tear shows as a horizontal line where the bands jump. NBUF
 * pre-drawn phases are cycled with page flips; NBUF*STEP must equal the band
 * period (2*BAND) for a seamless wrap. */
#define NBUF 8
#define STEP 4
#define BAND 16

/* Vertically-scrolling grating: each row is uniformly white or black, so a
 * per-row byte memset fills it. XRGB8888 white=0xFFFFFFFF (byte 0xFF),
 * black=0x00000000 (byte 0x00). */
static void draw_grating(uint8_t *map, uint32_t pitch, uint32_t h, uint32_t phase)
{
	uint32_t y;
	for (y = 0; y < h; y++) {
		int on = (((y + phase) / BAND) & 1);
		memset(map + (uint64_t)y * pitch, on ? 0xFF : 0x00, pitch);
	}
}

struct fbuf {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
};

static int create_fb(uint32_t w, uint32_t h, struct fbuf *f)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	struct drm_mode_fb_cmd2 fb;
	void *map;

	memset(&creq, 0, sizeof(creq));
	creq.width = w;
	creq.height = h;
	creq.bpp = 32;
	if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &creq, "CREATE_DUMB"))
		return -1;
	f->handle = creq.handle;
	f->pitch = creq.pitch;
	f->size = creq.size;

	memset(&fb, 0, sizeof(fb));
	fb.width = w;
	fb.height = h;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = creq.handle;
	fb.pitches[0] = creq.pitch;
	if (xioctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2"))
		return -1;
	f->fb_id = fb.fb_id;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = creq.handle;
	if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &mreq, "MAP_DUMB"))
		return -1;
	map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, mreq.offset);
	if (map == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	f->map = map;
	return 0;
}

static struct drm_mode_modeinfo make_mode(const char *which)
{
	struct drm_mode_modeinfo m;
	memset(&m, 0, sizeof(m));
	m.clock = 43833;
	m.hdisplay = 1024;
	m.hsync_start = 1072;	/* hfp 48 */
	m.hsync_end = 1104;	/* hsync 32 (<= OMAP3 64) */
	m.htotal = 1184;	/* hbp 80 */
	m.vdisplay = 600;
	m.vsync_start = 603;	/* vfp 3 */
	m.vsync_end = 613;	/* vsync 10 */
	m.vtotal = (strcmp(which, "fix") == 0) ? 617 : 619;
	m.vrefresh = 60;
	m.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;
	m.type = DRM_MODE_TYPE_USERDEF | DRM_MODE_TYPE_DRIVER;
	snprintf(m.name, sizeof(m.name), "1024x600%s", (strcmp(which, "fix") == 0) ? "F" : "S");
	return m;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "fix";
	int seconds = argc > 2 ? atoi(argv[2]) : 20;
	int do_vbl = strcmp(which, "vbl") == 0;
	const char *modesel = do_vbl ? "fix" : which;
	int use_forced = strcmp(which, "forced") == 0;
	int do_static = argc > 3 && strcmp(argv[3], "static") == 0;
	int do_solid = argc > 3 && strcmp(argv[3], "solid") == 0;
	int do_blue = argc > 3 && strcmp(argv[3], "blue") == 0;
	struct drm_mode_card_res res;
	uint32_t *conn_ids;
	struct drm_mode_get_connector conn;
	struct drm_mode_modeinfo *cmodes = NULL;
	struct drm_mode_get_encoder enc;
	struct drm_mode_crtc crtc, saved;
	struct drm_mode_modeinfo mode;
	uint32_t conn_id = 0, crtc_id = 0;
	struct fbuf fb[NBUF];
	unsigned i;
	int found = 0;
	int nbuf;
	uint32_t frames = 0, win_frames = 0;
	double t0, t_win;

	g_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (g_fd < 0) {
		perror("open card0");
		return 1;
	}
	if (xioctl(DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER"))
		fprintf(stderr, "warning: not DRM master; SETCRTC may fail\n");

	memset(&res, 0, sizeof(res));
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(count)"))
		return 1;
	if (!res.count_connectors) {
		fprintf(stderr, "no connectors\n");
		return 1;
	}
	conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	res.fb_id_ptr = res.crtc_id_ptr = res.encoder_id_ptr = 0;
	res.count_fbs = res.count_crtcs = res.count_encoders = 0;
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(fill)"))
		return 1;

	for (i = 0; i < res.count_connectors && !found; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];
		if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(count)"))
			continue;
		if (conn.connection != 1)	/* not connected */
			continue;

		if (use_forced && conn.count_modes) {
			cmodes = calloc(conn.count_modes, sizeof(*cmodes));
			conn.modes_ptr = (uint64_t)(uintptr_t)cmodes;
			conn.props_ptr = conn.prop_values_ptr = conn.encoders_ptr = 0;
			conn.count_props = conn.count_encoders = 0;
			if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(fill)")) {
				free(cmodes);
				cmodes = NULL;
				continue;
			}
		}
		conn_id = conn.connector_id;

		memset(&enc, 0, sizeof(enc));
		enc.encoder_id = conn.encoder_id;
		if (enc.encoder_id &&
		    xioctl(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER") == 0 && enc.crtc_id)
			crtc_id = enc.crtc_id;
		found = 1;
	}
	if (!found) {
		fprintf(stderr, "no connected connector\n");
		return 1;
	}
	if (!crtc_id) {
		uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
		struct drm_mode_card_res r2;
		memset(&r2, 0, sizeof(r2));
		r2.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
		r2.count_crtcs = res.count_crtcs;
		if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &r2, "GETRESOURCES(crtcs)") == 0 &&
		    res.count_crtcs)
			crtc_id = crtc_ids[0];
		free(crtc_ids);
	}
	if (!crtc_id) {
		fprintf(stderr, "no CRTC\n");
		return 1;
	}

	if (use_forced && cmodes)
		mode = cmodes[0];
	else
		mode = make_mode(modesel);

	printf("connector=%u crtc=%u mode=%s clk=%u H:%u %u %u %u V:%u %u %u %u -> %.3f Hz\n",
	       conn_id, crtc_id, mode.name, mode.clock,
	       mode.hdisplay, mode.hsync_start, mode.hsync_end, mode.htotal,
	       mode.vdisplay, mode.vsync_start, mode.vsync_end, mode.vtotal,
	       mode.clock * 1000.0 / ((double)mode.htotal * mode.vtotal));

	memset(&saved, 0, sizeof(saved));
	saved.crtc_id = crtc_id;
	xioctl(DRM_IOCTL_MODE_GETCRTC, &saved, "GETCRTC(save)");

	nbuf = (do_vbl || do_static || do_blue) ? 1 : (do_solid ? 2 : NBUF);
	for (i = 0; i < (unsigned)nbuf; i++) {
		if (create_fb(mode.hdisplay, mode.vdisplay, &fb[i]))
			return 1;
		if (do_blue) {
			uint32_t *px = (uint32_t *)fb[i].map;
			uint64_t k, n = fb[i].size / 4;
			for (k = 0; k < n; k++)
				px[k] = 0x000000FFu;	/* XRGB8888 blue */
		} else if (do_solid) {
			memset(fb[i].map, i ? 0xB0 : 0x50, fb[i].size);
		} else {
			draw_grating(fb[i].map, fb[i].pitch, mode.vdisplay, i * STEP);
		}
	}

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = crtc_id;
	crtc.fb_id = fb[0].fb_id;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (xioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC"))
		return 1;

	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);

	if (do_vbl) {
		union drm_wait_vblank v0, v1;
		double ta, tb;
		memset(&v0, 0, sizeof(v0));
		v0.request.type = _DRM_VBLANK_RELATIVE;
		v0.request.sequence = 0;
		ta = now_s();
		if (xioctl(DRM_IOCTL_WAIT_VBLANK, &v0, "WAIT_VBLANK(q0)"))
			goto teardown;
		sleep(seconds);
		memset(&v1, 0, sizeof(v1));
		v1.request.type = _DRM_VBLANK_RELATIVE;
		v1.request.sequence = 0;
		tb = now_s();
		if (xioctl(DRM_IOCTL_WAIT_VBLANK, &v1, "WAIT_VBLANK(q1)"))
			goto teardown;
		printf("true refresh: %u vblanks in %.2fs = %.4f Hz (mode claims %u/%.3f)\n",
		       v1.reply.sequence - v0.reply.sequence, tb - ta,
		       (v1.reply.sequence - v0.reply.sequence) / (tb - ta),
		       mode.clock, mode.clock * 1000.0 / ((double)mode.htotal * mode.vtotal));
		goto teardown;
	}

	if (do_static) {
		printf("holding STATIC grating on %s for %ds (Ctrl-C to stop)...\n",
		       mode.name, seconds);
		t0 = now_s();
		while (!g_stop && (now_s() - t0) < seconds)
			usleep(100000);
		goto teardown;
	}

	if (do_blue) {
		printf("holding SOLID BLUE on %s for %ds (Ctrl-C to stop)...\n",
		       mode.name, seconds);
		t0 = now_s();
		while (!g_stop && (now_s() - t0) < seconds)
			usleep(100000);
		goto teardown;
	}

	printf("cycling %d pre-drawn phases on %s for %ds (Ctrl-C to stop)...\n",
	       nbuf, mode.name, seconds);

	t0 = t_win = now_s();
	while (!g_stop && (now_s() - t0) < seconds) {
		struct drm_mode_crtc_page_flip flip;
		struct pollfd pfd;
		char buf[256];
		int next = (frames + 1) % nbuf;

		memset(&flip, 0, sizeof(flip));
		flip.crtc_id = crtc_id;
		flip.fb_id = fb[next].fb_id;
		flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
		flip.user_data = next;
		if (xioctl(DRM_IOCTL_MODE_PAGE_FLIP, &flip, "PAGE_FLIP"))
			break;

		pfd.fd = g_fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 1000) <= 0)
			continue;
		if (read(g_fd, buf, sizeof(buf)) < 0)
			break;

		frames++;
		win_frames++;
		if (now_s() - t_win >= 1.0) {
			double dt = now_s() - t_win;
			printf("  %.2f fps\n", win_frames / dt);
			fflush(stdout);
			win_frames = 0;
			t_win = now_s();
		}
	}
	printf("total %u frames in %.1fs = %.3f fps avg\n",
	       frames, now_s() - t0, frames / (now_s() - t0));

teardown:
	if (saved.mode_valid && saved.fb_id) {
		saved.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
		saved.count_connectors = 1;
		xioctl(DRM_IOCTL_MODE_SETCRTC, &saved, "SETCRTC(restore)");
	}
	for (i = 0; i < (unsigned)nbuf; i++) {
		struct drm_mode_destroy_dumb dreq;
		munmap(fb[i].map, fb[i].size);
		xioctl(DRM_IOCTL_MODE_RMFB, &fb[i].fb_id, "RMFB");
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = fb[i].handle;
		xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dreq, "DESTROY_DUMB");
	}
	xioctl(DRM_IOCTL_DROP_MASTER, NULL, "DROP_MASTER");
	close(g_fd);
	free(conn_ids);
	free(cmodes);
	printf("clean exit\n");
	return 0;
}
