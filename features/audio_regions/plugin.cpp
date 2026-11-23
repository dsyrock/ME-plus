#include "api.hpp"
#include "selection.hpp"
#include "detector.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <iomanip>
#include <locale>
#ifdef _WIN32
#include <commctrl.h>
#include "native_view.hpp"
#endif

namespace {
using namespace regions;
using Clock = std::chrono::steady_clock;
constexpr char section[] = "MediaExplorerAudioRegions";
constexpr int viewId = 27961;
REAPER_PLUGIN_HINSTANCE instance;
HWND browser, wave, strip;
HWND mainWindow;
Settings settings;
Selection current;
std::string path, message;
fs::file_time_type modified{};
std::unique_ptr<Detector> analysis;
using Source = std::unique_ptr<PCM_source, void (*)(PCM_source*)>;
Source decoder{nullptr, [](PCM_source* source){ PCM_Source_Destroy(source); }};
double rate;
int channels;
std::vector<double> samples;
bool ready, busy, enabled = true;
int picked = -1;
POINT press{};
int viewWidth, viewHeight;
#ifndef _WIN32
int previewHeight;
#endif
Clock::time_point nextPoll{}, nextStat{}, nextPaint{}, openAfter{};
gaccel_register_t actions[2]{};
constexpr int mediaExplorerSection = 32063;
custom_action_register_t explorerActions[2]{};
int explorerCommands[2]{};

#ifdef _WIN32
void hideDrop();
#endif
void repaint() { if (strip) InvalidateRect(strip, nullptr, FALSE); }
void cancelDrag() {
    bool wasDragging = picked >= 0;
    picked = -1;
#ifdef _WIN32
    hideDrop();
#endif
    if (wasDragging && GetCapture() ==
#ifdef _WIN32
        wave
#else
        strip
#endif
    ) ReleaseCapture();
}
void resetFile(std::string file) {
    cancelDrag();
    decoder.reset();
    analysis.reset();
    ready = false;
    path = std::move(file);
    std::error_code ec;
    modified = fs::last_write_time(fs::u8path(path), ec);
    message = path.empty() ? "Select an audio file" : "Reading audio...";
    openAfter = Clock::now() + std::chrono::milliseconds(100);
    repaint();
}
void fail(const char* why) {
    decoder.reset();
    analysis.reset();
    ready = false;
    message = why;
    repaint();
}
void scanAudio() {
    if (path.empty() || ready || Clock::now() < openAfter) return;
    if (!decoder) {
        openAfter = Clock::time_point::max();
        decoder.reset(PCM_Source_CreateFromFileEx(path.c_str(), true));
        if (!decoder || !decoder->IsAvailable()) return fail("Audio decoder unavailable");
        bool beats = false;
        double duration = GetMediaSourceLength(decoder.get(), &beats);
        rate = decoder->GetSampleRate();
        channels = decoder->GetNumChannels();
        double frames = duration * rate;
        if (beats || !std::isfinite(frames) || rate <= 0 || frames < 0.5 ||
            frames > 9e15 || channels < 1 || channels > 256)
            return fail("No supported audio stream");
        analysis = std::make_unique<Detector>(std::llround(frames), rate, settings);
        samples.resize(8192 * channels);
        openAfter = Clock::time_point{};
    }
    auto deadline = Clock::now() + std::chrono::milliseconds(4);
    do {
        PCM_source_transfer_t block{};
        block.time_s = analysis->position / rate;
        block.samplerate = rate;
        block.nch = channels;
        block.length = int(std::min<Frame>(analysis->total() - analysis->position, 8192));
        block.samples = samples.data();
        decoder->GetSamples(&block);
        if (block.samples_out <= 0 || block.samples_out > block.length)
            return fail("Could not decode the full audio file");
        analysis->append(samples.data(), block.samples_out, channels);
        if (analysis->position == analysis->total()) {
            analysis->finish();
            ready = true;
            decoder.reset();
            message = analysis->intervals.empty() ? "No sound above threshold" : "";
            repaint();
            return;
        }
    } while (Clock::now() < deadline);
    if (Clock::now() >= nextPaint) {
        message = "Reading audio... " + std::to_string(int(100. * analysis->position / analysis->total())) + "%";
        nextPaint = Clock::now() + std::chrono::milliseconds(160);
        repaint();
    }
}

void fill(HDC dc, RECT r, HBRUSH brush) { FillRect(dc, &r, brush); }
#ifdef _WIN32
constexpr COLORREF dropColor = RGB(128,128,128);
constexpr wchar_t className[] = L"ReaperAudioRegionStrip";
HWND dropPreview;
bool reveal;
bool dragStarted;
constexpr int previewScrollbars = 42169;
std::uintptr_t nativeViewportAddress;
int waveformBottom;
BOOL (WINAPI *getScrollInfo)(HWND,int,LPSCROLLINFO);
struct WaveView {
    // With no reliable viewport, map the full source to the available width.
    double first = 0, span = 1;
    bool operator==(const WaveView& v) const {
        return first == v.first && span == v.span;
    }
};
WaveView lastView;
WaveView waveView() {
    NativeViewport native{};
    if (nativeViewportAddress && readLocalImage(nativeViewportAddress,native) &&
        std::isfinite(native.first) && std::isfinite(native.end) &&
        native.first >= 0 && native.end <= 1.000001 && native.end > native.first)
        return {native.first,native.end-native.first};
    // Unknown DLLs can still expose their viewport through an enabled scrollbar.
    if (GetToggleCommandStateEx(32063,previewScrollbars) != 1) return {};
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!getScrollInfo(wave,SB_HORZ,&info) || !info.nPage || info.nMax <= info.nMin) return {};
    double extent = double(info.nMax)-info.nMin;
    return {(info.nPos-info.nMin)/extent, std::min(1.,info.nPage/extent)};
}
double frameAtPixel(int x, WaveView view) {
    return (view.first + double(x)*view.span/viewWidth)*analysis->total();
}
int pixelAtFrame(Frame frame, WaveView view, bool right) {
    double x = (double(frame)/analysis->total() - view.first)*viewWidth/view.span;
    x = std::clamp(x, -1., double(viewWidth)+1);
    return int(right ? std::ceil(x) : std::floor(x));
}
void paint(HWND w) {
    PAINTSTRUCT ps{};
    BeginPaint(w,&ps);
    EndPaint(w,&ps);
    const int width = viewWidth;
    const int height = waveformBottom;
    if (width <= 0 || height <= 0) return;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    auto dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    void* data = nullptr;
    auto bitmap = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
    if (!bitmap) { DeleteDC(dc); return; }
    auto previous = SelectObject(dc,bitmap);
    auto pixels = static_cast<std::uint32_t*>(data);
    std::fill_n(pixels,size_t(width)*height,0);

    const auto view = waveView();
    if (reveal && ready && analysis && height > 4) {
        constexpr int top = 2;
        const int bottom = height-2;
        // Premultiplied BGRA. Shade the gaps while leaving region interiors clear.
        constexpr std::uint32_t shade = 0x68000000;
        std::fill(pixels+size_t(top)*width,pixels+size_t(bottom)*width,shade);
        const auto& spans = analysis->intervals;
        auto first = std::lower_bound(spans.begin(),spans.end(),frameAtPixel(0,view),
            [](Interval span,double frame){ return span.end <= frame; });
        const double viewEnd = frameAtPixel(width,view);
        for (auto i=first; i!=spans.end() && i->first < viewEnd; ++i) {
            const int left = pixelAtFrame(i->first,view,false);
            const int right = pixelAtFrame(i->end,view,true);
            const int from = std::max(0,left);
            const int to = std::min(width,right);
            if (from >= to) continue;
            const std::uint32_t border = i-spans.begin()==picked ? 0xffffb54a : 0xff24aa9e;
            for (int y=top; y<bottom; ++y) {
                auto row = pixels+size_t(y)*width;
                const bool edge = y==top || y==bottom-1;
                std::fill(row+from,row+to,edge ? border : 0);
                if (left >= 0) row[from] = border;
                if (right <= width) row[to-1] = border;
            }
        }
    }
    SIZE size{width,height};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(w,nullptr,nullptr,&size,dc,&source,0,&blend,ULW_ALPHA);
    SelectObject(dc,previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
#else
void paint(HWND w) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(w, &ps);
    RECT rc{};
    GetClientRect(w, &rc);
    const int width = rc.right, height = rc.bottom, mid = height / 2;
    HBRUSH background = CreateSolidBrush(RGB(20,28,35));
    fill(dc, rc, background);
    DeleteObject(background);
    if (ready && analysis && width > 0 && height > 0) {
        HBRUSH box = CreateSolidBrush(RGB(86,197,194));
        HBRUSH inside = CreateSolidBrush(RGB(32,65,75));
        HBRUSH active = CreateSolidBrush(RGB(80,88,49));
        for (size_t i = 0; i < analysis->intervals.size(); ++i) {
            auto r = analysis->intervals[i];
            int left = int(double(r.first) * width / analysis->total());
            int right = int(std::ceil(double(r.end) * width / analysis->total()));
            fill(dc, {left, 1, right, height-1}, box);
            if (right-left > 2)
                fill(dc, {left+1, 2, right-1, height-2}, int(i) == picked ? active : inside);
        }
        DeleteObject(box); DeleteObject(inside); DeleteObject(active);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(148,217,232));
        auto old = SelectObject(dc, pen);
        const auto& peaks = analysis->peaks;
        for (int x = 0; x < width; ++x) {
            auto binAt = [&](int pixel) {
                return size_t((analysis->total() * double(pixel) / width) / analysis->binWidth());
            };
            size_t a = std::min(peaks.size()-1, binAt(x));
            size_t b = std::min(peaks.size(), std::max(a+1, binAt(x+1)));
            float peak = *std::max_element(peaks.begin()+a, peaks.begin()+b);
            int h = int(std::sqrt(peak) * std::max(0, mid-3));
            MoveToEx(dc, x, mid-h, nullptr);
            LineTo(dc, x, mid+h+1);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    if (!message.empty()) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(223,233,237));
        rc.left += 5;
#ifdef _WIN32
        DrawTextA(dc, message.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER);
#else
        DrawText(dc, message.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER);
#endif
    }
    EndPaint(w, &ps);
}
#endif
int hit(int x) {
    if (!ready || !analysis || x < 0 || x >= viewWidth) return -1;
#ifdef _WIN32
    const auto view = waveView();
    double first = frameAtPixel(x,view);
    double end = frameAtPixel(x+1,view);
#else
    double first = double(x) * analysis->total() / viewWidth;
    double end = double(x+1) * analysis->total() / viewWidth;
#endif
    auto& spans = analysis->intervals;
    auto it = std::lower_bound(spans.begin(), spans.end(), first,
        [](Interval r, double start){ return r.end <= start; });
    return it != spans.end() && it->first < end ? int(it-spans.begin()) : -1;
}
struct Target { MediaTrack* track = nullptr; double time = 0; int lane = -1; };
bool locate(POINT p, Target& target) {
    char what[128]{};
    target.track = GetThingFromPoint(p.x, p.y, what, sizeof(what));
    if (target.track) {
        if (std::strcmp(what, "arrange") != 0 || target.track == GetMasterTrack(nullptr)) return false;
    } else {
        // REAPER 7.84 returns an empty description below its last track.
        HWND arrange = GetDlgItem(mainWindow, 1000);
        if (!arrange || WindowFromPoint(p) != arrange) return false;
        POINT client = p;
        ScreenToClient(arrange,&client);
        RECT bounds{};
        GetClientRect(arrange,&bounds);
        if (client.x < 0 || client.y < 0 || client.x >= bounds.right || client.y >= bounds.bottom) return false;
    }
    double right = 0;
    GetSet_ArrangeView2(nullptr, false, p.x, p.x+1, &target.time, &right);
    if (GetToggleCommandStateEx(0, 1157) == 1) target.time = SnapToGrid(nullptr, target.time);
    if (target.track && GetMediaTrackInfo_Value(target.track, "I_FREEMODE") == 2) {
        int info = 0;
        GetTrackFromPoint(p.x, p.y, &info);
        target.lane = (info >> 8) & 255;
    }
    return std::isfinite(target.time);
}
#ifdef _WIN32
bool altHeld() {
    return browser && GetForegroundWindow() == GetAncestor(browser,GA_ROOT) &&
           (GetAsyncKeyState(VK_MENU) & 0x8000);
}
void hideDrop() { if (dropPreview) ShowWindow(dropPreview,SW_HIDE); }
void drawDrop(POINT p, const Target& target) {
    HWND arrange = GetDlgItem(mainWindow,1000);
    double viewStart = 0, viewEnd = 0;
    GetSet_ArrangeView2(nullptr,false,0,0,&viewStart,&viewEnd);
    double zoom = GetHZoomLevel();
    const auto span = analysis->intervals[picked];
    if (!arrange || !std::isfinite(zoom) || zoom <= 0) return hideDrop();
    RECT bounds{};
    GetClientRect(arrange,&bounds);
    ScreenToClient(arrange,&p);
    double start = (target.time-viewStart)*zoom;
    double end = start + (span.end-span.first)/rate*zoom;
    int left = int(std::clamp(std::floor(start),0.,double(bounds.right)));
    int right = int(std::clamp(std::ceil(end),0.,double(bounds.right)));
    int top = p.y, height = MulDiv(48,GetDpiForWindow(arrange),96);
    if (target.track) {
        top = int(GetMediaTrackInfo_Value(target.track,"I_TCPY"));
        height = int(GetMediaTrackInfo_Value(target.track,"I_TCPH"));
        if (target.lane >= 0) {
            // Anchor to the lane under the pointer rather than filling the entire track.
            int lanes = int(GetMediaTrackInfo_Value(target.track,"I_NUMFIXEDLANES"));
            if (lanes > 1 && target.lane < lanes) {
                int laneHeight = std::max(1,height/lanes);
                top += target.lane*laneHeight;
                height = laneHeight;
            }
        }
    }
    int bottom = std::min(int(bounds.bottom),top+height);
    top = std::max(0,top);
    if (right <= left || bottom <= top) return hideDrop();
    if (!dropPreview) {
        dropPreview = CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
            className,L"Audio region drop preview",WS_POPUP,0,0,1,1,mainWindow,nullptr,instance,nullptr);
        if (!dropPreview) return;
        if (!SetLayeredWindowAttributes(dropPreview,0,205,LWA_ALPHA)) {
            DestroyWindow(dropPreview);
            return;
        }
    }
    POINT origin{left,top};
    ClientToScreen(arrange,&origin);
    SetWindowPos(dropPreview,HWND_TOPMOST,origin.x,origin.y,right-left,bottom-top,
        SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
#endif
bool insert(const Target& target, int index) {
    const Interval span = analysis->intervals.at(index);
    Source source{PCM_Source_CreateFromFileEx(path.c_str(), true), decoder.get_deleter()};
    if (!source || !source->IsAvailable() || source->GetSampleRate() != rate) return false;
    std::string name = fs::u8path(path).filename().u8string() + " [" + std::to_string(index+1) + "]";
    MediaTrack* track = target.track;
    Undo_BeginBlock2(nullptr);
    if (!track) {
        int count = CountTracks(nullptr);
        InsertTrackAtIndex(count, true);
        track = GetTrack(nullptr, count);
    }
    auto* item = track ? AddMediaItemToTrack(track) : nullptr;
    auto* take = item ? AddTakeToMediaItem(item) : nullptr;
    bool ok = false;
    if (take) {
        auto* old = GetMediaItemTake_Source(take);
        GetSetMediaItemTakeInfo(take, "P_SOURCE", source.get());
        if (GetMediaItemTake_Source(take) == source.get()) {
            source.release();
            if (old) PCM_Source_Destroy(old);
            ok = SetMediaItemTakeInfo_Value(take, "D_STARTOFFS", span.first / rate) &&
                SetMediaItemInfo_Value(item, "D_LENGTH", (span.end-span.first) / rate) &&
                SetMediaItemInfo_Value(item, "D_POSITION", target.time);
            SetMediaItemInfo_Value(item, "B_LOOPSRC", 0);
            if (target.lane >= 0) SetMediaItemInfo_Value(item, "I_FIXEDLANE", target.lane);
            GetSetMediaItemTakeInfo_String(take, "P_NAME", name.data(), true);
        }
    }
    if (!ok && item) DeleteTrackMediaItem(track, item);
    if (!ok && !target.track && track) DeleteTrack(track);
    if (!target.track) TrackList_AdjustWindows(false);
    UpdateArrange();
    Undo_EndBlock2(nullptr, "Insert Media Explorer audio region", -1);
    return ok;
}
bool dragDistance(POINT p) { return std::abs(p.x-press.x) >= 4 || std::abs(p.y-press.y) >= 4; }
bool handle(HWND w, UINT msg, WPARAM, LPARAM lp) {
    POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
    switch (msg) {
    case WM_PAINT: paint(w); return true;
    case WM_ERASEBKGND: return true;
    case WM_LBUTTONDOWN:
        picked = hit(p.x);
        if (picked >= 0) { press = p; SetCapture(w); repaint(); }
        return true;
    case WM_MOUSEMOVE:
        if (picked >= 0 && dragDistance(p)) {
            ClientToScreen(w, &p);
            Target target;
            bool valid = locate(p,target);
#ifdef _WIN32
            if (valid) drawDrop(p,target); else hideDrop();
#endif
            SetCursor(LoadCursor(nullptr, valid ? IDC_SIZEALL : IDC_NO));
        }
#ifdef _WIN32
        else hideDrop();
#endif
        return true;
    case WM_LBUTTONUP: {
        int index = picked;
        bool moved = dragDistance(p);
        cancelDrag();
        if (index >= 0 && moved && ready && selected(browser) == current) {
            ClientToScreen(w, &p);
            Target target;
            std::error_code ec;
            if (locate(p,target) && fs::last_write_time(fs::u8path(path),ec) == modified && !ec &&
                !insert(target,index)) message = "Could not insert audio region";
        }
        repaint();
        return true;
    }
    case WM_CAPTURECHANGED:
#ifdef _WIN32
        hideDrop();
#endif
        picked = -1; repaint(); return true;
#ifdef _WIN32
    case WM_CANCELMODE:
        cancelDrag(); repaint(); return true;
#endif
    default: return false;
    }
}
#ifdef _WIN32
std::wstring previewTime(double seconds) {
    const auto minutes = static_cast<long long>(seconds/60);
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << minutes << L':' << std::fixed << std::setprecision(9) << seconds-minutes*60.;
    return out.str();
}
void selectPreviewRegion(int index) {
    if (!ready || !analysis || !(selected(browser) == current)) return;
    constexpr int startId = 1455, endId = 1456;
    HWND start = GetDlgItem(browser,startId), end = GetDlgItem(browser,endId);
    if (!start || !end) return;
    const auto span = analysis->intervals[index];
    const auto startText = previewTime(span.first/rate);
    const auto endText = previewTime(span.end/rate);
    if (SetWindowTextW(start,startText.c_str()) && SetWindowTextW(end,endText.c_str())) {
        // REAPER debounces EN_CHANGE and then commits both native time fields.
        SendMessage(browser,WM_COMMAND,MAKEWPARAM(endId,EN_CHANGE),reinterpret_cast<LPARAM>(end));
    }
}
LRESULT CALLBACK procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    if (m==WM_PAINT) {
        if (w == dropPreview) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(w,&ps);
            RECT bounds{};
            GetClientRect(w,&bounds);
            auto brush = CreateSolidBrush(dropColor);
            fill(dc,bounds,brush);
            DeleteObject(brush);
            EndPaint(w,&ps);
        } else paint(w);
        return 0;
    }
    if (m==WM_ERASEBKGND) return 1;
    if (m==WM_NCHITTEST) return HTTRANSPARENT;
    if (m==WM_DESTROY && strip==w) strip=nullptr;
    if (m==WM_DESTROY && dropPreview==w) dropPreview=nullptr;
    return DefWindowProcW(w,m,a,b);
}
LRESULT CALLBACK waveProcedure(HWND w, UINT m, WPARAM a, LPARAM b, UINT_PTR id, DWORD_PTR) {
    if (m==WM_NCDESTROY) {
        RemoveWindowSubclass(w,waveProcedure,id);
        wave=nullptr;
        picked=-1;
        hideDrop();
    } else if (m==WM_LBUTTONDOWN && reveal && altHeld() && !(a & (MK_SHIFT|MK_CONTROL)) &&
               short(HIWORD(b)) >= 2 && short(HIWORD(b)) < waveformBottom-2 &&
               hit(short(LOWORD(b))) >= 0) {
        dragStarted = false;
        handle(w,m,a,b);
        return 0;
    } else if (picked >= 0) {
        const POINT point{short(LOWORD(b)),short(HIWORD(b))};
        if (m==WM_MOUSEMOVE && dragDistance(point)) dragStarted = true;
        if (m==WM_LBUTTONUP && !dragStarted && !dragDistance(point)) {
            int index = picked;
            cancelDrag();
            selectPreviewRegion(index);
            repaint();
            return 0;
        } else if (m==WM_LBUTTONUP || m==WM_MOUSEMOVE || m==WM_CAPTURECHANGED || m==WM_CANCELMODE) {
            handle(w,m,a,b);
            return 0;
        }
    }
    return DefSubclassProc(w,m,a,b);
}
HWND makeStrip(HWND parent) {
    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        className,L"Audio region overlay",WS_CHILD,0,0,1,1,
        parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(viewId)),instance,nullptr);
    if (overlay && SetWindowSubclass(parent,waveProcedure,viewId,0)) return overlay;
    if (overlay) DestroyWindow(overlay);
    return nullptr;
}
#else
INT_PTR CALLBACK procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    if (m==WM_DESTROY && strip==w) { strip=nullptr; picked=-1; }
    return handle(w,m,a,b);
}
HWND makeStrip(HWND parent) {
    HWND w = SWELL_CreateDialog(nullptr,nullptr,parent,procedure,0);
    if (w) SetWindowLong(w,GWL_ID,viewId);
    return w;
}
#endif
void detachWave() {
    cancelDrag();
#ifdef _WIN32
    if (dropPreview) DestroyWindow(dropPreview);
    reveal = false;
    nativeViewportAddress = 0;
    if (wave && IsWindow(wave)) RemoveWindowSubclass(wave,waveProcedure,viewId);
#endif
    if (strip && IsWindow(strip)) DestroyWindow(strip);
    strip = wave = nullptr;
}
void detach() {
    detachWave();
    browser = nullptr;
    current = {};
    resetFile({});
}
void poll() {
    if (GetToggleCommandStateEx(0,50124) != 1) {
        if (browser) detach();
        return;
    }
    if (!browser || !IsWindow(browser)) {
        detach();
        browser = OpenMediaExplorer("",false);
    }
    if (!browser) return;
    HWND preview = GetDlgItem(browser,1046);
    if (preview != wave || !strip || !IsWindow(strip)) {
        detachWave();
        wave = preview;
        strip = wave ? makeStrip(wave) : nullptr;
#ifdef _WIN32
        if (strip) nativeViewportAddress = findNativeViewport();
#endif
        viewWidth = viewHeight = -1;
    }
    if (!strip) return;
#ifndef _WIN32
    RECT r{};
    GetClientRect(wave,&r);
    // Keep the upper preview available for REAPER's zoom, seek and time selection.
    int height = std::min(38, std::max(1, int(r.bottom)/3));
    if (viewWidth != r.right || viewHeight != height || previewHeight != r.bottom)
        SetWindowPos(strip,HWND_TOP,0,r.bottom-height,r.right,height,SWP_NOACTIVATE | SWP_SHOWWINDOW);
    viewWidth = r.right; viewHeight = height; previewHeight = r.bottom;
#endif
    auto s = selected(browser);
    if (!(s == current)) {
        auto file = selectedPath(browser,s);
        current = std::move(s);
        resetFile(std::move(file));
    } else if (!path.empty() && Clock::now() >= nextStat) {
        std::error_code ec;
        if (fs::last_write_time(fs::u8path(path),ec) != modified) resetFile(path);
        nextStat = Clock::now() + std::chrono::seconds(1);
    }
}
#ifdef _WIN32
void updateOverlay() {
    const bool show = altHeld() && IsWindowVisible(wave);
    if (show != reveal) {
        reveal = show;
        if (!show) ShowWindow(strip,SW_HIDE);
    }
    if (!strip || !wave || !IsWindow(strip) || !IsWindow(wave)) return;
    RECT r{};
    GetClientRect(wave,&r);
    // MEpeaks draws its ruler inside the client area. Reserve its bottom band.
    int bottom = std::max(0,int(r.bottom)-MulDiv(22,GetDpiForWindow(wave),96));
    if (viewWidth != r.right || viewHeight != r.bottom || waveformBottom != bottom) {
        viewWidth = r.right;
        viewHeight = r.bottom;
        waveformBottom = bottom;
        SetWindowPos(strip,HWND_TOP,0,0,viewWidth,waveformBottom,SWP_NOACTIVATE);
        repaint();
    }
    if (show && !IsWindowVisible(strip)) {
        ShowWindow(strip,SW_SHOWNOACTIVATE);
        repaint();
    }
    const auto view=waveView();
    if (!(view==lastView)) {
        lastView=view;
        repaint();
    }
}
#endif
void timer() {
    if (!enabled || busy) return;
    busy = true;
    try {
        if (picked >= 0 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
            cancelDrag();
            repaint();
        }
        if (Clock::now() >= nextPoll) {
            poll();
            nextPoll = Clock::now() + std::chrono::milliseconds(browser ? 120 : 400);
        }
        if (strip) {
#ifdef _WIN32
            updateOverlay();
            if (reveal) scanAudio();
#else
            scanAudio();
#endif
        }
    } catch (...) { openAfter = Clock::time_point::max(); fail("Could not read this selection"); }
    busy = false;
}
bool parseSettings(std::string text, Settings& s) {
    std::replace(text.begin(),text.end(),',',' ');
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    Settings t;
    if (!(in >> t.thresholdDb >> t.gapMs >> t.preMs >> t.postMs) ||
        !(in >> std::ws).eof() || t.thresholdDb < -120 || t.thresholdDb > 0 ||
        t.gapMs < 1 || t.gapMs > 60000 || t.preMs < 0 || t.preMs > 5000 ||
        t.postMs < 0 || t.postMs > 5000) return false;
    s = t; return true;
}
bool runAction(int action) {
    if (action == 0) {
        enabled = !enabled;
        SetExtState(section,"enabled",enabled ? "1" : "0",true);
        if (!enabled) detach();
        RefreshToolbar2(0, actions[0].accel.cmd);
        if (explorerCommands[0]) RefreshToolbar2(mediaExplorerSection, explorerCommands[0]);
        return true;
    }
    char buf[128];
    std::snprintf(buf,sizeof(buf),"%g,%g,%g,%g",settings.thresholdDb,settings.gapMs,settings.preMs,settings.postMs);
    if (GetUserInputs("Audio region detection",4,"Threshold (dBFS),Split after silence (ms),Pre-roll (ms),Tail padding (ms)",buf,sizeof(buf))) {
        if (parseSettings(buf,settings)) { SetExtState(section,"settings",buf,true); resetFile(path); }
        else ShowMessageBox("Use -120 to 0 dBFS, 1 to 60000 ms silence, and 0 to 5000 ms padding.","Audio regions",0);
    }
    return true;
}
bool command(int id, int) {
    for (int i = 0; i < 2; ++i)
        if (actions[i].accel.cmd && id == actions[i].accel.cmd) return runAction(i);
    return false;
}
bool explorerCommand(KbdSectionInfo* sectionInfo, int id, int, int, int, HWND) {
    if (!sectionInfo || sectionInfo->uniqueID != mediaExplorerSection) return false;
    for (int i = 0; i < 2; ++i)
        if (explorerCommands[i] && id == explorerCommands[i]) return runAction(i);
    return false;
}
int toggle(int id) {
    return id > 0 && (id == actions[0].accel.cmd || id == explorerCommands[0]) ? int(enabled) : -1;
}
}

