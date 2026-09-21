/*
 * A2600AppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "DisplayView.h"
#import "KeyForward.h"
#import "debugger/DebuggerWindow.h"
#import "keypad/KeypadWindow.h"

#include <stdlib.h>
#include <string.h>

#define APP_TITLE @"FujiNet Go Atari 2600"

@class A2600AppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface A2600ContentView : NSView
@property (nonatomic) a2600session *session;
@property (nonatomic, weak) A2600AppDelegate *owner;
@end

@interface A2600AppDelegate ()
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
@end

@implementation A2600ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = A2600KeysymFromEvent(e);
    if (!ks) return;
    if (ks == A2600_KEYSYM_F9) { [A2600KeypadWindow toggleWithSession:self.session]; return; }
    if (ks == A2600_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == A2600_KEYSYM_F12) { [A2600DebuggerWindow showForSession:self.session]; return; }

    const int sa = a2600session_key_sysaction(self.session, ks);
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    a2600session_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = A2600KeysymFromEvent(e);
    if (ks) a2600session_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a fire button
 * bound to Shift or Option would otherwise never fire. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = A2600KeysymFromFlagsChange(e, &down);
    if (ks) a2600session_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end

@implementation A2600AppDelegate {
    a2600session *_session;
    const char *_cartPath;
    NSWindow *_window;
    A2600DisplayView *_display;
    A2600ContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    NSMenuItem *_colorItem, *_leftDiffItem, *_rightDiffItem;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSPopUpButton *_padList, *_padPort;
    NSTextField *_portNote[2];
    NSTimer *_settingsTimer;
    unsigned _padGeneration;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(a2600session *)session cartPath:(const char *)cartPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _cartPath = cartPath;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 960, 720)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[A2600ContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    _display = [[A2600DisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:a2600session_get_int(_session, "tv_aspect", 1) != 0];
    [_display setSmooth:a2600session_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    a2600session_start_opts opts;
    a2600session_default_opts(_session, &opts);
    if (_cartPath) opts.cart_path = _cartPath;
    if (a2600session_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
        [a runModal];
    }

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("A2600_OPEN_KEYPAD")) [A2600KeypadWindow toggleWithSession:_session];
    if (getenv("A2600_OPEN_DEBUGGER")) [A2600DebuggerWindow showForSession:_session];
    if (getenv("A2600_OPEN_SETTINGS")) [self showSettings:nil];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit). */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
        block:^(NSTimer *t) {
            (void)t;
            int sa;
            while (a2600session_sysaction_take(self->_session, &sa)) [self runSysaction:sa];
        }];
    [self updateTitle];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case A2600_SYSACT_REBOOT_CONFIG: a2600session_sysaction(_session, sa); break;
    case A2600_SYSACT_PAUSE:
        [A2600DebuggerWindow showForSession:_session];
        a2600session_sysaction(_session, sa);
        break;
    default: break;
    }
}

- (void)updateTitle
{
    NSString *state;
    char st[128];
    if (!a2600session_is_running(_session))
        state = @"stopped";
    else if (a2600session_cart_link_up(_session) < 0)
        state = [[NSString stringWithUTF8String:a2600session_cart_path(_session)] lastPathComponent];
    else if (a2600session_cart_booted_game(_session))
        state = @"FujiNet: booted a game";
    else if (a2600session_cart_link_up(_session) == 1)
        state = @"FujiNet connected";
    else {
        a2600session_cart_status(_session, st, sizeof st);
        state = [NSString stringWithFormat:@"FujiNet: %s", st];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"%@ — %@", APP_TITLE, state]];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window) a2600session_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_display stop];
    a2600session_stop(_session);
    a2600session_free(_session);
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    [self loadMedia:filename];
    return YES;
}

