#!/usr/bin/env python3
"""Delta-debug a recorded GL stream on the B4 until it is 1-minimal for the differential:

    base stream  -> window stays black (final lit == 0, no GPU fault)
    + resizebefore (gl-stream-variant.py) -> window lit (final lit > MINLIT, no GPU fault)

usage: gl-ddmin.py <in.txt> <blob.bin> <out.txt> [max_tests]

FRAME / END markers are never removed. Candidates that crash, fault or fail to generate count as 'not reproducing'.
Needs on the board: /root/sweep.sh (prints 'frames=[..] final=N hwerr=N'), /root/gl-replay.
"""
import os
import re
import subprocess
import sys
import tempfile

SRC, BLOB, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
MAX_TESTS = int(sys.argv[4]) if len(sys.argv) > 4 else 120
HOST = "root@192.168.50.245"
MINLIT = 5000
VARIANT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gl-stream-variant.py")
SSH = ["-i", os.path.expanduser("~/.ssh/geoduck_truenas"), "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=NUL", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=15",
       "-o", "ServerAliveInterval=15"]
tmp = tempfile.mkdtemp(prefix="ddmin-")
tests = 0


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def sweep_line(out, label):
    for l in out.split("\n"):
        if l.startswith(label):
            m = re.search(r"frames=\[(.*?)\] final=(\d*) hwerr=(\d+)", l)
            if m:
                return m.group(1), m.group(2), int(m.group(3))
    return None


def reproduces(lines):
    """True if the differential holds for this candidate."""
    global tests
    tests += 1
    base = os.path.join(tmp, "c.txt")
    rb = os.path.join(tmp, "c-rb.txt")
    open(base, "w").write("\n".join(lines))
    g = run([sys.executable, VARIANT, base, rb, "resizebefore"])
    if g.returncode != 0 or not os.path.exists(rb):
        return False
    r = run(["scp", "-q"] + SSH + [base, rb, f"{HOST}:/root/"])
    if r.returncode != 0:
        print("  scp failed", r.stderr.strip())
        return False
    remote = ("B=/root/ddmin.bin; "
              "BLOB=$B LOG=/root/c.txt /root/sweep.sh BASE; "
              "BLOB=$B LOG=/root/c-rb.txt DUMP=/root/c-rb.raw /root/sweep.sh RB")
    r = run(["ssh"] + SSH + [HOST, remote], timeout=240)
    b, c = sweep_line(r.stdout, "BASE"), sweep_line(r.stdout, "RB")
    if not b or not c:
        return False
    ok_base = b[0] != "" and b[1] == "0" and b[2] == 0
    if not (ok_base and c[1] != "" and int(c[1]) > MINLIT and c[2] == 0):
        return False
    return tile_ok(os.path.join(tmp, "c-rb.raw"))


def tile_ok(path):
    """Window = one background colour plus ONE tile-sized block of a single colour (exact 150x96-ish)."""
    r = run(["scp", "-q"] + SSH + [f"{HOST}:/root/c-rb.raw", path])
    if r.returncode != 0 or not os.path.exists(path):
        return False
    raw = open(path, "rb").read()
    from collections import Counter
    cnt = Counter(raw[i:i + 4] for i in range(0, len(raw), 4))
    bg = cnt.most_common(1)[0][0]
    others = [(k, v) for k, v in cnt.most_common() if k != bg]
    if not others:
        return False
    colour, n = others[0]
    non_bg = sum(v for _, v in others)
    return 13000 <= n <= 14500 and non_bg <= 14500 and max(colour[:3]) > 60


def ddmin(lines, removable):
    """Classic ddmin over the removable indices (complement testing)."""
    keep = list(removable)
    n = 2
    while len(keep) >= 2 and tests < MAX_TESTS:
        chunk = max(1, len(keep) // n)
        subsets = [keep[i:i + chunk] for i in range(0, len(keep), chunk)]
        reduced = False
        for sub in subsets:
            comp = [i for i in keep if i not in set(sub)]
            cand = [l for i, l in enumerate(lines) if i not in removable or i in set(comp)]
            if reproduces(cand):
                keep = comp
                n = max(n - 1, 2)
                reduced = True
                print(f"[{tests}] removed {len(sub)} -> {len(cand)} ops", flush=True)
                open(OUT, "w").write("\n".join(cand))
                break
            if tests >= MAX_TESTS:
                break
        if not reduced:
            if n >= len(keep):
                break
            n = min(len(keep), n * 2)
    return keep


lines = open(SRC).read().split("\n")
subprocess.run(["scp", "-q"] + SSH + [BLOB, f"{HOST}:/root/ddmin.bin"], check=True)
print("input ops", len(lines))
assert reproduces(lines), "input stream does not show the differential"
print("input verified; reducing", flush=True)
# structural ops are never removed: dropping them faults the GPU (clears, FBO/RB setup, draws), breaks setup, or
# changes what is sampled (texture binds/params; a missing MIN_FILTER makes a texture incomplete = samples black)
PROTECTED = re.compile(r"^(FRAME|END|GEN\w*|BINDFB|BINDRB|RBSTORE|FBRB|FBTEX|CHECKFB|CLEAR |DRAW|TEXIMG|CREATESHADER|"
                       r"SHADERSRC|COMPILE|CREATEPROG|ATTACH|BINDATTR|LINK|DETACH|DELSHADER|BUFDATA|BUFSUB|BINDBUF|"
                       r"VAP|ENVA|DISVA|USEPROG|ACTIVETEX|BINDTEX|TEXPARAM|UNI1I|UNI2FV|UNI4FV|VIEWPORT|GETUNI)")
removable = {i for i, l in enumerate(lines) if not PROTECTED.match(l)}
print("removable ops", len(removable), flush=True)
keep = ddmin(lines, removable)
final = [l for i, l in enumerate(lines) if i not in removable or i in set(keep)]
open(OUT, "w").write("\n".join(final))
print(f"done after {tests} tests: {len(final)} ops -> {OUT}")
