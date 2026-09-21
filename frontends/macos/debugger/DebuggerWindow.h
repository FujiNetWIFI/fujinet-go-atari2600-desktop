/*
 * The AppKit debugger window over Stella's debugger engine (a2600debug.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a2600session.h"

@interface A2600DebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session, stopping the
 * machine as the other frontends do. */
+ (void)showForSession:(a2600session *)session;
@end
