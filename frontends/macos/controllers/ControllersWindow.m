/*
 * A2600ControllersWindow -- the AppKit Controllers panel: what is plugged into
 * the two controller ports (the CX40 joystick, the CX30 paddle pair, the CX20
 * driving controller or the CX50 keypad), the six-switch console, the gamepads
 * and the port each one plays, and Map mode. Everything is drawn from the
 * session's layout (a2600session_controls_layout) with NSBezierPath, so every
 * frontend shows the same machine; the core hit-tests it and does the drag
 * maths (a2600session_stick_from_vector and friends).
 *
 * The art follows ~/Workspace/fujinet-go-all/controllers-port/DESIGN-atari.md
 * (section numbers in the comments): black textured CX40 cases with a recessed
 * red fire button and a ribbed boot, CX30 wedges with a knurled knob, the CX50's
 * round keys with light-blue legends, and the "Heavy Sixer" console, a black
 * panel with silver slide switches over a woodgrain strip. The joystick and the
 * paddle drawing is shared with the Atari 800 desktop.
 *
 * Gestures: press a button, a key or a console switch and it is held until it is
 * released or dragged off (the toggling switches flip on the press; POWER and
 * STOP act on release over them, like a real button); press in the joystick's
 * ring and drag to steer it; drag on a knob to turn it (a paddle holds where it
 * is left), the wheel nudges it; drag round the driving wheel to turn it, one
 * notch per 22.5 degrees. The pointer is its own input source, so the panel lets
 * go of everything the mouse holds when it is hidden or loses key status. What
 * the console sees held, from any source (the host keyboard, a gamepad, the
 * mouse), is painted in orange, so the panel doubles as an input tester for a
 * new gamepad.
 *
 * Keys typed with the panel key go through the same path as the machine window
 * (a bound key drives its controller, a bound console action runs); no view here
 * takes the keyboard, so the panel itself is the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents; only
 * more pads than the two rows kept for them change its height. Every port view
 * has the same extent, so swapping a game's controller never resizes it. Hidden
 * at start, ordered out rather than closed, so its position survives. The 30 Hz
 * poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture (the
 * stick's four directions and the paddles' turn keys show as small labels) or
 * choose one from the list, then press a key or a gamepad button; the next one
 * is bound and whatever held it is told so in the status line. Esc cancels the
 * pick. Rebinding steals the key from its old target. F9, F11 and F12 are the
 * app's and never bound.
 *
 * Written to the family's AppKit pattern (see the Apple II, ADAM and Astrocade
 * panels); built and verified on CI only (no Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         21.0     /* points per layout unit */
#define WIN_W        664.0    /* the layout's 30.6 units, and a margin each side */
#define MARGIN       16.0
#define STAGE_PAD    10.0
#define TITLE_H      18.0
#define ROW_H        24.0
#define EFF_W        130.0
#define POPUP_W      110.0
#define BTN_H        28.0
#define MIN_PAD_ROWS 2       /* rows kept for pads, more when more are connected */
#define MAX_PAD_ROWS 4
#define SLOW_TICKS   8       /* the 30 Hz tick's 250 ms part */
#define PILL_W       58.0
#define PILL_H       18.0
#define MAX_PILLS    48
#define WHEEL_SPIN   4.0     /* degrees the wheel's mark turns per tick of a held key */
#define KNOB_FULL    32767.0f

/* ---- colours (DESIGN-atari.md section 1: [P] read from a photo, [I] inferred) ---- */

#define COL_STAGE_TOP   0x6B6C72
#define COL_STAGE_BOT   0x55565B
#define COL_STAGE_EDGE  0x3A3B3F
#define COL_CAPTION     0xEEEEF2
#define COL_BLACK       0x1C1C1C   /* plastic black [P] */
#define COL_BLACK_TOP   0x262626
#define COL_BEVEL       0x2A2A2A
#define COL_DEEP        0x101010
#define COL_RED         0xE23B1F   /* fire red [P] */
#define COL_RED_HI      0xF05A38
#define COL_RED_DOWN    0xB02A14
#define COL_HELD        0xFFB04A   /* held highlight on black plastic [I] */
#define COL_SILVER      0xC9C9C9   /* silver toggle [P] */
#define COL_SILVER_SH   0x8E8E8E
#define COL_ORANGE      0xE58A1C   /* console orange [P] */
#define COL_LEGEND      0xE08A1F
#define COL_POWER_RED   0xC8341E
#define COL_PANEL       0x151515
#define COL_PILL        0x1A1A1C

/* ---- helpers -------------------------------------------------------------------- */

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) a2600session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

static NSColor *rgba(uint32_t value, CGFloat alpha)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:alpha];
}

/* The colour a fraction of the way from a to b. */
static uint32_t mixRgb(uint32_t from, uint32_t to, double frac)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double ca = (double)((from >> shift) & 0xff), cb = (double)((to >> shift) & 0xff);
        out |= (uint32_t)lround(ca + (cb - ca) * frac) << shift;
    }
    return out;
}

/* The accent: the family's orange, with white text on it. */
static NSColor *accent(void) { return rgb(A2600SESSION_ACCENT_RGB); }

/* Toggled-on look for a push button: accent bezel, white title. */
static void setButtonAccent(NSButton *button, BOOL on)
{
    if (!button) return;
    NSString *title = [button.title copy];
    if (on) {
        NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
        para.alignment = NSTextAlignmentCenter;
        button.bezelColor = accent();
        button.attributedTitle = [[NSAttributedString alloc]
            initWithString:title
                attributes:@{
                    NSFontAttributeName : button.font ?: [NSFont systemFontOfSize:NSFont.systemFontSize],
                    NSForegroundColorAttributeName : NSColor.whiteColor,
                    NSParagraphStyleAttributeName : para
                }];
    } else {
        button.bezelColor = nil;
        button.title = title;
    }
}

/* A point and a rectangle in a device's own layout units, from its box's
 * top-left corner (DESIGN-atari.md draws every device in such a box). */
static inline NSPoint atp(NSPoint org, CGFloat uu, CGFloat vv)
{
    return NSMakePoint(org.x + uu * UNIT, org.y + vv * UNIT);
}

static inline NSRect atr(NSPoint org, CGFloat uu, CGFloat vv, CGFloat ww, CGFloat hh)
{
    return NSMakeRect(org.x + uu * UNIT, org.y + vv * UNIT, ww * UNIT, hh * UNIT);
}

/* The square round shapes sit in: centre (cu, cv), radius rr. */
static inline NSRect discRect(NSPoint org, CGFloat cu, CGFloat cv, CGFloat rr)
{
    return NSMakeRect(org.x + (cu - rr) * UNIT, org.y + (cv - rr) * UNIT, 2 * rr * UNIT, 2 * rr * UNIT);
}

/* A linear gradient clipped to a path, from a to b, with alpha at each end. */
static void fillLinear(NSBezierPath *path, NSPoint from, NSPoint to, uint32_t rgbA, CGFloat alphaA,
                       uint32_t rgbB, CGFloat alphaB)
{
    const CGFloat comps[8] = {
        ((rgbA >> 16) & 0xff) / 255.0, ((rgbA >> 8) & 0xff) / 255.0, (rgbA & 0xff) / 255.0, alphaA,
        ((rgbB >> 16) & 0xff) / 255.0, ((rgbB >> 8) & 0xff) / 255.0, (rgbB & 0xff) / 255.0, alphaB };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(from.x, from.y), CGPointMake(to.x, to.y),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A vertical gradient clipped to a path (the views here are flipped, so "top"
 * is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMinX(bnd), NSMaxY(bnd)),
               topRgb, 1.0, botRgb, 1.0);
}

/* A horizontal one, for the cylinders of the slide switches. */
static void fillGradientH(NSBezierPath *path, uint32_t leftRgb, uint32_t rightRgb)
{
    const NSRect bnd = [path bounds];
    fillLinear(path, NSMakePoint(NSMinX(bnd), NSMinY(bnd)), NSMakePoint(NSMaxX(bnd), NSMinY(bnd)),
               leftRgb, 1.0, rightRgb, 1.0);
}

/* Lines of text centred in rect, the font shrunk (to 5 pt) until they fit.
 * `oblique` slants it, for the console's italic title. */