/* ---- menu ------------------------------------------------------------------ */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About " APP_TITLE action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [self item:machine title:@"Reboot to CONFIG" action:@selector(rebootConfig:) key:@"r"];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Select" action:@selector(pressSelect:) key:@""];
    [self item:machine title:@"Reset" action:@selector(pressReset:) key:@""];
    _colorItem = [self item:machine title:@"Color (B&W when off)" action:@selector(toggleColor:) key:@""];
    _leftDiffItem = [self item:machine title:@"Left Difficulty A" action:@selector(toggleLeftDiff:) key:@""];
    _rightDiffItem = [self item:machine title:@"Right Difficulty A" action:@selector(toggleRightDiff:) key:@""];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Keypads" action:@selector(toggleKeypad:) key:@"k"];
    [self item:view title:@"Debugger" action:@selector(showDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *tv = [self item:view title:@"TV Aspect (4:3)" action:@selector(toggleAspect:) key:@""];
    [tv setState:(a2600session_get_int(_session, "tv_aspect", 1) ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [sm setState:(a2600session_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"Configuration" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    [NSApp setMainMenu:bar];
}

/* The console switches can be flipped from the keyboard and the keypad
 * window too, so the menu reads their position when it opens. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item == _colorItem)
        [item setState:a2600session_switch_get(_session, A2600_SW_COLOR_BW) ? NSControlStateValueOn : NSControlStateValueOff];
    else if (item == _leftDiffItem)
        [item setState:a2600session_switch_get(_session, A2600_SW_LEFT_DIFF) ? NSControlStateValueOn : NSControlStateValueOff];
    else if (item == _rightDiffItem)
        [item setState:a2600session_switch_get(_session, A2600_SW_RIGHT_DIFF) ? NSControlStateValueOn : NSControlStateValueOff];
    return YES;
}

/* ---- actions ---------------------------------------------------------------- */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (void)loadMedia:(NSString *)path
{
    if (a2600session_media_is_cartridge([path fileSystemRepresentation])) {
        if (a2600session_load_cart(_session, [path fileSystemRepresentation]) != 0)
            [self alert:@"Could not open" text:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
        return;
    }
    char dest[1024];
    if (a2600session_import_media(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
        return;
    }
    [self alert:@"Imported" text:@"Copied to FujiNet's SD folder. Mount it from the CONFIG client."];
}

- (NSString *)pickCartridge:(NSString *)title
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    [p setAllowedFileTypes:@[@"a26", @"bin", @"rom", @"fuji"]];
    if ([p runModal] != NSModalResponseOK) return nil;
    return [[p URL] path];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (!path) return;
    if (a2600session_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self alert:@"Could not open" text:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
}

- (void)ejectCart:(id)sender { (void)sender; a2600session_eject(_session); }

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (a2600session_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
        return;
    }
    [self alert:@"Imported"
           text:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from the CONFIG client.",
                 [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

- (void)rebootConfig:(id)sender { (void)sender; [self runSysaction:A2600_SYSACT_REBOOT_CONFIG]; }
- (void)pressSelect:(id)sender { (void)sender; a2600session_switch_pulse(_session, A2600_SW_SELECT); }
- (void)pressReset:(id)sender { (void)sender; a2600session_switch_pulse(_session, A2600_SW_RESET); }

- (void)flipSwitch:(int)sw
{
    a2600session_switch_set(_session, sw, !a2600session_switch_get(_session, sw));
}
- (void)toggleColor:(id)sender { (void)sender; [self flipSwitch:A2600_SW_COLOR_BW]; }
- (void)toggleLeftDiff:(id)sender { (void)sender; [self flipSwitch:A2600_SW_LEFT_DIFF]; }
- (void)toggleRightDiff:(id)sender { (void)sender; [self flipSwitch:A2600_SW_RIGHT_DIFF]; }

- (void)toggleKeypad:(id)sender { (void)sender; [A2600KeypadWindow toggleWithSession:_session]; }
- (void)showDebugger:(id)sender { (void)sender; [A2600DebuggerWindow showForSession:_session]; }

- (void)toggleAspect:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setTvAspect:on];
    a2600session_set_int(_session, "tv_aspect", on ? 1 : 0);
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    a2600session_set_int(_session, "smooth", on ? 1 : 0);
}

/* ---- settings ----------------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. Controller types, the
 * analog switches and the volume apply live; the TV format and the host
 * options restart the session when the window closes.
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    return n;
}

- (NSPopUpButton *)popUpForKey:(const char *)key fallback:(int)def names:(const char *(*)(int))names
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    for (int i = 0; names(i); i++) [popup addItemWithTitle:[NSString stringWithUTF8String:names(i)]];
    NSInteger current = a2600session_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)popup.numberOfItems) current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = a2600session_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

- (void)updatePortNotes
{
    for (int port = 0; port < 2; port++) {
        if (a2600session_port_type(_session, port) == A2600_CTRL_AUTO)
            _portNote[port].stringValue = [NSString stringWithFormat:@"Auto: Stella attached %s",
                a2600_ctrl_type_name(a2600session_detected_port_type(_session, port))];
        else
            _portNote[port].stringValue = @"Forced for every cartridge";
    }
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = a2600session_gamepad_count(_session);
    if (n == 0) [_padList addItemWithTitle:@"(no gamepads connected)"];
    for (int i = 0; i < n; i++) {
        char name[128];
        const int eff = a2600session_gamepad_effective_port(_session, i);
        a2600session_gamepad_name(_session, i, name, sizeof name);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [_padList addItemWithTitle:[NSString stringWithFormat:@"%d: %s  [%s port]", i + 1, name,
                                    eff == 0 ? "left" : eff == 1 ? "right" : "no"]];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a2600session_gamepad_count(_session))
        [_padPort selectItemAtIndex:a2600session_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < a2600session_gamepad_count(_session))
        a2600session_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)volumeChanged:(NSSlider *)sender
{
    a2600session_set_volume(_session, (int)sender.integerValue);
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    const char *key = [control.identifier UTF8String];
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "port0_type") || !strcmp(key, "port1_type")) {
        a2600session_set_port_type(_session, key[4] == '1', value);
        [self updatePortNotes];
        return;
    }
    a2600session_set_int(_session, key, value);
    if (!strncmp(key, "analog_", 7)) {
        a2600session_set_analog(_session,
                                a2600session_get_int(_session, "analog_joystick", 1),
                                a2600session_get_int(_session, "analog_paddle", 1),
                                a2600session_get_int(_session, "analog_driving", 1));
        return;
    }
    _sessionDirty = YES;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _portNote[0] = [self note:@""];
    _portNote[1] = [self note:@""];
    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Left port", @"Right port"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);

    NSSlider *volume = [NSSlider sliderWithValue:a2600session_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSStackView *analog = [NSStackView stackViewWithViews:@[
        [self checkBoxForKey:"analog_joystick" title:@"Joystick" fallback:1],
        [self checkBoxForKey:"analog_paddle" title:@"Paddles" fallback:1],
        [self checkBoxForKey:"analog_driving" title:@"Driving" fallback:1]]];
    analog.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@"TV format"],
           [self popUpForKey:"tv_format" fallback:A2600_TV_AUTO names:a2600_tv_format_name] ],
        @[ [self sectionLabel:@"Controllers"], [self note:@"applied immediately"] ],
        @[ [NSTextField labelWithString:@"Left port"],
           [self popUpForKey:"port0_type" fallback:A2600_CTRL_AUTO names:a2600_ctrl_type_name] ],
        @[ [NSTextField labelWithString:@""], _portNote[0] ],
        @[ [NSTextField labelWithString:@"Right port"],
           [self popUpForKey:"port1_type" fallback:A2600_CTRL_AUTO names:a2600_ctrl_type_name] ],
        @[ [NSTextField labelWithString:@""], _portNote[1] ],
        @[ [NSTextField labelWithString:@"Analog sticks drive"], analog ],
        @[ [NSTextField labelWithString:@"Gamepads"], padRow ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Host"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSStackView *root = [NSStackView stackViewWithViews:@[grid]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 520)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];

    [self updatePortNotes];
    _padGeneration = a2600session_gamepad_generation(_session);
    [self refreshPadList];
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        (void)t;
        const unsigned gen = a2600session_gamepad_generation(self->_session);
        if (gen != self->_padGeneration) { self->_padGeneration = gen; [self refreshPadList]; }
        [self updatePortNotes];
    }];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsTimer invalidate];
    _settingsTimer = nil;
    if (!_sessionDirty) return;
    _sessionDirty = NO;

    a2600session_start_opts o;
    a2600session_settings_flush(_session);
    a2600session_default_opts(_session, &o);
    a2600session_stop(_session);
    if (a2600session_start(_session, &o) != 0)
        [self alert:@"Could not start" text:[NSString stringWithUTF8String:a2600session_last_error(_session)]];
}

/* ---- FujiNet console log ------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = a2600session_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    [_logView setString:(n > 0 ? [NSString stringWithUTF8String:buf] : @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    _logWindow.contentView = scroll;
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!a2600session_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:a2600session_fujinet_webui_url(_session)]]];
}
@end
