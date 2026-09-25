/* tilerepro.c — geometry-complete C repro of the femtovg tilesredraw storm.
 *
 * state-churn.c proved per-draw glUseProgram/glBindTexture churn alone (trivial
 * geometry) does NOT storm. femtovg's storming frame has real geometry: 12
 * rounded tiles, each a fill fan + a tile-spanning AA-fringe strip (~192 verts
 * /tile, 2304 verts/frame), re-uploaded every frame, drawn un-batched. This
 * reconstructs that in dependency-free C so we can bisect which leg storms.
 *
 *   argv[1] tiles     (default 12)
 *   argv[2] fringe    1 = draw fill fan + fringe strip; 0 = fill fan only (default 1)
 *   argv[3] churn     1 = glUseProgram(alt)+glBindTexture(alt) before each draw (default 1)
 *   argv[4] reupload  1 = glBufferData(STREAM) every frame; 0 = upload once (default 1)
 *   argv[5] seg       corner segments per rounded corner => fringe density (default 8)
 *   argv[6] frames    (default 40)
 *
 * Build: sgx-cc -O0 -I/usr/include/sgx-ddk16 tilerepro.c -lEGL -lGLESv2 -lm -o /root/tilerepro
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
typedef char GLchar;

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

static const char *vs_src =
    "uniform vec2 viewSize;\n"
    "attribute vec2 pos;\n"
    "void main(){ gl_Position = vec4(2.0*pos.x/viewSize.x - 1.0, 1.0 - 2.0*pos.y/viewSize.y, 0.0, 1.0); }\n";
static const char *fs_src =
    "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
    "void main(){ gl_FragColor = vec4(0.3, 0.6, 0.9, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(s, sizeof l, NULL, l); fprintf(stderr, "%s\n", l); fail("compile"); }
    return s;
}
static GLuint mkprog(void)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) fail("link");
    return p;
}

/* Rounded-rect perimeter points (pixel space) into pts[2*(4*(seg+1))].
 * Returns point count. Center at (cx,cy) returned via *ocx,*ocy. */
static int perim(float x, float y, float w, float h, float r, int seg,
                 float *pts, float *ocx, float *ocy)
{
    *ocx = x + w * 0.5f; *ocy = y + h * 0.5f;
    /* corner centers TL, TR, BR, BL and their arc start angle (radians) */
    float ccx[4] = { x + r,     x + w - r, x + w - r, x + r     };
    float ccy[4] = { y + r,     y + r,     y + h - r, y + h - r };
    float a0[4]  = { 3.14159265f, 4.71238898f, 0.0f, 1.57079633f };
    int n = 0;
    for (int c = 0; c < 4; c++) {
        for (int s = 0; s <= seg; s++) {
            float a = a0[c] + 1.57079633f * (float)s / (float)seg;
            pts[n * 2]     = ccx[c] + r * cosf(a);
            pts[n * 2 + 1] = ccy[c] + r * sinf(a);
            n++;
        }
    }
    return n;
}

/* tile grid position (pixel space) */
static void tile_pos(int i, float *x, float *y)
{
    int col = i % 4, row = (i / 4) % 3;
    *x = 16.0f + col * 246.0f;
    *y = 16.0f + row * 190.0f;
}

