/*
 * The Controllers panel (F9 / Cmd+K / View menu): what is plugged into the two
 * controller ports (the CX40 joystick, the CX30 paddle pair, the CX20 driving
 * controller or the CX50 keypad, a view per port that follows the port's type)
 * over the six-switch console, the gamepads with the port each one plays, and
 * Map mode for rebinding. Hidden at start; a fixed-size floating NSPanel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "a2600session.h"

/* The keys the app itself owns (F9, F12): called with the keysym of one typed
 * with the panel key; returns YES when it took it. */
typedef BOOL (^A2600AppKeyHandler)(uint32_t keysym);

@interface A2600ControllersWindow : NSObject
- (instancetype)initWithSession:(a2600session *)session appKeys:(A2600AppKeyHandler)appKeys;
/* Runs a console action the panel's POWER and STOP buttons fire
 * (A2600_SYSACT_*); the app sets it so the action also shows its toast or the
 * debugger. Without one the session runs it. */
@property (nonatomic, copy) void (^sysactionHandler)(int sysact);
/* Show, or hide when showing. */
- (void)toggle;
- (void)show;
- (void)hide;
- (BOOL)isVisible;
/* The settings changed elsewhere (Settings): the port types, the pads and the
 * bindings are read again. */
- (void)refresh;
@end
