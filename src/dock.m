#import <AppKit/AppKit.h>
#include "launcher.h"

bool dockIconVisible(void) {
    return NSApp.activationPolicy == NSApplicationActivationPolicyRegular;
}

void setDockIconVisible(bool visible) {
    [NSApp setActivationPolicy:visible ? NSApplicationActivationPolicyRegular
                                      : NSApplicationActivationPolicyAccessory];
}
