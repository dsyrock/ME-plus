#pragma once
#include <array>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <commctrl.h>
#endif

namespace fs = std::filesystem;
inline std::string controlText(HWND window, int row = -1, int column = 0) {
#ifdef _WIN32
    std::array<wchar_t, 32768> buf{};
    if (row < 0) GetWindowTextW(window, buf.data(), int(buf.size()));
    else {
        LVITEMW cell{};
        cell.iSubItem = column;
        cell.pszText = buf.data();
        cell.cchTextMax = int(buf.size());
        SendMessageW(window, LVM_GETITEMTEXTW, row, reinterpret_cast<LPARAM>(&cell));
    }
    return fs::path(buf.data()).u8string();
#else
    std::array<char, 32768> buf{};
    if (row < 0) GetWindowText(window, buf.data(), int(buf.size()));
    else ListView_GetItemText(window, row, column, buf.data(), int(buf.size()));
    return buf.data();
#endif
}
struct Selection {
    int row = -1;
    std::string directory, name, ext;
    bool operator==(const Selection& b) const {
        return row == b.row && directory == b.directory && name == b.name && ext == b.ext;
    }
};
inline Selection selected(HWND browser) {
    HWND list = GetDlgItem(browser, 1001);
#ifdef __APPLE__
    int row = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    if (row >= 0 && !(ListView_GetItemState(list,row,LVIS_SELECTED) & LVIS_SELECTED)) row = -1;
#else
    int row = ListView_GetNextItem(list, -1, LVNI_SELECTED | LVNI_FOCUSED);
#endif
    if (row < 0) row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) return {};
    return {row, controlText(GetDlgItem(browser, 1002)), controlText(list, row), controlText(list, row, 3)};
}
inline std::string resolve(const Selection& s) {
    if (s.row < 0 || s.name.empty()) return {};
    const auto directory = fs::u8path(s.directory);
    auto candidate = fs::u8path(s.name);
    if (candidate.is_relative()) {
        if (!directory.is_absolute()) return {};
        candidate = directory / candidate;
    }
    auto media = [](const fs::path& p) {
        auto ext = p.extension().u8string();
        std::error_code ec;
        return ext.size() > 1 && IsMediaExtension(ext.c_str()+1, false) && fs::is_regular_file(p, ec);
    };
    if (media(candidate)) return candidate.lexically_normal().u8string();
    if (s.ext.empty() || s.ext.find_first_of("/\\:*?<>|") != std::string::npos) return {};
    candidate += fs::u8path((s.ext.front() == '.' ? "" : ".") + s.ext);
    return media(candidate) ? candidate.lexically_normal().u8string() : std::string{};
}
inline std::string selectedPath(HWND browser, const Selection& s) {
    auto path = resolve(s);
    if (!path.empty() || s.row < 0 || s.name == ".." || s.ext.empty()) return path;
    auto extension = s.ext.substr(s.ext.front() == '.' ? 1 : 0);
    if (!IsMediaExtension(extension.c_str(), false) || GetToggleCommandStateEx(32063, 42026) != 0) return {};
    // REAPER databases can display aliases. Read the same selected row in
    // full-path mode, then restore both mutually exclusive display preferences.
    struct Restore {
        HWND hwnd;
        bool leading;
        ~Restore() {
            if (GetToggleCommandStateEx(32063, 42026) == 1) SendMessage(hwnd, WM_COMMAND, 42026, 0);
            if (leading && GetToggleCommandStateEx(32063, 42134) != 1) SendMessage(hwnd, WM_COMMAND, 42134, 0);
        }
    } restore{browser, GetToggleCommandStateEx(32063, 42134) == 1};
    SendMessage(browser, WM_COMMAND, 42026, 0);
    return resolve(selected(browser));
}
