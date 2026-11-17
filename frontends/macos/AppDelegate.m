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
#import "help/HelpWindow.h"
#import "InputSettings.h"
#import "controllers/ControllersWindow.h"

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

@interface A2600AppDelegate () <NSTabViewDelegate>
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
- (void)toggleControllers:(id)sender;
@end

@implementation A2600ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command is
     * down, so forwarding one would leave the machine holding the key. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = A2600KeysymFromEvent(e);
    if (!ks) return;
    if (ks == A2600_KEYSYM_F9) { [self.owner toggleControllers:nil]; return; }
    if (ks == A2600_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == A2600_KEYSYM_F12) { [A2600DebuggerWindow showForSession:self.session]; return; }
    if (A2600ControllersMapKey(ks)) return;     /* Map is waiting for a key */

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
    if (!ks) return;
    if (down && A2600ControllersMapKey(ks)) return;
    a2600session_key(self.session, ks, down);
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
    id<NSObject> _activity;          /* no App Nap, no timer coalescing while we run */
    A2600ContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;

    NSMenuItem *_colorItem, *_leftDiffItem, *_rightDiffItem;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSTextField *_externalField;
    A2600InputPane *_inputPane;      /* the Settings window's Input tab */

    A2600ControllersWindow *_controllers;
    NSTextField *_toast;             /* over the picture, fades */
    unsigned _toastGeneration;       /* the pad generation already toasted */
    NSUInteger _toastSerial;

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

    /* An emulator is latency-critical work: App Nap must not throttle the
     * machine thread or coalesce its timers when the window is behind
     * another, and Game Mode (Info.plist) asks the system to favour it. */
    _activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                          reason:@"Emulating an Atari 2600"];

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
    /* layer-backed, so the display's layer and the toast over it composite in
     * the order they were added */
    [_content setWantsLayer:YES];
    [self buildToast:_content];

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

    __weak A2600AppDelegate *weakSelf = self;
    _controllers = [[A2600ControllersWindow alloc]
        initWithSession:_session
                appKeys:^BOOL(uint32_t keysym) { return [weakSelf handleAppKey:keysym]; }];
    _controllers.sysactionHandler = ^(int sysact) { [weakSelf runSysaction:sysact]; };
    /* No toast for the pads already attached at launch. */
    _toastGeneration = a2600session_gamepad_generation(_session);

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. A2600_OPEN_KEYPAD is the older name of the first. */
    if (getenv("A2600_OPEN_CONTROLLERS") || getenv("A2600_OPEN_KEYPAD")) [_controllers toggle];
    if (getenv("A2600_OPEN_DEBUGGER")) [A2600DebuggerWindow showForSession:_session];
    if (getenv("A2600_OPEN_SETTINGS")) [self showSettings:nil];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit), and a
     * toast when a gamepad came or went: well inside 50 ms. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES
        block:^(NSTimer *t) { (void)t; [self sysactTick]; }];
    _sysactTimer.tolerance = 0.01;
    [self updateTitle];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case A2600_SYSACT_REBOOT_CONFIG:
        a2600session_sysaction(_session, sa);
        [self showToast:@"Reboot to CONFIG"];
        break;
    case A2600_SYSACT_PAUSE:
        [A2600DebuggerWindow showForSession:_session];
        a2600session_sysaction(_session, sa);
        break;
    default: break;
    }
}

/* What the 50 ms timer does: the console actions a gamepad button or a bound
 * key posted, and a toast when a gamepad came or went. */
- (void)sysactTick
{
    int sa;
    while (a2600session_sysaction_take(_session, &sa)) [self runSysaction:sa];
    const unsigned gen = a2600session_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (a2600session_gamepad_last_event(_session, msg, (int)sizeof msg) > 0)
            [self showToast:[NSString stringWithUTF8String:msg]];
    }
}

/* The toast: a label over the top of the picture that fades. */
- (void)buildToast:(NSView *)content
{
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, content.bounds.size.height - 34, content.bounds.size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:_toast];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

/* F9 and F12 are the app's and not bindable; called with a key typed in the
 * Controllers panel. YES when taken. */
- (BOOL)handleAppKey:(uint32_t)keysym
{
    if (keysym == A2600_KEYSYM_F9) {
        [_controllers toggle];
        return YES;
    }
    if (keysym == A2600_KEYSYM_F12) {
        [A2600DebuggerWindow showForSession:_session];
        return YES;
    }
    return NO;
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
    if (_session && note.object == _window) a2600session_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    /* Close the windows while the session is alive: AppKit closes them after
     * this returns, and their resign-key / will-close handlers use it. */
    _window.delegate = nil;
    for (NSWindow *w in [[NSApp windows] copy])
        if (w != _window) [w close];
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_inputPane stop];
    _controllers = nil;
    [_display stop];
    if (_activity) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    a2600session_stop(_session);
    a2600session_free(_session);
    _session = NULL;
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
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"k"];
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

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [self item:help title:APP_TITLE @" Help" action:@selector(openHelp:) key:@"?"];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[A2600HelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[A2600HelpWindow shared] showFor:[NSApp keyWindow]];
}

