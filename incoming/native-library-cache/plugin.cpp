#include "reaper_plugin.h"
#include "disk_cache.h"

#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _M_X64
#error This prototype is specific to the Windows x64 Media Explorer.
#endif

namespace {

constexpr char section[] = "MEPlusNativeCachePrototype";
constexpr int explorerSection = 32063;
constexpr char actionLabel[] = "ME+: Toggle native library cache";
constexpr char mainActionName[] = "ME_NATIVE_LIBRARY_CACHE_TOGGLE";
constexpr char explorerActionName[] = "ME_NATIVE_LIBRARY_CACHE_EXPLORER_TOGGLE";
constexpr wchar_t moduleName[] = L"reaper_explorer.dll";
constexpr char originalDigest[] = "be5957206e14683ffe6557e8169d4dd92d04b5ec6eee8f758541948e8069f355";
constexpr std::size_t refreshRva = 0x509b0;
constexpr std::size_t parseRva = 0x0f9b4;
constexpr std::size_t sortRva = 0x14630;
constexpr std::size_t compareRva = 0x16620;
constexpr std::size_t modelRva = 0x10e0a0;
constexpr std::uint64_t minimumLibrarySize = 50ULL * 1024 * 1024;

using Address = std::uintptr_t;
using Refresh = void(*)(void*, unsigned, const void*, const void*);
using Parse = bool(*)(const char*, void*, void*);
using Compare = int(*)(const void*, const void*);
using Sort = void(*)(void*, std::size_t, std::size_t, Compare);
using SetState = void(*)(const char*, const char*, const char*, bool);
using RefreshToolbar = void(*)(int, int);

using media_cache::Buffer;
using media_cache::FileStamp;
using media_cache::SortSettings;
using Clock = std::chrono::steady_clock;

struct View {
    std::byte* state;

    const Buffer& visible() const { return *reinterpret_cast<const Buffer*>(state + 0x18); }
    const Buffer& records() const { return *reinterpret_cast<const Buffer*>(state + 0x30); }
    const Buffer& roots() const { return *reinterpret_cast<const Buffer*>(state + 0x48); }
};

struct OrderCache {
    std::filesystem::path directory;
    FileStamp stamp{};
    SortSettings sortSettings{};
    std::vector<std::uint32_t> ordinals;
};

struct Load {
    View view;
    std::filesystem::path source;
    std::filesystem::path directory;
    bool legacy = false;
    bool parsed = false;
    bool reentered = false;
    FileStamp stamp{};
};

struct Patch {
    std::size_t site, originalTarget;
    Address replacement;
    std::int32_t original = 0;
};

std::byte* explorer = nullptr;
DWORD uiThread = 0;
void* relayPage = nullptr;
std::size_t installed = 0;
reaper_plugin_info_t host{};
std::filesystem::path cacheDirectory, legacyDirectory, legacySource;
SetState setState = nullptr;
RefreshToolbar refreshToolbar = nullptr;
gaccel_register_t mainAction{};
custom_action_register_t explorerAction{explorerSection, explorerActionName, actionLabel, nullptr};
int explorerCommandId = 0;
bool mainActionRegistered = false;
bool enabled = true;
const char* installStatus = "waiting";
OrderCache cache;
Load* currentLoad = nullptr;
unsigned parses = 0, sortHits = 0, invalidations = 0;
unsigned refreshes = 0, parserCalls = 0;
unsigned snapshotHits = 0, snapshotWrites = 0, diskSortHits = 0;
unsigned smallLibraries = 0, migrations = 0;
double lastLoadMs = 0.0, lastWriteMs = 0.0;
media_cache::ReadMetrics lastRead;

template<class Fn>
Fn native(std::size_t offset)
{
    return reinterpret_cast<Fn>(explorer + offset);
}

std::wstring utf8ToWide(const char* text)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(std::size_t(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), length)) return {};
    result.pop_back();
    return result;
}

std::uint64_t fileSize(const FileStamp& stamp)
{
    return (std::uint64_t(stamp.sizeHigh) << 32) | stamp.sizeLow;
}

bool readStamp(const std::filesystem::path& path, FileStamp& stamp, std::wstring* canonical = nullptr)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    wchar_t resolved[32768];
    DWORD length = 0;
    if (ok) {
        stamp = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime};
        if (canonical && fileSize(stamp) >= minimumLibrarySize) {
            length = GetFinalPathNameByHandleW(file, resolved, DWORD(std::size(resolved)), FILE_NAME_NORMALIZED);
            if (!length) {
                length = GetFinalPathNameByHandleW(file, resolved, DWORD(std::size(resolved)), FILE_NAME_OPENED);
            }
        }
    }
    CloseHandle(file);
    if (canonical && length && length < std::size(resolved)) canonical->assign(resolved, length);
    return ok;
}

