#!/usr/bin/env python3
"""Derive replay-stream variants from a recorded cache-on stream (gl-record.c output).

Layer FBO ids are odd multiples of 70001 and cache FBO ids even multiples (femtovg creates the
layer then its cache). A "layer block" is the run of ops from BINDFB <layer> up to the next BINDFB:
it draws the cache texture into the layer. Variants:

  relayer   re-run every layer block at the start of frame 2 (caches are settled by then)
  relayer3  same, but at the start of frame 3 (checks later-frame timing too)
  layerfirst  none: placeholder for future variants
"""
import sys

src, out, variant = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(src).read().split("\n")

def fbid(l):
    p = l.split()
    return int(p[2]) if len(p) == 3 and p[0] == "BINDFB" and p[1] == "36160" else None

def is_layer(n):
    return n and n % 70001 == 0 and (n // 70001) % 2 == 1

if variant.startswith("edit:"):
    # edit:rep121=CLEAR 16384;del122;ins198=CLEARCOLOR a b c d|CLEAR 17408  (line numbers = 0-based indices in src;
    # ins inserts the '|'-separated ops AFTER that line; all edits refer to the original numbering)
    rep, dele, ins = {}, set(), {}
    for item in variant[5:].split(";"):
        kind, rest = item[:3], item[3:]
        if kind == "del":
            dele.add(int(rest))
        elif kind == "rep":
            n, t = rest.split("=", 1)
            rep[int(n)] = t
        elif kind == "ins":
            n, t = rest.split("=", 1)
            ins[int(n)] = t.split("|")
    outl = []
    for i, l in enumerate(lines):
        if i not in dele:
            outl.append(rep.get(i, l))
        outl += ins.get(i, [])
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant[:60], "ops", len(outl), file=sys.stderr)
    sys.exit(0)

if variant.startswith("dropre:") or variant.startswith("droprange:"):
    import re
    if variant.startswith("dropre:"):
        rx = re.compile(variant[7:])
        outl = [l for l in lines if not rx.search(l)]
    else:
        rng = [tuple(int(v) for v in r.split("-")) for r in variant[10:].split(",")]
        outl = [l for i, l in enumerate(lines) if not any(a <= i <= b for a, b in rng)]
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "removed", len(lines) - len(outl), "ops", len(outl), file=sys.stderr)
    sys.exit(0)

if variant == "keepprogs":
    # drop shader-program setup blocks (CREATESHADER..GETUNI) for programs the stream never USEPROGs
    setup = ("CREATESHADER", "SHADERSRC", "COMPILE", "CREATEPROG", "ATTACH", "BINDATTR", "LINK", "DETACH",
             "DELSHADER", "GETUNI")
    used = {l.split()[1] for l in lines if l.startswith("USEPROG ")}
    outl, i, dropped = [], 0, 0
    while i < len(lines):
        if lines[i].startswith("CREATESHADER 35633"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("CREATESHADER 35633") and lines[j].split()[0] in setup:
                j += 1
            blk = lines[i:j]
            pid = next(x.split()[1] for x in blk if x.startswith("CREATEPROG "))
            if pid in used:
                outl += blk
            else:
                dropped += 1
            i = j
        else:
            outl.append(lines[i])
            i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "dropped programs:", dropped, "ops", len(outl), file=sys.stderr)
    sys.exit(0)

if variant.startswith("trunc:"):
    # trunc:K -> keep everything up to and including the (K+1)th FRAME marker (K=1: prefix + bake frame only)
    k = int(variant[6:])
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    open(out, "w").write("\n".join(lines[: frames[k] + 1] + ["END"]))
    print("wrote", out, variant, "ops", frames[k] + 2, file=sys.stderr)
    sys.exit(0)

if variant.startswith("keep:"):
    # keep:0,3  -> keep only those tiles' bake regions; later-frame samples of dropped layers bind texture 0
    keep = {int(x) for x in variant[5:].split(",")}
    gen = [i for i, l in enumerate(lines) if l.startswith("GENFB 1 ") and is_layer(int(l.split()[2]))]
    end_bake = next(i for i in range(gen[-1], len(lines)) if lines[i].startswith("DISVA"))
    bounds = gen + [end_bake]
    drop_ids = {str(int(lines[g].split()[2])) for k, g in enumerate(gen) if k not in keep}
    outl = lines[: bounds[0]]
    for k in range(len(gen)):
        if k in keep:
            outl += lines[bounds[k]: bounds[k + 1]]
    for l in lines[end_bake:]:
        p = l.split()
        if len(p) == 3 and p[0] == "BINDTEX" and p[2] in drop_ids:
            l = "BINDTEX 3553 0"
        outl.append(l)
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "tiles kept", sorted(keep), file=sys.stderr)
    sys.exit(0)

