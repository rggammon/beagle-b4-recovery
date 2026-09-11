#!/bin/sh
set -eu

: "${SDK_ROOT:?SDK_ROOT is required}"
: "${DESTDIR:?DESTDIR is required}"

sdk_linux=$(find "$SDK_ROOT/install" -type d -name GFX_Linux_SDK -print -quit)
[ -n "$sdk_linux" ] || {
    echo 'SDK does not contain GFX_Linux_SDK' >&2
    exit 1
}
sdk=${sdk_linux%/GFX_Linux_SDK}
include="$DESTDIR/usr/include/sgx-ddk16"
pkgconfig="$DESTDIR/usr/lib/arm-linux-gnueabi/pkgconfig"

rm -rf "$DESTDIR"
mkdir -p "$include/GLES" "$include/GLES2" "$pkgconfig"

copy_headers()
{
    source=$1
    destination=$2
    [ -d "$source" ] || return 0
    cp -a "$source"/. "$destination"/
}

copy_top_level_headers()
{
    source=$1
    destination=$2
    [ -d "$source" ] || return 0
    for header in "$source"/*.h; do
        [ -f "$header" ] || continue
        cp -a "$header" "$destination"/
    done
}

copy_headers \
    "$sdk/GFX_Linux_SDK/OGLES2/SDKPackage/Builds/OGLES2/Include" \
    "$include"
copy_headers \
    "$sdk/GFX_Linux_SDK/OGLES/SDKPackage/Builds/OGLES/Include" \
    "$include"
copy_headers \
    "$sdk/GFX_Linux_SDK/OGLES/SDKPackage/Builds/OGLES/LinuxOMAP3/Include/GLES" \
    "$include/GLES"
copy_headers \
    "$sdk/GFX_Linux_SDK/OGLES2/SDKPackage/Builds/OGLES2/LinuxOMAP3/Include/GLES" \
    "$include/GLES2"
copy_top_level_headers "$sdk/include" "$include"
copy_top_level_headers "$sdk/include/wsegl" "$include"

find "$include" -type f ! -name '*.h' -delete

[ -f "$include/EGL/egl.h" ] || {
    echo 'development tree is missing EGL/egl.h' >&2
    exit 1
}
[ -f "$include/GLES2/gl2.h" ] || {
    echo 'development tree is missing GLES2/gl2.h' >&2
    exit 1
}

cat > "$pkgconfig/egl.pc" <<'EOF'
prefix=/usr
exec_prefix=${prefix}
libdir=/opt/sgx-ddk16/lib
includedir=${prefix}/include/sgx-ddk16

Name: EGL
Description: TI SGX DDK 1.6 EGL soft-float library
Version: 1.4
Libs: -L${libdir} -lEGL
Cflags: -I${includedir}
EOF

cat > "$pkgconfig/glesv2.pc" <<'EOF'
prefix=/usr
exec_prefix=${prefix}
libdir=/opt/sgx-ddk16/lib
includedir=${prefix}/include/sgx-ddk16

Name: GLESv2
Description: TI SGX DDK 1.6 OpenGL ES 2 soft-float library
Version: 2.0
Libs: -L${libdir} -lGLESv2
Cflags: -I${includedir}
EOF