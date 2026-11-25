#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace media_cache {

struct FileStamp {
    DWORD volume, fileHigh, fileLow, sizeHigh, sizeLow;
    FILETIME writeTime;

    bool operator==(const FileStamp& other) const;
};

struct SortSettings {
    int column;
    bool reverse;
    unsigned listFlags, explorerFlags;

    bool operator==(const SortSettings& other) const;
};

// WDL buffer layout in the verified Media Explorer build.
struct Buffer {
    void** data;
    int capacityBytes;
    int usedBytes;
    int granularity;
};

constexpr std::size_t maxBufferRows = std::numeric_limits<int>::max() / sizeof(void*);

struct NativeApi {
    void* (*allocate)(std::size_t);
    void (*release)(void*);
    void (*releaseRecord)(void*);
    void (*setUserTag)(void*, const char*, const char*, char);
};

struct ReadMetrics {
    double readMs{}, decompressMs{}, verifyMs{};
};

bool readSnapshot(const std::filesystem::path&, const FileStamp&, const NativeApi&,
    Buffer&, Buffer&, bool (*sourceUnchanged)(const FileStamp&), ReadMetrics&);
bool writeSnapshot(const std::filesystem::path&, const FileStamp&, const Buffer&, const Buffer&);
bool readOrder(const std::filesystem::path&, const FileStamp&, const SortSettings&, std::size_t,
    std::vector<std::uint32_t>&);
bool writeOrder(const std::filesystem::path&, const FileStamp&, const SortSettings&,
    const std::vector<std::uint32_t>&);

} // namespace media_cache
