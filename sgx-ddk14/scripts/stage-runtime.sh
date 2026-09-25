#!/bin/sh
set -eu

: "${SDK_ROOT:?SDK_ROOT is required}"
: "${RUNTIME_DESTDIR:?RUNTIME_DESTDIR is required}"
: "${TOOLS_DESTDIR:?TOOLS_DESTDIR is required}"

runtime_path_file="$SDK_ROOT/runtime.path"
[ -f "$runtime_path_file" ] || {
    echo "missing $runtime_path_file" >&2
    exit 2
}
runtime=$(cat "$runtime_path_file")
[ -d "$runtime" ] || {
    echo "runtime not found: $runtime" >&2
    exit 2
}

prefix=/opt/sgx-ddk14
runtime_lib="$RUNTIME_DESTDIR$prefix/lib"
tools_bin="$TOOLS_DESTDIR$prefix/bin"

rm -rf "$RUNTIME_DESTDIR" "$TOOLS_DESTDIR"
mkdir -p "$runtime_lib" "$TOOLS_DESTDIR/usr/bin" "$tools_bin"

# The DDK 1.4 (Graphics SDK 4.00.00.01) gfx_rel_es2.x library set differs
# slightly from 1.6 (adds X11 WSEGL, drops libusc). Require the GL core and
# install the remaining libraries when present so packaging survives the delta.
required_libraries='libEGL.so libGLESv2.so libIMGegl.so libsrv_um.so
libpvrPVR2D_FLIPWSEGL.so'

optional_libraries='libGLES_CM.so libglslcompiler.so libOpenVG.so libOpenVGU.so
libpvr2d.so libsrv_init.so libsrv_um_dri.so libusc.so libPVRScopeServices.so
libpvrPVR2D_BLITWSEGL.so libpvrPVR2D_FRONTWSEGL.so libpvrPVR2D_LINUXFBWSEGL.so
libpvrPVR2D_X11WSEGL.so libpvrPVR2D_DRIWSEGL.so'

required_tools='pvrsrvinit'

optional_tools='eglinfo gles1test1 gles1_texture_stream gles2test1
gles2_texture_stream ovg_unit_test pvr2d_test services_test sgx_blit_test
sgx_clipblit_test sgx_flip_test sgx_init_test sgx_render_flip_test'

optional_shaders='glsltest1_vertshader.txt glsltest1_fragshaderA.txt
glsltest1_fragshaderB.txt'

for library in $required_libraries; do
    [ -f "$runtime/$library" ] || {
        echo "runtime is missing required library $library" >&2
        exit 1
    }
    install -m 0644 "$runtime/$library" "$runtime_lib/$library"
done

for library in $optional_libraries; do
    [ -f "$runtime/$library" ] || continue
    install -m 0644 "$runtime/$library" "$runtime_lib/$library"
done

for tool in $required_tools; do
    [ -f "$runtime/$tool" ] || {
        echo "runtime is missing required tool $tool" >&2
        exit 1
    }
    install -m 0755 "$runtime/$tool" "$tools_bin/$tool"
done

for tool in $optional_tools; do
    [ -f "$runtime/$tool" ] || continue
    install -m 0755 "$runtime/$tool" "$tools_bin/$tool"
done

for shader in $optional_shaders; do
    [ -f "$runtime/$shader" ] || continue
    install -m 0644 "$runtime/$shader" "$tools_bin/$shader"
done

mkdir -p "$RUNTIME_DESTDIR/etc"
cat > "$RUNTIME_DESTDIR/etc/powervr.ini" <<'EOF'
[default]
WindowSystem=libpvrPVR2D_FLIPWSEGL.so
EOF

# The 1.4 dc_nohw display class is a plain buffer provider; the paced present=2
# integration is 1.6-only, so no dcnohw modprobe options are shipped here yet.

cat > "$TOOLS_DESTDIR/usr/bin/sgx-ddk14-run" <<'EOF'
#!/bin/sh
set -eu

invocation=${0##*/}
if [ "$invocation" = sgx-ddk14-run ]; then
    [ "$#" -gt 0 ] || {
        echo 'usage: sgx-ddk14-run PROGRAM [ARG ...]' >&2
        exit 2
    }
    case "$1" in
        */*) program=$1 ;;
        *) program=/opt/sgx-ddk14/bin/$1 ;;
    esac
    shift
else
    program=/opt/sgx-ddk14/bin/$invocation
fi

exec /lib/ld-linux.so.3 \
    --library-path /opt/sgx-ddk14/lib \
    "$program" "$@"
EOF
chmod 0755 "$TOOLS_DESTDIR/usr/bin/sgx-ddk14-run"
ln -s sgx-ddk14-run "$TOOLS_DESTDIR/usr/bin/pvrsrvinit"
