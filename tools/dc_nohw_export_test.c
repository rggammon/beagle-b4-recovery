/*
 * Stage 2 exporter smoke test.
 *
 * Opens /dev/dc_nohw_export, queries geometry, exports a swapchain back buffer
 * as a DMA-BUF FD, mmaps it, and reads back the pixels the Stage 1 probe
 * rendered into that buffer. Proves the FD names the real render target.
 *
 * Usage: dc_nohw_export_test [index] [expected_argb_hex]
 *   index             back-buffer index to export (default 0)
 *   expected_argb_hex if given, assert pixel[0] == this value (e.g. 0xff3366cc)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

/* UAPI mirror of dc_nohw_export.h (kept in sync with the driver). */
#define DC_NOHW_EXPORT_ABI_VERSION 1u
#define DC_NOHW_EXPORT_FOURCC_ARGB8888 0x34325241u

struct dc_nohw_export_abi {
    __u32 abi_version;
    __u32 width;
    __u32 height;
    __u32 stride;
    __u32 fourcc;
    __u32 buffer_count;
    __u32 buffer_size;
    __u32 reserved;
};

struct dc_nohw_export_buffer {
    __u32 index;
    __u32 flags;
    __s32 fd;
    __u32 reserved;
};

#define DC_NOHW_EXPORT_IOC_MAGIC 'D'
#define DC_NOHW_EXPORT_QUERY_ABI  _IOR(DC_NOHW_EXPORT_IOC_MAGIC, 1, struct dc_nohw_export_abi)
#define DC_NOHW_EXPORT_BUFFER     _IOWR(DC_NOHW_EXPORT_IOC_MAGIC, 2, struct dc_nohw_export_buffer)

int main(int argc, char **argv)
{
    const char *dev = "/dev/dc_nohw_export";
    unsigned index = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 0;
    int have_expected = argc > 2;
    uint32_t expected = have_expected ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
    struct dc_nohw_export_abi abi;
    struct dc_nohw_export_buffer req;
    int ctrl, dmabuf;
    void *map;
    uint32_t *px;
    unsigned long i, words;
    uint32_t checksum = 0;

    ctrl = open(dev, O_RDWR | O_CLOEXEC);
    if (ctrl < 0) { perror(dev); return 1; }

    memset(&abi, 0, sizeof(abi));
    if (ioctl(ctrl, DC_NOHW_EXPORT_QUERY_ABI, &abi) != 0) {
        perror("QUERY_ABI");
        return 1;
    }
    printf("abi=%u %ux%u stride=%u fourcc=0x%08x buffers=%u size=%u\n",
           abi.abi_version, abi.width, abi.height, abi.stride,
           abi.fourcc, abi.buffer_count, abi.buffer_size);
    if (abi.fourcc != DC_NOHW_EXPORT_FOURCC_ARGB8888)
        fprintf(stderr, "note: unexpected fourcc\n");

    memset(&req, 0, sizeof(req));
    req.index = index;
    if (ioctl(ctrl, DC_NOHW_EXPORT_BUFFER, &req) != 0) {
        perror("EXPORT_BUFFER");
        return 1;
    }
    dmabuf = req.fd;
    printf("exported index=%u dmabuf_fd=%d\n", index, dmabuf);

    map = mmap(NULL, abi.buffer_size, PROT_READ, MAP_SHARED, dmabuf, 0);
    if (map == MAP_FAILED) { perror("mmap(dmabuf)"); return 1; }

    px = (uint32_t *)map;
    words = abi.buffer_size / 4;
    for (i = 0; i < words; i++)
        checksum += px[i];

    uint32_t pixel0 = px[0];
    printf("pixel[0]=0x%08x pixel[mid]=0x%08x checksum=0x%08x\n",
           pixel0, px[words / 2], checksum);
    printf("first16bytes=");
    for (i = 0; i < 16; i++)
        printf("%02x", ((unsigned char *)map)[i]);
    printf("\n");

    /* DC_HOLD=<secs>: keep the mmap + dmabuf fd open (tests the unload guard). */
    if (getenv("DC_HOLD") != NULL) {
        int secs = atoi(getenv("DC_HOLD"));
        fprintf(stderr, "holding dmabuf fd=%d for %ds\n", dmabuf, secs);
        fflush(stderr);
        sleep(secs);
    }

    munmap(map, abi.buffer_size);
    close(dmabuf);
    close(ctrl);

    if (have_expected) {
        if (pixel0 == expected) {
            printf("PASS pixel[0]==0x%08x\n", expected);
            return 0;
        }
        fprintf(stderr, "FAIL pixel[0]=0x%08x expected=0x%08x\n", pixel0, expected);
        return 2;
    }
    return 0;
}