static void drawFittedStyled(NSString *text, NSRect rect, CGFloat size, BOOL bold, BOOL oblique, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        NSMutableDictionary *dict = [NSMutableDictionary dictionaryWithDictionary:@{
            NSFontAttributeName: font,
            NSParagraphStyleAttributeName: para,
            NSForegroundColorAttributeName: ink }];
        if (oblique) dict[NSObliquenessAttributeName] = @(0.2);
        attrs = dict;
        CGFloat widest = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((widest <= rect.size.width && lineHeight * count <= rect.size.height + 2) || size <= 5) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    drawFittedStyled(text, rect, size, bold, NO, ink);
}

static void strokeLine(NSPoint from, NSPoint to, CGFloat width, NSColor *colour)
{
    NSBezierPath *line = [NSBezierPath bezierPath];
    [line moveToPoint:from];
    [line lineToPoint:to];
    line.lineWidth = width;
    line.lineCapStyle = NSLineCapStyleRound;
    [colour setStroke];
    [line stroke];
}

static void fillOval(NSRect rect, NSColor *colour)
{
    [colour setFill];
    [[NSBezierPath bezierPathWithOvalInRect:rect] fill];
}

/* A regular polygon with `sides` corners, one at angle `start` degrees from
 * straight up, clockwise (an octagon cap). */
static NSBezierPath *polygon(NSPoint ctr, CGFloat rad, int sides, double start)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    for (int idx = 0; idx < sides; idx++) {
        const double ang = (start + 360.0 * idx / sides) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + rad * sin(ang), ctr.y - rad * cos(ang));
        if (idx == 0) [path moveToPoint:pnt]; else [path lineToPoint:pnt];
    }
    [path closePath];
    return path;
}

/* A label on a dark pill, filled with the accent when picked. */
static void drawPill(NSRect rect, NSString *text, BOOL picked)
{
    NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:rect.size.height / 2
                                                         yRadius:rect.size.height / 2];
    [(picked ? accent() : rgba(COL_PILL, 0.85)) setFill];
    [pill fill];
    drawFitted(text, NSInsetRect(rect, 6, 1), 10, YES, NSColor.whiteColor);
}

/* What the stick is deflected to from the directions the console sees held
 * (the opposed pairs are already cancelled): -1..1 on each axis, at most 1
 * long. */
static void deflection(unsigned held, float *ax, float *ay)
{
    float xval = (float)(((held >> A2600_ACT_JOY_RIGHT) & 1u)) - (float)(((held >> A2600_ACT_JOY_LEFT) & 1u));
    float yval = (float)(((held >> A2600_ACT_JOY_DOWN) & 1u)) - (float)(((held >> A2600_ACT_JOY_UP) & 1u));
    const float len = hypotf(xval, yval);
    if (len > 1.0f) {
        xval /= len;
        yval /= len;
    }
    *ax = xval;
    *ay = yval;
}

/* ---- views ---------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface A2600FlippedView : NSView
@end

@implementation A2600FlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class A2600ControllersWindow;
@interface A2600ControllersPanel : NSPanel
@property (nonatomic, weak) A2600ControllersWindow *owner;
@end

@interface A2600ControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)runSysaction:(int)sysact;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
- (BOOL)mapKeyFromApp:(uint32_t)keysym;
@end

/* One label the stage shows over a control while Map is armed or Show bindings
 * is on. */
typedef struct {
    NSRect rect;
    int target;
    char text[64];
} Pill;

/* The stage: both ports' controllers and the console on a grey backdrop.
 * Everything is drawn here and hit-tested by the core (the stick, the knob and
 * the wheel's drag maths are the core's too). */
@interface A2600StageView : A2600FlippedView
@property (nonatomic) a2600session *session;
@property (nonatomic, weak) A2600ControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads what the console sees held; redraws when anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation A2600StageView {
    const a2600_control *_ctls;
    int _count;
    float _dimW, _dimH;
    int _types[2];               /* the controller type drawn on each port */
    NSSize _size;

    unsigned _held[2];           /* what the console sees held, bit per a2600_action */
    int _swHeld[A2600_SW_COUNT];
    int _swPos[A2600_SW_COUNT];
    float _knob[2][2];           /* each paddle's position, -1..1 */
    float _wheel[2];             /* the driving wheel's mark, degrees clockwise from up */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downTarget;             /* the target the pointer holds with a press, or -1 */
    BOOL _stickDrag, _knobDrag, _wheelDrag, _sysDown;
    float _lastAngle, _wheelAcc;
    CGFloat _scrollAcc;
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downTarget = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

/* (Re)reads the layout for what the two ports have plugged in. */
- (void)loadLayout
{
    _types[0] = a2600session_view_type(_session, 0);
    _types[1] = a2600session_view_type(_session, 1);
    _ctls = a2600session_controls_layout(_types[0], _types[1], &_count, &_dimW, &_dimH);
    if (!_ctls) _count = 0;
    _size = NSMakeSize(WIN_W, STAGE_PAD * 2 + ceil(_dimH * UNIT));
    [self setNeedsDisplay:YES];
}

- (void)configure
{
    [self loadLayout];
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

/* Where the layout's origin is, in view points. */
- (NSPoint)origin
{
    return NSMakePoint(floor((WIN_W - _dimW * UNIT) / 2), STAGE_PAD);
}

- (NSRect)rectOf:(const a2600_control *)ctl
{
    const NSPoint org = [self origin];
    return NSMakeRect(org.x + ctl->x * UNIT, org.y + ctl->y * UNIT, ctl->w * UNIT, ctl->h * UNIT);
}

- (const a2600_control *)ctlAt:(int)idx
{
    return idx >= 0 && idx < _count ? &_ctls[idx] : NULL;
}

/* The pressable control under a view point, or -1. */
- (int)hit:(NSPoint)pnt
{
    const NSPoint org = [self origin];
    return a2600session_controls_hit(_types[0], _types[1], (float)((pnt.x - org.x) / UNIT),
                                     (float)((pnt.y - org.y) / UNIT));
}

/* The case a part belongs to: the last BODY entry before it with its port and
 * paddle, or NULL. */
- (const a2600_control *)bodyOf:(const a2600_control *)ctl
{
    const a2600_control *found = NULL;
    for (int idx = 0; idx < _count; idx++) {
        if (&_ctls[idx] == ctl) break;
        if (_ctls[idx].kind == A2600_CTL_BODY && _ctls[idx].port == ctl->port && _ctls[idx].paddle == ctl->paddle)
            found = &_ctls[idx];
    }
    return found;
}

- (int)swOf:(int)target
{
    const int base = A2600_TARGET_SWITCH(0);
    return target >= base && target < base + A2600_SW_COUNT ? target - base : -1;
}

/* Whether the console sees a target held (the window is a gamepad tester). */
- (BOOL)heldTarget:(int)target
{
    if (self.mapping || target < 0) return NO;
    if (target < 2 * A2600_ACT_PER_PORT)
        return ((_held[target / A2600_ACT_PER_PORT] >> (target % A2600_ACT_PER_PORT)) & 1u) != 0;
    const int sw = [self swOf:target];
    if (sw == A2600_SW_SELECT || sw == A2600_SW_RESET) return _swHeld[sw] != 0;
    return NO;
}

- (BOOL)isDown:(const a2600_control *)ctl
{
    return _sysDown && _downIdx >= 0 && &_ctls[_downIdx] == ctl;
}

/* ---- the labels shown over the controls ----------------------------------------- */

- (int)pills:(Pill *)out max:(int)max
{
    int num = 0;
    if (!(self.mapping || self.showKeys)) return 0;
#define ADD_PILL(cx, cy, wid, tgt, prefix) do { \
        if (num < max) { \
            Pill *pill = &out[num++]; \
            pill->rect = NSMakeRect((cx) - (wid) / 2, (cy) - PILL_H / 2, (wid), PILL_H); \
            pill->target = (tgt); \
            NSString *name = [self.owner legendForTarget:(tgt)]; \
            NSString *text = [NSString stringWithFormat:@"%@%@", prefix, name.length ? name : @"-"]; \
            strncpy(pill->text, text.UTF8String, sizeof pill->text - 1); \
            pill->text[sizeof pill->text - 1] = '\0'; \
        } } while (0)
    for (int idx = 0; idx < _count; idx++) {
        const a2600_control *ctl = &_ctls[idx];
        const NSRect rect = [self rectOf:ctl];
        const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
        switch (ctl->kind) {
        case A2600_CTL_STICK: {
            const int port = ctl->port;
            ADD_PILL(ctr.x - 2.35 * UNIT, ctr.y, PILL_W, A2600_TARGET_PORT(port, A2600_ACT_JOY_LEFT), @"◀ ");
            ADD_PILL(ctr.x + 2.35 * UNIT, ctr.y, PILL_W, A2600_TARGET_PORT(port, A2600_ACT_JOY_RIGHT), @"▶ ");
            ADD_PILL(ctr.x, ctr.y - 1.6 * UNIT, PILL_W, A2600_TARGET_PORT(port, A2600_ACT_JOY_UP), @"▲ ");
            ADD_PILL(ctr.x, ctr.y + 1.6 * UNIT, PILL_W, A2600_TARGET_PORT(port, A2600_ACT_JOY_DOWN), @"▼ ");
            break;
        }
        case A2600_CTL_TURN: {
            /* a pair under the case: the decrease key on the left, the increase on the right */
            const a2600_control *body = [self bodyOf:ctl];
            if (!body || ctl->target < 0) break;
            const NSRect brect = [self rectOf:body];
            const BOOL wheel = ctl->paddle < 0;
            const BOOL isDec = ctl->label && (ctl->label[0] == '<' || ctl->label[0] == 'C') &&
                               strcmp(ctl->label, "CW") != 0;
            const CGFloat xpos = NSMidX(brect) + (isDec ? -1.4 : 1.4) * UNIT;
            ADD_PILL(xpos, NSMaxY(brect) + 0.55 * UNIT, PILL_W, ctl->target,
                     (wheel ? (isDec ? @"↺ " : @"↻ ") : (isDec ? @"◀ " : @"▶ ")));
            break;
        }
        case A2600_CTL_BUTTON: {
            if (ctl->target < 0) break;
            if (ctl->port >= 0) {
                /* a paddle's fire button on the case's left edge: a label in the case */
                ADD_PILL(fmax(ctr.x + 0.5 * UNIT, NSMinX(rect) + 1.45 * UNIT), NSMaxY(rect) + 0.75 * UNIT, PILL_W,
                         ctl->target, @"");
            } else if (ctl->label && strcmp(ctl->label, "STOP") == 0) {
                ADD_PILL(NSMaxX(rect) + 1.55 * UNIT, ctr.y, PILL_W, ctl->target, @"");
            } else {
                ADD_PILL(ctr.x, NSMaxY(rect) + 0.45 * UNIT, 42.0, ctl->target, @"");
            }
            break;
        }
        case A2600_CTL_SWITCH:
            if (ctl->target >= 0) ADD_PILL(ctr.x, NSMaxY(rect) + 0.45 * UNIT, 42.0, ctl->target, @"");
            break;
        default:
            break;
        }
    }
#undef ADD_PILL
    return num;
}

/* The target Map would pick at a point (view points), or -1: a label, or a
 * button, a key, a turn arrow or a switch. */
- (int)mapTargetAt:(NSPoint)pnt
{
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        if (NSPointInRect(pnt, pills[idx].rect)) return pills[idx].target;
    const a2600_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl) return -1;
    switch (ctl->kind) {
    case A2600_CTL_BUTTON:
    case A2600_CTL_ROUND_BUTTON:
    case A2600_CTL_TURN:
    case A2600_CTL_SWITCH:
        return ctl->target;
    default:
        return -1;
    }
}

