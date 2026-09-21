/*
 * A2600KeypadWindow -- the AppKit keypad panel: both keyboard controllers
 * side by side, each a 3x4 keypad centred over its port's fire buttons,
 * then the console switches and the Map row.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click: a keypad key on this machine is HELD, and
 * a game polls it, so a value present only for the instant of a click
 * falls between frames. mouseUp arrives even when the pointer has left the
 * button (AppKit tracks the drag for the view that got mouseDown), so
 * dragging off cannot strand the machine with a key held forever.
 *
 * A utility panel of fixed size (no resize mask): the keys are a block of
 * fixed-size buttons, and there is nothing to gain by stretching them.
 * Singleton, ordered out rather than closed, so its position survives.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#import "../KeyForward.h"

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button. */
static int g_mapState = -2;
static A2600KeypadWindow *g_singleton;
static a2600session *g_session;

/* NOT named `target`: NSControl already has a `target` property. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) BOOL down;
@end

@implementation PadButton
- (void)setHeld:(BOOL)held
{
    self.bezelColor = held ? A2600AccentColor() : nil;
    [self highlight:held];
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"A2600PadRefresh" object:nil];
        return;
    }
    if (g_mapState >= 0) return;
    self.down = YES;
    [self setHeld:YES];
    if (self.padTarget >= A2600_TARGET_SYSACT(0)) return;   /* fires on release */
    a2600session_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self setHeld:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= A2600_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reboot the machine. */
        a2600session_sysaction(g_session, self.padTarget - A2600_TARGET_SYSACT(0));
        return;
    }
    a2600session_press(g_session, self.padTarget, 0);
}
@end

@implementation A2600KeypadWindow {
    NSMutableArray<PadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
    NSTextField *_typeLabel[2];
    NSTimer *_captureTimer;
    NSTimer *_typeTimer;
}

static NSString *const kFace[12] = { @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"*", @"0", @"#" };

#define KEY_W   56.0
#define KEY_H   40.0
#define GAP      6.0
#define WIDE_W 118.0
#define PAD_W  (3 * WIDE_W + 2 * GAP)
#define GRID_W (3 * KEY_W + 2 * GAP)
#define MARGIN  12.0

- (PadButton *)buttonWithFace:(NSString *)face target:(int)target frame:(NSRect)frame
{
    PadButton *b = [[PadButton alloc] initWithFrame:frame];
    [b setTitle:face];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

/* One controller, laid out downwards from topY (AppKit's y grows upwards,
 * so every row is placed by its bottom edge). Returns the y below it. */
- (CGFloat)buildController:(int)port intoView:(NSView *)parent x:(CGFloat)x0 topY:(CGFloat)topY
{
    CGFloat y = topY;

    NSTextField *title = [NSTextField labelWithString:(port ? @"Right Port" : @"Left Port")];
    [title setAlignment:NSTextAlignmentCenter];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 18, PAD_W, 16)];
    [parent addSubview:title];
    y -= 20;

    _typeLabel[port] = [NSTextField labelWithString:@""];
    [_typeLabel[port] setAlignment:NSTextAlignmentCenter];
    [_typeLabel[port] setFont:[NSFont systemFontOfSize:NSFont.smallSystemFontSize]];
    [_typeLabel[port] setFrame:NSMakeRect(x0, y - 16, PAD_W, 14)];
    [parent addSubview:_typeLabel[port]];
    y -= 16 + GAP;

    /* The 3x4 block, centred over the fire row so the digits sit tight. */
    const CGFloat gx = x0 + (PAD_W - GRID_W) / 2;
    for (int i = 0; i < 12; i++) {
        NSRect r = NSMakeRect(gx + (i % 3) * (KEY_W + GAP), y - (i / 3 + 1) * (KEY_H + GAP) + GAP, KEY_W, KEY_H);
        [parent addSubview:[self buttonWithFace:kFace[i]
                                         target:A2600_TARGET_PORT(port, A2600_ACT_KEY_1 + i)
                                          frame:r]];
    }
    y -= 4 * (KEY_H + GAP);

    [parent addSubview:[self buttonWithFace:@"Fire" target:A2600_TARGET_PORT(port, A2600_ACT_JOY_FIRE)
                                      frame:NSMakeRect(x0, y - KEY_H, WIDE_W, KEY_H)]];
    [parent addSubview:[self buttonWithFace:@"Paddle A" target:A2600_TARGET_PORT(port, A2600_ACT_PADDLE_A_FIRE)
                                      frame:NSMakeRect(x0 + WIDE_W + GAP, y - KEY_H, WIDE_W, KEY_H)]];
    [parent addSubview:[self buttonWithFace:@"Paddle B" target:A2600_TARGET_PORT(port, A2600_ACT_PADDLE_B_FIRE)
                                      frame:NSMakeRect(x0 + 2 * (WIDE_W + GAP), y - KEY_H, WIDE_W, KEY_H)]];
    return y - KEY_H;
}

