#import <AVFoundation/AVFoundation.h>
#import <dispatch/dispatch.h>
#include "launcher.h"

void requestMicrophoneAccess(void (*completed)(bool)) {
    const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status != AVAuthorizationStatusNotDetermined) {
        completed(status == AVAuthorizationStatusAuthorized);
        return;
    }
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL allowed) {
        dispatch_async(dispatch_get_main_queue(), ^{ completed(allowed); });
    }];
}