/* ---- polling ------------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    if (a2600session_view_type(_session, 0) != _types[0] || a2600session_view_type(_session, 1) != _types[1]) {
        [self letGo];                /* a held control may not exist in the new view */
        [self loadLayout];
        dirty = YES;
    }
    for (int port = 0; port < 2; port++) {
        const unsigned held = self.mapping ? 0u : a2600session_buttons_held(_session, port);
        if (held != _held[port]) { _held[port] = held; dirty = YES; }
        for (int pad = 0; pad < 2; pad++) {
            const float knob = (float)a2600session_analog_get(_session, port, pad) / KNOB_FULL;
            if (knob != _knob[port][pad]) { _knob[port][pad] = knob; dirty = YES; }
        }
        /* the wheel has no position the core reports: a held key turns its mark */
        if (!self.mapping && _types[port] == A2600_CTRL_DRIVING) {
            if (_held[port] & (1u << A2600_ACT_DRIVE_CCW)) { _wheel[port] -= (float)WHEEL_SPIN; dirty = YES; }
            if (_held[port] & (1u << A2600_ACT_DRIVE_CW)) { _wheel[port] += (float)WHEEL_SPIN; dirty = YES; }
        }
    }
    for (int sw = 0; sw < A2600_SW_COUNT; sw++) {
        const int held = !self.mapping && a2600session_switch_held(_session, sw) != 0 ? 1 : 0;
        const int pos = a2600session_switch_get(_session, sw) != 0 ? 1 : 0;
        if (held != _swHeld[sw] || pos != _swPos[sw]) {
            _swHeld[sw] = held;
            _swPos[sw] = pos;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------- */

- (NSString *)legendOfTarget:(int)target
{
    if (!(self.mapping || self.showKeys) || target < 0) return nil;
    return [self.owner legendForTarget:target];
}

- (BOOL)picked:(int)target
{
    return self.mapping && target >= 0 && target == self.mapTarget;
}

/* A held part: the orange outline. */
- (void)strokeHeld:(NSBezierPath *)path
{
    [rgb(COL_HELD) setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* The picked part in Map mode: the accent ring. */
- (void)ringPicked:(NSBezierPath *)path
{
    [accent() setStroke];
    path.lineWidth = 2.5;
    [path stroke];
}

/* A soft shadow on the stage under a shape. */
- (void)shadowOf:(NSBezierPath *)path
{
    NSBezierPath *shadow = [path copy];
    NSAffineTransform *move = [NSAffineTransform transform];
    [move translateXBy:2 yBy:4];
    [shadow transformUsingAffineTransform:move];
    [rgba(0x000000, 0.35) setFill];
    [shadow fill];
}

/* -- the CX40 joystick (DESIGN-atari.md section 2), seen from above in an 8 x 8 case -- */

- (void)drawJoystickCase:(NSRect)rect port:(int)port
{
    const NSPoint org = rect.origin;
    float defx = 0, defy = 0;
    deflection(_held[port], &defx, &defy);

    NSBezierPath *casePath = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.9 * UNIT yRadius:0.9 * UNIT];
    [self shadowOf:casePath];
    fillGradient(casePath, COL_BLACK_TOP, COL_BLACK);
    NSBezierPath *bevel = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.2 * UNIT, 0.2 * UNIT)
                                                          xRadius:0.75 * UNIT yRadius:0.75 * UNIT];
    bevel.lineWidth = 1.5;
    [rgb(COL_BEVEL) setStroke];
    [bevel stroke];

    /* the fire button's recess: a teardrop, its neck leading to the ring */
    strokeLine(atp(org, 1.7, 1.7), atp(org, 3.1, 3.1), 0.9 * UNIT, rgb(COL_DEEP));
    fillOval(discRect(org, 1.7, 1.7, 1.1), rgb(COL_DEEP));

    /* the ring: 36 tick dashes, the ones on the deflected side lit */
    fillOval(discRect(org, 4.5, 4.5, 3.0), rgb(COL_DEEP));
    const BOOL tilted = fabsf(defx) > 0.05f || fabsf(defy) > 0.05f;
    const double dirAngle = tilted ? atan2((double)defx, -(double)defy) * 180.0 / M_PI : 0;
    for (int tick = 0; tick < 36; tick++) {
        const double ang = tick * 10.0;
        double diff = fabs(fmod(ang - dirAngle + 540.0, 360.0) - 180.0);
        const BOOL lit = tilted && diff <= 25.0;
        const double rad = ang * M_PI / 180.0;
        strokeLine(atp(org, 4.5 + 2.55 * sin(rad), 4.5 - 2.55 * cos(rad)),
                   atp(org, 4.5 + 2.85 * sin(rad), 4.5 - 2.85 * cos(rad)), lit ? 2.5 : 1.4,
                   lit ? rgb(COL_HELD) : rgb(0x3A3A3A));
    }
    drawFitted(@"TOP", atr(org, 3.75, 1.6, 1.5, 0.6), 7, YES, rgb(COL_BEVEL));

    /* the boot: ribbed rings, squashed a little toward the tilt for depth */
    const CGFloat sx = defx * 0.14, sy = defy * 0.14;
    fillOval(discRect(org, 4.5 + sx, 4.5 + sy, 2.1), rgb(0x151515));
    fillOval(discRect(org, 4.5 + sx, 4.5 + sy, 1.7), rgb(0x262626));
    fillOval(discRect(org, 4.5 + sx * 1.3, 4.5 + sy * 1.3, 1.3), rgb(0x151515));
}

/* The shaft's cap: an octagon that slides toward the held direction. */
- (void)drawStickCap:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    float defx = 0, defy = 0;
    deflection(_held[ctl->port], &defx, &defy);
    const NSPoint ctr = NSMakePoint(NSMidX(rect) + defx * 1.0 * UNIT, NSMidY(rect) + defy * 1.0 * UNIT);
    NSBezierPath *cap = polygon(ctr, 0.85 * UNIT, 8, 22.5);
    [rgb(0x0C0C0C) setFill];
    [cap fill];
    [rgb(COL_BEVEL) setStroke];
    cap.lineWidth = 1.5;
    [cap stroke];
    fillOval(NSMakeRect(ctr.x - 0.2 * UNIT, ctr.y - 0.3 * UNIT, 0.4 * UNIT, 0.28 * UNIT), rgba(0xFFFFFF, 0.1));
    if (fabsf(defx) > 0.05f || fabsf(defy) > 0.05f)
        [self strokeHeld:polygon(ctr, 0.98 * UNIT, 8, 22.5)];
}

