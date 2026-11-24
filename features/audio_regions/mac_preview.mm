#import <Cocoa/Cocoa.h>
#include "reaper/reaper_plugin.h"
#include "mac_preview.hpp"
#include <utility>

@interface MEAudioRegionOverlay : NSView {
    @public
    MacRegionInput input;
    std::vector<MacRegionBox> boxes;
    id escapeMonitor;
    bool regionsVisible;
}
- (void)stopTrackingEscape;
@end

static POINT screenPoint(NSEvent* event) {
    NSWindow* window = [event window];
    NSPoint point = window ? [window convertPointToScreen:[event locationInWindow]]
                           : [NSEvent mouseLocation];
    return {static_cast<LONG>(point.x),static_cast<LONG>(point.y)};
}

@implementation MEAudioRegionOverlay
- (BOOL)isFlipped { return YES; }

- (NSView*)hitTest:(NSPoint)point {
    const auto keys = [NSEvent modifierFlags];
    if (!regionsVisible || !(keys & NSEventModifierFlagOption) ||
        (keys & (NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagCommand)))
        return nil;
    const auto local = [self convertPoint:point fromView:[self superview]];
    if (!NSPointInRect(local,[self bounds]) || !input.canStart(int(local.x))) return nil;
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!regionsVisible) return;
    const auto bounds = [self bounds];
    [[NSColor colorWithWhite:0.0 alpha:104.0/255.0] setFill];
    CGFloat covered = 0;
    for (const auto& box : boxes) {
        if (box.left > covered)
            NSRectFillUsingOperation(NSMakeRect(covered,0,box.left-covered,NSHeight(bounds)),
                                     NSCompositingOperationSourceOver);
        if (box.right > covered) covered = box.right;
    }
    if (NSWidth(bounds) > covered)
        NSRectFillUsingOperation(NSMakeRect(covered,0,NSWidth(bounds)-covered,NSHeight(bounds)),
                                 NSCompositingOperationSourceOver);
    for (const auto& box : boxes) {
        NSColor* color = box.active ? [NSColor colorWithSRGBRed:1 green:.71 blue:.29 alpha:1]
                                   : [NSColor colorWithSRGBRed:.14 green:.67 blue:.62 alpha:1];
        [color setFill];
        NSRectFill(NSMakeRect(box.left,0,box.right-box.left,1));
        NSRectFill(NSMakeRect(box.left,NSHeight(bounds)-1,box.right-box.left,1));
        if (box.left >= 0) NSRectFill(NSMakeRect(box.left,0,1,NSHeight(bounds)));
        if (box.right <= NSWidth(bounds)) NSRectFill(NSMakeRect(box.right-1,0,1,NSHeight(bounds)));
    }
}

- (void)mouseDown:(NSEvent*)event {
    const auto point = [self convertPoint:[event locationInWindow] fromView:nil];
    if (!input.begin(int(point.x),screenPoint(event))) return;
    [self stopTrackingEscape];
    escapeMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent*(NSEvent* key) {
            if ([key keyCode] != 53) return key; // Escape
            input.cancel();
            return nil;
        }];
}
- (void)mouseDragged:(NSEvent*)event { input.move(screenPoint(event)); }
- (void)mouseUp:(NSEvent*)event {
    [self stopTrackingEscape];
    input.end(screenPoint(event));
}
- (void)stopTrackingEscape {
    if (escapeMonitor) [NSEvent removeMonitor:escapeMonitor];
    escapeMonitor = nil;
}
@end

@interface MEAudioRegionDropPreview : NSView
@end

@implementation MEAudioRegionDropPreview
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor colorWithWhite:0.5 alpha:0.8] setFill];
    NSRectFillUsingOperation(dirtyRect, NSCompositingOperationSourceOver);
}
@end

namespace {
MEAudioRegionDropPreview* dropPreview;
}

bool macExplorerActive(HWND explorer) {
    id widget = (id)explorer;
    if ([widget isKindOfClass:[NSView class]]) return [[(NSView*)widget window] isKeyWindow];
    if ([widget isKindOfClass:[NSWindow class]]) return [(NSWindow*)widget isKeyWindow];
    return false;
}

bool macApplicationActive() { return [NSApp isActive]; }

HWND createMacRegionOverlay(HWND wave, MacRegionInput input) {
    id widget = (id)wave;
    if (![widget isKindOfClass:[NSView class]]) return nullptr;
    auto overlay = [[MEAudioRegionOverlay alloc] initWithFrame:NSZeroRect];
    overlay->input = input;
    [(NSView*)widget addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
    return (HWND)overlay;
}

bool macRegionOverlayAttached(HWND overlay, HWND wave) {
    return overlay && wave && [(NSView*)overlay superview] == (NSView*)wave;
}

void updateMacRegionOverlay(HWND handle, int width, int height, bool visible, std::vector<MacRegionBox> boxes) {
    auto overlay = (MEAudioRegionOverlay*)handle;
    NSView* parent = [overlay superview];
    if (!parent) return;
    const auto bounds = [parent bounds];
    const auto y = [parent isFlipped] ? NSMinY(bounds) : NSMaxY(bounds)-height;
    const auto frame = NSMakeRect(NSMinX(bounds),y,width,height);
    if (!NSEqualRects([overlay frame],frame)) [overlay setFrame:frame];
    overlay->regionsVisible = visible;
    overlay->boxes = std::move(boxes);
    [overlay setNeedsDisplay:YES];
}

void destroyMacRegionOverlay(HWND handle) {
    auto overlay = (MEAudioRegionOverlay*)handle;
    stopMacRegionTracking(handle);
    [overlay removeFromSuperview];
    [overlay release];
}

void stopMacRegionTracking(HWND handle) {
    [(MEAudioRegionOverlay*)handle stopTrackingEscape];
}

void showMacDropPreview(HWND arrange, const RECT& bounds) {
    id widget = (id)arrange;
    if (![widget isKindOfClass:[NSView class]]) { hideMacDropPreview(); return; }
    NSView* parent = (NSView*)widget;
    const auto parentBounds = [parent bounds];
    const auto y = [parent isFlipped] ? NSMinY(parentBounds)+bounds.top
                                      : NSMaxY(parentBounds)-bounds.bottom;
    const auto rect = NSMakeRect(NSMinX(parentBounds)+bounds.left, y,
                                 bounds.right-bounds.left,bounds.bottom-bounds.top);
    if ([dropPreview superview] != parent) {
        hideMacDropPreview();
        dropPreview = [[MEAudioRegionDropPreview alloc] initWithFrame:rect];
        [parent addSubview:dropPreview positioned:NSWindowAbove relativeTo:nil];
    } else {
        [dropPreview setFrame:rect];
    }
    [dropPreview setNeedsDisplay:YES];
}

void hideMacDropPreview() {
    [dropPreview removeFromSuperview];
    [dropPreview release];
    dropPreview = nil;
}