if variant.startswith("fbvp"):
    # fbvp<mode>: change only the FBO (150x96) viewports, window viewports untouched
    #   fbvpdrop   drop them (FBO draws then run with whatever viewport was last set)
    #   fbvpwin    replace by the window viewport 0 0 1024 600
    #   fbvpbig    replace by 0 0 2048 1200
    #   fbvplayer  replace only the LAYER-bound ones (BINDFB to an odd multiple of 70001) by the window viewport
    #   fbvpcache  replace only the CACHE-bound ones by the window viewport
    mode = variant[4:]
    cur, outl, n = None, [], 0
    for l in lines:
        b = fbid(l)
        if b is not None:
            cur = b
        if l == "VIEWPORT 0 0 150 96":
            is_l = is_layer(cur)
            if mode == "drop":
                n += 1
                continue
            if mode == "win" or (mode == "layer" and is_l) or (mode == "cache" and cur and not is_l):
                l = "VIEWPORT 0 0 1024 600"; n += 1
            elif mode == "big":
                l = "VIEWPORT 0 0 2048 1200"; n += 1
        outl.append(l)
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "changed", n, file=sys.stderr)
    sys.exit(0)

if variant == "nowinbake":
    # drop the window composite DRAWs that happen inside the bake frame (window bound, frame 1 only):
    # the real bake samples each layer to the window right after drawing it
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    cur, dropped, outl = None, 0, []
    for i, l in enumerate(lines):
        n = fbid(l)
        if n is not None:
            cur = n
        if frames[0] < i < frames[1] and l.startswith("DRAW ") and cur == 0:
            dropped += 1
            continue
        outl.append(l)
    open(out, "w").write("\n".join(outl))
    print("wrote", out, "window draws dropped from bake frame:", dropped, file=sys.stderr)
    sys.exit(0)

if variant in ("resizebefore", "resizebefore_same"):
    # Before EVERY cache->layer draw block of the bake frame, re-specify the bake VBO just before the draw:
    #   resizebefore       960 B (different size) then the original 28896 B
    #   resizebefore_same  the original 28896 B twice (control: no size change)
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    big = next(l for l in lines if l.startswith("BUFDATA 34962 28896 "))
    small = next(l for l in lines if l.startswith("BUFDATA 34962 960 "))
    pre = ["BINDBUF 34962 70001"] + ([small, big] if variant == "resizebefore" else [big, big]) + \
          ["ENVA 0", "ENVA 1", "VAP 0 2 5126 0 16 0", "VAP 1 2 5126 0 16 8"]
    outl, i, n_ins = lines[: frames[0] + 1], frames[0] + 1, 0
    while i < len(lines):
        n = fbid(lines[i])
        if i < end1 and is_layer(n):
            j = i + 1
            while j < end1 and fbid(lines[j]) is None:
                j += 1
            if any(x.startswith("DRAW ") for x in lines[i:j]):
                outl += pre
                n_ins += 1
        outl.append(lines[i])
        i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "inserted before", n_ins, "layer draw blocks", file=sys.stderr)
    sys.exit(0)

if variant in ("preorphan", "preorphan_same"):
    # Right after the bake VBO's first upload (before any draw reads it) re-specify the buffer:
    #   preorphan       with a DIFFERENT size (the 960 B composite data) and then the original 28896 B again
    #   preorphan_same  with the SAME original data twice (same size, control)
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    b0 = next(i for i in range(frames[0]) if lines[i].startswith("BUFDATA 34962 28896 "))
    small = next(l for l in lines if l.startswith("BUFDATA 34962 960 "))
    extra = ([small, lines[b0]] if variant == "preorphan" else [lines[b0]])
    open(out, "w").write("\n".join(lines[: b0 + 1] + extra + lines[b0 + 1 :]))
    print("wrote", out, variant, "after line", b0 + 1, file=sys.stderr)
    sys.exit(0)

