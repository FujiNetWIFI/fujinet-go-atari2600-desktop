/*
 * A2600InputPane -- the Input tab of the macOS Settings window: Enable
 * gamepads, what is plugged into each controller port, which gamepad sticks may
 * drive a joystick, a paddle or the driving controller, each gamepad and the
 * port it plays, Reset Controls and Open Controllers. See InputSettings.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a2600session.h"

@interface A2600InputPane : NSObject
/* controls runs from the buttons (YES: open the Controllers window, NO: the
 * default controls were restored); changed runs after a port type was set (the
 * Controllers window reads it again); restart runs when a setting that needs the
 * session restarted (Enable gamepads) changed. */
- (instancetype)initWithSession:(a2600session *)session
                       controls:(void (^)(BOOL open))controls
                        changed:(void (^)(void))changed
                        restart:(void (^)(void))restart;
@property (nonatomic, readonly) NSView *view;
/* Stops the hot-plug timer (the window closed). */
- (void)stop;
@end
