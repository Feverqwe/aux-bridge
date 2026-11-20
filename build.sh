#!/bin/sh
set -eu

cd "$(dirname "$0")"

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "--deploy" ]; }; then
    echo "Usage: $0 [--deploy]" >&2
    exit 2
fi

if [ -z "${QT_PREFIX:-}" ]; then
    QT_PREFIX=$(./scripts/prepare-qt.sh --installed) || QT_PREFIX=
fi
if [ -n "$QT_PREFIX" ]; then
    export PKG_CONFIG_PATH="$QT_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
if ! pkg-config --atleast-version=6.12 Qt6Widgets; then
    echo "Qt 6.12 Widgets or later is required. Run ./scripts/prepare-qt.sh or set QT_PREFIX." >&2
    exit 1
fi
qt_libdir=$(pkg-config --variable=libdir Qt6Widgets)
qt_bindir=$(pkg-config --variable=bindir Qt6Widgets)

# Some standalone Apple toolchains put libc++ headers only in the macOS SDK.
sdk=$(xcrun --sdk macosx --show-sdk-path)
if [ -f "$sdk/usr/include/c++/v1/expected" ]; then
    export CGO_CXXFLAGS="${CGO_CXXFLAGS:--O2 -g} -stdlib++-isystem '$sdk/usr/include/c++/v1'"
fi

export CGO_ENABLED=1
export MACOSX_DEPLOYMENT_TARGET=14.4
export CGO_LDFLAGS="${CGO_LDFLAGS:--O2 -g} -Wl,-headerpad_max_install_names"

app="build/Aux Bridge.app"
mkdir -p build
staging=$(mktemp -d "build/.bundle.XXXXXXXX")
trap 'rm -rf "$staging"' EXIT
bundle="$staging/Aux Bridge.app"

mkdir -p "$bundle/Contents/MacOS"
cp Info.plist "$bundle/Contents/Info.plist"
binary="$bundle/Contents/MacOS/Aux Bridge"
go build -trimpath -o "$binary" ./src
install_name_tool -add_rpath "$qt_libdir" "$binary"

if [ "${1:-}" = "--deploy" ]; then
    resources="$bundle/Contents/Resources"
    mkdir -p "$resources"
    cp THIRD_PARTY_NOTICES.md "$resources/"
    qt_licenses="$(pkg-config --variable=prefix Qt6Widgets)/share/qtbase/LICENSES"
    if [ -d "$qt_licenses" ]; then
        cp -R "$qt_licenses" "$resources/Qt-LICENSES"
    fi
    "$qt_bindir/macdeployqt" "$bundle" -no-strip
    if otool -l "$binary" | grep -Fq "path $qt_libdir (offset"; then
        install_name_tool -delete_rpath "$qt_libdir" "$binary"
    fi
fi
codesign --force --sign - "$bundle"
if [ -e "$app" ]; then
    mv "$app" "$staging/previous"
fi
if ! mv "$bundle" "$app"; then
    if [ -e "$staging/previous" ]; then
        mv "$staging/previous" "$app"
    fi
    exit 1
fi
echo "$app"
