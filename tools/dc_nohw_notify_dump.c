/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dc_nohw swap-notify dump (Stage 5 smoke test).
 *
 * Subscribes to /dev/dc_nohw_export and prints swap events
 * (SWAPCHAIN_CREATE/DESTROY and per-swap SWAP{index,seq}) as an *unmodified*
 * GLES app drives eglSwapBuffers. Proves the dc_nohw swap-notify emits from
 * ProcessFlip without any presenter.
 *
 * Usage: dc_nohw_notify_dump [max_events]   (default: run until killed)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* Mirror of dc_nohw_export.h (ABI v2). */
struct dc_nohw_export_event {
    uint32_t type;
    uint32_t index;
    uint32_t seq;
    uint32_t reserved;
};
#define DC_NOHW_EVENT_SWAPCHAIN_CREATE  1u
#define DC_NOHW_EVENT_SWAPCHAIN_DESTROY 2u
#define DC_NOHW_EVENT_SWAP              3u
#define DC_NOHW_EXPORT_SUBSCRIBE        _IO('D', 3)

static const char *type_name(uint32_t t)
{
    switch (t) {
    case DC_NOHW_EVENT_SWAPCHAIN_CREATE:  return "SWAPCHAIN_CREATE";
    case DC_NOHW_EVENT_SWAPCHAIN_DESTROY: return "SWAPCHAIN_DESTROY";
    case DC_NOHW_EVENT_SWAP:              return "SWAP";
    default:                             return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    long max = argc > 1 ? atol(argv[1]) : -1;
    long n = 0;
    int fd;

    fd = open("/dev/dc_nohw_export", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open dc_nohw_export"); return 1; }
    if (ioctl(fd, DC_NOHW_EXPORT_SUBSCRIBE)) { perror("SUBSCRIBE"); return 1; }

    printf("subscribed; waiting for swap events (max=%ld)\n", max);
    fflush(stdout);

    for (;;) {
        struct dc_nohw_export_event ev[16];
        ssize_t r = read(fd, ev, sizeof(ev));
        size_t i, count;

        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return 1;
        }
        count = (size_t)r / sizeof(ev[0]);
        for (i = 0; i < count; i++) {
            if (ev[i].type == DC_NOHW_EVENT_SWAP)
                printf("%s index=%u seq=%u\n", type_name(ev[i].type),
                       ev[i].index, ev[i].seq);
            else
                printf("%s count=%u\n", type_name(ev[i].type), ev[i].index);
            n++;
        }
        fflush(stdout);
        if (max >= 0 && n >= max)
            break;
    }

    close(fd);
    return 0;
}