bool unchanged(const FileStamp& stamp)
{
    FileStamp now{};
    return currentLoad && readStamp(currentLoad->source, now) && now == stamp;
}

std::string sha256Hex(const void* bytes, ULONG size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(static_cast<const unsigned char*>(bytes)), size,
        digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result != 0) return {};
    char printable[65]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < digest.size(); ++index) {
        printable[index * 2] = hex[digest[index] >> 4];
        printable[index * 2 + 1] = hex[digest[index] & 15];
    }
    return printable;
}

bool identifyLibrary(const char* path, Load& load)
{
    if (!path) return false;
    const std::filesystem::path source(utf8ToWide(path));
    if (_wcsicmp(source.extension().c_str(), L".ReaperFileList") != 0) return false;
    std::wstring canonical;
    if (!readStamp(source, load.stamp, &canonical)) return false;
    if (fileSize(load.stamp) < minimumLibrarySize) {
        ++smallLibraries;
        return false;
    }
    // The opened file's canonical path distinguishes same-named libraries.
    if (canonical.empty()) return false;
    const auto key = sha256Hex(canonical.data(), ULONG(canonical.size() * sizeof(wchar_t)));
    if (key.empty()) return false;
    load.source = source;
    load.directory = cacheDirectory / key;
    load.legacy = _wcsicmp(source.lexically_normal().c_str(), legacySource.c_str()) == 0;
    return true;
}

void moveLegacy(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code error;
    std::filesystem::create_directories(to.parent_path(), error);
    if (!error && MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ++migrations;
    }
}

std::filesystem::path getOrderPath(const Load& load, const SortSettings& settings)
{
    const auto name = L"order-" + std::to_wstring(settings.column)
        + L"-" + std::to_wstring(settings.reverse)
        + L"-" + std::to_wstring(settings.listFlags)
        + L"-" + std::to_wstring(settings.explorerFlags) + L".order";
    return load.directory / name;
}

double durationMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

SortSettings readSortSettings()
{
    return {*native<const int*>(0x10f8a4), *native<const char*>(0x10f8a8) != 0,
        *native<const unsigned*>(0x10f8d4), *native<const unsigned*>(0x10b124)};
}

void clearOrder()
{
    if (!cache.ordinals.empty()) ++invalidations;
    cache = {};
}

void publishState()
{
    if (!setState) return;
    char message[512];
    std::snprintf(message, sizeof(message),
        "{\"status\":\"%s\",\"enabled\":%s,\"parses\":%u,\"sort_hits\":%u,"
        "\"order_bytes\":%zu,\"invalidations\":%u,\"refreshes\":%u,\"parser_calls\":%u,"
        "\"snapshot_hits\":%u,\"snapshot_writes\":%u,\"disk_sort_hits\":%u,"
        "\"small_libraries\":%u,\"migrations\":%u,"
        "\"load_ms\":%.3f,\"write_ms\":%.3f,\"read_ms\":%.3f,\"decompress_ms\":%.3f,\"verify_ms\":%.3f}",
        enabled ? installStatus : "disabled", enabled ? "true" : "false",
        parses, sortHits, cache.ordinals.capacity() * sizeof(std::uint32_t),
        invalidations, refreshes, parserCalls,
        snapshotHits, snapshotWrites, diskSortHits, smallLibraries, migrations, lastLoadMs, lastWriteMs,
        lastRead.readMs, lastRead.decompressMs, lastRead.verifyMs);
    setState(section, "status", message, false);
}

void refreshHook(void* model, unsigned flags, const void* selection, const void* scroll)
{
    const auto refresh = native<Refresh>(refreshRva);
    if (GetCurrentThreadId() != uiThread || !enabled || model != explorer + modelRva) {
        refresh(model, flags, selection, scroll);
        return;
    }
    if (currentLoad) {
        currentLoad->reentered = true;
        refresh(model, flags, selection, scroll);
        return;
    }

    ++refreshes;
    Load load{};
    load.view.state = static_cast<std::byte*>(model);
    currentLoad = &load;
    refresh(model, flags, selection, scroll);
    currentLoad = nullptr;

    publishState();
}