/* The joystick's one red fire button, or a keypad key (both are round). */
- (void)drawRoundButton:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const BOOL held = [self heldTarget:ctl->target];
    const BOOL fire = ctl->label && strcmp(ctl->label, "FIRE") == 0;
    NSString *legend = [self legendOfTarget:ctl->target];

    if (fire) {
        const CGFloat rad = 0.8 * UNIT;
        NSRect face = NSMakeRect(ctr.x - rad, ctr.y - rad + (held ? 1.5 : 0), 2 * rad, 2 * rad);
        NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:face];
        fillGradient(disc, held ? COL_RED_DOWN : COL_RED_HI, held ? mixRgb(COL_RED_DOWN, 0x000000, 0.15) : COL_RED);
        [rgba(0x000000, 0.3) setStroke];
        disc.lineWidth = 0.75;
        [disc stroke];
        if (!held)
            fillOval(NSMakeRect(ctr.x - 0.55 * UNIT, ctr.y - 0.6 * UNIT, 0.5 * UNIT, 0.32 * UNIT), rgba(0xFFFFFF, 0.3));
        if (held) [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(face, -3, -3)]];
        if (legend.length) drawFitted(legend, NSInsetRect(face, 3, 3), 10, YES, NSColor.whiteColor);
    } else {
        /* a CX50 key: glossy black with a light-blue legend printed above and to its left */
        const CGFloat rad = 0.5 * UNIT;
        NSRect face = NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad);
        NSBezierPath *disc = [NSBezierPath bezierPathWithOvalInRect:face];
        fillGradient(disc, held ? 0x3A3A3A : 0x1A1A1A, 0x080808);
        fillOval(NSMakeRect(ctr.x - 0.32 * UNIT, ctr.y - 0.42 * UNIT, 0.64 * UNIT, 0.34 * UNIT), rgba(0x2C2C2C, 0.9));
        if (held) [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(face, -2, -2)]];
        if (legend.length)
            drawFitted(legend, NSInsetRect(face, 2, 4), 9, YES, NSColor.whiteColor);
        else
            drawFitted(nsstr(ctl->label), NSMakeRect(NSMinX(face) - 0.45 * UNIT, NSMinY(face) - 0.42 * UNIT,
                                                      0.62 * UNIT, 0.55 * UNIT), 10, YES, rgb(0x8CCFEA));
    }
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(NSMakeRect(ctr.x - (fire ? 0.8 : 0.5) * UNIT,
                                                                                       ctr.y - (fire ? 0.8 : 0.5) * UNIT,
                                                                                       (fire ? 1.6 : 1.0) * UNIT,
                                                                                       (fire ? 1.6 : 1.0) * UNIT), -5, -5)]];
}

/* -- the CX30 paddle and the CX20 driving controller (sections 3 and 4) --------- */

- (void)drawPaddleCase:(const a2600_control *)ctl rect:(NSRect)rect
{
    const NSPoint org = rect.origin;
    const BOOL driving = ctl->paddle < 0;
    /* the rounded trapezoid, narrower at the cable end; its corners are rounded by
     * a stroke of the same colour round a polygon pulled in toward its centre */
    static const CGFloat pts[6][2] = { {1.0, 0.3}, {5.6, 0.3}, {7.0, 5.0}, {6.2, 5.9}, {0.8, 5.9}, {0.0, 4.6} };
    const CGFloat cx = 3.4, cy = 3.1, shrink = 0.93;
    NSBezierPath *body = [NSBezierPath bezierPath];
    for (int idx = 0; idx < 6; idx++) {
        const NSPoint pnt = atp(org, cx + (pts[idx][0] - cx) * shrink, cy + (pts[idx][1] - cy) * shrink);
        if (idx == 0) [body moveToPoint:pnt]; else [body lineToPoint:pnt];
    }
    [body closePath];
    body.lineJoinStyle = NSLineJoinStyleRound;
    body.lineWidth = 0.8 * UNIT;
    [self shadowOf:body];
    [rgb(0x1F1F1F) setFill];
    [body fill];
    [rgb(0x1F1F1F) setStroke];
    [body stroke];
    NSBezierPath *edge = [body copy];
    edge.lineWidth = 1.0;
    [rgb(0x2E2E2E) setStroke];
    [edge stroke];

    /* the label plate: "paddle" with crossed tennis rackets, or "driving" with a racing car */
    const NSRect plate = atr(org, 3.6, 4.3, 2.8, 1.3);
    NSBezierPath *platePath = [NSBezierPath bezierPathWithRoundedRect:plate xRadius:0.15 * UNIT yRadius:0.15 * UNIT];
    [rgb(COL_DEEP) setFill];
    [platePath fill];
    [rgb(0x8A8A8A) setStroke];
    platePath.lineWidth = 1;
    [platePath stroke];
    if (!driving) {
        drawFitted(@"paddle", NSMakeRect(NSMinX(plate) + 0.12 * UNIT, NSMinY(plate) + 0.15 * UNIT, 1.5 * UNIT, 0.6 * UNIT),
                   9, YES, rgb(0xE6E6E6));
        /* two crossed rackets and a ball */
        const NSPoint rk = atp(org, 5.45, 4.95);
        strokeLine(NSMakePoint(rk.x - 0.35 * UNIT, rk.y + 0.4 * UNIT), NSMakePoint(rk.x + 0.15 * UNIT, rk.y - 0.15 * UNIT), 1.2,
                   rgb(0xDCCB78));
        strokeLine(NSMakePoint(rk.x + 0.35 * UNIT, rk.y + 0.4 * UNIT), NSMakePoint(rk.x - 0.15 * UNIT, rk.y - 0.15 * UNIT), 1.2,
                   rgb(0xDCCB78));
        [rgb(0xDCCB78) setStroke];
        NSBezierPath *head = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(rk.x + 0.02 * UNIT, rk.y - 0.42 * UNIT,
                                                                               0.34 * UNIT, 0.34 * UNIT)];
        head.lineWidth = 1.1;
        [head stroke];
        fillOval(NSMakeRect(rk.x - 0.5 * UNIT, rk.y - 0.35 * UNIT, 0.14 * UNIT, 0.14 * UNIT), rgb(0xDCCB78));
    } else {
        drawFitted(@"driving", NSMakeRect(NSMinX(plate) + 0.1 * UNIT, NSMinY(plate) + 0.12 * UNIT, 1.5 * UNIT, 0.6 * UNIT),
                   9, YES, rgb(0xE6E6E6));
        /* the racing car: a low orange body, a cockpit and two wheels */
        const NSPoint car = atp(org, 5.0, 5.05);
        NSBezierPath *bodyCar = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(car.x - 0.55 * UNIT, car.y - 0.12 * UNIT,
                                                                                    1.1 * UNIT, 0.28 * UNIT)
                                                                xRadius:3 yRadius:3];
        [rgb(0xE8501A) setFill];
        [bodyCar fill];
        fillOval(NSMakeRect(car.x - 0.12 * UNIT, car.y - 0.3 * UNIT, 0.3 * UNIT, 0.24 * UNIT), rgb(0xE8501A));
        fillOval(NSMakeRect(car.x - 0.5 * UNIT, car.y + 0.08 * UNIT, 0.26 * UNIT, 0.26 * UNIT), rgb(0xE6E6E6));
        fillOval(NSMakeRect(car.x + 0.26 * UNIT, car.y + 0.08 * UNIT, 0.26 * UNIT, 0.26 * UNIT), rgb(0xE6E6E6));
    }
}

