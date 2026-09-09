/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw KMS presenter (Stage 3) — hard-float, raw DRM UAPI ioctls, no libdrm.
 *
 * The permanent hard-float presenter for the dc_nohw pipeline. Stage 3 fills the
 * scanout buffer with CPU colour bars (no SGX); later stages replace the fill
 * with imported dc_nohw DMA-BUF content (Phase 3B) and renderer IPC (Stage 4).
 *
 * Phase 3A (this file): create a DRM dumb buffer, draw colour bars, and set the
 * mode on the connected DVI-D output to prove the KMS pipeline.
 *
 * Usage: dc_nohw_kms_present [seconds]   (default 5)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

/* dc_nohw exporter UAPI (mirror of dc_nohw_export.h). */
struct dc_nohw_export_abi {
    uint32_t abi_version, width, height, stride, fourcc, buffer_count, buffer_size, reserved;
};
struct dc_nohw_export_buffer { uint32_t index, flags; int32_t fd; uint32_t reserved; };
#define DC_NOHW_EXPORT_QUERY_ABI _IOR('D', 1, struct dc_nohw_export_abi)
#define DC_NOHW_EXPORT_BUFFER    _IOWR('D', 2, struct dc_nohw_export_buffer)

static int g_fd = -1;

static int xioctl(unsigned long req, void *arg, const char *name)
{
    int r = ioctl(g_fd, req, arg);
    if (r)
        fprintf(stderr, "FAIL %s: %s\n", name, strerror(errno));
    return r;
}

/* SMPTE-ish 8-bar palette, XRGB8888. */
static const uint32_t bars[8] = {
    0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
    0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
};

static void draw_bars(uint32_t *px, uint32_t width, uint32_t height, uint32_t pitch)
{
    uint32_t stride_words = pitch / 4;
    uint32_t x, y;

    for (y = 0; y < height; y++) {
        uint32_t *row = px + (uint64_t)y * stride_words;
        for (x = 0; x < width; x++)
            row[x] = bars[x / (width / 8)];
    }
}

