#!/usr/bin/env python
# ctypes port of sgx-pbuffer-latency.c for hosts with no compiler (e.g. the
# native OpenPandora DDK 1.4 stack, glibc 2.9 + Python 2.6). Same EGL/GLES2 FBO
# attachment-churn semantics: create one FBO + two color attachments, then per
# frame re-attach the color target and glClear/glFinish. Detects the device-
# memory leak by watching /proc/meminfo MemFree, and bails before OOM.
#
# args:  frames  alternate  use_fbo  use_texture   (all optional, default 1)
# env:   SGX_REBIND_ATTACHMENT (default 1), SGX_SKIP_RENDER (default 0),
#        SGX_MEMFREE_FLOOR_KB (default 30000; bail if MemFree drops below)
import ctypes
import os
import sys
import time

RTLD_GLOBAL = getattr(ctypes, "RTLD_GLOBAL", 0x100)

# EGL / GLES2 enums (mirror the C probe)
EGL_DEFAULT_DISPLAY = 0
EGL_OPENGL_ES_API = 0x30A0
EGL_SURFACE_TYPE = 0x3033
EGL_PBUFFER_BIT = 0x0001
EGL_RENDERABLE_TYPE = 0x3040
EGL_OPENGL_ES2_BIT = 0x0004
EGL_RED_SIZE = 0x3024
EGL_GREEN_SIZE = 0x3023
EGL_BLUE_SIZE = 0x3022
EGL_ALPHA_SIZE = 0x3021
EGL_WIDTH = 0x3057
EGL_HEIGHT = 0x3056
EGL_NONE = 0x3038
EGL_CONTEXT_CLIENT_VERSION = 0x3098
GL_COLOR_BUFFER_BIT = 0x00004000
GL_RENDERER = 0x1F01
GL_FRAMEBUFFER = 0x8D40
GL_RENDERBUFFER = 0x8D41
GL_COLOR_ATTACHMENT0 = 0x8CE0
GL_RGBA4 = 0x8056
GL_FRAMEBUFFER_COMPLETE = 0x8CD5
GL_TEXTURE_2D = 0x0DE1
GL_RGBA = 0x1908
GL_UNSIGNED_BYTE = 0x1401
GL_TEXTURE_MIN_FILTER = 0x2801
GL_TEXTURE_MAG_FILTER = 0x2800
GL_NEAREST = 0x2600

WIDTH = 1024
HEIGHT = 600

c_int = ctypes.c_int
c_uint = ctypes.c_uint
c_void_p = ctypes.c_void_p
c_float = ctypes.c_float
c_char_p = ctypes.c_char_p
byref = ctypes.byref
POINTER = ctypes.POINTER


def fail(message):
    sys.stderr.write("FAIL: %s\n" % message)
    sys.exit(1)


def memfree_kb():
    try:
        f = open("/proc/meminfo")
        try:
            for line in f:
                if line.startswith("MemFree:"):
                    return int(line.split()[1])
        finally:
            f.close()
    except Exception:
        pass
    return -1


