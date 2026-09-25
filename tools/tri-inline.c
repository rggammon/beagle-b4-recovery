/* tri-inline.c — dependency-free inlined nanovg-render-path single-file repro.
 *
 * Zero nanovg dependency: this file inlines the exact GL call sequence NanoVG's
 * GLES2 backend emits for one AA-filled triangle. Same shaders, same 11-vec4
 * frag uniform block, same per-frame renderFlush state, same convexFill dispatch,
 * same 11 verts (nanovg's dumped triangle).
 *
 * Three independent knobs (bisect the AA trigger):
 *     argv[1] EDGE_AA : 0 = compile uber-FS without #define EDGE_AA
 *                       1 = compile uber-FS with    #define EDGE_AA
 *     argv[2] FRINGE  : 0 = draw fill fan only  (verts 0..2)
 *                       1 = draw fill fan + fringe strip (verts 0..10)
 *     argv[3] FS      : 0 = TRIVIAL FS (constant color, like vreplay)
 *                       1 = full uber-FS (default; EDGE_AA gates a sub-branch)
 *     argv[4] STATE   : bitmask of per-frame nanovg-renderFlush pieces
 *                       0   = vreplay-style minimal (setup once, per-frame just draw+swap)
 *                       255 = full nanovg renderFlush every frame (default)
 *                       bits: 1=clear D+S | 2=cull enable/BACK/CCW | 4=disable DEPTH+SCISSOR
 *                             8=colorMask+stencilMask/Op/Func | 16=activeTex+bindTex churn
 *                            32=uniform1i tex+uniform2fv view+uniform4fv frag
 *                            64=useProgram+bindBuffer+bufferData+attrib re-enable+pointers
 *                           128=glBlendFuncSeparate per frame (else one-time glBlendFunc)
 *                       Half A (pipeline state) = 1|2|4|8|128 = 143
 *                       Half B (rebind churn)   = 16|32|64    = 112
 *     argv[5] frames  (default 300)
 *
 *     ./tri-inline 0 1 0 0   30   trivial+fan+fringe+MINIMAL   -> baseline CLEAN
 *     ./tri-inline 0 1 0 143 30   trivial+fan+fringe+halfA     -> pipeline-state bisect
 *     ./tri-inline 0 1 0 112 30   trivial+fan+fringe+halfB     -> rebind-churn bisect
 *
 * Build: sgx-cc -O0 -I/usr/include/sgx-ddk16 tri-inline.c -lEGL -lGLESv2 -o /root/tri-inline
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
typedef char GLchar;  /* DDK 1.6 gl2.h predates GLchar */

static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

/* NanoVG's fillVertShader, verbatim (GLES2 branch), with viewSize + attrs.    */
static const char *vertShader =
    "uniform vec2 viewSize;\n"
    "attribute vec2 vertex;\n"
    "attribute vec2 tcoord;\n"
    "varying vec2 ftcoord;\n"
    "varying vec2 fpos;\n"
    "void main(void) {\n"
    "  ftcoord = tcoord;\n"
    "  fpos = vertex;\n"
    "  gl_Position = vec4(2.0*vertex.x/viewSize.x - 1.0, 1.0 - 2.0*vertex.y/viewSize.y, 0, 1);\n"
    "}\n";

/* Truly trivial FS — constant premult amber, matches vreplay's shader profile.
 * References ftcoord/fpos so the varyings link identically. */
static const char *trivFragShader =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 ftcoord;\n"
    "varying vec2 fpos;\n"
    "void main(void) {\n"
    "  vec2 dummy = ftcoord + fpos;\n"
    "  gl_FragColor = vec4(0.941, 0.706, 0.275, 1.0) + vec4(dummy*0.0, 0.0, 0.0);\n"
    "}\n";

/* NanoVG's fillFragShader — the full uber-shader (GLES2 branch, gradient +
 * image + stencil-fill + textured-tris + EDGE_AA branch guarded by a #define
 * we prepend at compile time). UNIFORMARRAY_SIZE = 11 (nanovg default). */
