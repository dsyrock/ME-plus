#pragma once
#include <vector>

bool macExplorerActive(HWND explorer);
bool macApplicationActive();
struct MacRegionBox { int left, right; bool active; };
struct MacRegionInput {
    bool (*canStart)(int x);
    bool (*begin)(int x, POINT screen);
    void (*move)(POINT screen);
    void (*end)(POINT screen);
    void (*cancel)();
};
HWND createMacRegionOverlay(HWND wave, MacRegionInput input);
bool macRegionOverlayAttached(HWND overlay, HWND wave);
void updateMacRegionOverlay(HWND overlay, int width, int height, bool visible, std::vector<MacRegionBox> boxes);
void stopMacRegionTracking(HWND overlay);
void destroyMacRegionOverlay(HWND overlay);
void showMacDropPreview(HWND arrange, const RECT& bounds);
void hideMacDropPreview();