def main():
    argv = sys.argv
    iterations = int(argv[1]) if len(argv) > 1 else 120
    alternate = (len(argv) <= 2) or (int(argv[2]) != 0)
    use_fbo = (len(argv) <= 3) or (int(argv[3]) != 0)
    use_texture = (len(argv) <= 4) or (int(argv[4]) != 0)
    rebind_attachment = os.environ.get("SGX_REBIND_ATTACHMENT") is None or \
        int(os.environ["SGX_REBIND_ATTACHMENT"]) != 0
    skip_render = os.environ.get("SGX_SKIP_RENDER") is not None and \
        int(os.environ["SGX_SKIP_RENDER"]) != 0
    memfree_floor = int(os.environ.get("SGX_MEMFREE_FLOOR_KB", "30000"))

    if iterations <= 0:
        fail("iterations must be positive")

    egl = None
    for name in ("libEGL.so", "libEGL.so.1"):
        try:
            egl = ctypes.CDLL(name, mode=RTLD_GLOBAL)
            break
        except OSError:
            continue
    if egl is None:
        fail("cannot load libEGL")
    gles = None
    for name in ("libGLESv2.so", "libGLESv2.so.2", "libGLESv2.so.1"):
        try:
            gles = ctypes.CDLL(name, mode=RTLD_GLOBAL)
            break
        except OSError:
            continue
    if gles is None:
        fail("cannot load libGLESv2")

    egl.eglGetDisplay.restype = c_void_p
    egl.eglGetDisplay.argtypes = [c_void_p]
    egl.eglInitialize.restype = c_uint
    egl.eglInitialize.argtypes = [c_void_p, POINTER(c_int), POINTER(c_int)]
    egl.eglBindAPI.restype = c_uint
    egl.eglBindAPI.argtypes = [c_uint]
    egl.eglChooseConfig.restype = c_uint
    egl.eglChooseConfig.argtypes = [c_void_p, POINTER(c_int), POINTER(c_void_p),
                                    c_int, POINTER(c_int)]
    egl.eglCreateContext.restype = c_void_p
    egl.eglCreateContext.argtypes = [c_void_p, c_void_p, c_void_p, POINTER(c_int)]
    egl.eglCreatePbufferSurface.restype = c_void_p
    egl.eglCreatePbufferSurface.argtypes = [c_void_p, c_void_p, POINTER(c_int)]
    egl.eglMakeCurrent.restype = c_uint
    egl.eglMakeCurrent.argtypes = [c_void_p, c_void_p, c_void_p, c_void_p]
    egl.eglGetError.restype = c_int
    egl.eglGetError.argtypes = []

    gles.glGetString.restype = c_char_p
    gles.glGetString.argtypes = [c_uint]
    gles.glClearColor.argtypes = [c_float, c_float, c_float, c_float]
    gles.glClear.argtypes = [c_uint]
    gles.glFinish.argtypes = []
    gles.glGenFramebuffers.argtypes = [c_int, POINTER(c_uint)]
    gles.glBindFramebuffer.argtypes = [c_uint, c_uint]
    gles.glGenRenderbuffers.argtypes = [c_int, POINTER(c_uint)]
    gles.glBindRenderbuffer.argtypes = [c_uint, c_uint]
    gles.glRenderbufferStorage.argtypes = [c_uint, c_uint, c_int, c_int]
    gles.glFramebufferRenderbuffer.argtypes = [c_uint, c_uint, c_uint, c_uint]
    gles.glCheckFramebufferStatus.restype = c_uint
    gles.glCheckFramebufferStatus.argtypes = [c_uint]
    gles.glGenTextures.argtypes = [c_int, POINTER(c_uint)]
    gles.glBindTexture.argtypes = [c_uint, c_uint]
    gles.glTexParameteri.argtypes = [c_uint, c_uint, c_int]
    gles.glTexImage2D.argtypes = [c_uint, c_int, c_int, c_int, c_int, c_int,
                                  c_uint, c_uint, c_void_p]
    gles.glFramebufferTexture2D.argtypes = [c_uint, c_uint, c_uint, c_uint, c_int]
    gles.glDeleteFramebuffers.argtypes = [c_int, POINTER(c_uint)]
    gles.glDeleteRenderbuffers.argtypes = [c_int, POINTER(c_uint)]
    gles.glDeleteTextures.argtypes = [c_int, POINTER(c_uint)]

    config_attributes = (c_int * 13)(
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE)
    context_attributes = (c_int * 3)(EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE)
    pbuffer_attributes = (c_int * 5)(EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE)

    display = egl.eglGetDisplay(EGL_DEFAULT_DISPLAY)
    major = c_int(0)
    minor = c_int(0)
    if not display or not egl.eglInitialize(display, byref(major), byref(minor)):
        fail("eglInitialize error=0x%x" % egl.eglGetError())
    if not egl.eglBindAPI(EGL_OPENGL_ES_API):
        fail("eglBindAPI")
    config = c_void_p()
    count = c_int(0)
    if not egl.eglChooseConfig(display, config_attributes, byref(config), 1,
                               byref(count)) or count.value != 1:
        fail("eglChooseConfig")
    context = egl.eglCreateContext(display, config, None, context_attributes)
    if not context:
        fail("eglCreateContext")
    surfaces = [egl.eglCreatePbufferSurface(display, config, pbuffer_attributes),
                egl.eglCreatePbufferSurface(display, config, pbuffer_attributes)]
    if not surfaces[0] or not surfaces[1]:
        fail("eglCreatePbufferSurface error=0x%x" % egl.eglGetError())
    if not egl.eglMakeCurrent(display, surfaces[0], surfaces[0], context):
        fail("initial eglMakeCurrent")

    framebuffer = c_uint(0)
    textures = (c_uint * 2)()
    renderbuffers = (c_uint * 2)()
    if use_fbo:
        gles.glGenFramebuffers(1, byref(framebuffer))
        gles.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer.value)
        if use_texture:
            gles.glGenTextures(2, textures)
        else:
            gles.glGenRenderbuffers(2, renderbuffers)
        for index in range(2):
            if use_texture:
                gles.glBindTexture(GL_TEXTURE_2D, textures[index])
                gles.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
                gles.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
                gles.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0,
                                  GL_RGBA, GL_UNSIGNED_BYTE, None)
                gles.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, textures[index], 0)
            else:
                gles.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffers[index])
                gles.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, WIDTH, HEIGHT)
                gles.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_RENDERBUFFER, renderbuffers[index])
            if gles.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE:
                fail("incomplete framebuffer")

    renderer = gles.glGetString(GL_RENDERER)
    print("egl=%d.%d renderer=%s dimensions=%dx%d iterations=%d alternate=%d "
          "fbo=%d texture=%d rebind_attachment=%d skip_render=%d" % (
              major.value, minor.value, renderer, WIDTH, HEIGHT, iterations,
              int(alternate), int(use_fbo), int(use_texture),
              int(rebind_attachment), int(skip_render)))
    sys.stdout.flush()

    start_free = memfree_kb()
    min_free = start_free
    print("frame,buffer,finish_us,memfree_kb")
    sys.stdout.flush()

    frame = 0
    bailed = False
    while frame < iterations:
        index = (frame & 1) if alternate else 0
        if use_fbo and (rebind_attachment or frame == 0):
            if use_texture:
                gles.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                            GL_TEXTURE_2D, textures[index], 0)
            else:
                gles.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_RENDERBUFFER, renderbuffers[index])
        elif not use_fbo:
            if not egl.eglMakeCurrent(display, surfaces[index], surfaces[index], context):
                fail("eglMakeCurrent")
        t0 = time.time()
        if not skip_render:
            gles.glClearColor(0.125 if index else 0.5, 0.25, 0.5, 1.0)
            gles.glClear(GL_COLOR_BUFFER_BIT)
            gles.glFinish()
        finish_us = int((time.time() - t0) * 1e6)
        free = memfree_kb()
        if free >= 0 and free < min_free:
            min_free = free
        print("%d,%d,%d,%d" % (frame, index, finish_us, free))
        sys.stdout.flush()
        if free >= 0 and free < memfree_floor:
            sys.stderr.write("BAIL: MemFree %d kB < floor %d kB at frame %d "
                             "(leak) -- stopping before OOM\n"
                             % (free, memfree_floor, frame))
            sys.stderr.flush()
            bailed = True
            break
        frame += 1

    end_free = memfree_kb()
    leaked_kb = (start_free - min_free) if (start_free >= 0 and min_free >= 0) else -1
    print("summary frames=%d start_free_kb=%d min_free_kb=%d end_free_kb=%d "
          "drop_kb=%d bailed=%d" % (frame, start_free, min_free, end_free,
                                    leaked_kb, int(bailed)))
    sys.stdout.flush()

    sys.stderr.write("cleanup begin\n")
    sys.stderr.flush()
    if use_fbo:
        if use_texture:
            gles.glDeleteTextures(2, textures)
        else:
            gles.glDeleteRenderbuffers(2, renderbuffers)
        gles.glDeleteFramebuffers(1, byref(framebuffer))
    gles.glFinish()
    egl.eglMakeCurrent(display, None, None, None)
    egl.eglDestroySurface(display, surfaces[1])
    egl.eglDestroySurface(display, surfaces[0])
    egl.eglDestroyContext(display, context)
    egl.eglTerminate(display)
    sys.stderr.write("cleanup complete\n")
    sys.stderr.flush()


if __name__ == "__main__":
    main()