if variant in ("nobufdata", "newbuf"):
    # The end-of-bake-frame BUFDATA (960 B) re-specifies the SAME buffer object the ~36 bake draws read from.
    #   nobufdata  drop that BUFDATA (the composite then reads the big bake buffer; offsets will differ,
    #              so this only tests whether the bake draws survive, via the frame-2+ composite lit count)
    #   newbuf     keep it but upload into a NEW buffer object instead of re-specifying the bake buffer
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    idx = [i for i in range(frames[0], frames[1]) if lines[i].startswith("BUFDATA 34962 960 ")]
    assert len(idx) == 1, idx
    b = idx[0]
    if variant == "nobufdata":
        new = lines[:b] + lines[b + 1 :]
    else:
        # also fix the bind just before it so it targets a fresh id; add GENBUF for that id
        assert lines[b - 1] == "BINDBUF 34962 70001", lines[b - 1]
        new = lines[: b - 1] + ["GENBUF 1 990099", "BINDBUF 34962 990099"] + lines[b:]
        # later frames re-bind 70001 and re-specify it themselves, so nothing else to change
    open(out, "w").write("\n".join(new))
    print("wrote", out, variant, "(BUFDATA at line", b + 1, ")", file=sys.stderr)
    sys.exit(0)

if variant.startswith("r1k"):
    # r1k<K>: tile 0 redraw (bake VBO restored) inserted after the first K of the 35 end-of-bake-frame ops
    # (lines 1300..1334: DISVA .. CLEARCOLOR). K=0 behaves like r1pre (fails), K=35 like r1post (works).
    K = int(variant[3:])
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    bufline = next(l for l in lines if l.startswith("BUFDATA 34962 "))
    reup = ["BINDBUF 34962 70001", bufline, "ENVA 0", "ENVA 1", "VAP 0 2 5126 0 16 0", "VAP 1 2 5126 0 16 8"]
    i = frames[0]
    blk0 = None
    while blk0 is None:
        if fbid(lines[i]) == 70001:
            j = i + 1
            while fbid(lines[j]) is None:
                j += 1
            if any(x.startswith("DRAW ") for x in lines[i:j]):
                blk0 = lines[i:j]
            i = j
        else:
            i += 1
    tail = ["BINDFB 36160 0", "VIEWPORT 0 0 1024 600", "UNI2FV 1 1 44800000 44160000", "BLENDSEP 1 771 1 771"]
    start = next(i for i in range(frames[0], frames[1]) if lines[i].startswith("DISVA"))
    at = start + K
    open(out, "w").write("\n".join(lines[:at] + reup + blk0 + tail + lines[at:]))
    print("wrote", out, variant, "after op", lines[at - 1] if K else "(none)", file=sys.stderr)
    sys.exit(0)

if variant in ("r1pre", "r1post", "r1f2end", "r1f2start"):
    # tile 0's cache->layer block (bake VBO restored first), inserted at one position:
    #   r1pre     in frame 1, after the last tile's bake, BEFORE femtovg's end-of-frame state/VBO ops
    #   r1post    in frame 1, after those end-of-frame ops, just before the frame-2 marker
    #   r1f2start in frame 2, right after its first CLEAR (the known-good 'relayer1' position)
    #   r1f2end   at the very end of frame 2, just before the frame-3 marker
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    bufline = next(l for l in lines if l.startswith("BUFDATA 34962 "))
    reup = ["BINDBUF 34962 70001", bufline, "ENVA 0", "ENVA 1", "VAP 0 2 5126 0 16 0", "VAP 1 2 5126 0 16 8"]
    i = frames[0]
    blk0 = None
    while blk0 is None:
        if fbid(lines[i]) == 70001:
            j = i + 1
            while fbid(lines[j]) is None:
                j += 1
            if any(x.startswith("DRAW ") for x in lines[i:j]):
                blk0 = lines[i:j]
            i = j
        else:
            i += 1
    tail = ["BINDFB 36160 0", "VIEWPORT 0 0 1024 600", "UNI2FV 1 1 44800000 44160000", "BLENDSEP 1 771 1 771"]
    ins = reup + blk0 + tail
    if variant == "r1pre":
        at = next(i for i in range(frames[0], frames[1]) if lines[i].startswith("DISVA"))
    elif variant == "r1post":
        at = frames[1]
    elif variant == "r1f2start":
        at = next(i for i in range(frames[1], frames[2]) if lines[i].startswith("CLEAR ")) + 1
    else:
        at = frames[2]
    open(out, "w").write("\n".join(lines[:at] + ins + lines[at:]))
    print("wrote", out, variant, "at line", at + 1, file=sys.stderr)
    sys.exit(0)

