/*
 * A2600DisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

@implementation A2600DisplayView {
    a2600session *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    int _height;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    A2600DisplayView *self = (__bridge A2600DisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(a2600session *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 960, 720)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = (size_t)A2600SESSION_FB_WIDTH * A2600SESSION_FB_MAX_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _cs = CGColorSpaceCreateDeviceRGB();
    /* Stella's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit context with the alpha byte skipped reads them as they are. */
    _ctx = CGBitmapContextCreate(_fb, A2600SESSION_FB_WIDTH, A2600SESSION_FB_MAX_HEIGHT, 8,
                                 A2600SESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
}

- (void)tick:(uint64_t)hostTime
{
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    a2600session_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    int h = 0;
    if (!a2600session_copy_frame(_session, _fb, &h, &_serial)) return;
    _height = h;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (_height <= 0) return;

    CGImageRef whole = CGBitmapContextCreateImage(_ctx);
    if (!whole) return;
    /* Only the lines the ROM drew this frame. */
    CGImageRef img = CGImageCreateWithImageInRect(whole, CGRectMake(0, 0, A2600SESSION_FB_WIDTH, _height));
    CGImageRelease(whole);
    if (!img) return;

    /* A television showed whatever lines the game drew inside its 4:3
     * screen, with each TIA pixel about twice as wide as it is tall. */
    const double want = _tv ? (4.0 / 3.0) : (double)A2600SESSION_FB_WIDTH / (double)_height;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}
@end
