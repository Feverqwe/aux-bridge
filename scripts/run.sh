#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

app="build/Aux Bridge.app"
if [ ! -d "$app" ]; then
    echo "Build Aux Bridge with ./build.sh first." >&2
    exit 1
fi
open "$app"