bool parseHook(const char* path, void* records, void* roots)
{
    const auto parse = native<Parse>(parseRva);
    if (GetCurrentThreadId() != uiThread || !enabled || !currentLoad || currentLoad->reentered
        || records != &currentLoad->view.records() || roots != &currentLoad->view.roots()) {
        return parse(path, records, roots);
    }

    ++parserCalls;
    auto& load = *currentLoad;
    std::filesystem::path snapshotPath;
    try {
        if (identifyLibrary(path, load)) snapshotPath = load.directory / L"records.snapshot";
    } catch (const std::bad_alloc&) {
        // Caching is optional when a path or key cannot be allocated.
    }
    if (snapshotPath.empty()) return parse(path, records, roots);
    const auto& stamp = load.stamp;
    auto& nativeRecords = *static_cast<Buffer*>(records);
    auto& nativeRoots = *static_cast<Buffer*>(roots);
    if (!cache.ordinals.empty() && cache.directory == load.directory && !(cache.stamp == stamp)) clearOrder();
    const auto start = Clock::now();
    lastWriteMs = 0.0;
    lastRead = {};
    const media_cache::NativeApi api{
        native<void*(*)(std::size_t)>(0xb8de0),
        native<void(*)(void*)>(0xb8280),
        native<void(*)(void*)>(0x17590),
        native<void(*)(void*, const char*, const char*, char)>(0x10fb8)
    };
    try {
        auto readSnapshot = [&](const std::filesystem::path& file) {
            return media_cache::readSnapshot(file, stamp, api, nativeRecords, nativeRoots, unchanged, lastRead);
        };
        load.parsed = readSnapshot(snapshotPath);
        if (!load.parsed && load.legacy) {
            const auto old = legacyDirectory / L"06.snapshot";
            load.parsed = readSnapshot(old);
            if (load.parsed) {
                moveLegacy(old, snapshotPath);
            }
        }
    } catch (const std::bad_alloc&) {
        // A failed restore discards its temporary native records.
    }
    if (load.parsed) {
        ++snapshotHits;
        lastLoadMs = durationMs(start);
        return true;
    }

    ++parses;
    const bool result = parse(path, records, roots);
    lastLoadMs = durationMs(start);
    load.parsed = result && unchanged(stamp);
    if (load.parsed && enabled && !load.reentered) {
        const auto writing = Clock::now();
        try {
            if (media_cache::writeSnapshot(snapshotPath, stamp, nativeRecords, nativeRoots)) ++snapshotWrites;
        } catch (const std::bad_alloc&) {
            // The native result already belongs to REAPER.
        }
        lastWriteMs = durationMs(writing);
    }
    return result;
}

bool fullFileOrder(const View& view, const void* data, std::size_t bytes)
{
    const auto& visible = view.visible();
    const auto& records = view.records();
    return data == visible.data && records.data && visible.data != records.data
        && visible.usedBytes == int(bytes) && records.usedBytes == int(bytes)
        && std::memcmp(visible.data, records.data, bytes) == 0;
}

void rememberOrder(const Load& load, std::size_t count, const SortSettings& settings)
{
    try {
        const auto& view = load.view;
        // Convert native pointers to file ordinals while both buffers are
        // live. Only the integer permutation survives the refresh.
        std::unordered_map<void*, std::uint32_t> positions;
        positions.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!positions.emplace(view.records().data[i], i).second) return;
        }
        OrderCache next{load.directory, load.stamp, settings, {}};
        next.ordinals.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto source = positions.find(view.visible().data[i]);
            if (source == positions.end()) return;
            next.ordinals.push_back(source->second);
        }
        cache = std::move(next);
        media_cache::writeOrder(getOrderPath(load, settings), load.stamp, settings, cache.ordinals);
    } catch (const std::bad_alloc&) {
        // Native sorting is already complete.
    }
}

void sortHook(void* data, std::size_t count, std::size_t stride, Compare compare)
{
    const auto sort = native<Sort>(sortRva);
    if (GetCurrentThreadId() != uiThread || !enabled || !currentLoad || !currentLoad->parsed
        || currentLoad->reentered || !data
        || stride != sizeof(void*) || count == 0 || count > media_cache::maxBufferRows
        || compare != native<Compare>(compareRva)) {
        sort(data, count, stride, compare);
        return;
    }

    const auto settings = readSortSettings();
    // The file name and the user's current metadata column have been checked.
    // Rating/favourite priority, random sorts and custom columns use REAPER.
    const auto view = currentLoad->view;
    if ((settings.column != 0 && settings.column != 13) || (settings.listFlags & 0x20)
        || !fullFileOrder(view, data, count * stride)) {
        sort(data, count, stride, compare);
        return;
    }

    const auto& load = *currentLoad;
    const auto& stamp = load.stamp;
    auto matches = [&] {
        return cache.directory == load.directory && cache.stamp == stamp
            && cache.sortSettings == settings && cache.ordinals.size() == count;
    };
    if (!matches()) {
        try {
            const auto orderPath = getOrderPath(load, settings);
            OrderCache saved{load.directory, stamp, settings, {}};
            bool found = media_cache::readOrder(orderPath, stamp, settings, count, saved.ordinals);
            if (!found && load.legacy) {
                const auto old = legacyDirectory / L"06.order";
                found = media_cache::readOrder(old, stamp, settings, count, saved.ordinals);
                if (found) moveLegacy(old, orderPath);
            }
            if (found) {
                cache = std::move(saved);
                ++diskSortHits;
            }
        } catch (const std::bad_alloc&) {
            // Native sorting remains available.
        }
    }
    if (matches()) {
        auto** rows = static_cast<void**>(data);
        auto* const* records = view.records().data;
        for (std::size_t i = 0; i < count; ++i) rows[i] = records[cache.ordinals[i]];
        ++sortHits;
        return;
    }
    sort(data, count, stride, compare);
    if (enabled && !load.reentered) rememberOrder(load, count, settings);
}