static const char *fragShaderBody =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform vec4 frag[11];\n"
    "uniform sampler2D tex;\n"
    "varying vec2 ftcoord;\n"
    "varying vec2 fpos;\n"
    "#define scissorMat mat3(frag[0].xyz, frag[1].xyz, frag[2].xyz)\n"
    "#define paintMat mat3(frag[3].xyz, frag[4].xyz, frag[5].xyz)\n"
    "#define innerCol frag[6]\n"
    "#define outerCol frag[7]\n"
    "#define scissorExt frag[8].xy\n"
    "#define scissorScale frag[8].zw\n"
    "#define extent frag[9].xy\n"
    "#define radius frag[9].z\n"
    "#define feather frag[9].w\n"
    "#define strokeMult frag[10].x\n"
    "#define strokeThr frag[10].y\n"
    "#define texType int(frag[10].z)\n"
    "#define type int(frag[10].w)\n"
    "float sdroundrect(vec2 pt, vec2 ext, float rad) {\n"
    "  vec2 ext2 = ext - vec2(rad,rad);\n"
    "  vec2 d = abs(pt) - ext2;\n"
    "  return min(max(d.x,d.y),0.0) + length(max(d,0.0)) - rad;\n"
    "}\n"
    "float scissorMask(vec2 p) {\n"
    "  vec2 sc = (abs((scissorMat * vec3(p,1.0)).xy) - scissorExt);\n"
    "  sc = vec2(0.5,0.5) - sc * scissorScale;\n"
    "  return clamp(sc.x,0.0,1.0) * clamp(sc.y,0.0,1.0);\n"
    "}\n"
    "#ifdef EDGE_AA\n"
    "float strokeMask() {\n"
    "  return min(1.0, (1.0-abs(ftcoord.x*2.0-1.0))*strokeMult) * min(1.0, ftcoord.y);\n"
    "}\n"
    "#endif\n"
    "void main(void) {\n"
    "  vec4 result;\n"
    "  float scissor = scissorMask(fpos);\n"
    "#ifdef EDGE_AA\n"
    "  float strokeAlpha = strokeMask();\n"
    "  if (strokeAlpha < strokeThr) discard;\n"
    "#else\n"
    "  float strokeAlpha = 1.0;\n"
    "#endif\n"
    "  if (type == 0) {\n"
    "    vec2 pt = (paintMat * vec3(fpos,1.0)).xy;\n"
    "    float d = clamp((sdroundrect(pt, extent, radius) + feather*0.5) / feather, 0.0, 1.0);\n"
    "    vec4 color = mix(innerCol,outerCol,d);\n"
    "    color *= strokeAlpha * scissor;\n"
    "    result = color;\n"
    "  } else if (type == 1) {\n"
    "    vec2 pt = (paintMat * vec3(fpos,1.0)).xy / extent;\n"
    "    vec4 color = texture2D(tex, pt);\n"
    "    if (texType == 1) color = vec4(color.xyz*color.w,color.w);\n"
    "    if (texType == 2) color = vec4(color.x);\n"
    "    color *= innerCol;\n"
    "    color *= strokeAlpha * scissor;\n"
    "    result = color;\n"
    "  } else if (type == 2) {\n"
    "    result = vec4(1,1,1,1);\n"
    "  } else if (type == 3) {\n"
    "    vec4 color = texture2D(tex, ftcoord);\n"
    "    if (texType == 1) color = vec4(color.xyz*color.w,color.w);\n"
    "    if (texType == 2) color = vec4(color.x);\n"
    "    color *= scissor;\n"
    "    result = color * innerCol;\n"
    "  }\n"
    "  gl_FragColor = result;\n"
    "}\n";

static GLuint compile(GLenum type, const char *hdr, const char *body)
{
    const char *srcs[2] = { hdr ? hdr : "", body };
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 2, srcs, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), NULL, log); fprintf(stderr, "shader:\n%s\n", log); fail("compile"); }
    return s;
}

/* nanovg's dumped triangle: 11 verts, fill fan [0,3) + fringe strip [3,11).
 * Format matches NVGvertex { float x,y,u,v; } — 4 floats per vert.        */
static const float tri_verts[11 * 4] = {
    382.966f, 374.500f, 0.5000f, 1.0000f,  /* 0 */
    641.034f, 374.500f, 0.5000f, 1.0000f,  /* 1  fill fan */
    512.000f, 151.000f, 0.5000f, 1.0000f,  /* 2 */
    382.966f, 374.500f, 0.5000f, 1.0000f,  /* 3 */
    381.234f, 375.500f, 1.0000f, 1.0000f,  /* 4 */
    641.034f, 374.500f, 0.5000f, 1.0000f,  /* 5  fringe strip */
    642.766f, 375.500f, 1.0000f, 1.0000f,  /* 6 */
    512.000f, 151.000f, 0.5000f, 1.0000f,  /* 7 */
    512.000f, 149.000f, 1.0000f, 1.0000f,  /* 8 */
    382.966f, 374.500f, 0.5000f, 1.0000f,  /* 9 */
    381.234f, 375.500f, 1.0000f, 1.0000f,  /* 10 */
};