/* The console switches can be flipped from the keyboard and the Controllers
 * panel too, so the menu reads their position when it opens. */
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

- (void)toggleControllers:(id)sender { (void)sender; [_controllers toggle]; }
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

    a2600session_set_int(_session, key, value);
    if (!strcmp(key, "fujinet_external"))
        _externalField.enabled = value != 0;
    _sessionDirty = YES;
}

/* The external-FujiNet address: valid text (or none, meaning
 * 127.0.0.1:11504) is saved; anything else is shown in red and not saved. */
- (BOOL)externalAddrValid
{
    NSString *text = [_externalField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (a2600session_parse_hostport(text.UTF8String, host, sizeof host, &port) != 0) {
        _externalField.textColor = NSColor.systemRedColor;
        return NO;
    }
    _externalField.textColor = NSColor.controlTextColor;
    if (strcmp(a2600session_get_str(_session, "fujinet_external_addr", ""), text.UTF8String) != 0) {
        a2600session_set_str(_session, "fujinet_external_addr", text.UTF8String);
        _sessionDirty = YES;
    }
    return YES;
}

- (void)externalAddrChanged:(id)sender
{
    (void)sender;
    [self externalAddrValid];
}

/* A bad address keeps the Settings window open. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    if (sender != _settingsWindow) return YES;
    [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
    if (_externalField.enabled == NO || [self externalAddrValid]) return YES;
    [self alert:@"That is not an address:port" text:@"Example: 127.0.0.1:11504"];
    return NO;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    NSSlider *volume = [NSSlider sliderWithValue:a2600session_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    _externalField = [NSTextField textFieldWithString:@(
        a2600session_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11504";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled = a2600session_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@"TV format"],
           [self popUpForKey:"tv_format" fallback:A2600_TV_AUTO names:a2600_tv_format_name] ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Host"], [self note:@"applied by restarting the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"fujinet_external" title:@"Use external FujiNet" fallback:0] ],
        @[ [NSTextField labelWithString:@"Address:port"], _externalField ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
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
        initWithContentRect:NSMakeRect(0, 0, 600, 480)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;

    /* the machine and host settings over a strip with the Help button */
    NSButton *helpBtn = A2600HelpButton(@"prefs/Machine");
    NSStackView *wrap = [NSStackView stackViewWithViews:@[root, helpBtn]];
    wrap.orientation = NSUserInterfaceLayoutOrientationVertical;
    wrap.alignment = NSLayoutAttributeLeading;
    wrap.edgeInsets = NSEdgeInsetsMake(0, 16, 12, 16);
    const NSSize fit = [wrap fittingSize];
    const CGFloat machineH = fit.height > 330 ? fit.height : 330;
    NSView *machineView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 580, machineH)];
    [wrap setFrame:NSMakeRect(0, machineH - fit.height, fit.width, fit.height)];
    /* AppKit resizes a tab's view to the tab; the options stay at the top */
    [wrap setAutoresizingMask:NSViewMinYMargin];
    [machineView addSubview:wrap];

    __weak A2600AppDelegate *weakSelf = self;
    [_inputPane stop];
    _inputPane = [[A2600InputPane alloc]
        initWithSession:_session
               controls:^(BOOL open) {
                   A2600AppDelegate *strong = weakSelf;
                   if (!strong) return;
                   if (open) {
                       [strong->_controllers show];
                   } else {
                       [strong->_controllers refresh];
                       [strong showToast:@"Default controls restored"];
                   }
               }
                changed:^{
                    A2600AppDelegate *strong = weakSelf;
                    if (strong) [strong->_controllers refresh];
                }
                restart:^{
                    A2600AppDelegate *strong = weakSelf;
                    if (strong) strong->_sessionDirty = YES;
                }];

    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, 600, 480)];
    tabs.delegate = self;
    NSTabViewItem *machineItem = [[NSTabViewItem alloc] initWithIdentifier:@"machine"];
    machineItem.label = @"Machine";
    machineItem.view = machineView;
    [tabs addTabViewItem:machineItem];
    NSTabViewItem *inputItem = [[NSTabViewItem alloc] initWithIdentifier:@"input"];
    inputItem.label = @"Input";
    inputItem.view = _inputPane.view;
    [tabs addTabViewItem:inputItem];
    _settingsWindow.contentView = tabs;
    A2600SetHelpContext(_settingsWindow, @"prefs/Machine");
    [_settingsWindow center];

    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* Command+? and the Help button follow the tab. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    A2600SetHelpContext(_settingsWindow, [item.identifier isEqual:@"input"] ? @"prefs/Input" : @"prefs/Machine");
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
    [_inputPane stop];
    _inputPane = nil;
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
    if (a2600session_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Its console output is in the process that is running it."];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 860, 564)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

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
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
        NSButton *help = A2600HelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    A2600SetHelpContext(_logWindow, @"fujilog");
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (a2600session_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Use the web UI of that FujiNet instance."];
        return;
    }
    if (!a2600session_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:a2600session_fujinet_webui_url(_session)]]];
}
@end
