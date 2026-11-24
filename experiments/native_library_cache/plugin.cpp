#include "reaper_plugin.h"

#include <bcrypt.h>

#include <array>
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
#include <string_view>
#include <vector>

#ifndef _M_X64
#error This prototype is specific to the Windows x64 Media Explorer.
#endif

namespace {

constexpr char section[] = "MEPlusNativeCachePrototype";
constexpr wchar_t moduleName[] = L"reaper_explorer.dll";
constexpr char originalDigest[] = "be5957206e14683ffe6557e8169d4dd92d04b5ec6eee8f758541948e8069f355";
constexpr std::size_t refreshRva = 0x509b0;
constexpr std::size_t parseRva = 0x0f9b4;
constexpr std::size_t sortRva = 0x14630;
constexpr std::size_t compareRva = 0x16620;
constexpr std::size_t modelRva = 0x10e0a0;

using Address = std::uintptr_t;
using NativeDeleter = void(*)(void*);
using Refresh = void(*)(void*, unsigned, const void*, const void*);
using Parse = bool(*)(const char*, void*, void*);
using Compare = int(*)(const void*, const void*);
using Sort = void(*)(void*, std::size_t, std::size_t, Compare);
using SetState = void(*)(const char*, const char*, const char*, bool);

// In this build these are WDL buffers. The visible rows borrow the records
// owned by the first buffer; the second owns the MediaDB scan root strings.
struct Buffer {
    void** data;
    int capacityBytes;
    int usedBytes;
    int granularity;
};

static_assert(sizeof(Buffer) == 24 && offsetof(Buffer, usedBytes) == 12);

struct View {
    std::byte* state;

    Buffer& visible() const { return *reinterpret_cast<Buffer*>(state + 0x18); }
    Buffer& records() const { return *reinterpret_cast<Buffer*>(state + 0x30); }
    Buffer& roots() const { return *reinterpret_cast<Buffer*>(state + 0x48); }

    std::string_view pathAt(std::size_t offset) const
    {
        const auto& string = *reinterpret_cast<const Buffer*>(state + offset);
        if (!string.data || string.usedBytes <= 0 || string.usedBytes > 32768) return {};
        return {reinterpret_cast<const char*>(string.data), std::size_t(string.usedBytes - 1)};
    }
};

struct FileStamp {
    DWORD volume, fileHigh, fileLow, sizeHigh, sizeLow;
    FILETIME writeTime;

    bool operator==(const FileStamp& other) const
    {
        return volume == other.volume && fileHigh == other.fileHigh && fileLow == other.fileLow
            && sizeHigh == other.sizeHigh && sizeLow == other.sizeLow
            && CompareFileTime(&writeTime, &other.writeTime) == 0;
    }
};

struct SortSettings {
    int column;
    bool reverse;
    unsigned listFlags, explorerFlags;

