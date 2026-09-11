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

prefix=/opt/sgx-ddk16
runtime_lib="$RUNTIME_DESTDIR$prefix/lib"
tools_bin="$TOOLS_DESTDIR$prefix/bin"

rm -rf "$RUNTIME_DESTDIR" "$TOOLS_DESTDIR"
mkdir -p "$runtime_lib" "$RUNTIME_DESTDIR/etc/modprobe.d" "$tools_bin" \
    "$TOOLS_DESTDIR/usr/bin"

runtime_libraries='libEGL.so libGLES_CM.so libGLESv2.so libglslcompiler.so
libIMGegl.so libOpenVG.so libOpenVGU.so libpvr2d.so
libpvrPVR2D_BLITWSEGL.so libpvrPVR2D_FLIPWSEGL.so
libpvrPVR2D_FRONTWSEGL.so libpvrPVR2D_LINUXFBWSEGL.so
libPVRScopeServices.so libsrv_init.so libsrv_um.so libusc.so'

tools='eglinfo gles1test1 gles1_texture_stream gles2test1
gles2_texture_stream ovg_unit_test pvr2d_test pvrsrvinit services_test
sgx_blit_test sgx_clipblit_test sgx_flip_test sgx_init_test
sgx_render_flip_test'

for library in $runtime_libraries; do
    [ -f "$runtime/$library" ] || {
        echo "runtime is missing $library" >&2
        exit 1
    }
    install -m 0644 "$runtime/$library" "$runtime_lib/$library"
done

for tool in $tools; do
    [ -f "$runtime/$tool" ] || {
        echo "runtime is missing $tool" >&2
        exit 1
    }
    install -m 0755 "$runtime/$tool" "$tools_bin/$tool"
done

for shader in glsltest1_vertshader.txt glsltest1_fragshaderA.txt \
    glsltest1_fragshaderB.txt; do
    install -m 0644 "$runtime/$shader" "$tools_bin/$shader"
done

cat > "$RUNTIME_DESTDIR/etc/powervr.ini" <<'EOF'
[default]
WindowSystem=libpvrPVR2D_FLIPWSEGL.so
EOF

cat > "$RUNTIME_DESTDIR/etc/modprobe.d/sgx-ddk16.conf" <<'EOF'
options dcnohw present=2
EOF

cat > "$TOOLS_DESTDIR/usr/bin/sgx-ddk16-run" <<'EOF'
#!/bin/sh
set -eu

invocation=${0##*/}
if [ "$invocation" = sgx-ddk16-run ]; then
    [ "$#" -gt 0 ] || {
        echo 'usage: sgx-ddk16-run PROGRAM [ARG ...]' >&2
        exit 2
    }
    case "$1" in
        */*) program=$1 ;;
        *) program=/opt/sgx-ddk16/bin/$1 ;;
    esac
    shift
else
    program=/opt/sgx-ddk16/bin/$invocation
fi

exec /lib/ld-linux.so.3 \
    --library-path /opt/sgx-ddk16/lib \
    "$program" "$@"
EOF
chmod 0755 "$TOOLS_DESTDIR/usr/bin/sgx-ddk16-run"
ln -s sgx-ddk16-run "$TOOLS_DESTDIR/usr/bin/pvrsrvinit"
