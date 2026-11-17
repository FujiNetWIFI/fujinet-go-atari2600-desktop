/*
 * A2600InputPane -- see InputSettings.h. Enable gamepads applies when the
 * Settings window closes (the pad thread starts or stops with the session); the
 * controller types, the analog switches and the port a gamepad plays apply at
 * once. NOT RUN-VERIFIED on macOS: the first real compile is the macOS CI job.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "InputSettings.h"

#import "help/HelpWindow.h"

#include <string.h>

#define PANE_W 560.0
#define PANE_H 400.0

static NSTextField *label(NSString *text, NSRect frame)
{
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = frame;
    return field;
}

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

@implementation A2600InputPane {
    a2600session *_session;
    void (^_controls)(BOOL);
    void (^_changed)(void);
    void (^_restart)(void);
    NSView *_view;
    NSPopUpButton *_typePopup[2];
    NSTextField *_typeNote[2];
    NSPopUpButton *_padList;
    NSPopUpButton *_padPort;
    NSTimer *_timer;
    unsigned _padGeneration;
}

@synthesize view = _view;

- (instancetype)initWithSession:(a2600session *)session
                       controls:(void (^)(BOOL open))controls
                        changed:(void (^)(void))changed
                        restart:(void (^)(void))restart
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _controls = [controls copy];
    _changed = [changed copy];
    _restart = [restart copy];
    [self build];
    [self refreshPadList];
    [self updateTypeNotes];
    [self startTimer];
    return self;
}

- (void)dealloc
{
    [_timer invalidate];
}

- (void)stop
{
    [_timer invalidate];
    _timer = nil;
}

/* ---- the gamepads ----------------------------------------------------------------- */

/* The connected gamepads and the port the chosen one is set to. */
- (void)refreshPadList
{
    const NSInteger keep = _padList.indexOfSelectedItem;
    const int count = a2600session_gamepad_count(_session);
    _padGeneration = a2600session_gamepad_generation(_session);
    [_padList removeAllItems];
    for (int idx = 0; idx < count; idx++) {
        char name[128];
        name[0] = '\0';
        a2600session_gamepad_name(_session, idx, name, (int)sizeof name);
        const int eff = a2600session_gamepad_effective_port(_session, idx);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [[_padList menu] addItemWithTitle:[NSString stringWithFormat:@"%d: %@  [%@]", idx + 1,
                                           nsstr(name).length ? nsstr(name) : @"Gamepad",
                                           eff == 0 ? @"left port" : eff == 1 ? @"right port" : @"no port"]
                                   action:nil keyEquivalent:@""];
    }
    if (count == 0) [[_padList menu] addItemWithTitle:@"No gamepads connected" action:nil keyEquivalent:@""];
    _padList.enabled = count > 0;
    _padPort.enabled = count > 0;
    [_padList selectItemAtIndex:keep >= 0 && keep < count ? keep : 0];
    [self padSelected:_padList];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    int assign = -1;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a2600session_gamepad_count(_session))
        assign = a2600session_gamepad_assignment(_session, (int)sel);
    [_padPort selectItemAtIndex:assign < 0 || assign > 1 ? 0 : assign + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a2600session_gamepad_count(_session))
        a2600session_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

/* What Stella attached on each port, and hot-plugged gamepads, twice a second
 * while the window is open. */
- (void)startTimer
{
    __weak A2600InputPane *weakSelf = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *tmr) {
        (void)tmr;
        A2600InputPane *strong = weakSelf;
        if (!strong) return;
        if (a2600session_gamepad_generation(strong->_session) != strong->_padGeneration)
            [strong refreshPadList];
        [strong updateTypeNotes];
    }];
    _timer.tolerance = 0.25;
}

- (void)updateTypeNotes
{
    for (int port = 0; port < 2; port++) {
        const int type = a2600session_port_type(_session, port);
        if (type >= 0 && type < _typePopup[port].numberOfItems && _typePopup[port].indexOfSelectedItem != type)
            [_typePopup[port] selectItemAtIndex:type];
        if (type == A2600_CTRL_AUTO)
            _typeNote[port].stringValue = [NSString stringWithFormat:@"Auto: Stella attached %@",
                nsstr(a2600_ctrl_type_name(a2600session_detected_port_type(_session, port)))];
        else
            _typeNote[port].stringValue = @"Forced for every cartridge";
    }
}

/* ---- the settings ------------------------------------------------------------------- */

/* Enable gamepads: the pad thread starts or stops with the session. */
- (void)padsToggled:(NSButton *)box
{
    const int on = [box state] == NSControlStateValueOn ? 1 : 0;
    if (a2600session_get_int(_session, "enable_gamepad", 1) != on) {
        a2600session_set_int(_session, "enable_gamepad", on);
        if (_restart) _restart();
    }
}