std::array<Patch, 9> patches{{
    {0x150c3, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x3e778, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x4876f, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x4d8b0, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x4e26b, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x61281, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0xaa04f, refreshRva, reinterpret_cast<Address>(refreshHook)},
    {0x51087, parseRva, reinterpret_cast<Address>(parseHook)},
    {0x51cce, sortRva, reinterpret_cast<Address>(sortHook)},
}};

bool expectedImage(HMODULE module)
{
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != 1188352) return false;
    std::vector<unsigned char> bytes(1188352);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;

    return sha256Hex(bytes.data(), ULONG(bytes.size())) == originalDigest;
}

void* allocateNearby()
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const Address step = system.dwAllocationGranularity;
    const Address start = reinterpret_cast<Address>(explorer) & ~(step - 1);
    const Address minimum = reinterpret_cast<Address>(system.lpMinimumApplicationAddress);
    for (Address distance = step; distance < 0x70000000; distance += step) {
        if (void* page = VirtualAlloc(reinterpret_cast<void*>(start + distance),
                system.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return page;
        if (start >= minimum + distance) {
            if (void* page = VirtualAlloc(reinterpret_cast<void*>(start - distance),
                    system.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return page;
        }
    }
    return nullptr;
}

bool writeRelative(const Patch& patch, std::int32_t value)
{
    auto* site = explorer + patch.site;
    DWORD protection{};
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(site + 1, &value, sizeof(value));
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    DWORD ignored{};
    VirtualProtect(site, 5, protection, &ignored);
    return true;
}

void removePatches()
{
    while (installed > 0) {
        const auto& patch = patches[installed - 1];
        if (!writeRelative(patch, patch.original)) return;
        --installed;
    }
    if (relayPage) VirtualFree(relayPage, 0, MEM_RELEASE);
    relayPage = nullptr;
}

bool installPatches()
{
    for (auto& patch : patches) {
        const auto* site = explorer + patch.site;
        std::memcpy(&patch.original, site + 1, sizeof(patch.original));
        if (*site != std::byte{0xe8}
            || std::int64_t(patch.site + 5) + patch.original != std::int64_t(patch.originalTarget)) {
            return false;
        }
    }

    relayPage = allocateNearby();
    if (!relayPage) return false;
    auto* relay = static_cast<std::byte*>(relayPage);
    constexpr std::size_t stride = 16;
    for (const auto& patch : patches) {
        // mov rax, imm64; jmp rax. The original CALL supplies the return address.
        const unsigned char stub[12]{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
        std::memcpy(relay, stub, sizeof(stub));
        std::memcpy(relay + 2, &patch.replacement, sizeof(patch.replacement));
        relay += stride;
    }
    DWORD protection{};
    if (!VirtualProtect(relayPage, patches.size() * stride, PAGE_EXECUTE_READ, &protection)) {
        removePatches();
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), relayPage, patches.size() * stride);
    relay = static_cast<std::byte*>(relayPage);
    for (const auto& patch : patches) {
        const auto difference = std::int64_t(reinterpret_cast<Address>(relay))
            - std::int64_t(reinterpret_cast<Address>(explorer + patch.site + 5));
        if (difference < std::numeric_limits<std::int32_t>::min()
            || difference > std::numeric_limits<std::int32_t>::max()
            || !writeRelative(patch, static_cast<std::int32_t>(difference))) {
            removePatches();
            return false;
        }
        ++installed;
        relay += stride;
    }
    return true;
}

void timer()
{
    HMODULE module = GetModuleHandleW(moduleName);
    if (!module) return;
    host.Register("-timer", reinterpret_cast<void*>(timer));
    try {
        if (!expectedImage(module)) {
            installStatus = "unsupported-image";
            publishState();
            return;
        }
        explorer = reinterpret_cast<std::byte*>(module);
        uiThread = GetCurrentThreadId();
        // Keep the hook code resident even if the host unloads extensions
        // before the original module during shutdown.
        HMODULE pinned{};
        const bool resident = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(timer), &pinned);
        installStatus = resident && installPatches() ? "installed" : "install-failed";
    } catch (const std::bad_alloc&) {
        installStatus = "install-failed";
    }
    publishState();
}

void toggleCache()
{
    enabled = !enabled;
    setState(section, "enabled", enabled ? "1" : "0", true);
    if (!enabled) {
        if (currentLoad) currentLoad->reentered = true;
        clearOrder();
    }
    publishState();
    if (refreshToolbar) {
        refreshToolbar(0, mainAction.accel.cmd);
        refreshToolbar(explorerSection, explorerCommandId);
    }
}

bool mainCommand(int command, int)
{
    if (!mainActionRegistered || command != mainAction.accel.cmd) return false;
    toggleCache();
    return true;
}

bool explorerCommand(KbdSectionInfo* context, int command, int, int, int, HWND)
{
    if (!context || context->uniqueID != explorerSection || !explorerCommandId
        || command != explorerCommandId) return false;
    toggleCache();
    return true;
}

int toggleState(int command)
{
    if (command > 0 && (command == explorerCommandId || command == mainAction.accel.cmd)) {
        return int(enabled);
    }
    return -1;
}

void unregisterActions()
{
    host.Register("-toggleaction", reinterpret_cast<void*>(toggleState));
    host.Register("-hookcommand2", reinterpret_cast<void*>(explorerCommand));
    host.Register("-hookcommand", reinterpret_cast<void*>(mainCommand));
    if (explorerCommandId) host.Register("-custom_action", &explorerAction);
    if (mainActionRegistered) host.Register("-gaccel", &mainAction);
    explorerCommandId = 0;
    mainActionRegistered = false;
}

bool registerActions()
{
    const int commandId = host.Register("command_id", const_cast<char*>(mainActionName));
    if (commandId <= 0 || commandId > std::numeric_limits<WORD>::max()) return false;
    mainAction.accel.cmd = WORD(commandId);
    mainAction.desc = actionLabel;
    mainActionRegistered = host.Register("gaccel", &mainAction) != 0;
    if (!mainActionRegistered) return false;
    explorerCommandId = host.Register("custom_action", &explorerAction);
    if (explorerCommandId <= 0) {
        explorerCommandId = 0;
        return false;
    }
    return host.Register("hookcommand", reinterpret_cast<void*>(mainCommand))
        && host.Register("hookcommand2", reinterpret_cast<void*>(explorerCommand))
        && host.Register("toggleaction", reinterpret_cast<void*>(toggleState));
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* info)
{
    if (!info) {
        if (host.Register) {
            host.Register("-timer", reinterpret_cast<void*>(timer));
            unregisterActions();
        }
        removePatches();
        clearOrder();
        return 0;
    }
    if (info->caller_version != REAPER_PLUGIN_VERSION || !info->Register || !info->GetFunc) return 0;
    try {
        const auto getResource = reinterpret_cast<const char*(*)()>(info->GetFunc("GetResourcePath"));
        const auto getVersion = reinterpret_cast<const char*(*)()>(info->GetFunc("GetAppVersion"));
        const auto getState = reinterpret_cast<const char*(*)(const char*, const char*)>(info->GetFunc("GetExtState"));
        setState = reinterpret_cast<SetState>(info->GetFunc("SetExtState"));
        refreshToolbar = reinterpret_cast<RefreshToolbar>(info->GetFunc("RefreshToolbar2"));
        if (!getResource || !getVersion || !getState || !setState
            || std::strcmp(getVersion(), "7.82/x64") != 0) return 0;
        const auto resource = utf8ToWide(getResource());
        if (resource.empty()) return 0;
        const std::filesystem::path root(resource);
        cacheDirectory = root / L"UserPlugins" / L"MEPlusNativeCache";
        legacyDirectory = root / L"MEPlusNativeCache";
        legacySource = (root / L"MediaDB" / L"06.ReaperFileList").lexically_normal();
        host = *info;
        enabled = std::strcmp(getState(section, "enabled"), "0") != 0;
        if (!registerActions() || !host.Register("timer", reinterpret_cast<void*>(timer))) {
            unregisterActions();
            host = {};
            return 0;
        }
        publishState();
        return 1;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}
