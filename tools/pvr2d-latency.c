#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pvr2d.h"

#define WIDTH 1024
#define HEIGHT 600
#define BYTES_PER_PIXEL 4
#define ITERATIONS 120

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }

    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void check(PVR2DERROR error, const char *operation)
{
    if (error != PVR2D_OK) {
        fprintf(stderr, "FAIL: %s returned %d\n", operation, error);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    PVR2DDEVICEINFO devices[8];
    PVR2DCONTEXTHANDLE context = NULL;
    PVR2DMEMINFO *buffers[2] = { NULL, NULL };
    PVR2DBLTINFO blit;
    const unsigned long stride = WIDTH * BYTES_PER_PIXEL;
    const unsigned long bytes = stride * HEIGHT;
    int device_count;
    int frame;
    int iterations = argc > 1 ? atoi(argv[1]) : ITERATIONS;

    if (iterations <= 0) {
        fprintf(stderr, "FAIL: iterations must be positive\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    device_count = PVR2DEnumerateDevices(NULL);
    if (device_count <= 0 || device_count > (int)(sizeof(devices) / sizeof(devices[0]))) {
        fprintf(stderr, "FAIL: PVR2DEnumerateDevices returned %d\n", device_count);
        return 1;
    }

    if (PVR2DEnumerateDevices(devices) != PVR2D_OK) {
        fprintf(stderr, "FAIL: device enumeration failed\n");
        return 1;
    }

    printf("device=%lu name=%s dimensions=%dx%d bytes=%lu iterations=%d\n",
           devices[0].ulDevID, devices[0].szDeviceName,
            WIDTH, HEIGHT, bytes, iterations);

    check(PVR2DCreateDeviceContext(devices[0].ulDevID, &context, 0),
          "PVR2DCreateDeviceContext");
    check(PVR2DMemAlloc(context, bytes, 4096, 0, &buffers[0]),
          "PVR2DMemAlloc(buffer 0)");
    check(PVR2DMemAlloc(context, bytes, 4096, 0, &buffers[1]),
          "PVR2DMemAlloc(buffer 1)");

    printf("buffer0_dev=0x%08lx buffer1_dev=0x%08lx\n",
           buffers[0]->ui32DevAddr, buffers[1]->ui32DevAddr);
    puts("frame,buffer,submit_us,wait_us,total_us");

    memset(&blit, 0, sizeof(blit));
    blit.CopyCode = PVR2DPATROPcopy;
    blit.BlitFlags = PVR2D_BLIT_DISABLE_ALL;
    blit.DstStride = stride;
    blit.DstX = 0;
    blit.DstY = 0;
    blit.DSizeX = WIDTH;
    blit.DSizeY = HEIGHT;
    blit.DstFormat = PVR2D_ARGB8888;
    blit.DstSurfWidth = WIDTH;
    blit.DstSurfHeight = HEIGHT;
    blit.SrcX = 0;
    blit.SrcY = 0;
    blit.SizeX = WIDTH;
    blit.SizeY = HEIGHT;

    for (frame = 0; frame < iterations; frame++) {
        const int index = frame & 1;
        uint64_t start_ns;
        uint64_t submitted_ns;
        uint64_t completed_ns;

        blit.pDstMemInfo = buffers[index];
        blit.Colour = index ? 0xff204080UL : 0xff804020UL;

        start_ns = monotonic_ns();
        check(PVR2DBlt(context, &blit), "PVR2DBlt");
        submitted_ns = monotonic_ns();
        check(PVR2DQueryBlitsComplete(context, buffers[index], 1),
              "PVR2DQueryBlitsComplete");
        completed_ns = monotonic_ns();

        printf("%d,%d,%llu,%llu,%llu\n",
               frame, index,
               (unsigned long long)((submitted_ns - start_ns) / 1000ULL),
               (unsigned long long)((completed_ns - submitted_ns) / 1000ULL),
               (unsigned long long)((completed_ns - start_ns) / 1000ULL));
    }

    check(PVR2DMemFree(context, buffers[1]), "PVR2DMemFree(buffer 1)");
    check(PVR2DMemFree(context, buffers[0]), "PVR2DMemFree(buffer 0)");
    check(PVR2DDestroyDeviceContext(context), "PVR2DDestroyDeviceContext");
    return 0;
}