if variant in ("t0sampled", "t0unsampled"):
    # tile 0 only. Bake it and composite it to the window, then swap. t0sampled: the NEXT frame has
    # already sampled the layer (window composite) before the layer is re-drawn; t0unsampled: re-draw
    # right after the swap without that earlier window sample. Final window composite follows either way.
    li = next(i for i, l in enumerate(lines) if l == "FRAME")
    d = [i for i in range(li, len(lines)) if lines[i].startswith("DRAW ")]
    upto = d[3]
    b0 = max(i for i in range(li, upto) if lines[i] == "BINDFB 36160 0")
    win = lines[b0 : upto + 1]
    i = next(i for i in range(li, upto) if fbid(lines[i]) == 70001 and
             any(x.startswith("DRAW ") for x in lines[i + 1 : i + 20]) and i > li + 40)
    j = i + 1
    while fbid(lines[j]) is None:
        j += 1
    blk0 = lines[i:j]
    mid = ["BINDFB 36160 0", "CLEAR 16384", "FRAME"]
    seq = lines[: upto + 1] + mid
    if variant == "t0sampled":
        seq += win + ["BINDFB 36160 0", "CLEAR 16384", "FRAME"]
    seq += blk0 + ["BINDFB 36160 0", "CLEAR 16384", "FRAME"] + win + ["END"]
    open(out, "w").write("\n".join(seq))
    print("wrote", out, variant, "layer block ops:", len(blk0), "window block ops:", len(win), file=sys.stderr)
    sys.exit(0)

if variant.startswith("sep"):
    # sep<K> / sepswap<K>: re-draw ONLY tile 0's cache->layer block after K further tiles have been baked
    # (within the bake frame, bake VBO still bound). sepswap puts a swap (window bound + window CLEAR)
    # just before the redraw. Tile 0's window composite in the next frame is lit iff the redraw worked.
    swap = variant.startswith("sepswap")
    K = int(variant[7:] if swap else variant[3:])
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    blk0 = None
    i = frames[0]
    while i < end1 and blk0 is None:
        if fbid(lines[i]) == 70001:
            j = i + 1
            while j < end1 and fbid(lines[j]) is None:
                j += 1
            if any(x.startswith("DRAW ") for x in lines[i:j]):
                blk0 = lines[i:j]
            i = j
        else:
            i += 1
    genfb = [i for i in range(frames[0], end1)
             if lines[i].startswith("GENFB") and is_layer(int(lines[i].split()[2]))]
    if K + 1 < len(genfb):
        at = genfb[K + 1]                       # before tile K+1 is created
    else:
        at = next(i for i in range(genfb[-1], end1) if lines[i].startswith("DISVA"))
    ins = (["BINDFB 36160 0", "CLEAR 16384", "FRAME"] if swap else []) + blk0 + ["BINDFB 36160 0"]
    open(out, "w").write("\n".join(lines[:at] + ins + lines[at:]))
    print("wrote", out, variant, "inserted", len(ins), "ops before line", at + 1, file=sys.stderr)
    sys.exit(0)

if variant.startswith("tileframes"):
    # tileframes[K]: spread the bake over several frames: swap (window bound, with a window CLEAR so the
    # swap has work) before every K-th tile's layer GENFB. K defaults to 1 (every tile in its own frame).
    k_every = int(variant[len("tileframes"):] or 1)
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    outl, tile = [], 0
    for i, l in enumerate(lines):
        p = l.split()
        if frames[0] < i < end1 and len(p) == 3 and p[0] == "GENFB" and is_layer(int(p[2])):
            if tile and tile % k_every == 0:
                outl += ["BINDFB 36160 0", "CLEAR 16384", "FRAME"]
            tile += 1
        outl.append(l)
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "tiles:", tile, file=sys.stderr)
    sys.exit(0)

