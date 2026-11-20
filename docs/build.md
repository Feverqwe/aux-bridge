# Build Aux Bridge

Requires Go 1.24 or later, pkg-config, and an Apple Clang toolchain with C++23 library support. Install CMake and Ninja to build Qt from source.

Run these commands from the repository root. The first command downloads and builds the pinned Qt release once. Later builds reuse it.

```sh
./scripts/prepare-qt.sh
./build.sh
./scripts/run.sh
```

If you already have Qt 6.12 Widgets or later, build with `QT_PREFIX="<Qt prefix>" ./build.sh`. Pass `--deploy` to include the Qt runtime in the app bundle.

## Checks and releases

Run `./scripts/test-audio.sh` to check buffering and playback across sample rates without audio hardware. Test the actual bridge with the input and output you intend to use.

Run `./scripts/release.sh --archive` to package the current build. For a GitHub release, update the version in `Info.plist`, commit the release, run `./scripts/release.sh --tag`, and push the tag. GitHub Actions runs only when you push a version tag. It builds the Apple Silicon ZIP and attaches it to the release. The ZIP contains an ad-hoc signed build for macOS 14.4 or later.