int main(int argc, char **argv)
{
    int tiles    = argc > 1 ? atoi(argv[1]) : 12;
    int fringe   = argc > 2 ? atoi(argv[2]) : 1;
    int churn    = argc > 3 ? atoi(argv[3]) : 1;
    int reupload = argc > 4 ? atoi(argv[4]) : 1;
    int seg      = argc > 5 ? atoi(argv[5]) : 8;
    int frames   = argc > 6 ? atoi(argv[6]) : 40;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 8, EGL_NONE };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint mj, mn, ncfg = 0, W = 0, H = 0; EGLConfig cfg;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &mj, &mn)) fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) fail("eglBindAPI");
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg != 1) fail("eglChooseConfig");
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    if (surf == EGL_NO_SURFACE) fail("eglCreateWindowSurface");
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) fail("eglMakeCurrent");
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);

    GLuint P[2] = { mkprog(), mkprog() };
    GLuint T[2]; unsigned char px[4] = { 255,255,255,255 };
    glGenTextures(2, T);
    for (int i = 0; i < 2; i++) { glBindTexture(GL_TEXTURE_2D, T[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); }

    /* per-tile geometry layout: fill fan [center + perimeter + first], then
     * fringe strip [inner,outer per perimeter point]. */
    int np = 4 * (seg + 1);            /* perimeter points */
    int fill_n = np + 2;               /* fan: center + perim + close */
    int fringe_n = 2 * (np + 1);       /* strip: inner+outer, closed */
    int per_tile = fill_n + fringe_n;
    int total = tiles * per_tile;
    float *buf = malloc(sizeof(float) * 2 * total);
    float *pp = malloc(sizeof(float) * 2 * np);

    /* build all tile geometry (positions animate slightly if reupload) */
    void build(float t) {
        int o = 0;
        for (int i = 0; i < tiles; i++) {
            float tx, ty; tile_pos(i, &tx, &ty);
            tx += (i == 1 || i == 6) ? 20.0f * sinf(t) : 0.0f;
            float cx, cy;
            perim(tx + 2, ty + 2, 216, 132, 12, seg, pp, &cx, &cy);
            /* fill fan */
            buf[o*2]=cx; buf[o*2+1]=cy; o++;
            for (int k = 0; k < np; k++) { buf[o*2]=pp[k*2]; buf[o*2+1]=pp[k*2+1]; o++; }
            buf[o*2]=pp[0]; buf[o*2+1]=pp[1]; o++;
            /* fringe strip: inner (on edge) + outer (1px out along normal from center) */
            for (int k = 0; k <= np; k++) {
                int kk = k % np;
                float ix = pp[kk*2], iy = pp[kk*2+1];
                float dx = ix - cx, dy = iy - cy; float l = sqrtf(dx*dx+dy*dy); if (l<1e-3f) l=1;
                buf[o*2]=ix; buf[o*2+1]=iy; o++;
                buf[o*2]=ix + dx/l; buf[o*2+1]=iy + dy/l; o++;
            }
        }
    }

    GLuint vbo; glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    build(1.0f);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*2*total, buf, reupload ? GL_STREAM_DRAW : GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(P[0]);
    GLint locView = glGetUniformLocation(P[0], "viewSize");
    GLint locView1 = glGetUniformLocation(P[1], "viewSize");
    glUniform2f(locView, (float)W, (float)H);
    glUseProgram(P[1]); glUniform2f(locView1, (float)W, (float)H);
    glUseProgram(P[0]);
    glBindTexture(GL_TEXTURE_2D, T[0]);

    printf("tilerepro: tiles=%d fringe=%d churn=%d reupload=%d seg=%d frames=%d verts/frame=%d %dx%d\n",
           tiles, fringe, churn, reupload, seg, frames, total, W, H);
    fflush(stdout);

    int draw_idx = 0;
    for (int f = 0; f < frames; f++) {
        glClear(GL_COLOR_BUFFER_BIT);
        if (reupload) { build((float)f * 0.1f); glBufferData(GL_ARRAY_BUFFER, sizeof(float)*2*total, buf, GL_STREAM_DRAW); }
        int o = 0;
        for (int i = 0; i < tiles; i++) {
            if (churn) { glUseProgram(P[draw_idx & 1]); glUniform2f(glGetUniformLocation(P[draw_idx & 1], "viewSize"), (float)W, (float)H); glBindTexture(GL_TEXTURE_2D, T[draw_idx & 1]); draw_idx++; }
            glDrawArrays(GL_TRIANGLE_FAN, o, fill_n);
            o += fill_n;
            if (fringe) {
                if (churn) { glUseProgram(P[draw_idx & 1]); glUniform2f(glGetUniformLocation(P[draw_idx & 1], "viewSize"), (float)W, (float)H); glBindTexture(GL_TEXTURE_2D, T[draw_idx & 1]); draw_idx++; }
                glDrawArrays(GL_TRIANGLE_STRIP, o, fringe_n);
            }
            o += fringe_n;
        }
        eglSwapBuffers(dpy, surf);
    }
    printf("done frames=%d\n", frames);
    return 0;
}
