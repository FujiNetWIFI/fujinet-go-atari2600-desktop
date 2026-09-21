/*
 * Debugger window (AppKit) over Stella's own debugger engine, via
 * core/include/a2600debug.h. Mirrors the GTK, Qt and Win32 debuggers tab
 * for tab: Prompt, CPU & RAM, Disassembly, TIA, I/O, Breaks & Traps,
 * States & Cart.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import "../KeyForward.h"

#include <stdlib.h>
#include <string.h>

#include "a2600debug.h"

#define DISASM_WINDOW 48

static A2600DebuggerWindow *g_debugger;

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line; the wheel browses. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onToggleLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onToggleLine) self.onToggleLine(line);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (self.onScroll) self.onScroll((int)(-event.scrollingDeltaY / 6.0));
}
@end

/* The TIA picture: nearest-neighbour, 4:3, letterboxed. */
@interface TiaPictureView : NSView
@property (nonatomic) CGImageRef image;
@end

@implementation TiaPictureView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (!_image) return;
    const double want = 4.0 / 3.0;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), _image);
}
@end

@interface A2600DebuggerWindow ()
- (instancetype)initWithSession:(a2600session *)session;
- (void)refreshAll;
@end

@implementation A2600DebuggerWindow {
    a2600session *_session;
    a2600debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _runningTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;

    NSTextField *_reg[6];
    NSButton *_flag[7];
    NSTextField *_cycles;
    NSTextView *_ram;
    NSTextField *_ramAddr, *_ramVal;

    NSPopUpButton *_bank;
    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    int _disasmFirst;
    int _disasmBank;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    NSTextView *_tiaText;
    NSTextField *_tiaReg, *_tiaVal;
    TiaPictureView *_tiaPic;
    NSButton *_tiaPartial;
    uint32_t *_tiaPx;

    NSTextView *_riot;

    NSTextField *_bpEntry;
    NSTextView *_bps;

    NSTextField *_cartInfo;
}

static const char *const kFileKinds[5] = { "dis", "rom", "access", "ses", "snap" };
static NSString *const kFileTitles[5] = { @"Disassembly…", @"ROM (patched)…", @"Access counters…", @"Session…", @"TIA snapshot…" };

/* ---- helpers --------------------------------------------------------------- */

static NSString *stripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