/* The ribbed knob (a paddle's, turning +-135 degrees) or the driving wheel
 * (turning freely, with 12 gear-code ticks); the mark shows where it is. */
- (void)drawKnob:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const NSPoint ctr = NSMakePoint(NSMidX(rect), NSMidY(rect));
    const CGFloat rad = NSWidth(rect) / 2;
    const BOOL wheel = ctl->kind == A2600_CTL_WHEEL;
    const int port = ctl->port;

    fillOval(NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad), rgb(0x0E0E0E));
    for (int rib = 0; rib < 60; rib++) {
        const double ang = rib * M_PI / 30.0;
        strokeLine(NSMakePoint(ctr.x + rad * 0.9 * sin(ang), ctr.y - rad * 0.9 * cos(ang)),
                   NSMakePoint(ctr.x + rad * 0.99 * sin(ang), ctr.y - rad * 0.99 * cos(ang)), 1.2, rgb(0x2A2A2A));
    }
    fillOval(NSMakeRect(ctr.x - rad * 0.8, ctr.y - rad * 0.8, rad * 1.6, rad * 1.6), rgb(0x1A1A1A));
    NSBezierPath *cap = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(ctr.x - rad * 0.75, ctr.y - rad * 0.75,
                                                                          rad * 1.5, rad * 1.5)];
    fillGradient(cap, 0x2B2B2B, 0x1C1C1C);
    if (wheel) {
        /* a gear-code tick every 30 degrees on the cap's edge */
        for (int tick = 0; tick < 12; tick++) {
            const double ang = tick * M_PI / 6.0;
            strokeLine(NSMakePoint(ctr.x + rad * 0.62 * sin(ang), ctr.y - rad * 0.62 * cos(ang)),
                       NSMakePoint(ctr.x + rad * 0.7 * sin(ang), ctr.y - rad * 0.7 * cos(ang)), 1, rgb(0x3A3A3A));
        }
    }
    /* the position mark (added art: the real knob has none) */
    const double deg = wheel ? (double)_wheel[port] : (double)_knob[port][ctl->paddle] * A2600_KNOB_TRAVEL_DEG;
    const double ang = deg * M_PI / 180.0;
    strokeLine(NSMakePoint(ctr.x + rad * 0.4 * sin(ang), ctr.y - rad * 0.4 * cos(ang)),
               NSMakePoint(ctr.x + rad * 0.7 * sin(ang), ctr.y - rad * 0.7 * cos(ang)), 0.12 * UNIT, rgb(0xD8D8D8));
    if (_knobDrag && _downIdx >= 0 && &_ctls[_downIdx] == ctl)
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad), -2, -2)]];
    if (_wheelDrag && _downIdx >= 0 && &_ctls[_downIdx] == ctl)
        [self strokeHeld:[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(NSMakeRect(ctr.x - rad, ctr.y - rad, 2 * rad, 2 * rad), -2, -2)]];
}

/* The paddle's red fire button, on the case's left edge. */
- (void)drawPaddleButton:(const a2600_control *)ctl
{
    NSRect rect = [self rectOf:ctl];
    const BOOL held = [self heldTarget:ctl->target];
    if (held) rect.origin.x += 1;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.2 * UNIT yRadius:0.2 * UNIT];
    fillGradient(path, held ? 0xB22E18 : 0xE8492C, held ? 0x9A2412 : 0xD93A22);
    [rgba(0x000000, 0.35) setStroke];
    path.lineWidth = 0.75;
    [path stroke];
    if (held) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -3, -3) xRadius:6 yRadius:6]];
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -5, -5) xRadius:8 yRadius:8]];
}

/* A small turn arrow under a knob (the paddle's left and right, the wheel's CCW
 * and CW): a triangle that lights while its key is held. */
- (void)drawTurn:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL held = [self heldTarget:ctl->target];
    const BOOL left = ctl->label && (ctl->label[0] == '<' || (ctl->label[0] == 'C' && strcmp(ctl->label, "CW") != 0));
    NSBezierPath *tri = [NSBezierPath bezierPath];
    if (left) {
        [tri moveToPoint:NSMakePoint(NSMaxX(rect), NSMinY(rect))];
        [tri lineToPoint:NSMakePoint(NSMinX(rect), NSMidY(rect))];
        [tri lineToPoint:NSMakePoint(NSMaxX(rect), NSMaxY(rect))];
    } else {
        [tri moveToPoint:NSMakePoint(NSMinX(rect), NSMinY(rect))];
        [tri lineToPoint:NSMakePoint(NSMaxX(rect), NSMidY(rect))];
        [tri lineToPoint:NSMakePoint(NSMinX(rect), NSMaxY(rect))];
    }
    [tri closePath];
    [(held ? rgb(COL_HELD) : rgb(0x6A6A6A)) setFill];
    [tri fill];
    if ([self picked:ctl->target]) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -5, -5) xRadius:6 yRadius:6];
        [self ringPicked:ring];
    }
}

/* -- the CX50 keypad controller (section 5) -------------------------------------- */

- (void)drawKeypadCase:(NSRect)rect
{
    const NSPoint org = rect.origin;
    NSBezierPath *casePath = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.6 * UNIT yRadius:0.6 * UNIT];
    [self shadowOf:casePath];
    [rgb(0x141414) setFill];
    [casePath fill];
    NSBezierPath *face = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.45 * UNIT, 0.45 * UNIT)
                                                         xRadius:0.35 * UNIT yRadius:0.35 * UNIT];
    [rgb(0x1A1A1A) setFill];
    [face fill];
    [rgb(0xD7D7D7) setStroke];
    face.lineWidth = 1.2;
    [face stroke];
    drawFitted(@"Keyboard\nController", atr(org, 0.7, 0.85, 4.2, 1.35), 12, YES, rgb(0x8CCFEA));
}

/* -- the console (section 6) ----------------------------------------------------- */

- (void)drawConsolePanel:(NSRect)rect
{
    NSBezierPath *panel = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:0.4 * UNIT yRadius:0.4 * UNIT];
    [self shadowOf:panel];
    [rgb(COL_PANEL) setFill];
    [panel fill];
    NSBezierPath *pin = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.25 * UNIT, 0.25 * UNIT)
                                                        xRadius:0.25 * UNIT yRadius:0.25 * UNIT];
    [rgb(COL_ORANGE) setStroke];
    pin.lineWidth = 1.2;
    [pin stroke];
    drawFittedStyled(@"video computer system", NSMakeRect(NSMidX(rect) - 4.2 * UNIT, NSMinY(rect) + 0.55 * UNIT,
                                                           8.4 * UNIT, 0.7 * UNIT), 12, YES, YES, rgb(0xE6DFCF));
}

- (void)drawCartridgeSlot:(NSRect)rect
{
    NSBezierPath *rim = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -2) xRadius:5 yRadius:5];
    [rgb(COL_BLACK) setFill];
    [rim fill];
    NSBezierPath *slot = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    [rgb(0x0A0A0A) setFill];
    [slot fill];
}

/* The woodgrain strip, with the maker's name at its right end. */
- (void)drawWood:(NSRect)rect
{
    NSBezierPath *strip = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:3 yRadius:3];
    fillGradient(strip, 0x8A5A2B, 0x6E4521);
    [NSGraphicsContext saveGraphicsState];
    [strip addClip];
    for (int line = 0; line < 22; line++) {
        const CGFloat yy = NSMinY(rect) + (CGFloat)line * NSHeight(rect) / 22.0 + 1.5;
        NSBezierPath *grain = [NSBezierPath bezierPath];
        [grain moveToPoint:NSMakePoint(NSMinX(rect), yy)];
        for (int seg = 1; seg <= 24; seg++) {
            const CGFloat xx = NSMinX(rect) + NSWidth(rect) * seg / 24.0;
            [grain lineToPoint:NSMakePoint(xx, yy + 1.6 * sin(seg * 0.9 + line * 1.7))];
        }
        grain.lineWidth = 0.8;
        [rgba(0x4A2F16, 0.18) setStroke];
        [grain stroke];
    }
    [NSGraphicsContext restoreGraphicsState];
    drawFittedStyled(@"ATARI", NSMakeRect(NSMaxX(rect) - 4.2 * UNIT, NSMinY(rect) + 0.25 * UNIT, 3.6 * UNIT, 1.1 * UNIT),
                     16, YES, NO, rgb(0xC7C7C7));
}

