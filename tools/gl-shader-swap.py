#!/usr/bin/env python3
"""Replace fragment/vertex shader sources in a recorded stream with trivial equivalents.

usage: gl-shader-swap.py <src.txt> <src.bin> <out.txt> <out.bin> <fill|copy|both|vs|all>

The out.bin is src.bin with the new sources appended, so existing blob offsets stay valid.
The shader ids are found from USEPROG order: the program that draws 'DRAW 4 18 102'-style cache fills is the
SELECT_SHADER 5 (plain colour) program, the layer/window sampler is SELECT_SHADER 6 (plain texture copy).
"""
import sys

src_txt, src_bin, out_txt, out_bin, which = sys.argv[1:6]
lines = open(src_txt).read().split("\n")
blob = bytearray(open(src_bin, "rb").read())

FS_FILL = b"precision highp float;\nuniform vec4 frag[14];\nvoid main(void) { gl_FragColor = frag[6]; }\n"
FS_COPY = (b"precision highp float;\nuniform vec4 frag[14];\nuniform sampler2D tex;\nvarying vec2 ftcoord;\n"
           b"void main(void) { gl_FragColor = texture2D(tex, ftcoord) * frag[6]; }\n")
VS_MIN = (b"uniform vec2 viewSize;\nattribute vec2 vertex;\nattribute vec2 tcoord;\nvarying vec2 ftcoord;\n"
          b"void main(void) { ftcoord = tcoord; gl_Position = vec4(2.0 * vertex.x / viewSize.x - 1.0, "
          b"1.0 - 2.0 * vertex.y / viewSize.y, 0.0, 1.0); }\n")


def shader_text(sid):
    for l in lines:
        p = l.split()
        if p[0] == "SHADERSRC" and p[1] == sid:
            return blob[int(p[2]): int(p[2]) + int(p[3])].decode("latin1")
    return ""


def select_of(txt):
    for l in txt.split("\n"):
        if l.startswith("#define SELECT_SHADER "):
            return int(l.split()[2])
    return None


repl = {}
for l in lines:
    p = l.split()
    if p[0] == "SHADERSRC":
        t = shader_text(p[1])
        sel = select_of(t)
        if sel == 5 and which in ("fill", "both", "all"):
            repl[p[1]] = FS_FILL
        elif sel == 6 and which in ("copy", "both", "all"):
            repl[p[1]] = FS_COPY
        elif sel is None and "gl_Position" in t and which in ("vs", "all"):
            repl[p[1]] = VS_MIN

outl = []
for l in lines:
    p = l.split()
    if p[0] == "SHADERSRC" and p[1] in repl:
        off = len(blob)
        blob += repl[p[1]]
        l = f"SHADERSRC {p[1]} {off} {len(repl[p[1]])}"
    outl.append(l)

open(out_txt, "w").write("\n".join(outl))
open(out_bin, "wb").write(bytes(blob))
print("replaced", len(repl), "shaders ->", out_txt, file=sys.stderr)