/* Build the 11-vec4 frag uniform block for nvgFillColor(240,180,70,255) with no
 * scissor, no image, no gradient — matches glnvg__convertPaint output exactly. */
static void build_frag_uniforms(float u[44])
{
    memset(u, 0, sizeof(float) * 44);
    /* frag[0..2] scissorMat 3x3 (packed 3x4 with mat3-in-mat3x4): identity->zeroed
     * with scissorExt=(1,1) and scissorScale=(1,1) yields scissorMask==1 everywhere
     * (matches convertPaint's `scissor==NULL` branch). */
    u[8*4+0] = 1.0f; u[8*4+1] = 1.0f;   /* scissorExt = (1,1) */
    u[8*4+2] = 1.0f; u[8*4+3] = 1.0f;   /* scissorScale = (1,1) */

    /* frag[3..5] paintMat: identity xform inverse packed row-major mat3.
     * For a solid color the paint xform is identity, so invxform is identity too.
     * glnvg__xformToMat3x4 packs (a,c,0, b,d,0, e,f,0) into 3 vec4s. Identity
     * NVGtransform = {1,0, 0,1, 0,0} -> mat3(1,0,0, 0,1,0, 0,0,0). */
    u[3*4+0] = 1.0f;                     /* frag[3] = (1,0,0,_) */
    u[4*4+1] = 1.0f;                     /* frag[4] = (0,1,0,_) */
                                          /* frag[5] = (0,0,0,_) — zeroed */

    /* frag[6] innerCol, frag[7] outerCol — premultiplied amber (nanovg premuls). */
    const float R = 240.0f/255.0f, G = 180.0f/255.0f, B = 70.0f/255.0f, A = 1.0f;
    u[6*4+0] = R*A; u[6*4+1] = G*A; u[6*4+2] = B*A; u[6*4+3] = A;
    u[7*4+0] = R*A; u[7*4+1] = G*A; u[7*4+2] = B*A; u[7*4+3] = A;

    /* frag[9] extent(0,0), radius=0, feather=1 — solid color degenerate gradient. */
    u[9*4+0] = 0.0f; u[9*4+1] = 0.0f;
    u[9*4+2] = 0.0f;                     /* radius */
    u[9*4+3] = 1.0f;                     /* feather */

    /* frag[10] strokeMult, strokeThr, texType, type.
     * convexFill uses strokeThr=-1, fringe=1 -> strokeMult=(0.5+0.5)/1=1.0 */
    u[10*4+0] = 1.0f;                    /* strokeMult */
    u[10*4+1] = -1.0f;                   /* strokeThr — negative -> discard never triggers */
    u[10*4+2] = 0.0f;                    /* texType (unused for type=0) */
    u[10*4+3] = 0.0f;                    /* type = NSVG_SHADER_FILLGRAD (solid color) */
}