/* $hex, 0xhex, #dec, or bare hex, as Stella's prompt reads numbers. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

- (int)resolveAddr:(NSString *)text
{
    long v;
    int addr = a2600debug_label_address(_dbg, text.UTF8String);
    if (addr < 0 && parseNum(text, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(a2600session *)session
{
    if (!g_debugger) g_debugger = [[A2600DebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    a2600debug_stop(g_debugger->_dbg);
    [g_debugger refreshAll];
}

- (instancetype)initWithSession:(a2600session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = a2600session_debugger(session);
    _disasmBank = -1;
    _tiaPx = calloc((size_t)A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT, sizeof *_tiaPx);
    [self buildWindow];
    __weak A2600DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    free(_tiaPx);
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = a2600debug_generation(_dbg);
    const BOOL stopped = a2600debug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (!stopped && ++_runningTicks >= 5) {
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshCpu];
        [self refreshTia];
        [self refreshRiot];
    }
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = monoView(&outScroll, YES);
    _promptOut.string = @"Stella debugger prompt. Type 'help' for every command.\n";
    _promptIn = [self field:@"Stella debugger command (help, break, breakIf, trap, watch, frame, tia, ...) — Tab completes"
                      width:0 action:@selector(runPrompt:)];
    _promptIn.delegate = self;
    NSButton *sym = [self button:@"Load symbols" action:@selector(loadSymbols:)];
    NSStackView *row = hstack(@[_promptIn, sym]);
    NSStackView *v = vstack(@[outScroll, row]);
    fill(v, outScroll);
    fill(v, row);
    return v;
}

- (NSView *)buildCpu
{
    static NSString *const names[6] = { @"PC", @"SP", @"A", @"X", @"Y", @"PS" };
    NSMutableArray *regs = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        [regs addObject:[self label:names[i]]];
        _reg[i] = [self field:@"" width:56 action:@selector(applyRegister:)];
        _reg[i].tag = i;
        [regs addObject:_reg[i]];
    }
    static NSString *const fnames[7] = { @"N", @"V", @"B", @"D", @"I", @"Z", @"C" };
    NSMutableArray *flags = [NSMutableArray array];
    for (int i = 0; i < 7; i++) {
        _flag[i] = [NSButton checkboxWithTitle:fnames[i] target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    _cycles = [self label:@""];
    _cycles.textColor = NSColor.secondaryLabelColor;
    [flags addObject:_cycles];

    NSScrollView *ramScroll;
    _ram = monoView(&ramScroll, NO);
    _ramAddr = [self field:@"$80" width:80 action:@selector(writeRam:)];
    _ramVal = [self field:@"$00" width:60 action:@selector(writeRam:)];
    NSStackView *edit = hstack(@[[self label:@"Write address"], _ramAddr, [self label:@"value"], _ramVal]);

    NSStackView *v = vstack(@[hstack(regs), hstack(flags), [self label:@"Zero-page RAM ($80-$FF)"], ramScroll, edit]);
    fill(v, ramScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _bank = [[NSPopUpButton alloc] init];
    [_bank addItemWithTitle:@"PC's bank"];
    const int n = a2600debug_bank_count(_dbg);
    for (int i = 0; i < n; i++) [_bank addItemWithTitle:[NSString stringWithFormat:@"Bank %d", i]];
    _bank.target = self;
    _bank.action = @selector(bankChanged:);
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSTextField *hint = [self label:@"Click a line to toggle its breakpoint; scroll to browse"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *row = hstack(@[_bank, _followPc, [self label:@"Jump to"], _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak A2600DebuggerWindow *weakSelf = self;
    _disasm.onToggleLine = ^(int line) {
        A2600DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        const int bank = s->_disasmBank < 0 ? a2600debug_current_bank(s->_dbg) : s->_disasmBank;
        a2600debug_breakpoint_toggle(s->_dbg, s->_lineAddr[line], bank);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        A2600DebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmFirst += lines;
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, scroll);
    return v;
}

- (NSView *)buildTia
{
    NSScrollView *textScroll;
    _tiaText = monoView(&textScroll, YES);
    _tiaReg = [self field:@"colup0" width:100 action:@selector(applyTia:)];
    _tiaVal = [self field:@"$1E" width:70 action:@selector(applyTia:)];
    NSStackView *edit = hstack(@[[self label:@"Register / strobe"], _tiaReg, [self label:@"value"], _tiaVal]);
    NSStackView *left = vstack(@[textScroll, edit]);
    fill(left, textScroll);

    _tiaPic = [[TiaPictureView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    [_tiaPic.widthAnchor constraintGreaterThanOrEqualToConstant:320].active = YES;
    [_tiaPic.heightAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
    _tiaPartial = [NSButton checkboxWithTitle:@"Frame in progress (to the beam)" target:self action:@selector(refreshTia)];
    NSStackView *right = vstack(@[_tiaPic, _tiaPartial]);
    fill(right, _tiaPic);

    NSStackView *h = hstack(@[left, right]);
    h.distribution = NSStackViewDistributionFillEqually;
    h.alignment = NSLayoutAttributeTop;
    return h;
}

- (NSView *)buildRiot
{
    NSScrollView *scroll;
    _riot = monoView(&scroll, YES);
    return scroll;
}

- (NSView *)buildBreaks
{
    _bpEntry = [self field:@"address/label to toggle, or breakIf {..}, trap $80, trapWrite $80 $ff, watch a ..."
                     width:0 action:@selector(bpEntry:)];
    NSButton *clear = [self button:@"Clear all" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpEntry, clear]);
    NSScrollView *scroll;
    _bps = monoView(&scroll, NO);
    NSStackView *v = vstack(@[row, scroll]);
    fill(v, row);
    fill(v, scroll);
    return v;
}

- (NSView *)buildStates
{
    NSMutableArray *rows = [NSMutableArray array];
    for (int row = 0; row < 4; row++) {
        NSMutableArray *cells = [NSMutableArray array];
        for (int col = 0; col < 5; col++) {
            const int slot = (row / 2) * 5 + col;
            NSButton *b = [self button:[NSString stringWithFormat:@"%@ %d", (row % 2) ? @"Load" : @"Save", slot]
                                action:(row % 2) ? @selector(loadState:) : @selector(saveState:)];
            b.tag = slot;
            [cells addObject:b];
        }
        [rows addObject:cells];
    }
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 4;
    grid.columnSpacing = 6;

    NSMutableArray *files = [NSMutableArray array];
    for (int i = 0; i < 5; i++) {
        NSButton *b = [self button:kFileTitles[i] action:@selector(saveFile:)];
        b.tag = i;
        [files addObject:b];
    }
    _cartInfo = [NSTextField wrappingLabelWithString:@""];
    _cartInfo.textColor = NSColor.secondaryLabelColor;

    NSStackView *v = vstack(@[[self label:@"Emulator states (Stella's saveState / loadState slots)"], grid,
                              [self label:@"Save to a file"], hstack(files),
                              [self label:@"Cartridge"], _cartInfo]);
    return v;
}

- (void)buildWindow
{
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1100, 760)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *trace = [self button:@"Trace (F8)" action:@selector(trace:)];
    NSButton *scan = [self button:@"Scan+1" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame+1 (⇧F8)" action:@selector(frame:)];
    NSButton *rewind = [self button:@"Rewind" action:@selector(rewind:)];
    NSButton *unwind = [self button:@"Unwind" action:@selector(unwind:)];
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    trace.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    trace.keyEquivalentModifierMask = 0;
    frame.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    frame.keyEquivalentModifierMask = NSEventModifierFlagShift;
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    NSStackView *toolbar = hstack(@[_runBtn, step, trace, scan, frame, rewind, unwind, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"TIA", [self buildTia]],
                        @[@"I/O", [self buildRiot]], @[@"Breaks & Traps", [self buildBreaks]],
                        @[@"States & Cart", [self buildStates]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [NSTabViewItem tabViewItemWithViewController:nil];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("A2600_DEBUGGER_TAB");
    if (tab && *tab) {
        const int t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (a2600debug_is_stopped(_dbg)) a2600debug_resume(_dbg); else a2600debug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; a2600debug_step(_dbg); [self refreshAll]; }
- (void)trace:(id)sender { (void)sender; a2600debug_trace(_dbg); [self refreshAll]; }
- (void)scanline:(id)sender { (void)sender; a2600debug_scanline(_dbg, 1); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; a2600debug_frame(_dbg, 1); [self refreshAll]; }
- (void)rewind:(id)sender { (void)sender; a2600debug_rewind(_dbg, 1); [self refreshAll]; }
- (void)unwind:(id)sender { (void)sender; a2600debug_unwind(_dbg, 1); [self refreshAll]; }

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    a2600debug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[stripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* Tab in the prompt completes instead of moving the focus. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn || sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = a2600debug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        [self appendPrompt:[NSString stringWithUTF8String:comps] ?: @""];
    }
    return YES;
}

- (void)loadSymbols:(id)sender
{
    (void)sender;
    char msg[512];
    a2600debug_load_symbols(_dbg, msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    [self refreshAll];
}

- (void)applyRegister:(NSTextField *)sender
{
    static const int regIds[6] = { A2600_REG_PC, A2600_REG_SP, A2600_REG_A, A2600_REG_X, A2600_REG_Y, A2600_REG_PS };
    long v;
    if (parseNum(sender.stringValue, &v)) a2600debug_cpu_set(_dbg, regIds[sender.tag], (int)v);
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    static const int flagIds[7] = { A2600_FLAG_N, A2600_FLAG_V, A2600_FLAG_B, A2600_FLAG_D, A2600_FLAG_I, A2600_FLAG_Z, A2600_FLAG_C };
    if (a2600debug_is_stopped(_dbg))
        a2600debug_cpu_set(_dbg, flagIds[sender.tag], sender.state == NSControlStateValueOn);
    [self refreshCpu];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long a, v;
    if (parseNum(_ramAddr.stringValue, &a) && parseNum(_ramVal.stringValue, &v))
        a2600debug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

- (void)bankChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _bank.indexOfSelectedItem;
    _disasmBank = (int)sel - 1;
    if (sel > 0) _followPc.state = NSControlStateValueOff;
    _disasmFirst = 0;
    [self refreshDisasm];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    static a2600debug_line lines[256];
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    int total = 0, pcLine = -1, first = 0;
    _followPc.state = NSControlStateValueOff;
    for (;;) {
        const int n = a2600debug_disassemble(_dbg, _disasmBank, first, lines, 256, &total, &pcLine);
        if (n == 0) break;
        for (int i = 0; i < n; i++)
            if (lines[i].address >= addr) { _disasmFirst = first + i; [self refreshDisasm]; return; }
        first += n;
        if (first >= total) break;
    }
}

- (void)applyTia:(id)sender
{
    (void)sender;
    NSString *reg = [[_tiaReg.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] lowercaseString];
    if (a2600debug_tia_strobe(_dbg, reg.UTF8String) == 0) { [self refreshAll]; return; }
    long v;
    if (parseNum(_tiaVal.stringValue, &v)) a2600debug_tia_set(_dbg, reg.UTF8String, (int)v);
    [self refreshAll];
}

- (void)bpEntry:(id)sender
{
    (void)sender;
    NSString *text = [_bpEntry.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (text.length == 0) return;
    long v;
    int addr = a2600debug_label_address(_dbg, text.UTF8String);
    if (addr < 0 && strchr("$#0123456789abcdefABCDEF", [text characterAtIndex:0]) && parseNum(text, &v) && v >= 0 && v <= 0xffff)
        addr = (int)v;
    if (addr >= 0) {
        a2600debug_breakpoint_toggle(_dbg, (uint16_t)addr, A2600DEBUG_ANY_BANK);
    } else {
        static char out[2048];
        a2600debug_command(_dbg, text.UTF8String, out, sizeof out);
    }
    _bpEntry.stringValue = @"";
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    static char out[512];
    a2600debug_breakpoint_clear(_dbg);
    a2600debug_command(_dbg, "clearTraps", out, sizeof out);
    [self refreshAll];
}

- (void)saveState:(NSButton *)sender { a2600debug_state_save(_dbg, (int)sender.tag); [self refreshAll]; }
- (void)loadState:(NSButton *)sender { a2600debug_state_load(_dbg, (int)sender.tag); [self refreshAll]; }

- (void)saveFile:(NSButton *)sender
{
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[sender.tag]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    a2600debug_save(_dbg, kFileKinds[sender.tag], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160], info[256];
    int addr;
    const BOOL stopped = a2600debug_is_stopped(_dbg) != 0;
    a2600debug_stop_reason(_dbg, reason, sizeof reason, &addr);
    a2600debug_cart_info(_dbg, info, sizeof info);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    _cartInfo.stringValue = stripControl(info);
    _runBtn.title = stopped ? @"Run (F5)" : @"Stop (F5)";
    _runBtn.bezelColor = stopped ? A2600AccentColor() : nil;
}

- (void)refreshCpu
{
    a2600debug_cpu c;
    a2600debug_cpu_get(_dbg, &c);
    const int vals[6] = { c.pc, c.sp, c.a, c.x, c.y, c.ps };
    for (int i = 0; i < 6; i++)
        if (!_reg[i].currentEditor)
            _reg[i].stringValue = [NSString stringWithFormat:(i == 0 ? @"%04X" : @"%02X"), vals[i]];
    const int flags[7] = { c.n, c.v, c.b, c.d, c.i, c.z, c.c };
    for (int i = 0; i < 7; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _cycles.stringValue = [NSString stringWithFormat:@"last instruction: %d cycles, total %llu", c.cycles,
                           (unsigned long long)c.total_cycles];
}

- (void)refreshRam
{
    uint8_t ram[128];
    a2600debug_ram_get(_dbg, ram);
    NSMutableString *text = [NSMutableString stringWithString:@"      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"];
    for (int row = 0; row < 8; row++) {
        [text appendFormat:@"$%02X: ", 0x80 + row * 16];
        for (int col = 0; col < 16; col++) [text appendFormat:@"%02X ", ram[row * 16 + col]];
        [text appendString:@"\n"];
    }
    _ram.string = text;
}

- (void)refreshDisasm
{
    static a2600debug_line lines[DISASM_WINDOW];
    int total = 0, pcLine = -1;
    a2600debug_disassemble(_dbg, _disasmBank, 0, lines, 1, &total, &pcLine);
    if (_followPc.state == NSControlStateValueOn && pcLine >= 0)
        _disasmFirst = pcLine > DISASM_WINDOW / 3 ? pcLine - DISASM_WINDOW / 3 : 0;
    if (_disasmFirst > total - 1) _disasmFirst = total > 0 ? total - 1 : 0;
    if (_disasmFirst < 0) _disasmFirst = 0;
    const int n = a2600debug_disassemble(_dbg, _disasmBank, _disasmFirst, lines, DISASM_WINDOW, &total, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    int pcRow = -1;
    NSUInteger pcStart = 0, pcLen = 0;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-10s %-14s %-22s %s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm, lines[i].cycles];
        if (lines[i].is_pc) { pcRow = i; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly)\n"];
    _disasm.string = text;
    if (pcRow >= 0) {
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: A2600AccentColor(),
                                              NSForegroundColorAttributeName: NSColor.blackColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshTia
{
    a2600debug_tia t;
    a2600debug_tia_get(_dbg, &t);
    static const char *const coll[15] = { "M0-P1", "M0-P0", "M1-P0", "M1-P1", "P0-PF", "P0-BL", "P1-PF",
        "P1-BL", "M0-PF", "M0-BL", "M1-PF", "M1-BL", "BL-PF", "P0-P1", "M0-M1" };
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Frame %d   scanline %d (last frame %d)   frame cycles %d (WSYNC %d)\n",
        t.frame_count, t.scanlines, t.scanlines_last, t.frame_cycles, t.wsync_cycles];
    [s appendFormat:@"clocks this line %d   cycles this line %d   beam %d,%d\nVSYNC %d   VBLANK %d\n\n",
        t.clocks_this_line, t.cycles_this_line, t.beam_x, t.beam_y, t.vsync, t.vblank];
    [s appendFormat:@"Colours   COLUP0 %02X   COLUP1 %02X   COLUPF %02X   COLUBK %02X\n", t.colup0, t.colup1, t.colupf, t.colubk];
    [s appendFormat:@"Players   GRP0 %02X  GRP1 %02X   NUSIZ0 %02X  NUSIZ1 %02X   REFP0 %d  REFP1 %d   VDELP0 %d  VDELP1 %d\n",
        t.graphics_p0, t.graphics_p1, t.nusiz0, t.nusiz1, t.refp0, t.refp1, t.vdelp0, t.vdelp1];
    [s appendFormat:@"Missiles  ENAM0 %d  ENAM1 %d   RESMP0 %d  RESMP1 %d\nBall      ENABL %d   VDELBL %d\n",
        t.enam0, t.enam1, t.resmp0, t.resmp1, t.enabl, t.vdelbl];
    [s appendFormat:@"Playfield PF0 %02X  PF1 %02X  PF2 %02X   CTRLPF %02X   REF %d  SCORE %d  PRIORITY %d\n\n",
        t.pf0, t.pf1, t.pf2, t.ctrlpf, t.refpf, t.scorepf, t.pripf];
    [s appendFormat:@"Positions      P0 %3d   P1 %3d   M0 %3d   M1 %3d   BL %3d\n", t.pos_p0, t.pos_p1, t.pos_m0, t.pos_m1, t.pos_bl];
    [s appendFormat:@"Motion (HM)    P0 %02X    P1 %02X    M0 %02X    M1 %02X    BL %02X\n\n", t.hm_p0, t.hm_p1, t.hm_m0, t.hm_m1, t.hm_bl];
    [s appendFormat:@"Audio     AUDC0 %02X  AUDF0 %02X  AUDV0 %02X  (%s)\n          AUDC1 %02X  AUDF1 %02X  AUDV1 %02X  (%s)\n\nCollisions:",
        t.audc0, t.audf0, t.audv0, t.aud_freq0, t.audc1, t.audf1, t.audv1, t.aud_freq1];
    BOOL any = NO;
    for (int i = 0; i < 15; i++) if (t.collisions & (1u << i)) { [s appendFormat:@" %s", coll[i]]; any = YES; }
    if (!any) [s appendString:@" none"];
    [s appendString:@"\n\nSet a register: name and value below (colup0, pf1, posp0, refp0, vsync ...); "
                     "strobes: wsync rsync resp0 resp1 resm0 resm1 resbl hmove hmclr cxclr\n"];
    _tiaText.string = s;

    const int h = a2600debug_frame_snapshot(_dbg, _tiaPx, _tiaPartial.state == NSControlStateValueOn);
    if (h > 0) {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_tiaPx, (CFIndex)A2600SESSION_FB_WIDTH * h * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate(A2600SESSION_FB_WIDTH, h, 8, 32, A2600SESSION_FB_WIDTH * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                       NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(space);
        _tiaPic.image = img;   /* the view owns it now */
    }
}

