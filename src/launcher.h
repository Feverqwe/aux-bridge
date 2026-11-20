#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int runAuxBridge(int argc, char** argv);
void requestMicrophoneAccess(void (*completed)(bool));
bool dockIconVisible(void);
void setDockIconVisible(bool visible);

#ifdef __cplusplus
}
#endif