if variant == "doubleswap":
    # layer draw, then swap (window bound), then the same layer draw again in the next frame
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    outl, i, n_dup = lines[: frames[0] + 1], frames[0] + 1, 0
    while i < len(lines):
        n = fbid(lines[i])
        if i < end1 and is_layer(n):
            j = i + 1
            while j < end1 and fbid(lines[j]) is None:
                j += 1
            blk = lines[i:j]
            outl += blk
            if any(x.startswith("DRAW ") for x in blk):
                outl += ["BINDFB 36160 0", "CLEAR 16384", "FRAME"] + blk
                n_dup += 1
            i = j
            continue
        outl.append(lines[i])
        i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, "doubleswap:", n_dup, file=sys.stderr)
    sys.exit(0)

if variant in ("tile0swap", "tile0same"):
    # only tile 0: replay its bake through its first window composite, then draw its layer to the window
    # AGAIN, either in the next frame (tile0swap) or immediately (tile0same). Lit => the layer was written
    # correctly and is lost later; black => never written.
    li = next(i for i, l in enumerate(lines) if l == "FRAME")
    d = [i for i in range(li, len(lines)) if lines[i].startswith("DRAW ")]
    last_layer_draw = d[3]                      # 4th draw: the window composite of tile 0 (bake, bake, layer, window)
    b0 = max(i for i in range(li, last_layer_draw) if lines[i] == "BINDFB 36160 0")
    win = lines[b0 : last_layer_draw + 1]
    tail = (["BINDFB 36160 0", "FRAME", "CLEAR 16384"] if variant == "tile0swap" else []) + win + ["END"]
    open(out, "w").write("\n".join(lines[: last_layer_draw + 1] + tail))
    print("wrote", out, variant, "window block ops:", len(win), file=sys.stderr)
    sys.exit(0)

if variant in ("double2", "doublewin"):
    # double2:   layer draw, re-upload the vertex buffer (BUFDATA), layer draw again
    # doublewin: layer draw, the window composite that follows it, layer draw again
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    bufline = next(l for l in lines if l.startswith("BUFDATA 34962 "))
    reup = ["BINDBUF 34962 70001", bufline, "ENVA 0", "ENVA 1", "VAP 0 2 5126 0 16 0", "VAP 1 2 5126 0 16 8"]
    outl, i, n_dup = lines[: frames[0] + 1], frames[0] + 1, 0
    while i < len(lines):
        n = fbid(lines[i])
        if i < end1 and is_layer(n):
            j = i + 1
            while j < end1 and fbid(lines[j]) is None:
                j += 1
            blk = lines[i:j]
            outl += blk
            if any(x.startswith("DRAW ") for x in blk):
                if variant == "double2":
                    outl += reup + blk
                else:
                    k = j + 1                      # window block: lines[j] is BINDFB 0 ... up to next BINDFB
                    while k < end1 and fbid(lines[k]) is None:
                        k += 1
                    win = lines[j:k]
                    outl += win + blk
                    i = k
                    n_dup += 1
                    continue
                n_dup += 1
            i = j
            continue
        outl.append(lines[i])
        i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, variant, "doubled:", n_dup, file=sys.stderr)
    sys.exit(0)

if variant == "double":
    # repeat each cache->layer draw block immediately (2nd draw = 2nd use of the cache texture)
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    outl, i, n_dup = lines[: frames[0] + 1], frames[0] + 1, 0
    while i < len(lines):
        n = fbid(lines[i])
        if i < end1 and is_layer(n):
            j = i + 1
            while j < end1 and fbid(lines[j]) is None:
                j += 1
            blk = lines[i:j]
            outl += blk
            if any(x.startswith("DRAW ") for x in blk):
                outl += blk
                n_dup += 1
            i = j
            continue
        outl.append(lines[i])
        i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, "layer draw blocks doubled:", n_dup, file=sys.stderr)
    sys.exit(0)