    bool operator==(const SortSettings& other) const
    {
        return column == other.column && reverse == other.reverse
            && listFlags == other.listFlags && explorerFlags == other.explorerFlags;
    }
};

struct Cache {
    Buffer records{}, roots{};
    FileStamp stamp{};
    std::vector<void*> originalOrder, sortedOrder;
    SortSettings sortSettings{};
    bool parked = false;
    bool active = false;
    Buffer liveRecords{}, liveRoots{};
};

struct Load {
    View view;
    bool parsed = false;
    bool hit = false;
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
std::wstring databasePath;
SetState setState = nullptr;
Cache cache;
Load* currentLoad = nullptr;
unsigned parses = 0, hits = 0, sortHits = 0, parks = 0, invalidations = 0;
unsigned refreshes = 0, parserCalls = 0;

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

bool readStamp(FileStamp& stamp)
{
    const HANDLE file = CreateFileW(databasePath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(file);
    if (ok) stamp = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
        info.nFileSizeHigh, info.nFileSizeLow, info.ftLastWriteTime};
    return ok;
}

bool sameAllocation(const Buffer& left, const Buffer& right)
{
    return left.data == right.data && left.usedBytes == right.usedBytes;
}

void freeNativeBuffer(Buffer& buffer)
{
    if (buffer.data) native<NativeDeleter>(0xb8280)(buffer.data);
    const auto granularity = buffer.granularity;
    buffer = {};
    buffer.granularity = granularity;
}

void clearSort()
{
    cache.originalOrder.clear();
    cache.sortedOrder.clear();
}

void discardParked()
{
    if (!cache.parked) return;
    using ClearRoots = void(*)(void*, char, NativeDeleter);
    using ClearRecords = void(*)(void*, NativeDeleter);
    native<ClearRoots>(0x69774)(&cache.roots, 1, native<NativeDeleter>(0xb8280));
    native<ClearRecords>(0x115e4)(&cache.records, native<NativeDeleter>(0x17590));
    freeNativeBuffer(cache.roots);
    freeNativeBuffer(cache.records);
    cache.parked = false;
    clearSort();
    ++invalidations;
}

void moveBuffer(Buffer& destination, Buffer& source)
{
    destination = source;
    const int granularity = source.granularity;
    source = {};
    source.granularity = granularity;
}

bool validBuffer(const Buffer& buffer)
{
    return buffer.capacityBytes >= buffer.usedBytes && buffer.usedBytes >= 0
        && buffer.usedBytes % sizeof(void*) == 0 && (buffer.data || buffer.usedBytes == 0);
}

SortSettings readSortSettings()
{
    return {*native<const int*>(0x10f8a4), *native<const char*>(0x10f8a8) != 0,
        *native<const unsigned*>(0x10f8d4), *native<const unsigned*>(0x10b124)};
}

void publishState(const char* status)
{
    if (!setState) return;
    char message[320];
    std::snprintf(message, sizeof(message),
        "{\"status\":\"%s\",\"parses\":%u,\"hits\":%u,\"sort_hits\":%u,\"parks\":%u,"
        "\"invalidations\":%u,\"parked\":%s,\"refreshes\":%u,\"parser_calls\":%u}",
        status, parses, hits, sortHits, parks, invalidations, cache.parked ? "true" : "false",
        refreshes, parserCalls);
    setState(section, "status", message, false);
}

void park(View view)
{
    FileStamp stamp{};
    if (cache.parked || !readStamp(stamp) || !(cache.stamp == stamp)
        || !sameAllocation(view.records(), cache.liveRecords)
        || !sameAllocation(view.roots(), cache.liveRoots)
        || !validBuffer(view.records()) || !validBuffer(view.roots())
        || view.records().usedBytes == 0) {
        clearSort();
        return;
    }
    if (!cache.sortedOrder.empty()
        && (std::size_t(view.visible().usedBytes) != cache.sortedOrder.size() * sizeof(void*)
            || !view.visible().data
            || std::memcmp(view.visible().data, cache.sortedOrder.data(), view.visible().usedBytes) != 0)) {
        clearSort();
    }
    // The caller clears its borrowed visible pointers before rebuilding.
    moveBuffer(cache.records, view.records());
    moveBuffer(cache.roots, view.roots());
    cache.parked = true;
    ++parks;
}

void refreshHook(void* model, unsigned flags, const void* selection, const void* scroll)
{
    const auto refresh = native<Refresh>(refreshRva);
    if (GetCurrentThreadId() != uiThread || model != explorer + modelRva) {
        refresh(model, flags, selection, scroll);
        return;
    }
    if (currentLoad) {
        currentLoad->reentered = true;
        refresh(model, flags, selection, scroll);
        return;
    }

    const View view{static_cast<std::byte*>(model)};
    ++refreshes;
    const bool changedPath = view.pathAt(0x68) != view.pathAt(0x80);
    const bool previouslyTarget = cache.active;
    const bool leaving = previouslyTarget && changedPath;
    // Navigation requests a full refresh. A partial refresh is left alone.
    if (leaving && (flags & 0xff) == 1) park(view);
    cache.active = false;
    Load load{view};
    currentLoad = &load;
    refresh(model, flags, selection, scroll);
    currentLoad = nullptr;

    if (load.parsed && !load.reentered && validBuffer(view.records()) && validBuffer(view.roots())) {
        cache.stamp = load.stamp;
        cache.liveRecords = view.records();
        cache.liveRoots = view.roots();
        cache.active = true;
    } else if (previouslyTarget && !leaving) {
        // Filtering uses the raw records already installed in the view.
        // Keep those records eligible, but let the next refresh sort them.
        cache.active = (flags & 0xff) == 0 && !load.reentered
            && sameAllocation(view.records(), cache.liveRecords)
            && sameAllocation(view.roots(), cache.liveRoots);
        discardParked();
        clearSort();
    }
    publishState("installed");
}

bool parseHook(const char* path, void* records, void* roots)
{
    const auto parse = native<Parse>(parseRva);
    if (GetCurrentThreadId() != uiThread || !currentLoad
        || records != &currentLoad->view.records() || roots != &currentLoad->view.roots()) {
        return parse(path, records, roots);
    }

    ++parserCalls;
    wchar_t parsedPath[32768];
    if (!path || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
            parsedPath, int(std::size(parsedPath)))
        || _wcsicmp(parsedPath, databasePath.c_str()) != 0) {
        return parse(path, records, roots);
    }
    auto& load = *currentLoad;
    FileStamp stamp{};
    const bool haveStamp = readStamp(stamp);
    if (cache.parked && haveStamp && cache.stamp == stamp
        && load.view.records().usedBytes == 0 && load.view.roots().usedBytes == 0
        && validBuffer(load.view.records()) && validBuffer(load.view.roots())) {
        freeNativeBuffer(load.view.records());
        freeNativeBuffer(load.view.roots());
        moveBuffer(load.view.records(), cache.records);
        moveBuffer(load.view.roots(), cache.roots);
        cache.parked = false;
        load.hit = load.parsed = true;
        load.stamp = stamp;
        ++hits;
        return true;
    }