/* A slide switch: a slot with a silver toggle at its top or bottom, the legends
 * the real console prints round it. `up` says where the toggle sits; the
 * momentary switches (SELECT, RESET) rest up and tilt down, lighting their arrow,
 * while held. */
- (void)drawSlide:(NSRect)rect legend:(NSString *)legend topLabel:(NSString *)topLabel bottomLabel:(NSString *)bottomLabel
               up:(BOOL)up arrow:(BOOL)arrow lit:(BOOL)lit pressed:(BOOL)pressed picked:(BOOL)picked
        legendInk:(uint32_t)legendInk
{
    const CGFloat midX = NSMidX(rect);
    const NSRect slot = NSMakeRect(midX - 0.25 * UNIT, NSMinY(rect) + 0.35 * UNIT, 0.5 * UNIT, 1.5 * UNIT);
    NSBezierPath *slotPath = [NSBezierPath bezierPathWithRoundedRect:slot xRadius:3 yRadius:3];
    [rgb(0x0A0A0A) setFill];
    [slotPath fill];
    const CGFloat tog = 0.7 * UNIT;
    const CGFloat ty = up ? NSMinY(slot) - 1 : NSMaxY(slot) - tog + 1;
    NSBezierPath *toggle = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(midX - 0.275 * UNIT, ty, 0.55 * UNIT, tog)
                                                           xRadius:2.5 yRadius:2.5];
    fillGradientH(toggle, COL_SILVER, COL_SILVER_SH);
    [rgba(0x000000, 0.4) setStroke];
    toggle.lineWidth = 0.75;
    [toggle stroke];
    strokeLine(NSMakePoint(midX - 0.2 * UNIT, ty + tog / 2), NSMakePoint(midX + 0.2 * UNIT, ty + tog / 2), 0.8,
               rgba(0x000000, 0.35));
    if (pressed) [self strokeHeld:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(slot, -4, -2) xRadius:5 yRadius:5]];

    /* the printed legends */
    if (legend.length)
        drawFitted(legend, NSMakeRect(midX - 1.6 * UNIT, NSMinY(rect) - 0.85 * UNIT, 3.2 * UNIT, 0.6 * UNIT), 10, YES,
                   rgb(legendInk));
    if (topLabel.length)
        drawFitted(topLabel, NSMakeRect(midX + 0.35 * UNIT, NSMinY(slot) - 0.05 * UNIT, 1.5 * UNIT, 0.5 * UNIT), 9, NO,
                   rgb(COL_LEGEND));
    if (bottomLabel.length)
        drawFitted(bottomLabel, NSMakeRect(midX + 0.35 * UNIT, NSMaxY(slot) - 0.5 * UNIT, 1.5 * UNIT, 0.5 * UNIT), 9, NO,
                   rgb(COL_LEGEND));
    if (arrow) {
        /* the orange sawtooth arrow under the momentary switches */
        NSBezierPath *saw = [NSBezierPath bezierPath];
        const CGFloat ay = NSMaxY(slot) + 0.45 * UNIT;
        [saw moveToPoint:NSMakePoint(midX - 0.6 * UNIT, ay)];
        for (int tooth = 0; tooth < 4; tooth++) {
            [saw lineToPoint:NSMakePoint(midX - 0.6 * UNIT + (tooth + 1) * 0.3 * UNIT, ay + (tooth % 2 ? 0 : -0.18 * UNIT))];
        }
        saw.lineWidth = 1.4;
        [(lit ? rgb(COL_HELD) : rgb(COL_LEGEND)) setStroke];
        [saw stroke];
    }
    if (picked) {
        NSBezierPath *ring = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -2, -6) xRadius:7 yRadius:7];
        [self ringPicked:ring];
    }
}

/* The STOP button on the wood strip (the session's Pause, which the real console
 * has no switch for). */
- (void)drawStop:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const BOOL down = [self isDown:ctl];
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:5 yRadius:5];
    fillGradient(path, down ? COL_SILVER_SH : 0xDADADA, down ? 0x7A7A7A : COL_SILVER_SH);
    [rgba(0x000000, 0.4) setStroke];
    path.lineWidth = 1;
    [path stroke];
    NSString *legend = [self legendOfTarget:ctl->target];
    drawFitted(@"STOP", rect, 10, YES, rgb(0x1A1A1A));
    (void)legend;
    if ([self picked:ctl->target])
        [self ringPicked:[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, -4, -4) xRadius:8 yRadius:8]];
}

- (void)drawConsoleControl:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const int sw = [self swOf:ctl->target];
    const BOOL picked = [self picked:ctl->target];
    NSString *legend = ctl->legend ? [NSString stringWithUTF8String:ctl->legend] : @"";
    if (ctl->kind == A2600_CTL_SWITCH) {
        /* the position the core reports: 1 is colour / A, the toggle's top */
        NSString *topLabel = @"", *bottomLabel = @"";
        if (sw == A2600_SW_COLOR_BW) { topLabel = @"color"; bottomLabel = @"b-w"; }
        else { topLabel = @"a"; bottomLabel = @"b"; }
        [self drawSlide:rect legend:legend topLabel:topLabel bottomLabel:bottomLabel up:sw >= 0 && _swPos[sw] != 0
                  arrow:NO lit:NO pressed:NO picked:picked legendInk:COL_LEGEND];
    } else if (sw == A2600_SW_SELECT || sw == A2600_SW_RESET) {
        const BOOL held = sw >= 0 && _swHeld[sw] != 0;
        [self drawSlide:rect legend:legend topLabel:@"" bottomLabel:@"" up:!held arrow:YES lit:held pressed:held
                 picked:picked legendInk:COL_LEGEND];
    } else {
        /* POWER: drawn on, but pressing it power-cycles (Reboot to CONFIG) */
        const BOOL down = [self isDown:ctl];
        [self drawSlide:rect legend:legend topLabel:@"on" bottomLabel:@"off" up:!down arrow:NO lit:NO pressed:down
                 picked:picked legendInk:COL_POWER_RED];
    }
}

/* ---- the whole picture ------------------------------------------------------------- */

- (void)drawBody:(const a2600_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    const char *name = ctl->label ? ctl->label : "";
    if (!strcmp(name, "Joystick")) [self drawJoystickCase:rect port:ctl->port];
    else if (!strcmp(name, "Paddle A") || !strcmp(name, "Paddle B") || !strcmp(name, "Driving")) [self drawPaddleCase:ctl rect:rect];
    else if (!strcmp(name, "Keypad")) [self drawKeypadCase:rect];
    else if (!strcmp(name, "Console")) [self drawConsolePanel:rect];
    else if (!strcmp(name, "Cartridge slot")) [self drawCartridgeSlot:rect];
    else if (!strcmp(name, "Wood")) [self drawWood:rect];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *stage = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, WIN_W, _size.height) xRadius:10 yRadius:10];
    fillGradient(stage, COL_STAGE_TOP, COL_STAGE_BOT);
    [rgb(COL_STAGE_EDGE) setStroke];
    stage.lineWidth = 1;
    [stage stroke];

    for (int idx = 0; idx < _count; idx++) {
        const a2600_control *ctl = &_ctls[idx];
        switch (ctl->kind) {
        case A2600_CTL_CAPTION:
            drawFitted(nsstr(ctl->label), [self rectOf:ctl], 12, YES, rgb(COL_CAPTION));
            break;
        case A2600_CTL_BODY:
            [self drawBody:ctl];
            break;
        case A2600_CTL_STICK:
            [self drawStickCap:ctl];
            break;
        case A2600_CTL_KNOB:
        case A2600_CTL_WHEEL:
            [self drawKnob:ctl];
            break;
        case A2600_CTL_TURN:
            [self drawTurn:ctl];
            break;
        case A2600_CTL_ROUND_BUTTON:
            [self drawRoundButton:ctl];
            break;
        case A2600_CTL_BUTTON:
            if (ctl->port >= 0) [self drawPaddleButton:ctl];
            else if (ctl->label && !strcmp(ctl->label, "STOP")) [self drawStop:ctl];
            else [self drawConsoleControl:ctl];
            break;
        case A2600_CTL_SWITCH:
            [self drawConsoleControl:ctl];
            break;
        default:
            break;
        }
    }

    /* the Map / Show bindings labels, over everything */
    Pill pills[MAX_PILLS];
    const int num = [self pills:pills max:MAX_PILLS];
    for (int idx = 0; idx < num; idx++)
        drawPill(pills[idx].rect, nsstr(pills[idx].text), [self picked:pills[idx].target]);
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)letGo
{
    if (_downTarget >= 0) a2600session_press(_session, _downTarget, 0);
    if (_downIdx >= 0 && _downIdx < _count) {
        const a2600_control *ctl = &_ctls[_downIdx];
        if (_stickDrag) a2600session_pointer_set(_session, ctl->port, 0);
        if (_knobDrag) a2600session_pointer_analog(_session, ctl->port, ctl->paddle, 0, 0);
    }
    _downIdx = -1;
    _downTarget = -1;
    _stickDrag = _knobDrag = _wheelDrag = _sysDown = NO;
    _wheelAcc = 0;
    [self setNeedsDisplay:YES];
}

