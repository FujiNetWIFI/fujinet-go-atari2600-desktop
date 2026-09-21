/*
 * The keypad panel: both keyboard controllers, the console switches and
 * the Map row, in a fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a2600session.h"

@interface A2600KeypadWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(a2600session *)session;
+ (BOOL)isVisible;
@end
