#!/usr/bin/env python3
"""Generate a hard-float ABI shim for a soft-float (softfp) DDK library.

The TI DDK 1.6 EGL/GLES libraries are softfp: VFP is used for computation, but
scalar float/double arguments cross function boundaries in core registers
(base AAPCS), not VFP registers. A hard-float (armhf/AAPCS-VFP) caller therefore
passes floats in the wrong registers. Because TI shipped the libraries with full
DWARF debug info, every exported function's exact signature is recoverable, so we
can emit a per-function veneer that GCC ABI-translates for free.

For each exported function this emits a hard-float wrapper of the same name that
forwards to the real soft-float symbol (resolved via dlopen/dlsym). Functions
that take or return float/double get __attribute__((pcs("aapcs"))) on the
forwarding pointer, so GCC moves those scalars between VFP and core registers.
ABI-neutral functions (pointer/integer only) forward directly.

Usage:
    gen-hf-shim.py <soft-float.so> <real_soname> <out.c> [--headers h1,h2]

The generated .c #includes the given headers so the vendor typedefs
(GLfloat, EGLDisplay, ...) resolve; compile it hard-float:
    arm-linux-gnueabihf-gcc -shared -fPIC -I<ddk headers> out.c -ldl -o libX_hf.so
"""
import argparse
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

DW_ATE_float = 0x04


def exported_functions(elf):
    """Names of defined (non-UNDEF) global/weak STT_FUNC dynamic symbols."""
    names = set()
    for section in elf.iter_sections():
        if not isinstance(section, SymbolTableSection):
            continue
        if section.name != ".dynsym":
            continue
        for sym in section.iter_symbols():
            info = sym.entry["st_info"]
            if info["type"] != "STT_FUNC":
                continue
            if info["bind"] not in ("STB_GLOBAL", "STB_WEAK"):
                continue
            if sym.entry["st_shndx"] == "SHN_UNDEF":
                continue
            if sym.name:
                names.add(sym.name)
    return names


class Types:
    """DWARF type resolver over a single .debug_info."""

    def __init__(self, dwarf):
        self.dwarf = dwarf
        self.by_offset = {}
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                self.by_offset[die.offset] = die

    def _target(self, die):
        attr = die.attributes.get("DW_AT_type")
        if attr is None:
            return None
        return self.by_offset.get(attr.value + die.cu.cu_offset) or \
            self.by_offset.get(attr.value)

    def _name(self, die):
        attr = die.attributes.get("DW_AT_name")
        if attr is None:
            return None
        value = attr.value
        return value.decode() if isinstance(value, bytes) else value

    def render(self, die):
        """A C type string usable as a prefix before a declarator name."""
        if die is None:
            return "void"
        tag = die.tag
        if tag == "DW_TAG_base_type":
            return self._name(die) or "int"
        if tag == "DW_TAG_typedef":
            return self._name(die) or self.render(self._target(die))
        if tag == "DW_TAG_pointer_type":
            return self.render(self._target(die)) + " *"
        if tag == "DW_TAG_const_type":
            return "const " + self.render(self._target(die))
        if tag == "DW_TAG_volatile_type":
            return "volatile " + self.render(self._target(die))
        if tag == "DW_TAG_enumeration_type":
            return self._name(die) and ("enum " + self._name(die)) or "int"
        if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            kind = "struct" if tag == "DW_TAG_structure_type" else "union"
            name = self._name(die)
            # by-value aggregates are out of scope for the spike
            return (kind + " " + name) if name else None
        return "int"

    def is_float(self, die):
        """True if the type is (a typedef/qualifier over) float or double."""
        seen = 0
        while die is not None and seen < 32:
            seen += 1
            tag = die.tag
            if tag == "DW_TAG_base_type":
                enc = die.attributes.get("DW_AT_encoding")
                return bool(enc and enc.value == DW_ATE_float)
            if tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                       "DW_TAG_volatile_type"):
                die = self._target(die)
                continue
            return False
        return False

    def has_fn_pointer(self, die):
        """Detect function-pointer params (callbacks) — unsupported in spike."""
        seen = 0
        while die is not None and seen < 32:
            seen += 1
            if die.tag == "DW_TAG_pointer_type":
                target = self._target(die)
                if target is not None and target.tag == "DW_TAG_subroutine_type":
                    return True
                die = target
                continue
            if die.tag in ("DW_TAG_typedef", "DW_TAG_const_type",
                           "DW_TAG_volatile_type"):
                die = self._target(die)
                continue
            return False
        return False