/* What is plugged into a port: applied live, a FujiNet-booted game survives. */
- (void)typeChanged:(NSPopUpButton *)sender
{
    a2600session_set_port_type(_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
    [self updateTypeNotes];
    if (_changed) _changed();
}

/* Which gamepad sticks may drive which controller: applied live. */
- (void)analogChanged:(NSButton *)box
{
    a2600session_set_int(_session, box.identifier.UTF8String, [box state] == NSControlStateValueOn ? 1 : 0);
    a2600session_set_analog(_session,
                            a2600session_get_int(_session, "analog_joystick", 1),
                            a2600session_get_int(_session, "analog_paddle", 1),
                            a2600session_get_int(_session, "analog_driving", 1));
}

- (void)resetControls:(id)sender
{
    (void)sender;
    a2600session_bindings_reset(_session);
    if (_controls) _controls(NO);
}

- (void)openControllers:(id)sender
{
    (void)sender;
    if (_controls) _controls(YES);
}

- (NSButton *)analogBox:(const char *)key title:(NSString *)title frame:(NSRect)frame
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(analogChanged:)];
    box.state = a2600session_get_int(_session, key, 1) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    box.frame = frame;
    return box;
}

- (void)build
{
    _view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, PANE_W, PANE_H)];
    const CGFloat top = PANE_H;

    NSButton *enable = [NSButton checkboxWithTitle:@"Enable gamepads" target:self action:@selector(padsToggled:)];
    enable.state = a2600session_get_int(_session, "enable_gamepad", 1) ? NSControlStateValueOn : NSControlStateValueOff;
    enable.frame = NSMakeRect(16, top - 36, 220, 22);
    [_view addSubview:enable];

    for (int port = 0; port < 2; port++) {
        const CGFloat rowY = top - 76 - port * 50;
        [_view addSubview:label(port ? @"Right port" : @"Left port", NSMakeRect(16, rowY + 3, 90, 20))];
        _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(112, rowY - 1, 150, 26) pullsDown:NO];
        for (int type = 0; a2600_ctrl_type_name(type); type++)
            [_typePopup[port] addItemWithTitle:nsstr(a2600_ctrl_type_name(type))];
        [_typePopup[port] selectItemAtIndex:a2600session_port_type(_session, port)];
        _typePopup[port].tag = port;
        _typePopup[port].target = self;
        _typePopup[port].action = @selector(typeChanged:);
        [_view addSubview:_typePopup[port]];
        _typeNote[port] = [NSTextField labelWithString:@""];
        _typeNote[port].font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
        _typeNote[port].textColor = NSColor.secondaryLabelColor;
        _typeNote[port].frame = NSMakeRect(112, rowY - 20, 400, 16);
        [_view addSubview:_typeNote[port]];
    }

    [_view addSubview:label(@"Analog sticks drive", NSMakeRect(16, top - 188, 150, 20))];
    [_view addSubview:[self analogBox:"analog_joystick" title:@"Joystick" frame:NSMakeRect(170, top - 188, 90, 20)]];
    [_view addSubview:[self analogBox:"analog_paddle" title:@"Paddles" frame:NSMakeRect(264, top - 188, 90, 20)]];
    [_view addSubview:[self analogBox:"analog_driving" title:@"Driving" frame:NSMakeRect(358, top - 188, 90, 20)]];

    [_view addSubview:label(@"Gamepad", NSMakeRect(16, top - 228, 90, 20))];
    _padList = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(112, top - 232, 270, 26) pullsDown:NO];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    [_view addSubview:_padList];
    _padPort = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(390, top - 232, 140, 26) pullsDown:NO];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Left port", @"Right port"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);
    [_view addSubview:_padPort];

    NSButton *reset = [NSButton buttonWithTitle:@"Reset Controls" target:self action:@selector(resetControls:)];
    reset.frame = NSMakeRect(16, top - 282, 130, 28);
    [_view addSubview:reset];
    NSButton *open = [NSButton buttonWithTitle:@"Open Controllers" target:self action:@selector(openControllers:)];
    open.frame = NSMakeRect(156, top - 282, 150, 28);
    [_view addSubview:open];

    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"The controller types, the analog switches and the port a gamepad plays apply at once. Keys and "
        @"gamepad buttons are re-mapped in the Controllers window (View > Controllers, Cmd+K or F9): click "
        @"Map, choose a control, then press the key or the gamepad button. Reset Controls puts the defaults "
        @"back. Turning gamepads on or off applies when this window is closed."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    note.frame = NSMakeRect(16, top - 372, PANE_W - 32, 80);
    [_view addSubview:note];

    /* AppKit resizes a tab's view to the tab: everything stays at the top,
     * except the Help button at the bottom */
    for (NSView *sub in [_view subviews])
        [sub setAutoresizingMask:NSViewMinYMargin];
    NSButton *help = A2600HelpButton(@"prefs/Input");
    [help setFrameOrigin:NSMakePoint(16, 12)];
    [_view addSubview:help];
}
@end
