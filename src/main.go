package main

/*
#cgo pkg-config: Qt6Widgets
#cgo CXXFLAGS: -std=c++23 -Wall -Wextra
#cgo LDFLAGS: -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -framework AVFoundation -framework Foundation
#include <stdlib.h>
#include "launcher.h"
*/
import "C"

import (
	"os"
	"runtime"
	"unsafe"
)

func init() {
	// Qt and Cocoa run their event loop on the initial macOS thread.
	runtime.LockOSThread()
}

func run() int {
	args := make([]*C.char, len(os.Args)+1)
	for i, arg := range os.Args {
		args[i] = C.CString(arg)
		defer C.free(unsafe.Pointer(args[i]))
	}
	return int(C.runAuxBridge(C.int(len(os.Args)), &args[0]))
}

func main() {
	os.Exit(run())
}