namespace meplus::audio_regions {
namespace {
reaper_plugin_info_t* host = nullptr;
}

void unload() {
    if (!host) return;
    host->Register("-timer",(void*)timer);
    host->Register("-hookcommand",(void*)command);
    host->Register("-hookcommand2",(void*)explorerCommand);
    host->Register("-toggleaction",(void*)toggle);
    for (auto& action : actions) host->Register("-gaccel",&action);
    for (int i = 0; i < 2; ++i)
        if (explorerCommands[i]) host->Register("-custom_action",&explorerActions[i]);
    detach();
#ifdef _WIN32
    UnregisterClassW(className,instance);
#endif
    host = nullptr;
}

bool load(REAPER_PLUGIN_HINSTANCE inst, reaper_plugin_info_t& info) {
    auto* rec = &info;
    if (REAPERAPI_LoadAPI(rec->GetFunc)) return false;
    instance = inst;
    mainWindow = rec->hwnd_main;
#ifdef _WIN32
    getScrollInfo = reinterpret_cast<decltype(getScrollInfo)>(rec->GetFunc("CoolSB_GetScrollInfo"));
    if (!getScrollInfo) return false;
    WNDCLASSW klass{};
    klass.lpfnWndProc = procedure;
    klass.hInstance = inst;
    klass.hCursor = LoadCursor(nullptr,IDC_ARROW);
    klass.lpszClassName = className;
    if (!RegisterClassW(&klass)) return false;
#endif
    host = rec;
    enabled = std::strcmp(GetExtState(section,"enabled"),"0") != 0;
    parseSettings(GetExtState(section,"settings"),settings);
    const char* ids[] = {"ME_AUDIO_REGIONS_TOGGLE","ME_AUDIO_REGIONS_SETTINGS"};
    const char* explorerIds[] = {"ME_AUDIO_REGIONS_EXPLORER_TOGGLE","ME_AUDIO_REGIONS_EXPLORER_SETTINGS"};
    const char* labels[] = {"ME+: Show draggable audio regions","ME+: Configure audio region detection"};
    for (int i = 0; i < 2; ++i) {
        actions[i].accel.cmd = (unsigned short)rec->Register("command_id",(void*)ids[i]);
        actions[i].desc = labels[i];
        rec->Register("gaccel",&actions[i]);
        explorerActions[i] = {mediaExplorerSection,explorerIds[i],labels[i],nullptr};
        explorerCommands[i] = rec->Register("custom_action",&explorerActions[i]);
    }
    rec->Register("hookcommand",(void*)command);
    rec->Register("hookcommand2",(void*)explorerCommand);
    rec->Register("toggleaction",(void*)toggle);
    rec->Register("timer",(void*)timer);
    return true;
}
} // namespace meplus::audio_regions
