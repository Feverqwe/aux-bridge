#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

version=6.12.0
archive="qtbase-everywhere-src-$version.tar.xz"
sha256=a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307
url="https://download.qt.io/official_releases/qt/6.12/$version/submodules/$archive"
base="$(pwd -P)/.local/qt"
prefix="$base/$version"

if [ -x "$prefix/bin/macdeployqt" ]; then
    printf '%s\n' "$prefix"
    exit 0
fi

if [ "${1:-}" = "--installed" ]; then
    exit 1
fi

mkdir -p "$base"
scratch=$(mktemp -d "$base/.build.XXXXXXXX")
trap 'rm -rf "$scratch"' EXIT
if [ -f "$base/$archive" ]; then
    cp "$base/$archive" "$scratch/$archive"
else
    curl --fail --location --retry 2 --output "$scratch/$archive" "$url"
fi
(cd "$scratch" && printf '%s  %s\n' "$sha256" "$archive" | shasum -a 256 -c >&2)
tar -xJf "$scratch/$archive" -C "$scratch"

# The standalone Command Line Tools provide the SDK without xcodebuild.
if ! xcodebuild -version >/dev/null 2>&1; then
    set -- "$@" -DQT_NO_XCODE_MIN_VERSION_CHECK=ON
fi
sdk=$(xcrun --sdk macosx --show-sdk-path)
if [ -f "$sdk/usr/include/c++/v1/expected" ]; then
    set -- "$@" "-DCMAKE_CXX_FLAGS=-stdlib++-isystem '$sdk/usr/include/c++/v1'"
    set -- "$@" "-DCMAKE_OBJCXX_FLAGS=-stdlib++-isystem '$sdk/usr/include/c++/v1'"
fi

cmake -S "$scratch/qtbase-everywhere-src-$version" -B "$scratch/build" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4 -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DFEATURE_sql=OFF -DFEATURE_dbus=OFF -DFEATURE_opengl=OFF -DFEATURE_printsupport=OFF \
    -DFEATURE_icu=OFF -DFEATURE_openssl=OFF -DFEATURE_zstd=OFF -DFEATURE_brotli=OFF "$@" >&2
cmake --build "$scratch/build" --parallel "$(sysctl -n hw.ncpu)" >&2
DESTDIR="$scratch/install" cmake --install "$scratch/build" >&2
mkdir -p "$scratch/install$prefix/share/qtbase"
cp -R "$scratch/qtbase-everywhere-src-$version/LICENSES" "$scratch/install$prefix/share/qtbase/"
mv "$scratch/install$prefix" "$prefix"
printf '%s\n' "$prefix"