int main(int argc, char **argv)
{
    const char *modearg = argc > 1 ? argv[1] : "dumb";
    int do_import = strcmp(modearg, "import") == 0;
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    int ctrl = -1, dmabuf_fd = -1;
    uint32_t gem_handle = 0;
    uint64_t map_size = 0;
    struct drm_mode_card_res res;
    uint32_t *conn_ids;
    struct drm_mode_get_connector conn;
    struct drm_mode_modeinfo *modes = NULL;
    struct drm_mode_get_encoder enc;
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    struct drm_mode_fb_cmd2 fb;
    struct drm_mode_crtc crtc;
    struct drm_mode_crtc saved_crtc;
    struct drm_mode_modeinfo mode;
    uint32_t conn_id = 0, crtc_id = 0;
    uint32_t *fbp;
    void *map;
    unsigned i;
    int found = 0;

    g_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_fd < 0) { perror("open card0"); return 1; }

    if (xioctl(DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER"))
        fprintf(stderr, "warning: not DRM master; SETCRTC may fail\n");

    /* Resources: two-pass for the connector id list. */
    memset(&res, 0, sizeof(res));
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(count)")) return 1;
    if (res.count_connectors == 0) { fprintf(stderr, "no connectors\n"); return 1; }
    conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.fb_id_ptr = 0; res.crtc_id_ptr = 0; res.encoder_id_ptr = 0;
    res.count_fbs = 0; res.count_crtcs = 0; res.count_encoders = 0;
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES(fill)")) return 1;

    /* Find a connected connector with modes. */
    for (i = 0; i < res.count_connectors && !found; i++) {
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conn_ids[i];
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(count)")) continue;
        if (conn.connection != 1 /* connected */ || conn.count_modes == 0)
            continue;

        modes = calloc(conn.count_modes, sizeof(*modes));
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.props_ptr = 0; conn.prop_values_ptr = 0; conn.encoders_ptr = 0;
        conn.count_props = 0; conn.count_encoders = 0;
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR(fill)")) { free(modes); modes = NULL; continue; }

        conn_id = conn.connector_id;
        mode = modes[0]; /* preferred/first mode */

        /* Resolve a CRTC via the connector's current/first encoder. */
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = conn.encoder_id;
        if (enc.encoder_id && xioctl(DRM_IOCTL_MODE_GETENCODER, &enc, "GETENCODER") == 0 && enc.crtc_id)
            crtc_id = enc.crtc_id;
        found = 1;
    }
    if (!found) { fprintf(stderr, "no connected connector with modes\n"); return 1; }

    /* If the encoder had no bound CRTC, take the first CRTC from resources. */
    if (!crtc_id) {
        uint32_t *crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
        struct drm_mode_card_res r2;
        memset(&r2, 0, sizeof(r2));
        r2.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
        r2.count_crtcs = res.count_crtcs;
        if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &r2, "GETRESOURCES(crtcs)") == 0 && res.count_crtcs)
            crtc_id = crtc_ids[0];
        free(crtc_ids);
    }
    if (!crtc_id) { fprintf(stderr, "no CRTC\n"); return 1; }

    printf("connector=%u crtc=%u mode=%s %ux%u@%u\n", conn_id, crtc_id,
           mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh);

    /* Save the current (fbcon) CRTC state so exit can restore the console. */
    memset(&saved_crtc, 0, sizeof(saved_crtc));
    saved_crtc.crtc_id = crtc_id;
    xioctl(DRM_IOCTL_MODE_GETCRTC, &saved_crtc, "GETCRTC(save)");

    if (do_import) {
        struct dc_nohw_export_abi eabi;
        struct dc_nohw_export_buffer ereq;
        struct drm_prime_handle prime;

        ctrl = open("/dev/dc_nohw_export", O_RDWR | O_CLOEXEC);
        if (ctrl < 0) { perror("open dc_nohw_export"); return 1; }
        memset(&eabi, 0, sizeof(eabi));
        if (ioctl(ctrl, DC_NOHW_EXPORT_QUERY_ABI, &eabi)) { perror("QUERY_ABI"); return 1; }
        memset(&ereq, 0, sizeof(ereq));
        if (ioctl(ctrl, DC_NOHW_EXPORT_BUFFER, &ereq)) { perror("EXPORT_BUFFER"); return 1; }
        dmabuf_fd = ereq.fd;

        memset(&prime, 0, sizeof(prime));
        prime.fd = dmabuf_fd;
        if (xioctl(DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime, "PRIME_FD_TO_HANDLE")) return 1;
        gem_handle = prime.handle;

        memset(&fb, 0, sizeof(fb));
        fb.width = eabi.width;
        fb.height = eabi.height;
        fb.pixel_format = DRM_FORMAT_ARGB8888;
        fb.handles[0] = gem_handle;
        fb.pitches[0] = eabi.stride;
        if (xioctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2(import)")) return 1;

        map = mmap(NULL, eabi.buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
        if (map == MAP_FAILED) { perror("mmap dmabuf"); return 1; }
        map_size = eabi.buffer_size;
        draw_bars((uint32_t *)map, eabi.width, eabi.height, eabi.stride);
        printf("import: dmabuf_fd=%d gem=%u %ux%u stride=%u fb_id=%u\n",
               dmabuf_fd, gem_handle, eabi.width, eabi.height, eabi.stride, fb.fb_id);
    } else {
        /* Dumb buffer sized to the mode. */
        memset(&creq, 0, sizeof(creq));
        creq.width = mode.hdisplay;
        creq.height = mode.vdisplay;
        creq.bpp = 32;
        if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &creq, "CREATE_DUMB")) return 1;

        memset(&mreq, 0, sizeof(mreq));
        mreq.handle = creq.handle;
        if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &mreq, "MAP_DUMB")) return 1;

        map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, mreq.offset);
        if (map == MAP_FAILED) { perror("mmap dumb"); return 1; }
        map_size = creq.size;
        fbp = (uint32_t *)map;
        draw_bars(fbp, mode.hdisplay, mode.vdisplay, creq.pitch);

        memset(&fb, 0, sizeof(fb));
        fb.width = mode.hdisplay;
        fb.height = mode.vdisplay;
        fb.pixel_format = DRM_FORMAT_XRGB8888;
        fb.handles[0] = creq.handle;
        fb.pitches[0] = creq.pitch;
        if (xioctl(DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2")) return 1;
        printf("dumb: handle=%u pitch=%u size=%llu fb_id=%u\n",
               creq.handle, creq.pitch, (unsigned long long)creq.size, fb.fb_id);
    }

    /* Set the mode: scan the fb out on the connector's CRTC. */
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = crtc_id;
    crtc.fb_id = fb.fb_id;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
    crtc.count_connectors = 1;
    crtc.mode = mode;
    crtc.mode_valid = 1;
    if (xioctl(DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC")) return 1;
    printf("SETCRTC ok: colour bars on %s for %ds\n", mode.name, seconds);

    sleep(seconds);

    /* Teardown: restore the saved (fbcon) CRTC, drop our fb + buffer, release master. */
    {
        if (saved_crtc.mode_valid && saved_crtc.fb_id) {
            saved_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
            saved_crtc.count_connectors = 1;
            xioctl(DRM_IOCTL_MODE_SETCRTC, &saved_crtc, "SETCRTC(restore)");
        }
        xioctl(DRM_IOCTL_MODE_RMFB, &fb.fb_id, "RMFB");
        munmap(map, map_size);
        if (do_import) {
            struct drm_gem_close gc;
            memset(&gc, 0, sizeof(gc));
            gc.handle = gem_handle;
            xioctl(DRM_IOCTL_GEM_CLOSE, &gc, "GEM_CLOSE");
            if (dmabuf_fd >= 0) close(dmabuf_fd);
            if (ctrl >= 0) close(ctrl);
        } else {
            struct drm_mode_destroy_dumb dreq;
            memset(&dreq, 0, sizeof(dreq));
            dreq.handle = creq.handle;
            xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dreq, "DESTROY_DUMB");
        }
    }
    xioctl(DRM_IOCTL_DROP_MASTER, NULL, "DROP_MASTER");
    close(g_fd);
    free(conn_ids);
    free(modes);
    printf("clean exit\n");
    return 0;
}
