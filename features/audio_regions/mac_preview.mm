#import <Cocoa/Cocoa.h>
#include "reaper/reaper_plugin.h"
#include "mac_preview.hpp"

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