if variant == "clearlate":
    # Move each layer's setup block (BINDFB layer .. up to the cache's GENFB: viewport, uniforms, scissor,
    # CLEAR) down to just before the BINDFB layer that does the cache->layer DRAW, so the layer is never
    # cleared, left, and re-entered before it is drawn into.
    frames = [i for i, l in enumerate(lines) if l == "FRAME"]
    end1 = frames[1]
    seen = {}            # layer id -> count of BINDFB <layer> seen so far in frame 1
    moved, skip = {}, set()
    i = frames[0]
    while i < end1:
        n = fbid(lines[i])
        if is_layer(n):
            seen[n] = seen.get(n, 0) + 1
            if seen[n] == 2:     # setup block after the BINDFB 0 that follows creation
                j = i + 1
                while j < end1 and not lines[j].startswith("GENFB"):
                    j += 1
                moved[n] = lines[i:j]
                skip.update(range(i, j))
                i = j
                continue
        i += 1
    outl, seen = [], {}
    for i, l in enumerate(lines):
        if i in skip:
            continue
        n = fbid(l) if frames[0] <= i < end1 else None
        if is_layer(n):
            seen[n] = seen.get(n, 0) + 1
            if seen[n] == 2 and n in moved:   # now the 2nd surviving bind = the draw block
                outl += moved[n]
        outl.append(l)
    open(out, "w").write("\n".join(outl))
    print("wrote", out, "layer setup blocks moved:", len(moved), file=sys.stderr)
    sys.exit(0)

if variant in ("swapmid", "readmid"):
    # in place: before each cache->layer draw block, force an eglSwapBuffers (FRAME marker) so the
    # cache render and the layer draw are in different frames; "readmid" instead reads 1 px from
    # the window after a swap (not from the FBO, which hangs the GPU)
    first = next(i for i, l in enumerate(lines) if l == "FRAME")
    second = [i for i, l in enumerate(lines) if l == "FRAME"][1]
    outl, i = lines[: first + 1], first + 1
    n_ins = 0
    while i < len(lines):
        n = fbid(lines[i])
        if i < second and is_layer(n):
            j = i + 1
            while j < second and fbid(lines[j]) is None:
                j += 1
            if any(x.startswith("DRAW ") for x in lines[i:j]):
                outl.append("BINDFB 36160 0")
                if variant == "readmid":
                    outl.append("CLEAR 17408")  # real window work so the swap has a render to kick
                outl.append("FRAME")
                n_ins += 1
        outl.append(lines[i])
        i += 1
    open(out, "w").write("\n".join(outl))
    print("wrote", out, "swaps inserted:", n_ins, file=sys.stderr)
    sys.exit(0)

# collect layer blocks from frame 1 (everything before the second FRAME marker)
frame_idx = [i for i, l in enumerate(lines) if l == "FRAME"]
blocks, i = [], frame_idx[0]
while i < frame_idx[1]:
    n = fbid(lines[i])
    if is_layer(n):
        j = i + 1
        while j < frame_idx[1] and fbid(lines[j]) is None:
            j += 1
        blocks.append(lines[i:j])
        i = j
    else:
        i += 1
blocks = [b for b in blocks if any(x.startswith("DRAW ") for x in b)]  # only the cache->layer draws
print("layer blocks:", len(blocks), file=sys.stderr)

nsub = None
if variant.startswith("relayer") and variant[7:].isdigit() and len(variant) == 8 and variant[7] in "12456789":
    nsub, variant = int(variant[7]), "relayerN"
if variant == "relayerN":
    blocks = blocks[:nsub]
    print("re-drawing only the first", nsub, "layer block(s)", file=sys.stderr)

target = {"relayer": 1, "relayer3": 2, "relayerN": 1}[variant]   # index into frame_idx of the frame to patch
f = frame_idx[1 + target]
# insert after that frame's first CLEAR
k = f + 1
while not lines[k].startswith("CLEAR "):
    k += 1
ins = []
# the bake-frame vertex buffer is re-specified (smaller) at the end of frame 1, so restore the
# original one first or the replayed draws read past the end of the new buffer
bufline = next(l for l in lines if l.startswith("BUFDATA 34962 "))
ins += ["BINDBUF 34962 70001", bufline, "ENVA 0", "ENVA 1",
        "VAP 0 2 5126 0 16 0", "VAP 1 2 5126 0 16 8"]
for b in blocks:
    ins += b
ins += ["BINDFB 36160 0", "VIEWPORT 0 0 1024 600", "UNI2FV 1 1 44800000 44160000", "BLENDSEP 1 771 1 771"]
new = lines[: k + 1] + ins + lines[k + 1 :]
open(out, "w").write("\n".join(new))
print("wrote", out, "inserted", len(ins), "ops at line", k + 1, file=sys.stderr)
