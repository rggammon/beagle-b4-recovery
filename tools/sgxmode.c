/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sgxmode - Stage 5a transparent presenter (mailbox), hard-float, raw DRM ioctls.
 *
 * Owns the display (DRM master) and flips an UNMODIFIED GLES app onto the panel:
 * it imports every dc_nohw back buffer as a framebuffer once, subscribes to the
 * dc_nohw swap-notify, and PAGE_FLIPs to whichever buffer the app's
 * eglSwapBuffers just completed into. Free-running mailbox: always present the
 * latest completed buffer, coalescing swaps it cannot keep up with. Paces on the
 * KMS flip-done event.
 *
 * The app is launched as a child so its lifetime bounds the presenter. The app
 * still renders through pvrsrvkm/dc_nohw; sgxmode only touches omapdrm/card0.
 *
 * Usage: sgxmode -- <app> [args...]      (e.g. the pandora-armel loader line)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

/* dc_nohw exporter UAPI (mirror of dc_nohw_export.h, ABI v2). */
struct dc_nohw_export_abi {
    uint32_t abi_version, width, height, stride, fourcc, buffer_count, buffer_size, reserved;
};
struct dc_nohw_export_buffer { uint32_t index, flags; int32_t fd; uint32_t reserved; };
struct dc_nohw_export_event { uint32_t type, index, seq, reserved; };
#define DC_NOHW_EXPORT_QUERY_ABI _IOR('D', 1, struct dc_nohw_export_abi)
#define DC_NOHW_EXPORT_BUFFER    _IOWR('D', 2, struct dc_nohw_export_buffer)
#define DC_NOHW_EXPORT_SUBSCRIBE _IO('D', 3)
#define DC_NOHW_EVENT_SWAPCHAIN_CREATE  1u
#define DC_NOHW_EVENT_SWAPCHAIN_DESTROY 2u
#define DC_NOHW_EVENT_SWAP              3u

#define MAX_BUFS 8

static int g_card = -1;
static int g_sigpipe[2] = { -1, -1 };

static void sigchld(int sig)
{
    (void)sig;
    if (g_sigpipe[1] >= 0) {
        char c = 1;
        ssize_t n = write(g_sigpipe[1], &c, 1);
        (void)n;
    }
}

static int cardctl(unsigned long req, void *arg, const char *name)
{
    int r = ioctl(g_card, req, arg);
    if (r)
        fprintf(stderr, "FAIL %s: %s\n", name, strerror(errno));
    return r;
}

