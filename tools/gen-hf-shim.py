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

# Standard libm signatures for the reverse interposer (--imports). The DDK is
# softfp: it calls these with the base-AAPCS (soft-float) convention, so in a
# hard-float process they must not bind the system hard-float libm directly.
# Each wrapper is defined pcs("aapcs") (receives args soft-float) and forwards to
# the real hard-float libm via RTLD_NEXT.
_F1 = ("float", ["float"])
_F2 = ("float", ["float", "float"])
_D1 = ("double", ["double"])
_D2 = ("double", ["double", "double"])
LIBM_SIGS = {}
for _n in ("sinf cosf tanf asinf acosf atanf sinhf coshf tanhf expf exp2f logf "
           "log2f log10f sqrtf cbrtf floorf ceilf truncf roundf rintf nearbyintf "
           "fabsf expm1f log1pf").split():
    LIBM_SIGS[_n] = _F1
for _n in ("powf fmodf atan2f hypotf copysignf fdimf fmaxf fminf nextafterf "
           "remainderf").split():
    LIBM_SIGS[_n] = _F2
for _n in ("sin cos tan asin acos atan sinh cosh tanh exp exp2 log log2 log10 "
           "sqrt cbrt floor ceil trunc round rint nearbyint fabs expm1 log1p").split():
    LIBM_SIGS[_n] = _D1
for _n in ("pow fmod atan2 hypot copysign fdim fmax fmin nextafter remainder").split():
    LIBM_SIGS[_n] = _D2
LIBM_SIGS["frexpf"] = ("float", ["float", "int *"])
LIBM_SIGS["ldexpf"] = ("float", ["float", "int"])
LIBM_SIGS["scalbnf"] = ("float", ["float", "int"])
LIBM_SIGS["modff"] = ("float", ["float", "float *"])
LIBM_SIGS["frexp"] = ("double", ["double", "int *"])
LIBM_SIGS["ldexp"] = ("double", ["double", "int"])
LIBM_SIGS["scalbn"] = ("double", ["double", "int"])
LIBM_SIGS["modf"] = ("double", ["double", "double *"])



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


def emit(out, real_soname, headers, subprograms, types, shared_loader=False):
    w = out.write
    w("/* Generated by tools/gen-hf-shim.py — hard-float ABI shim. Do not edit. */\n")
    w("#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <stdio.h>\n"
      "#include <stdlib.h>\n")
    for h in headers:
        w("#include <%s>\n" % h)
    w('\n#define REAL_SONAME "%s"\n' % real_soname)
    w("#define PCS __attribute__((pcs(\"aapcs\")))\n\n")
    if shared_loader:
        # Resolve through the shared namespace loader (libsgxhf): the real DDK is
        # dlmopen'd into one isolated namespace with libSGXm interposition.
        w('extern void *sgxhf_dlsym(const char *soname, const char *sym);\n')
        w("#define ddk_sym(n) sgxhf_dlsym(REAL_SONAME, (n))\n\n")
    else:
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
    wrapped = set()
    for name in sorted(subprograms):
        ret_die, params = subprograms[name]
        # eglGetProcAddress returns a function pointer, so it can't be a plain
        # forwarding veneer: a raw DDK pointer would bypass ABI translation for
        # float-touching extension entry points. Instead return this shim's own
        # veneer for the name (dlsym RTLD_DEFAULT sees only the shim exports; the
        # real DDK is in a private dlmopen namespace), falling back to the DDK
        # for anything we did not wrap.
        if name == "eglGetProcAddress":
            w("__eglMustCastToProperFunctionPointerType "
              "eglGetProcAddress(const char *procname) {\n")
            w("    void *v = dlsym(RTLD_DEFAULT, procname);\n")
            w("    if (v) return (__eglMustCastToProperFunctionPointerType) v;\n")
            w("    typedef __eglMustCastToProperFunctionPointerType "
              "(*fn_t)(const char *);\n")
            w("    static fn_t real;\n")
            w('    if (!real) real = (fn_t) ddk_sym("eglGetProcAddress");\n')
            w("    return real(procname);\n}\n\n")
            emitted += 1
            wrapped.add(name)
            continue
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
        wrapped.add(name)

    return emitted, skipped, wrapped