/* The stick follows the pointer: the core turns the offset into one of eight
 * directions (nothing near the middle). */
- (void)dragStickAt:(NSPoint)pnt
{
    const a2600_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const unsigned mask = a2600session_stick_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)),
                                                         (float)(NSWidth(rect) / 2));
    a2600session_pointer_set(_session, ctl->port, mask);
}

/* The knob goes where the pointer's angle puts it, and holds there. */
- (void)dragKnobAt:(NSPoint)pnt
{
    const a2600_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const int value = a2600session_knob_from_vector((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)));
    a2600session_pointer_analog(_session, ctl->port, ctl->paddle, value, 1);
}

/* The driving wheel: one notch of the core's twist pulse per A2600_WHEEL_STEP_DEG
 * of the pointer's travel round it, the mark following. */
- (void)dragWheelAt:(NSPoint)pnt
{
    const a2600_control *ctl = [self ctlAt:_downIdx];
    if (!ctl) return;
    const NSRect rect = [self rectOf:ctl];
    const float angle = a2600session_vector_angle((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)));
    float delta = angle - _lastAngle;
    while (delta > 180.0f) delta -= 360.0f;      /* the shortest way round */
    while (delta < -180.0f) delta += 360.0f;
    _lastAngle = angle;
    _wheel[ctl->port] += delta;
    _wheelAcc += delta;
    while (_wheelAcc >= A2600_WHEEL_STEP_DEG) {
        a2600session_twist_pulse(_session, ctl->port, 1);
        _wheelAcc -= A2600_WHEEL_STEP_DEG;
    }
    while (_wheelAcc <= -A2600_WHEEL_STEP_DEG) {
        a2600session_twist_pulse(_session, ctl->port, -1);
        _wheelAcc += A2600_WHEEL_STEP_DEG;
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    if (self.mapping) {
        /* Map: a label, a button, a key, a turn arrow or a switch is picked */
        const int target = [self mapTargetAt:pnt];
        if (target >= 0) [self.owner mapPick:target];
        return;
    }

    const int idx = [self hit:pnt];
    const a2600_control *ctl = [self ctlAt:idx];
    if (!ctl) return;
    switch (ctl->kind) {
    case A2600_CTL_STICK:
        _downIdx = idx;
        _stickDrag = YES;
        [self dragStickAt:pnt];
        break;
    case A2600_CTL_KNOB:
        _downIdx = idx;
        _knobDrag = YES;
        [self dragKnobAt:pnt];
        break;
    case A2600_CTL_WHEEL: {
        const NSRect rect = [self rectOf:ctl];
        _downIdx = idx;
        _wheelDrag = YES;
        _wheelAcc = 0;
        _lastAngle = a2600session_vector_angle((float)(pnt.x - NSMidX(rect)), (float)(pnt.y - NSMidY(rect)));
        break;
    }
    case A2600_CTL_BUTTON:
    case A2600_CTL_ROUND_BUTTON:
    case A2600_CTL_TURN:
    case A2600_CTL_SWITCH:
        if (ctl->target < 0) break;
        _downIdx = idx;
        if (ctl->target >= A2600_TARGET_SYSACT(0)) {
            _sysDown = YES;       /* POWER and STOP act on release over them */
        } else {
            _downTarget = ctl->target;
            a2600session_press(_session, ctl->target, 1);
        }
        break;
    default:
        break;
    }
    [self setNeedsDisplay:YES];
    [self poll];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downIdx < 0 || _sysDown) return;
    if (_knobDrag) {
        [self dragKnobAt:pnt];
        [self poll];
        return;
    }
    if (_wheelDrag) {
        [self dragWheelAt:pnt];
        return;
    }
    if (_stickDrag) {
        [self dragStickAt:pnt];
        [self poll];
        return;
    }
    /* a button: dragging off lets go */
    const a2600_control *ctl = [self ctlAt:_downIdx];
    if (ctl && !NSPointInRect(pnt, [self rectOf:ctl])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)evt
{
    [self dragTo:[self convertPoint:[evt locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)evt
{
    if (_downIdx < 0) return;
    if (_sysDown) {
        /* a console action acts on release over the button, like a real one */
        const a2600_control *ctl = [self ctlAt:_downIdx];
        const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
        const BOOL inside = ctl && NSPointInRect(pnt, [self rectOf:ctl]);
        const int target = ctl ? ctl->target : -1;
        [self letGo];
        if (inside && target >= A2600_TARGET_SYSACT(0)) [self.owner runSysaction:target - A2600_TARGET_SYSACT(0)];
        return;
    }
    [self letGo];
    [self poll];
}

/* The wheel over a knob nudges it (up turns it right); over the driving wheel it
 * notches it. */
- (void)scrollWheel:(NSEvent *)evt
{
    if (self.mapping) return;
    const a2600_control *ctl = [self ctlAt:[self hit:[self convertPoint:[evt locationInWindow] fromView:nil]]];
    if (!ctl) return;
    if (ctl->kind == A2600_CTL_KNOB) {
        const CGFloat step = [evt hasPreciseScrollingDeltas] ? [evt scrollingDeltaY] * 0.01
                                                              : [evt scrollingDeltaY] * 0.05;
        int value = a2600session_analog_get(_session, ctl->port, ctl->paddle) + (int)lround(step * 40000.0);
        if (value > 32767) value = 32767;
        if (value < -32767) value = -32767;
        a2600session_pointer_analog(_session, ctl->port, ctl->paddle, value, 1);
        a2600session_pointer_analog(_session, ctl->port, ctl->paddle, value, 0);
        [self poll];
    } else if (ctl->kind == A2600_CTL_WHEEL) {
        _scrollAcc += [evt scrollingDeltaY];
        const CGFloat per = [evt hasPreciseScrollingDeltas] ? 6.0 : 1.0;
        while (_scrollAcc >= per) {
            a2600session_twist_pulse(_session, ctl->port, 1);
            _wheel[ctl->port] += (float)A2600_WHEEL_STEP_DEG;
            _scrollAcc -= per;
        }
        while (_scrollAcc <= -per) {
            a2600session_twist_pulse(_session, ctl->port, -1);
            _wheel[ctl->port] -= (float)A2600_WHEEL_STEP_DEG;
            _scrollAcc += per;
        }
        [self setNeedsDisplay:YES];
    }
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    const a2600_control *ctl = [self ctlAt:[self hit:pnt]];
    if (!ctl || ctl->target < 0) return nil;
    const a2600_binding bind = a2600session_binding_get(_session, ctl->target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"none";
    NSString *padText = bind.button != A2600_PAD_BTN_NONE ? nsstr(a2600_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(a2600_target_name(ctl->target)), key, padText];
}

/* One tooltip rect over the whole stage; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation A2600ControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* AppKit would beep for Esc with no responder for it. */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

static __weak A2600ControllersWindow *g_window;

@implementation A2600ControllersWindow {
    a2600session *_session;
    A2600AppKeyHandler _appKeys;
    A2600ControllersPanel *_panel;
    A2600FlippedView *_content;
    A2600StageView *_stage;
    NSTextField *_typeLabel[2];
    NSPopUpButton *_typePopup[2];
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "plays the left port" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;                           /* the rows showing a pad */
    int _reservedRows;                      /* the rows the panel has room for */
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(a2600session *)session appKeys:(A2600AppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _padGeneration = (unsigned)-1;
    _reservedRows = MIN_PAD_ROWS;
    g_window = self;

    _panel = [[A2600ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, WIN_W, 700)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    A2600SetHelpContext(_panel, @"controllers");

    _content = [[A2600FlippedView alloc] initWithFrame:NSMakeRect(0, 0, WIN_W, 700)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _stage = [[A2600StageView alloc] initWithFrame:NSZeroRect];
    _stage.owner = self;
    _stage.session = _session;
    _stage.mapTarget = -1;
    _stage.showKeys = a2600session_get_int(_session, "controllers_show_bindings", 0) > 0;
    [_content addSubview:_stage];

    /* what each port has plugged in (also in Settings) */
    for (int port = 0; port < 2; port++) {
        _typeLabel[port] = [NSTextField labelWithString:(port ? @"Right port" : @"Left port")];
        [_content addSubview:_typeLabel[port]];
        _typePopup[port] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _typePopup[port].tag = port;
        _typePopup[port].target = self;
        _typePopup[port].action = @selector(typeChanged:);
        [_typePopup[port] setRefusesFirstResponder:YES];
        for (int type = 0; a2600_ctrl_type_name(type); type++)
            [_typePopup[port] addItemWithTitle:nsstr(a2600_ctrl_type_name(type))];
        [_content addSubview:_typePopup[port]];
    }

    /* Gamepads: one row per pad, its name, the port it plays and the one it is
     * set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_stage.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = A2600HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_stage configure];
    [self rebuildPickList];
    [self refreshTypes];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- layout ------------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize ssize = _stage.contentSize;
    const CGFloat inner = WIN_W - 2 * MARGIN;
    CGFloat ypos = ssize.height + 8;
    const CGFloat typeY = ypos;
    ypos += 26 + 8;
    const CGFloat padsY = ypos;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + _reservedRows * ROW_H + 6;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat hintY = pickY + 26 + 4;
    const CGFloat total = hintY + 30 + MARGIN;

    [self fitPanel:NSMakeSize(WIN_W, total)];
    [_stage setFrame:NSMakeRect(0, 0, ssize.width, ssize.height)];
    [_typeLabel[0] setFrame:NSMakeRect(MARGIN, typeY + 4, 62, 18)];
    [_typePopup[0] setFrame:NSMakeRect(MARGIN + 66, typeY, 200, 26)];
    [_typeLabel[1] setFrame:NSMakeRect(MARGIN + 300, typeY + 4, 68, 18)];
    [_typePopup[1] setFrame:NSMakeRect(MARGIN + 372, typeY, 200, 26)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 3, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat rowY = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, rowY + 4, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, rowY + 4, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, rowY - 1, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(WIN_W - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, inner - 54, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 30)];
    [_stage setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_stage letGo];
}

/* ---- the port types ------------------------------------------------------------------ */

/* What each port has plugged in, as the session has it. The Auto row says what
 * Stella attached. */
- (void)refreshTypes
{
    for (int port = 0; port < 2; port++) {
        NSPopUpButton *popup = _typePopup[port];
        const int type = a2600session_port_type(_session, port);
        NSString *autoTitle = [NSString stringWithFormat:@"Auto: %@",
                               nsstr(a2600_ctrl_type_name(a2600session_detected_port_type(_session, port)))];
        if (popup.numberOfItems > 0 && ![[popup itemTitleAtIndex:0] isEqualToString:autoTitle])
            [[popup itemAtIndex:0] setTitle:autoTitle];
        if (type >= 0 && type < popup.numberOfItems && popup.indexOfSelectedItem != type)
            [popup selectItemAtIndex:type];
    }
}

- (void)typeChanged:(NSPopUpButton *)sender
{
    [self setMapState:-2];
    [self letGo];
    a2600session_set_port_type(_session, (int)sender.tag, (int)sender.indexOfSelectedItem);
    [_stage poll];
    [self refreshTypes];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (void)rebuildPickList
{
    [_pickPopup removeAllItems];
    for (int target = 0; target < A2600_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(a2600_target_name(target)) action:nil keyEquivalent:@""];
}

/* The bound host key as the picture's legend. */
- (NSString *)legendForTarget:(int)target
{
    return keyName(a2600session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const a2600_binding bind = a2600session_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"no key";
    if (bind.button != A2600_PAD_BTN_NONE)
        return [NSString stringWithFormat:@"%@, pad %@", key, nsstr(a2600_pad_button_name(bind.button))];
    return key;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _stage.mapping = _mapState != -2;
    _stage.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_stage setNeedsDisplay:YES];
    [_stage poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        a2600session_release_all(_session);
        a2600session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(a2600_target_name(state)), [self bindingText:state]];
    } else {
        a2600session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control or a label, or choose one from the list, to remap it."
            : @"Hold a button, drag the stick or a knob, turn the wheel, flip a switch. "
              @"Keys and gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    setButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < _pickPopup.numberOfItems) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2 && target >= 0 && target < A2600_TARGET_COUNT) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < A2600_TARGET_COUNT) [self setMapState:(int)sel];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(a2600_target_name(target));
    _hint.stringValue = stolen && stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym
{
    const int target = _mapState;
    char stolen[128];
    stolen[0] = '\0';
    a2600session_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = A2600_PAD_BTN_NONE;
    if (_mapState < 0 || !a2600session_gamepad_capture_poll(_session, &button)) return;
    const int target = _mapState;
    char stolen[128];
    stolen[0] = '\0';
    a2600session_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(a2600_pad_button_name(button)) target:target stolen:stolen];
}

/* A key typed while Map waits for one, from any window (KeyForward's hook): Esc
 * backs out of the pick; F9, F11 and F12 are the app's. */
- (BOOL)mapKeyFromApp:(uint32_t)keysym
{
    if (_mapState < 0) return NO;
    if (a2600session_key_reserved(keysym)) return NO;
    if (keysym == A2600_KEYSYM_ESCAPE) [self setMapState:-1];
    else [self bindKey:keysym];
    return YES;
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    a2600session_binding_clear(_session, target);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(a2600_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    a2600session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    a2600session_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _stage.showKeys = show;
    [_stage setNeedsDisplay:YES];
}

/* POWER or STOP released over itself on the panel. */
- (void)runSysaction:(int)sysact
{
    if (self.sysactionHandler) self.sysactionHandler(sysact);
    else a2600session_sysaction(_session, sysact);
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = a2600session_gamepad_effective_port(_session, row);
        NSString *text = port < 0 ? @"plays nothing" : (port == 0 ? @"plays the left port" : @"plays the right port");
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = a2600session_gamepad_count(_session);
    _padGeneration = a2600session_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    /* room for the pads there are (two rows to start with, more when more
     * pads are connected); the panel changes size only then */
    const int rooms = _padRows > MIN_PAD_ROWS ? _padRows : MIN_PAD_ROWS;
    if (rooms != _reservedRows) {
        _reservedRows = rooms;
        [self relayout];
    }
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        a2600session_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        [_padAssign[row] removeAllItems];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Left", @"Right"]];
        const int assign = a2600session_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    a2600session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads and a port type changed in Settings. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (a2600session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        [self refreshTypes];
    }
    [_stage poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak A2600ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command
     * is down, so forwarding one would leave the machine holding the key. */
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = A2600KeysymFromEvent(evt);
    if (keysym == A2600_KEYSYM_F9 || keysym == A2600_KEYSYM_F12) {
        if (![evt isARepeat] && _appKeys) _appKeys(keysym);
        return;
    }
    if (!keysym || keysym == A2600_KEYSYM_F11) return;
    if ([evt isARepeat]) return;
    if (_mapState >= 0) {
        /* the next key is the binding; a Ctrl chord is not one; Esc backs out */
        if (!([evt modifierFlags] & NSEventModifierFlagControl)) [self mapKeyFromApp:keysym];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* the main window's path: a bound key drives its controller, or a bound
     * console action runs */
    const int sa = a2600session_key_sysaction(_session, keysym);
    if (sa >= 0) { [self runSysaction:sa]; return; }
    a2600session_key(_session, keysym, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    if (_mapState != -2) return;
    const uint32_t keysym = A2600KeysymFromEvent(evt);
    if (keysym) a2600session_key(_session, keysym, 0);
}

/* Shift, Control and Option bind too. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    const uint32_t keysym = A2600KeysymFromFlagsChange(evt, &down);
    if (!keysym || _mapState == -1) return;
    if (_mapState >= 0) {
        if (down) [self mapKeyFromApp:keysym];
        return;
    }
    a2600session_key(_session, keysym, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    a2600session_release_all(_session);
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self rebuildPickList];
    [self refreshTypes];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    [self refreshTypes];
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    a2600session_release_all(_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end

BOOL A2600ControllersMapKey(uint32_t keysym)
{
    A2600ControllersWindow *win = g_window;
    return win ? [win mapKeyFromApp:keysym] : NO;
}