int main(int argc, char **argv)
{
    struct dc_nohw_export_abi abi;
    struct drm_mode_card_res res;
    uint32_t *conn_ids;
    struct drm_mode_get_connector conn;
    struct drm_mode_modeinfo *modes = NULL;
    struct drm_mode_get_encoder enc;
    struct drm_mode_crtc crtc, saved_crtc;
    struct drm_mode_modeinfo mode;
    uint32_t conn_id = 0, crtc_id = 0;
    uint32_t fb_id[MAX_BUFS];
    uint32_t gem[MAX_BUFS];
    unsigned nbuf = 0, i;
    int ctrl = -1, found = 0, argstart = 1;
    int mode_set = 0, flip_pending = 0, have_new = 0;
    unsigned latest = 0;
    pid_t child;

    /* argv: allow an optional "--" separator before the app command. */
    if (argc > 1 && strcmp(argv[1], "--") == 0)
        argstart = 2;
    if (argc <= argstart) {
        fprintf(stderr, "usage: %s [--] <app> [args...]\n", argv[0]);
        return 2;
    }

    ctrl = open("/dev/dc_nohw_export", O_RDONLY | O_CLOEXEC);
    if (ctrl < 0) { perror("open dc_nohw_export"); return 1; }
    if (ioctl(ctrl, DC_NOHW_EXPORT_SUBSCRIBE)) { perror("SUBSCRIBE"); return 1; }

    memset(&abi, 0, sizeof(abi));
    if (ioctl(ctrl, DC_NOHW_EXPORT_QUERY_ABI, &abi)) { perror("QUERY_ABI"); return 1; }
    nbuf = abi.buffer_count;
    if (nbuf == 0 || nbuf > MAX_BUFS) { fprintf(stderr, "bad buffer_count %u\n", nbuf); return 1; }

    g_card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_card < 0) { perror("open card0"); return 1; }
    if (cardctl(DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER"))
        fprintf(stderr, "warning: not DRM master\n");

    /* Enumerate a connected connector + CRTC + mode. */
    memset(&res, 0, sizeof(res));
    if (cardctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(count)")) return 1;
    if (res.count_connectors == 0) { fprintf(stderr, "no connectors\n"); return 1; }
    conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.fb_id_ptr = 0; res.crtc_id_ptr = 0; res.encoder_id_ptr = 0;
    res.count_fbs = 0; res.count_crtcs = 0; res.count_encoders = 0;
    if (cardctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(fill)")) return 1;

    for (i = 0; i < res.count_connectors && !found; i++) {
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conn_ids[i];
        if (cardctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(count)")) continue;
        if (conn.connection != 1 || conn.count_modes == 0) continue;
        modes = calloc(conn.count_modes, sizeof(*modes));
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.props_ptr = 0; conn.prop_values_ptr = 0; conn.encoders_ptr = 0;
        conn.count_props = 0; conn.count_encoders = 0;
        if (cardctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(fill)")) { free(modes); modes = NULL; continue; }
        conn_id = conn.connector_id;
        mode = modes[0];
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = conn.encoder_id;
        if (enc.encoder_id && cardctl(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER") == 0 && enc.crtc_id)
            crtc_id = enc.crtc_id;
        found = 1;
    }
    if (!found) { fprintf(stderr, "no connected connector\n"); return 1; }
    if (!crtc_id) {
        uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
        struct drm_mode_card_res r2;
        memset(&r2, 0, sizeof(r2));
        r2.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
        r2.count_crtcs = res.count_crtcs;
        if (cardctl(DRM_IOCTL_MODE_GETRESOURCES, &r2, "GETRESOURCES(crtcs)") == 0 && res.count_crtcs)
            crtc_id = crtc_ids[0];
        free(crtc_ids);
    }
    if (!crtc_id) { fprintf(stderr, "no CRTC\n"); return 1; }

    memset(&saved_crtc, 0, sizeof(saved_crtc));
    saved_crtc.crtc_id = crtc_id;
    cardctl(DRM_IOCTL_MODE_GETCRTC, &saved_crtc, "GETCRTC(save)");

    /* Import every dc_nohw back buffer as a framebuffer once. */
    for (i = 0; i < nbuf; i++) {
        struct dc_nohw_export_buffer ereq;
        struct drm_prime_handle prime;
        struct drm_mode_fb_cmd2 fb;

        memset(&ereq, 0, sizeof(ereq));
        ereq.index = i;
        if (ioctl(ctrl, DC_NOHW_EXPORT_BUFFER, &ereq)) { perror("EXPORT_BUFFER"); return 1; }
        memset(&prime, 0, sizeof(prime));
        prime.fd = ereq.fd;
        if (cardctl(DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime, "PRIME_FD_TO_HANDLE")) return 1;
        gem[i] = prime.handle;
        memset(&fb, 0, sizeof(fb));
        fb.width = abi.width; fb.height = abi.height;
        fb.pixel_format = DRM_FORMAT_ARGB8888;
        fb.handles[0] = gem[i];
        fb.pitches[0] = abi.stride;
        if (cardctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2")) return 1;
        fb_id[i] = fb.fb_id;
        close(ereq.fd);
    }
    printf("sgxmode: connector=%u crtc=%u %ux%u nbuf=%u\n",
           conn_id, crtc_id, abi.width, abi.height, nbuf);

    /* SIGCHLD self-pipe so poll() wakes when the app exits. */
    if (pipe(g_sigpipe) == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGCHLD, &sa, NULL);
    }

    child = fork();
    if (child == 0) {
        execvp(argv[argstart], &argv[argstart]);
        perror("execvp");
        _exit(127);
    }
    if (child < 0) { perror("fork"); return 1; }

    for (;;) {
        struct pollfd pfd[3];
        int n;

        pfd[0].fd = ctrl;          pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = g_card;        pfd[1].events = POLLIN; pfd[1].revents = 0;
        pfd[2].fd = g_sigpipe[0];  pfd[2].events = POLLIN; pfd[2].revents = 0;
        n = poll(pfd, 3, 2000);
        if (n < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        /* Child exited? */
        if (pfd[2].revents & POLLIN) {
            char buf[16];
            ssize_t r = read(g_sigpipe[0], buf, sizeof(buf));
            (void)r;
            if (waitpid(child, NULL, WNOHANG) == child)
                break;
        }

        /* Swap events: take the latest completed buffer (mailbox). */
        if (pfd[0].revents & POLLIN) {
            struct dc_nohw_export_event ev[32];
            ssize_t r = read(ctrl, ev, sizeof(ev));
            size_t k, cnt = r > 0 ? (size_t)r / sizeof(ev[0]) : 0;
            for (k = 0; k < cnt; k++) {
                if (ev[k].type == DC_NOHW_EVENT_SWAP && ev[k].index < nbuf) {
                    latest = ev[k].index;
                    have_new = 1;
                }
            }
        }

        /* KMS flip-done: mailbox slot free again. */
        if (pfd[1].revents & POLLIN) {
            char buf[256];
            ssize_t r = read(g_card, buf, sizeof(buf));
            ssize_t off = 0;
            while (off + (ssize_t)sizeof(struct drm_event) <= r) {
                struct drm_event *e = (struct drm_event *)(buf + off);
                if (e->type == DRM_EVENT_FLIP_COMPLETE)
                    flip_pending = 0;
                if (e->length == 0) break;
                off += e->length;
            }
        }

        /* Present the latest completed buffer. */
        if (have_new && !flip_pending) {
            if (!mode_set) {
                memset(&crtc, 0, sizeof(crtc));
                crtc.crtc_id = crtc_id;
                crtc.fb_id = fb_id[latest];
                crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
                crtc.count_connectors = 1;
                crtc.mode = mode;
                crtc.mode_valid = 1;
                if (cardctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC")) break;
                mode_set = 1;
                have_new = 0;
            } else {
                struct drm_mode_crtc_page_flip flip;
                memset(&flip, 0, sizeof(flip));
                flip.crtc_id = crtc_id;
                flip.fb_id = fb_id[latest];
                flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
                if (ioctl(g_card, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
                    flip_pending = 1;
                    have_new = 0;
                } else if (errno == EINVAL || errno == ENOSYS) {
                    /* Fallback: no async flip - just re-set the CRTC. */
                    memset(&crtc, 0, sizeof(crtc));
                    crtc.crtc_id = crtc_id;
                    crtc.fb_id = fb_id[latest];
                    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
                    crtc.count_connectors = 1;
                    crtc.mode = mode;
                    crtc.mode_valid = 1;
                    cardctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC(flip)");
                    have_new = 0;
                }
            }
        }
    }

    /* Teardown: restore the console, drop the framebuffers, release master. */
    if (saved_crtc.mode_valid && saved_crtc.fb_id) {
        saved_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
        saved_crtc.count_connectors = 1;
        cardctl(DRM_IOCTL_MODE_SETCRTC, &saved_crtc, "SETCRTC(restore)");
    }
    for (i = 0; i < nbuf; i++) {
        struct drm_gem_close gc;
        cardctl(DRM_IOCTL_MODE_RMFB, &fb_id[i], "RMFB");
        memset(&gc, 0, sizeof(gc));
        gc.handle = gem[i];
        cardctl(DRM_IOCTL_GEM_CLOSE, &gc, "GEM_CLOSE");
    }
    cardctl(DRM_IOCTL_DROP_MASTER, NULL, "DROP_MASTER");
    close(g_card);
    close(ctrl);
    free(conn_ids);
    free(modes);
    printf("sgxmode: clean exit\n");
    return 0;
}