def collect_subprograms(dwarf, types, wanted):
    """Map exported name -> (return_die, [(ptype_die, pname), ...])."""
    found = {}
    for cu in dwarf.iter_CUs():
        for die in cu.iter_DIEs():
            if die.tag != "DW_TAG_subprogram":
                continue
            name_attr = die.attributes.get("DW_AT_name")
            if name_attr is None:
                continue
            name = name_attr.value
            name = name.decode() if isinstance(name, bytes) else name
            if name not in wanted or name in found:
                continue
            if "DW_AT_declaration" in die.attributes:
                continue
            params = []
            for child in die.iter_children():
                if child.tag != "DW_TAG_formal_parameter":
                    continue
                pname_attr = child.attributes.get("DW_AT_name")
                pname = pname_attr.value if pname_attr else None
                if isinstance(pname, bytes):
                    pname = pname.decode()
                params.append((types._target(child), pname))
            found[name] = (types._target(die), params)
    return found


def emit(out, real_soname, headers, subprograms, types):
    w = out.write
    w("/* Generated by tools/gen-hf-shim.py — hard-float ABI shim. Do not edit. */\n")
    w("#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <stdio.h>\n"
      "#include <stdlib.h>\n")
    for h in headers:
        w("#include <%s>\n" % h)
    w('\n#define REAL_SONAME "%s"\n' % real_soname)
    w("#define PCS __attribute__((pcs(\"aapcs\")))\n\n")
    w("static void *ddk_handle;\n"
      "static void *ddk_sym(const char *n) {\n"
      "    if (!ddk_handle) {\n"
      "        ddk_handle = dlopen(REAL_SONAME, RTLD_NOW | RTLD_LOCAL);\n"
      "        if (!ddk_handle) { fprintf(stderr, \"hf-shim: dlopen %s: %s\\n\","
      " REAL_SONAME, dlerror()); abort(); }\n"
      "    }\n"
      "    void *p = dlsym(ddk_handle, n);\n"
      "    if (!p) { fprintf(stderr, \"hf-shim: missing %s\\n\", n); abort(); }\n"
      "    return p;\n}\n\n")

    emitted = 0
    skipped = []
    for name in sorted(subprograms):
        ret_die, params = subprograms[name]
        if types.has_fn_pointer(ret_die):
            skipped.append((name, "function-pointer return"))
            continue
        ret = types.render(ret_die)
        if ret is None:
            skipped.append((name, "aggregate return"))
            continue
        ptypes = []
        pdecls = []
        args = []
        bad = None
        for i, (pdie, pname) in enumerate(params):
            if types.has_fn_pointer(pdie):
                bad = "callback param"
                break
            t = types.render(pdie)
            if t is None:
                bad = "aggregate param"
                break
            a = pname or ("a%d" % i)
            ptypes.append(t)
            pdecls.append("%s %s" % (t, a))
            args.append(a)
        if bad:
            skipped.append((name, bad))
            continue

        float_touching = types.is_float(ret_die) or \
            any(types.is_float(p[0]) for p in params)
        pcs = " PCS" if float_touching else ""
        proto_params = ", ".join(pdecls) if pdecls else "void"
        ptype_list = ", ".join(ptypes) if ptypes else "void"
        call_args = ", ".join(args)
        ret_kw = "" if ret == "void" else "return "

        w("%s %s(%s) {\n" % (ret, name, proto_params))
        w("    typedef %s (*fn_t)(%s)%s;\n" % (ret, ptype_list, pcs))
        w("    static fn_t real;\n")
        w('    if (!real) real = (fn_t) ddk_sym("%s");\n' % name)
        w("    %sreal(%s);\n}\n\n" % (ret_kw, call_args))
        emitted += 1

    return emitted, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("solib")
    ap.add_argument("real_soname")
    ap.add_argument("out")
    ap.add_argument("--headers", default="")
    args = ap.parse_args()
    headers = [h for h in args.headers.split(",") if h]

    with open(args.solib, "rb") as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
            sys.exit("no DWARF in %s" % args.solib)
        dwarf = elf.get_dwarf_info()
        types = Types(dwarf)
        wanted = exported_functions(elf)
        subs = collect_subprograms(dwarf, types, wanted)
        with open(args.out, "w") as out:
            emitted, skipped = emit(out, args.real_soname, headers, subs, types)

    float_n = sum(
        1 for n in subs
        if types.is_float(subs[n][0]) or any(types.is_float(p[0]) for p in subs[n][1])
    )
    sys.stderr.write(
        "%s: %d exports, %d with DWARF, %d emitted (%d float-touching), "
        "%d skipped\n" % (args.solib, len(wanted), len(subs), emitted,
                          float_n, len(skipped)))
    for name, why in skipped:
        sys.stderr.write("  skip %s (%s)\n" % (name, why))


if __name__ == "__main__":
    main()
