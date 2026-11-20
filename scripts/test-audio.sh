#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sdk=$(xcrun --sdk macosx --show-sdk-path)
if [ -f "$sdk/usr/include/c++/v1/expected" ]; then
    set -- -stdlib++-isystem "$sdk/usr/include/c++/v1"
else
    set --
fi
xcrun clang++ -std=c++23 -Wall -Wextra -O2 "$@" tests/audio_queue.cpp -o "$tmp/audio-queue"
"$tmp/audio-queue"