- (instancetype)init
{
    const CGFloat x1 = MARGIN + PAD_W + 2 * MARGIN;
    const CGFloat width = x1 + PAD_W + MARGIN;
    const CGFloat height = MARGIN + 20 + 16 + GAP + 4 * (KEY_H + GAP) + KEY_H   /* a controller */
                         + MARGIN + 4 + KEY_H + MARGIN + 30 + MARGIN;           /* console + map rows */

    NSPanel *win = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Keypads"];
    /* Floats above the machine's window and stays out of the way. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];
    NSView *content = [win contentView];
    CGFloat y = height - MARGIN;
    [self buildController:0 intoView:content x:MARGIN topY:y];
    CGFloat bottom = [self buildController:1 intoView:content x:x1 topY:y];

    /* Console row, centred. */
    bottom -= MARGIN + 4;
    static const struct { NSString *__unsafe_unretained face; int target; } console[6] = {
        { @"Select",           A2600_TARGET_SWITCH(A2600_SW_SELECT) },
        { @"Reset",            A2600_TARGET_SWITCH(A2600_SW_RESET) },
        { @"Color / B&W",      A2600_TARGET_SWITCH(A2600_SW_COLOR_BW) },
        { @"Left Diff",        A2600_TARGET_SWITCH(A2600_SW_LEFT_DIFF) },
        { @"Right Diff",       A2600_TARGET_SWITCH(A2600_SW_RIGHT_DIFF) },
        { @"Reboot to CONFIG", A2600_TARGET_SYSACT(A2600_SYSACT_REBOOT_CONFIG) },
    };
    const CGFloat cx = (width - (6 * WIDE_W + 5 * GAP)) / 2;
    for (int i = 0; i < 6; i++)
        [content addSubview:[self buttonWithFace:console[i].face target:console[i].target
                                           frame:NSMakeRect(cx + i * (WIDE_W + GAP), bottom - KEY_H, WIDE_W, KEY_H)]];
    bottom -= KEY_H + MARGIN;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(MARGIN, bottom - 30, 70, 30)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(MARGIN + 76, bottom - 30, 90, 30)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(MARGIN + 176, bottom - 24, width - MARGIN - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh)
                                                 name:@"A2600PadRefresh" object:nil];
    [self refresh];
    return self;
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a2600session_bindings_reset(g_session);
    [self refresh];
}

- (void)setMapState:(int)state
{
    g_mapState = state;
    [_captureTimer invalidate];
    _captureTimer = nil;
    if (state >= 0) {
        a2600session_gamepad_capture_begin(g_session);
        _captureTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self pollCapture];
        }];
    } else {
        a2600session_gamepad_capture_cancel(g_session);
    }
    [self refresh];
}

- (void)pollCapture
{
    int button;
    if (g_mapState < 0) return;
    if (a2600session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        a2600session_binding_set_button(g_session, g_mapState, button, stolen, sizeof stolen);
        [self setMapState:-1];
        if (stolen[0])
            _hint.stringValue = [NSString stringWithFormat:@"Bound %s (was %s)", a2600_pad_button_name(button), stolen];
    }
}

- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Cancel")];
    _mapButton.bezelColor = g_mapState == -2 ? nil : A2600AccentColor();
    if (g_mapState == -2)
        [_hint setStringValue:@""];
    else if (g_mapState == -1)
        [_hint setStringValue:@"Click a control to remap"];
    else
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key or gamepad button for %s",
                               a2600_target_name(g_mapState)]];

    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const a2600_binding bind = a2600session_binding_get(g_session, b.padTarget);
            char key[32];
            a2600session_keysym_name(bind.keysym, key, sizeof key);
            NSString *text = key[0] ? [NSString stringWithUTF8String:key] : @"—";
            if (bind.button != A2600_PAD_BTN_NONE)
                text = [text stringByAppendingFormat:@" / %s", a2600_pad_button_name(bind.button)];
            [b setTitle:text];
            b.bezelColor = (b.padTarget == g_mapState) ? A2600AccentColor() : nil;
        } else {
            [b setTitle:b.face];
            if (!b.down) b.bezelColor = nil;
        }
    }
}

- (void)refreshTypes
{
    for (int port = 0; port < 2; port++) {
        const int det = a2600session_detected_port_type(g_session, port);
        _typeLabel[port].stringValue = [NSString stringWithFormat:@"%s attached", a2600_ctrl_type_name(det)];
        _typeLabel[port].textColor = det == A2600_CTRL_KEYPAD ? A2600AccentColor() : [NSColor secondaryLabelColor];
    }
}

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key. */
- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = A2600KeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            char stolen[128], name[32];
            a2600session_binding_set_key(g_session, g_mapState, ks, stolen, sizeof stolen);
            a2600session_keysym_name(ks, name, sizeof name);
            [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
            if (stolen[0])
                _hint.stringValue = [NSString stringWithFormat:@"Bound %s (was %s)", name, stolen];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;
    if (ks == A2600_KEYSYM_F9) { [A2600KeypadWindow toggleWithSession:g_session]; return; }

    const int sa = a2600session_key_sysaction(g_session, ks);
    if (sa >= 0) { a2600session_sysaction(g_session, sa); return; }
    a2600session_key(g_session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = A2600KeysymFromEvent(e);
    if (ks) a2600session_key(g_session, ks, 0);
}

- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = A2600KeysymFromFlagsChange(e, &down);
    if (ks && g_mapState == -2) a2600session_key(g_session, ks, down);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self setMapState:-2];
    [_typeTimer invalidate];
    _typeTimer = nil;
    a2600session_release_all(g_session);
}

+ (void)toggleWithSession:(a2600session *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[A2600KeypadWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        [g_singleton setMapState:-2];
        [g_singleton->_typeTimer invalidate];
        g_singleton->_typeTimer = nil;
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton refreshTypes];
        g_singleton->_typeTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [g_singleton refreshTypes];
        }];
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end
