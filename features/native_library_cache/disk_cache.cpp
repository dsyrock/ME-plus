#include "disk_cache.h"

#include <bcrypt.h>
#include <compressapi.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <string>

namespace media_cache {
namespace {

constexpr std::uint64_t snapshotMagic = 0x315453444350454d;
constexpr std::uint64_t orderMagic = 0x3152444f4350454d;
constexpr std::size_t chunkSize = 1024 * 1024;
constexpr std::size_t maxChunkSize = 8 * chunkSize;
constexpr std::size_t maxTextSize = 8192;
constexpr std::size_t metadataSlots = 19;
constexpr std::uint32_t hasMetadata = 1U << 31;
constexpr std::uint32_t metadataBits = (1U << metadataSlots) - 1U;
using Digest = std::array<unsigned char, 32>;
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct NativeTagValue { char* value; char userEdited; };
struct NativeTag { const char* key; NativeTagValue* value; };

struct NativeMetadata {
    char* values[metadataSlots];
    std::uint32_t references, rating, flags, reserved;
    const Buffer* tags;
};

struct NativeRecord {
    char* path;
    int pathCapacity, pathBytes, pathGranularity;
    std::uint64_t size, date;
    NativeMetadata* metadata;
    std::uint32_t flags, generation, references;
    std::int32_t type;
    std::uint32_t randomOrder, displayOffset;
};

static_assert(sizeof(Buffer) == 24 && offsetof(Buffer, usedBytes) == 12);
static_assert(sizeof(NativeRecord) == 0x48 && offsetof(NativeRecord, metadata) == 0x28
    && offsetof(NativeRecord, type) == 0x3c);
static_assert(sizeof(NativeMetadata) == 0xb0 && offsetof(NativeMetadata, tags) == 0xa8);
static_assert(sizeof(NativeTag) == 0x10 && offsetof(NativeTagValue, userEdited) == 8);

struct SnapshotHeader {
    std::uint64_t magic;
    FileStamp stamp;
    std::uint32_t roots, records;
};

struct BlockHeader {
    std::uint32_t bytes, storedBytes;
    Digest digest;
};

struct RecordFields {
    std::uint64_t size, date;
    std::uint32_t flags, generation;
    std::int32_t type;
    std::uint32_t metadataMask;
};

struct MetadataFields { std::uint32_t rating, flags, tags; };

struct OrderHeader {
    std::uint64_t magic;
    FileStamp stamp;
    std::int32_t column;
    std::uint32_t reverse, listFlags, explorerFlags, count;
    Digest digest;
};

template<class T>
bool readValue(std::ifstream& file, T& value)
{
    return bool(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template<class T>
bool writeValue(std::ofstream& file, const T& value)
{
    return bool(file.write(reinterpret_cast<const char*>(&value), sizeof(value)));
}

class Sha256 {
public:
    Sha256() { BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
    ~Sha256() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    bool hash(const void* data, std::size_t length, Digest& result) const
    {
        return handle && BCryptHash(handle, nullptr, 0,
            const_cast<PUCHAR>(static_cast<const unsigned char*>(data)), ULONG(length),
            result.data(), ULONG(result.size())) == 0;
    }
private:
    BCRYPT_ALG_HANDLE handle = nullptr;
};

class AtomicOutput {
public:
    explicit AtomicOutput(const std::filesystem::path& destination)
        : target(destination), temporary(destination)
    {
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (!error) file.open(temporary, std::ios::binary | std::ios::trunc);
    }

    ~AtomicOutput()
    {
        if (file.is_open()) file.close();
        if (!committed) DeleteFileW(temporary.c_str());
    }

    bool commit()
    {
        file.close();
        committed = bool(file) && MoveFileExW(temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        return committed;
    }

    std::ofstream file;
private:
    std::filesystem::path target, temporary;
    bool committed = false;
};

class BlockWriter {
public:
    explicit BlockWriter(std::ofstream& file) : out(file)
    {
        plain.reserve(chunkSize);
        CreateCompressor(COMPRESS_ALGORITHM_XPRESS, nullptr, &compressor);
    }
    ~BlockWriter() { if (compressor) CloseCompressor(compressor); }

    template<class T> bool value(const T& val) { return append(&val, sizeof(val)); }

    bool text(const char* text)
    {
        if (!text) return false;
        const auto length = std::strlen(text) + 1;
        return length <= maxTextSize && value(std::uint32_t(length)) && append(text, length);
    }
    bool boundary() { return plain.size() < chunkSize || flush(); }

    bool flush()
    {
        if (plain.empty()) return true;
        BlockHeader header{std::uint32_t(plain.size()), std::uint32_t(plain.size()), {}};
        if (!hash.hash(plain.data(), plain.size(), header.digest)) return false;
        const auto* data = plain.data();
        if (compressor) {
            stored.resize(plain.size());
            SIZE_T bytes = 0;
            if (Compress(compressor, plain.data(), plain.size(), stored.data(), stored.size(), &bytes)
                && bytes < plain.size()) {
                header.storedBytes = std::uint32_t(bytes);
                data = stored.data();
            }
        }
        const bool ok = writeValue(out, header) && bool(out.write(data, header.storedBytes));
        plain.clear();
        return ok;
    }

private:
    bool append(const void* data, std::size_t bytes)
    {
        if (bytes > maxChunkSize - plain.size()) return false;
        const auto* first = static_cast<const char*>(data);
        plain.insert(plain.end(), first, first + bytes);
        return true;
    }

    std::ofstream& out;
    COMPRESSOR_HANDLE compressor = nullptr;
    Sha256 hash;
    std::vector<char> plain, stored;
};

class BlockReader {
public:
    explicit BlockReader(std::ifstream& file, ReadMetrics& stats) : in(file), metrics(stats)
    {
        CreateDecompressor(COMPRESS_ALGORITHM_XPRESS, nullptr, &decompressor);
    }
    ~BlockReader() { if (decompressor) CloseDecompressor(decompressor); }

    template<class T> bool value(T& val)
    {
        const auto* data = next(sizeof(val));
        if (!data) return false;
        std::memcpy(&val, data, sizeof(val));
        return true;
    }

    const char* text(std::uint32_t& length)
    {
        if (!value(length) || !length || length > maxTextSize) return nullptr;
        const auto* result = next(length);
        return result && std::memchr(result, 0, length) == result + length - 1 ? result : nullptr;
    }

    bool finished() { return cursor == plain.size() && in.peek() == EOF; }

private:
    const char* next(std::size_t size)
    {
        if (cursor == plain.size() && !load()) return nullptr;
        if (size > plain.size() - cursor) return nullptr;
        const auto* result = plain.data() + cursor;
        cursor += size;
        return result;
    }

    bool load()
    {
        const auto readStart = Clock::now();
        BlockHeader header{};
        if (!readValue(in, header) || !header.bytes || header.bytes > maxChunkSize
            || !header.storedBytes || header.storedBytes > header.bytes) return false;
        plain.resize(header.bytes);
        if (header.bytes == header.storedBytes) {
            if (!in.read(plain.data(), header.bytes)) return false;
            metrics.readMs += elapsedMs(readStart);
        } else {
            stored.resize(header.storedBytes);
            if (!decompressor || !in.read(stored.data(), header.storedBytes)) return false;
            metrics.readMs += elapsedMs(readStart);
            const auto unpackStart = Clock::now();
            SIZE_T unpacked = 0;
            if (!Decompress(decompressor, stored.data(), stored.size(), plain.data(), plain.size(), &unpacked)
                || unpacked != plain.size()) return false;
            metrics.decompressMs += elapsedMs(unpackStart);
        }
        const auto verifyStart = Clock::now();
        Digest digest{};
        if (!hash.hash(plain.data(), plain.size(), digest) || digest != header.digest) return false;
        metrics.verifyMs += elapsedMs(verifyStart);
        cursor = 0;
        return true;
    }

    std::ifstream& in;
    ReadMetrics& metrics;
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    Sha256 hash;
    std::vector<char> plain, stored;
    std::size_t cursor = 0;
};

class NativeArray {
public:
    NativeArray(const NativeApi& native, unsigned count, int granularity, void (*releaseItem)(void*))
        : api(native), releaseItem(releaseItem),
          storage{nullptr, int(count * sizeof(void*)), 0, granularity}
    {
        if (storage.capacityBytes) storage.data = static_cast<void**>(allocate(storage.capacityBytes));
    }

    ~NativeArray()
    {
        for (std::size_t i = 0; i < std::size_t(storage.usedBytes) / sizeof(void*); ++i) {
            releaseItem(storage.data[i]);
        }
        if (storage.data) api.release(storage.data);
    }

    void append(void* item)
    {
        storage.data[std::size_t(storage.usedBytes) / sizeof(void*)] = item;
        storage.usedBytes += int(sizeof(void*));
    }

    void moveTo(Buffer& target)
    {
        if (target.data) api.release(target.data);
        target = storage;
        storage = {};
    }

    void* allocate(std::size_t bytes)
    {
        auto* result = api.allocate(bytes);
        if (!result) throw std::bad_alloc{};
        return result;
    }

    char* copyText(const char* text, std::uint32_t size)
    {
        auto* result = static_cast<char*>(allocate(size));
        std::memcpy(result, text, size);
        return result;
    }

private:
    const NativeApi& api;
    void (*releaseItem)(void*);
    Buffer storage;
};

bool valid(const Buffer& buffer, int stride)
{
    return buffer.usedBytes >= 0 && buffer.capacityBytes >= buffer.usedBytes
        && buffer.usedBytes % stride == 0 && (buffer.data || !buffer.usedBytes);
}

bool writeRecord(BlockWriter& out, const NativeRecord& record)
{
    RecordFields fields{record.size, record.date, record.flags, record.generation, record.type, 0};
    if (record.metadata) {
        fields.metadataMask = hasMetadata;
        for (unsigned i = 0; i < metadataSlots; ++i) {
            if (record.metadata->values[i]) fields.metadataMask |= 1U << i;
        }
    }
    if (!out.value(fields) || !out.text(record.path)) return false;
    if (const auto* meta = record.metadata) {
        if (meta->tags && !valid(*meta->tags, sizeof(NativeTag))) return false;
        const auto count = meta->tags ? unsigned(meta->tags->usedBytes) / sizeof(NativeTag) : 0;
        if (!out.value(MetadataFields{meta->rating, meta->flags, std::uint32_t(count)})) return false;
        for (unsigned i = 0; i < metadataSlots; ++i) {
            if (meta->values[i] && !out.text(meta->values[i])) return false;
        }
        const auto* tags = count ? reinterpret_cast<const NativeTag*>(meta->tags->data) : nullptr;
        for (std::size_t i = 0; i < count; ++i) {
            if (!tags[i].value || !out.text(tags[i].key) || !out.text(tags[i].value->value)
                || !out.value(std::uint8_t(tags[i].value->userEdited != 0))) return false;
        }
    }
    return out.boundary();
}

bool readRecord(BlockReader& in, const NativeApi& api, NativeArray& records)
{
    RecordFields fields{};
    std::uint32_t size{};
    if (!in.value(fields) || (fields.metadataMask & ~(hasMetadata | metadataBits))) return false;
    const auto* path = in.text(size);
    if (!path) return false;
    auto* record = new (records.allocate(sizeof(NativeRecord))) NativeRecord{};
    record->references = 1;
    records.append(record);
    record->path = records.copyText(path, size);
    record->pathCapacity = record->pathBytes = int(size);
    record->pathGranularity = 0x80;
    record->size = fields.size;
    record->date = fields.date;
    record->type = fields.type;
    record->flags = fields.flags;
    record->generation = fields.generation;
    if (!fields.metadataMask) return true;
    if (!(fields.metadataMask & hasMetadata)) return false;

    MetadataFields metadata{};
    if (!in.value(metadata) || metadata.tags > 8192) return false;
    auto* meta = new (records.allocate(sizeof(NativeMetadata))) NativeMetadata{};
    meta->references = 1;
    meta->rating = metadata.rating;
    record->metadata = meta;
    for (unsigned i = 0; i < metadataSlots; ++i) {
        if (!(fields.metadataMask & (1U << i))) continue;
        const auto* text = in.text(size);
        if (!text) return false;
        meta->values[i] = records.copyText(text, size);
    }
    for (std::uint32_t i = 0; i < metadata.tags; ++i) {
        const auto* key = in.text(size);
        if (!key) return false;
        // The value may move the reader to its next block.
        const std::string name(key, size - 1);
        const auto* value = in.text(size);
        if (!value) return false;
        const std::string tagValue(value, size - 1);
        std::uint8_t edited{};
        if (!in.value(edited) || edited > 1) return false;
        api.setUserTag(meta, name.c_str(), tagValue.c_str(), char(edited));
    }
    meta->flags = metadata.flags;
    return true;
}

} // namespace

bool FileStamp::operator==(const FileStamp& other) const
{
    return volume == other.volume && fileHigh == other.fileHigh && fileLow == other.fileLow
        && sizeHigh == other.sizeHigh && sizeLow == other.sizeLow
        && CompareFileTime(&writeTime, &other.writeTime) == 0;
}

bool SortSettings::operator==(const SortSettings& other) const
{
    return column == other.column && reverse == other.reverse
        && listFlags == other.listFlags && explorerFlags == other.explorerFlags;
}

bool writeSnapshot(const std::filesystem::path& path, const FileStamp& stamp,
    const Buffer& records, const Buffer& roots)
{
    if (!valid(records, sizeof(void*)) || !valid(roots, sizeof(void*))) return false;
    const auto count = std::uint32_t(records.usedBytes / sizeof(void*));
    const auto rootCount = std::uint32_t(roots.usedBytes / sizeof(void*));
    if (!count || count > maxBufferRows || rootCount > maxBufferRows) return false;
    AtomicOutput out(path);
    if (!out.file || !writeValue(out.file, SnapshotHeader{snapshotMagic, stamp, rootCount, count})) return false;
    BlockWriter writer(out.file);
    for (unsigned i = 0; i < rootCount; ++i) {
        if (!writer.text(static_cast<const char*>(roots.data[i])) || !writer.boundary()) return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto* record = static_cast<const NativeRecord*>(records.data[i]);
        if (!record || !writeRecord(writer, *record)) return false;
    }
    return writer.flush() && out.commit();
}

bool readSnapshot(const std::filesystem::path& path, const FileStamp& stamp,
    const NativeApi& api, Buffer& records, Buffer& roots, bool (*sourceUnchanged)(const FileStamp&),
    ReadMetrics& metrics)
{
    if (records.usedBytes || roots.usedBytes || !valid(records, sizeof(void*))
        || !valid(roots, sizeof(void*))) return false;
    std::ifstream file(path, std::ios::binary);
    SnapshotHeader header{};
    const auto sourceBytes = (std::uint64_t(stamp.sizeHigh) << 32) | stamp.sizeLow;
    if (!file || !readValue(file, header) || header.magic != snapshotMagic || !(header.stamp == stamp)
        || !header.records || header.records > maxBufferRows || header.roots > maxBufferRows
        || (std::uint64_t(header.records) + header.roots) > sourceBytes / 5) return false;
    BlockReader reader(file, metrics);
    NativeArray rootArray(api, header.roots, roots.granularity, api.release);
    NativeArray recordArray(api, header.records, records.granularity, api.releaseRecord);
    for (unsigned i = 0; i < header.roots; ++i) {
        std::uint32_t size{};
        const auto* root = reader.text(size);
        if (!root) return false;
        rootArray.append(rootArray.copyText(root, size));
    }
    for (unsigned i = 0; i < header.records; ++i) {
        if (!readRecord(reader, api, recordArray)) return false;
    }
    if (!reader.finished() || !sourceUnchanged(stamp)) return false;
    rootArray.moveTo(roots);
    recordArray.moveTo(records);
    return true;
}

bool readOrder(const std::filesystem::path& path, const FileStamp& stamp,
    const SortSettings& settings, std::size_t count, std::vector<std::uint32_t>& ordinals)
{
    if (count > maxBufferRows) return false;
    std::ifstream file(path, std::ios::binary);
    OrderHeader header{};
    if (!file || !readValue(file, header) || header.magic != orderMagic || !(header.stamp == stamp)
        || header.column != settings.column || header.reverse != std::uint32_t(settings.reverse)
        || header.listFlags != settings.listFlags || header.explorerFlags != settings.explorerFlags
        || header.count != count) return false;
    std::vector<std::uint32_t> order(count);
    const auto bytes = count * sizeof(order[0]);
    Digest digest{};
    if (!file.read(reinterpret_cast<char*>(order.data()), bytes) || file.peek() != EOF
        || !Sha256{}.hash(order.data(), bytes, digest) || digest != header.digest) return false;
    std::vector<bool> seen(count);
    for (const auto ordinal : order) {
        if (ordinal >= count || seen[ordinal]) return false;
        seen[ordinal] = true;
    }
    ordinals.swap(order);
    return true;
}

bool writeOrder(const std::filesystem::path& path, const FileStamp& stamp,
    const SortSettings& settings, const std::vector<std::uint32_t>& order)
{
    if (order.size() > maxBufferRows) return false;
    OrderHeader header{orderMagic, stamp, settings.column, std::uint32_t(settings.reverse),
        settings.listFlags, settings.explorerFlags, std::uint32_t(order.size()), {}};
    const auto bytes = order.size() * sizeof(order[0]);
    if (!Sha256{}.hash(order.data(), bytes, header.digest)) return false;
    AtomicOutput out(path);
    return out.file && writeValue(out.file, header)
        && out.file.write(reinterpret_cast<const char*>(order.data()), bytes) && out.commit();
}

} // namespace media_cache
