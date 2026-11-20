# Aux Bridge

Aux Bridge is a macOS utility for listening to an audio input through a selected output. It can route a monitor's AUX signal from a USB audio interface to a Bluetooth speaker without changing the Mac's system output.

## Run

1. Connect the input and output so they appear in macOS Sound settings.
2. Open Aux Bridge and choose the devices.
3. Click **Start** and allow microphone access when macOS asks. The same permission covers external audio inputs.
4. Click **Stop** to end playback. Closing the window leaves Aux Bridge in the menu bar, where you can reopen it or quit.

The bridge targets about 2 ms of internal buffering. It can temporarily request a shorter capture period when the input is idle and supports it. The displayed queue is an estimate for the application only. Device and Bluetooth latency contribute separately, and the result depends on the selected route. Start with the speaker at low volume when using a room microphone.

## Build

Requires macOS 14.4 or later, Go 1.24 or later, pkg-config, and an Apple Clang toolchain with C++23 library support. Install CMake and Ninja to build Qt from source.

Run these commands from the repository root. The first command downloads and builds the pinned Qt release once. Later builds reuse it.

```sh
./scripts/prepare-qt.sh
./build.sh
./scripts/run.sh
```

If you already have Qt 6.12 Widgets or later, build with `QT_PREFIX="<Qt prefix>" ./build.sh`. Pass `--deploy` to include the Qt runtime in the app bundle.

## Checks and releases

Run `./scripts/test-audio.sh` to check buffering and playback across sample rates without audio hardware. Test the actual bridge with the input and output you intend to use.

Run `./scripts/release.sh --archive` to package the current build. For a GitHub release, update the version in `Info.plist`, commit the release, run `./scripts/release.sh --tag`, and push the tag. GitHub Actions builds the Apple Silicon ZIP and attaches it to the release. The ZIP contains an ad-hoc signed build for macOS 14.4 or later.