int main(int argc, char **argv)
{
    const int edge_aa = argc > 1 ? atoi(argv[1]) : 1;
    const int fringe  = argc > 2 ? atoi(argv[2]) : 1;
    const int fs_mode = argc > 3 ? atoi(argv[3]) : 1;
    const int st_mask = argc > 4 ? atoi(argv[4]) : 255;
    const int frames  = argc > 5 ? atoi(argv[5]) : 300;
    /* clear_mode used only when STF_CLEAR_DS bit is set:
       1 = COLOR|DEPTH|STENCIL (default, original CLEAR_DS)
       2 = COLOR|STENCIL only
       3 = COLOR|DEPTH only */
    const int clear_mode = argc > 6 ? atoi(argv[6]) : 1;

    #define STF_CLEAR_DS  0x01
    #define STF_CULL      0x02
    #define STF_DEPSC     0x04
    #define STF_MASKS     0x08
    #define STF_TEX       0x10
    #define STF_UNIF      0x20
    #define STF_REBIND    0x40
    #define STF_BLENDSEP  0x80

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
        EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min, ncfg = 0, W = 0, H = 0;
    EGLConfig cfg;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) fail("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) fail("eglBindAPI");
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg != 1) fail("eglChooseConfig");
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) fail("eglCreateContext");
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, NULL);
    if (surf == EGL_NO_SURFACE) fail("eglCreateWindowSurface");
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) fail("eglMakeCurrent");
    eglQuerySurface(dpy, surf, EGL_WIDTH, &W);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &H);

    const char *hdr = edge_aa ? "#define EDGE_AA 1\n" : NULL;
    GLuint vs = compile(GL_VERTEX_SHADER, NULL, vertShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER,
                        fs_mode ? hdr : NULL,
                        fs_mode ? fragShaderBody : trivFragShader);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "vertex");
    glBindAttribLocation(prog, 1, "tcoord");
    glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { char log[1024]; glGetProgramInfoLog(prog, sizeof(log), NULL, log); fprintf(stderr, "link:\n%s\n", log); fail("link"); }
    GLint locViewSize = glGetUniformLocation(prog, "viewSize");
    GLint locTex      = glGetUniformLocation(prog, "tex");
    GLint locFrag     = glGetUniformLocation(prog, "frag");
    /* 1x1 dummy texture, like nanovg's dummyTex (bound so sampler is never null). */
    GLuint dummyTex; unsigned char one[4] = {255,255,255,255};
    glGenTextures(1, &dummyTex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, dummyTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint vbo; glGenBuffers(1, &vbo);

    float view[2] = { (float)W, (float)H };
    float fragU[44];
    build_frag_uniforms(fragU);

    printf("renderer=%s surface=%dx%d EDGE_AA=%d FRINGE=%d FS=%s STATE=0x%02x CLR=%d frames=%d\n",
           glGetString(GL_RENDERER), W, H, edge_aa, fringe,
           fs_mode ? "uber" : "trivial", st_mask, clear_mode, frames);

    /* One-time init for state pieces NOT covered by per-frame bits. */
    if (!(st_mask & STF_REBIND)) {
        glUseProgram(prog);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tri_verts), tri_verts, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (const void *)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (const void *)8);
    }
    glEnable(GL_BLEND);
    if (!(st_mask & STF_BLENDSEP)) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!(st_mask & STF_TEX)) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dummyTex);
    }
    if (!(st_mask & STF_UNIF)) {
        if (locTex      >= 0) glUniform1i(locTex, 0);
        if (locViewSize >= 0) glUniform2fv(locViewSize, 1, view);
        if (locFrag     >= 0) glUniform4fv(locFrag, 11, fragU);
    }

    for (int f = 0; f < frames; f++) {
        glViewport(0, 0, W, H);
        glClearColor(0.1f, 0.11f, 0.14f, 1.0f);
        if (st_mask & STF_CLEAR_DS) {
            GLbitfield mask = GL_COLOR_BUFFER_BIT;
            if (clear_mode == 1) mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
            else if (clear_mode == 2) mask |= GL_STENCIL_BUFFER_BIT;
            else if (clear_mode == 3) mask |= GL_DEPTH_BUFFER_BIT;
            glClear(mask);
        } else
            glClear(GL_COLOR_BUFFER_BIT);

        if (st_mask & STF_REBIND) {
            glUseProgram(prog);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(tri_verts), tri_verts, GL_STREAM_DRAW);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (const void *)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (const void *)8);
        }
        if (st_mask & STF_CULL) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
        }
        if (st_mask & STF_DEPSC) {
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
        }
        if (st_mask & STF_MASKS) {
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilMask(0xffffffff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glStencilFunc(GL_ALWAYS, 0, 0xffffffff);
        }
        if (st_mask & STF_TEX) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        if (st_mask & STF_UNIF) {
            if (locTex      >= 0) glUniform1i(locTex, 0);
            if (locViewSize >= 0) glUniform2fv(locViewSize, 1, view);
        }
        if (st_mask & STF_BLENDSEP)
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        if (st_mask & STF_UNIF) {
            if (locFrag >= 0) glUniform4fv(locFrag, 11, fragU);
        }
        if (st_mask & STF_TEX) {
            glBindTexture(GL_TEXTURE_2D, dummyTex);
        }

        glDrawArrays(GL_TRIANGLE_FAN, 0, 3);
        if (fringe) glDrawArrays(GL_TRIANGLE_STRIP, 3, 8);

        if (st_mask & STF_REBIND) {
            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glUseProgram(0);
        }
        if (st_mask & STF_CULL) glDisable(GL_CULL_FACE);
        if (st_mask & STF_TEX)  glBindTexture(GL_TEXTURE_2D, 0);

        if (!eglSwapBuffers(dpy, surf)) fail("eglSwapBuffers");
    }
    printf("done frames=%d\n", frames);
    return 0;
}