- (void)refreshRiot
{
    a2600debug_riot r;
    a2600debug_riot_get(_dbg, &r);
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"SWCHA  %02X   SWACNT %02X     left: %s   right: %s\nSWCHB  %02X   SWBCNT %02X\nINPT0-5  ",
        r.swcha, r.swacnt, r.dir_left, r.dir_right, r.swchb, r.swbcnt];
    for (int i = 0; i < 6; i++) [s appendFormat:@"%02X ", r.inpt[i]];
    [s appendFormat:@"\n\nTimer  INTIM %02X   TIMINT %02X   clocks %d   divider %d\n\n", r.intim, r.timint, r.tim_clocks, r.tim_divider];
    [s appendFormat:@"Switches   Select %s   Reset %s   %s   Left difficulty %s   Right difficulty %s\n\n",
        r.select ? "pressed" : "up", r.reset ? "pressed" : "up", r.color ? "Color" : "B&W",
        r.diff_left_a ? "A" : "B", r.diff_right_a ? "A" : "B"];
    [s appendString:@"The switches can be flipped from the menu, the keypad window or the prompt (swchb $xx); "
                     "the ports with joy0up, joy0fire ..."];
    _riot.string = s;
}

- (void)refreshBps
{
    uint32_t bps[64];
    static char out[8192];
    const int n = a2600debug_breakpoint_list(_dbg, bps, 64);
    NSMutableString *s = [NSMutableString stringWithFormat:@"Breakpoints (%d):\n", n];
    for (int i = 0; i < n; i++) {
        const unsigned bank = bps[i] >> 16;
        if (bank == A2600DEBUG_ANY_BANK) [s appendFormat:@"  $%04X  any bank\n", bps[i] & 0xffff];
        else [s appendFormat:@"  $%04X  bank %u\n", bps[i] & 0xffff, bank];
    }
    a2600debug_command(_dbg, "listTraps", out, sizeof out);
    [s appendFormat:@"\nTraps:\n%@\n", stripControl(out)];
    a2600debug_command(_dbg, "listBreaks", out, sizeof out);
    [s appendFormat:@"\nConditional (breakIf):\n%@\n", stripControl(out)];
    _bps.string = s;
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshCpu];
    [self refreshRam];
    [self refreshDisasm];
    [self refreshTia];
    [self refreshRiot];
    [self refreshBps];
}
@end
