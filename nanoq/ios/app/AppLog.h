#pragma once

// The app's diagnostics: to the device console (NSLog) and to Nano Q's own log, nanoq.log in the
// profile's logs folder. The console is out of reach for an iOS 12 device from a current Xcode,
// and ios-deploy --download fetches the file (docs/plans/nano-q-ios.md). Not for the audio thread.

#import <Foundation/Foundation.h>

namespace nanoq::ios
{
void AppLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);
} // namespace nanoq::ios
