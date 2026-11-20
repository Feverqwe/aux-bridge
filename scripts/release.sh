#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Info.plist)
tag="v$version"
if ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "Set a release version in Info.plist first." >&2
    exit 1
fi

case "${1:-}" in
    --archive)
        if [ "$#" -gt 2 ] || { [ "$#" -eq 2 ] && [ "$2" != "$tag" ]; }; then
            echo "The release tag must match Info.plist ($tag)." >&2
            exit 1
        fi
        ./build.sh --deploy
        archive="build/aux-bridge-$tag-macos-$(uname -m).zip"
        stage=$(mktemp -d "build/.release.XXXXXXXX")
        trap 'rm -rf "$stage"' EXIT
        ditto -c -k --sequesterRsrc --keepParent 'build/Aux Bridge.app' "$stage/app.zip"
        mv "$stage/app.zip" "$archive"
        printf '%s\n' "$archive"
        ;;
    --tag)
        if [ "$#" -ne 1 ]; then
            exit 2
        fi
        if [ -n "$(git status --porcelain)" ]; then
            echo "Commit the release changes before tagging." >&2
            exit 1
        fi
        git tag -a "$tag" -m "Aux Bridge $version"
        printf 'Created %s. Publish with git push origin %s\n' "$tag" "$tag"
        ;;
    *)
        echo "Usage: $0 --archive [v<version>] | --tag" >&2
        exit 2
        ;;
esac