def undefined_functions(elf):
    """Names of UNDEF (imported) STT_FUNC dynamic symbols."""
    names = set()
    for section in elf.iter_sections():
        if not isinstance(section, SymbolTableSection):
            continue
        if section.name != ".dynsym":
            continue
        for sym in section.iter_symbols():
            if sym.entry["st_info"]["type"] != "STT_FUNC":
                continue
            if sym.entry["st_shndx"] != "SHN_UNDEF":
                continue
            if sym.name:
                names.add(sym.name)
    return names


def emit_imports(out, imported):
    """Reverse interposer: soft-float pcs wrappers for imported libm functions."""
    w = out.write
    w("/* Generated by tools/gen-hf-shim.py --imports — libm reverse interposer. */\n")
    w("#define _GNU_SOURCE\n#include <dlfcn.h>\n\n"
      "#define PCS __attribute__((pcs(\"aapcs\")))\n\n")
    names = sorted(n for n in imported if n in LIBM_SIGS)
    for name in names:
        ret, ptypes = LIBM_SIGS[name]
        decls = ", ".join("%s a%d" % (t, i) for i, t in enumerate(ptypes))
        args = ", ".join("a%d" % i for i in range(len(ptypes)))
        tlist = ", ".join(ptypes)
        w("PCS %s %s(%s) {\n" % (ret, name, decls))
        w("    static %s (*real)(%s);\n" % (ret, tlist))
        w('    if (!real) real = (%s (*)(%s)) dlsym(RTLD_NEXT, "%s");\n'
          % (ret, tlist, name))
        w("    return real(%s);\n}\n\n" % args)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("solib")
    ap.add_argument("real_soname")
    ap.add_argument("out")
    ap.add_argument("--headers", default="")
    ap.add_argument("--imports", action="store_true",
                    help="emit a libm reverse interposer instead of a forward shim")
    ap.add_argument("--loader", action="store_true",
                    help="forward through the shared sgxhf_dlsym namespace loader")
    ap.add_argument("--manifest", default="",
                    help="write the exported-function ABI surface (one "
                         "'<F|.|?> name' per line) for the reuse coverage gate")
    args = ap.parse_args()
    headers = [h for h in args.headers.split(",") if h]

    if args.imports:
        import glob
        import os
        directory = os.path.dirname(args.solib) or "."
        imported = set()
        for so in sorted(glob.glob(os.path.join(directory, "*.so"))):
            with open(so, "rb") as f:
                imported |= undefined_functions(ELFFile(f))
        with open(args.out, "w") as out:
            names = emit_imports(out, imported)
        sys.stderr.write("%s/*.so: %d imported libm functions interposed: %s\n"
                         % (directory, len(names), " ".join(names)))
        return

    with open(args.solib, "rb") as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
            sys.exit("no DWARF in %s" % args.solib)
        dwarf = elf.get_dwarf_info()
        types = Types(dwarf)
        wanted = exported_functions(elf)
        subs = collect_subprograms(dwarf, types, wanted)
        with open(args.out, "w") as out:
            emitted, skipped, wrapped = emit(out, args.real_soname, headers,
                                             subs, types, shared_loader=args.loader)

    if args.manifest:
        # The ABI surface is every exported function, tagged F (float-touching,
        # needs pcs translation), . (wrapped, ABI-neutral) or ? (no DWARF / not
        # wrapped). The reuse coverage gate compares a stripped DDK's exports
        # against this surface to catch any symbol drift between DDK versions.
        def _tag(n):
            if n not in subs:
                return "?"
            if n not in wrapped:
                return "?"
            if types.is_float(subs[n][0]) or any(types.is_float(p[0])
                                                 for p in subs[n][1]):
                return "F"
            return "."
        with open(args.manifest, "w") as mf:
            for n in sorted(wanted):
                mf.write("%s %s\n" % (_tag(n), n))

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