    discardParked();
    clearSort();
    ++parses;
    const bool result = parse(path, records, roots);
    FileStamp after{};
    load.parsed = result && haveStamp && readStamp(after) && after == stamp;
    load.stamp = stamp;
    return result;
}

void sortHook(void* data, std::size_t count, std::size_t stride, Compare compare)
{
    const auto sort = native<Sort>(sortRva);
    if (GetCurrentThreadId() != uiThread || !currentLoad || !currentLoad->parsed || !data
        || stride != sizeof(void*) || count == 0 || count > 2000000
        || compare != native<Compare>(compareRva)) {
        sort(data, count, stride, compare);
        return;
    }

    const auto settings = readSortSettings();
    // The name comparator has a small, verified set of inputs. Use REAPER's
    // comparator for rating/favourite sorts, random sorts and custom columns.
    if (settings.column != 0 || (settings.listFlags & 0x20)) {
        clearSort();
        sort(data, count, stride, compare);
        return;
    }

    auto** rows = static_cast<void**>(data);
    if (currentLoad->hit && cache.sortSettings == settings
        && cache.originalOrder.size() == count && cache.sortedOrder.size() == count
        && std::memcmp(rows, cache.originalOrder.data(), count * stride) == 0) {
        std::memcpy(rows, cache.sortedOrder.data(), count * stride);
        ++sortHits;
        return;
    }
    try {
        cache.originalOrder.assign(rows, rows + count);
    } catch (const std::bad_alloc&) {
        clearSort();
    }
    sort(data, count, stride, compare);
    if (cache.originalOrder.size() != count) return;
    try {
        cache.sortedOrder.assign(rows, rows + count);
        cache.sortSettings = settings;
    } catch (const std::bad_alloc&) {
        clearSort();
    }
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

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, bytes.data(), ULONG(bytes.size()),
        digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result != 0) return false;
    char printable[65]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < digest.size(); ++index) {
        printable[index * 2] = hex[digest[index] >> 4];
        printable[index * 2 + 1] = hex[digest[index] & 15];
    }
    return std::strcmp(printable, originalDigest) == 0;
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
            publishState("unsupported-image");
            return;
        }
        explorer = reinterpret_cast<std::byte*>(module);
        uiThread = GetCurrentThreadId();
        // Keep the hook code resident even if the host unloads extensions
        // before the original module during shutdown.
        HMODULE pinned{};
        const bool resident = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(timer), &pinned);
        publishState(resident && installPatches() ? "installed" : "install-failed");
    } catch (const std::bad_alloc&) {
        publishState("install-failed");
    }
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t* info)
{
    if (!info) {
        if (host.Register) host.Register("-timer", reinterpret_cast<void*>(timer));
        removePatches();
        if (installed == 0 && cache.parked && GetModuleHandleW(moduleName) == reinterpret_cast<HMODULE>(explorer)) {
            discardParked();
        }
        clearSort();
        return 0;
    }
    if (info->caller_version != REAPER_PLUGIN_VERSION || !info->Register || !info->GetFunc) return 0;
    try {
        const auto getResource = reinterpret_cast<const char*(*)()>(info->GetFunc("GetResourcePath"));
        const auto getVersion = reinterpret_cast<const char*(*)()>(info->GetFunc("GetAppVersion"));
        setState = reinterpret_cast<SetState>(info->GetFunc("SetExtState"));
        if (!getResource || !getVersion || std::strcmp(getVersion(), "7.82/x64") != 0) return 0;
        const auto resource = utf8ToWide(getResource());
        if (resource.empty()) return 0;
        if (GetFileAttributesW((resource + L"\\native_cache_prototype.enable").c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
        databasePath = resource + L"\\MediaDB\\06.ReaperFileList";
        host = *info;
        publishState("waiting");
        return host.Register("timer", reinterpret_cast<void*>(timer)) ? 1 : 0;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}
