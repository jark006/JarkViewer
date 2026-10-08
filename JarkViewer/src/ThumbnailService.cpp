#ifndef NOMINMAX
#define NOMINMAX
#endif
// 同 framework.h：不让 Windows.h 引入旧 winsock.h，避免与 libraw 链里的 winsock2.h 冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../include/ThumbnailService.h"
#include <Windows.h>
#include <ShlObj.h>
#include <thumbcache.h>
#include <wrl/client.h>
#include <opencv2/opencv.hpp>
#include "ImageDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "uuid.lib")

namespace jark {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
constexpr uint32_t kSlots = 1000;
constexpr uint32_t kFormatVersion = 2; // 固定为 GDI 自顶向下副本及直通 alpha
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kIndexBytes = 64;
constexpr uint64_t kDataStart = kHeaderBytes + uint64_t(kSlots) * kIndexBytes;
constexpr uint32_t kEdge = 256;
constexpr uint32_t kMaxPng = 512 * 1024;
constexpr uint32_t kMaxPath = 128 * 1024;
constexpr uint32_t kMaxBlob = kMaxPng + kMaxPath;
constexpr uint64_t kMaxFile = 256ull * 1024 * 1024;
constexpr size_t kVisibleLimit = 96;
constexpr size_t kPrefetchLimit = 8;
constexpr size_t kMemoryLimit = 128;
constexpr size_t kQueueLimit = kVisibleLimit + kPrefetchLimit;
constexpr size_t kInvalidationLimit = 128;
constexpr uint32_t kValid = 1, kFailed = 2, kMissing = 4;

std::filesystem::path absolutePath(const std::filesystem::path& path);

// 仅做 Win32 路径的词法规范化，不打开原文件，也不解析联接或符号链接。
// 同样的大小写无关映射用于跨进程命名互斥锁的路径标识。
std::wstring pathKey(const std::wstring& input) {
    if (input.empty() || input.size() > 32767 || input.find(L'\0') != input.npos)
        return {};
    std::wstring key = absolutePath(input).wstring();
    if (key.empty()) return {};
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::wstring folded(key.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        key.data(), static_cast<int>(key.size()), folded.data(),
        static_cast<int>(folded.size()), nullptr, nullptr, 0))
        key = std::move(folded);
    return key;
}

std::filesystem::path absolutePath(const std::filesystem::path& path) {
    if (path.empty()) return {};
    const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32768) return {};
    std::wstring full(n, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
    if (!written || written >= n) return {};
    full.resize(written);
    return std::filesystem::path(full).lexically_normal();
}

std::string utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0 || n > int(kMaxPath)) return {};
    std::string result(n, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), n, nullptr, nullptr)) return {};
    return result;
}

uint64_t hash64(const std::wstring& text) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : text) {
        hash = (hash ^ (c & 255)) * 1099511628211ull;
        hash = (hash ^ (c >> 8)) * 1099511628211ull;
    }
    return hash;
}

uint64_t freshEpoch() {
    // 创建 GUID 不需要初始化 COM，也不依赖进程全局随机数发生器。
    GUID id{};
    if (SUCCEEDED(CoCreateGuid(&id))) {
        const auto* p = reinterpret_cast<const unsigned char*>(&id);
        uint64_t value = 14695981039346656037ull;
        for (size_t i = 0; i < sizeof(id); ++i) value = (value ^ p[i]) * 1099511628211ull;
        return value ? value : 1;
    }
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (uint64_t(counter.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32)) | 1;
}

uint32_t crc32(const unsigned char* data, size_t length) {
    // 使用局部查表，不让自持生命周期的工作线程依赖静态对象析构顺序。
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < table.size(); ++i) {
        uint32_t v = i;
        for (int b = 0; b < 8; ++b) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1)));
        table[i] = v;
    }
    uint32_t c = ~0u;
    for (size_t i = 0; i < length; ++i) c = table[(c ^ data[i]) & 255] ^ (c >> 8);
    return ~c;
}

void put32(unsigned char* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}
void put64(unsigned char* p, uint64_t value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}
uint32_t get32(const unsigned char* p) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i);
    return value;
}
uint64_t get64(const unsigned char* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}

struct Metadata {
    uint64_t mtime = 0, size = 0;
    bool exists = false;
    bool operator==(const Metadata&) const = default;
};
Metadata sourceMetadata(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)
        || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return { (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32)
            | data.ftLastWriteTime.dwLowDateTime,
        (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow, true };
}

struct IoFailure : std::runtime_error { IoFailure() : std::runtime_error("thumbnail cache I/O") {} };
struct BadFormat : std::runtime_error { BadFormat() : std::runtime_error("thumbnail cache format") {} };

class File {
public:
    HANDLE handle = INVALID_HANDLE_VALUE;
    File() = default;
    File(const std::filesystem::path& path, DWORD disposition = OPEN_ALWAYS) { open(path, disposition); }
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void close() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(handle, INVALID_HANDLE_VALUE)); }
    void open(const std::filesystem::path& path, DWORD disposition = OPEN_ALWAYS) {
        close();
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw IoFailure();
    }
    uint64_t size() const {
        LARGE_INTEGER n{};
        if (!GetFileSizeEx(handle, &n) || n.QuadPart < 0) throw IoFailure();
        return uint64_t(n.QuadPart);
    }
    void seek(uint64_t offset) {
        if (offset > kMaxFile) throw BadFormat();
        LARGE_INTEGER pos{}; pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, pos, nullptr, FILE_BEGIN)) throw IoFailure();
    }
    void read(uint64_t offset, void* target, size_t count) {
        if (count > kMaxBlob && count != kSlots * kIndexBytes) throw BadFormat();
        seek(offset);
        DWORD got = 0;
        if (!ReadFile(handle, target, static_cast<DWORD>(count), &got, nullptr) || got != count) throw IoFailure();
    }
    void write(uint64_t offset, const void* data, size_t count) {
        if (offset > kMaxFile || count > kMaxFile - offset) throw BadFormat();
        seek(offset);
        DWORD done = 0;
        if (!WriteFile(handle, data, static_cast<DWORD>(count), &done, nullptr) || done != count) throw IoFailure();
    }
    void flush() { if (!FlushFileBuffers(handle)) throw IoFailure(); }
};

class NamedMutex {
public:
    explicit NamedMutex(const std::filesystem::path& path) {
        const auto name = L"Global\\JarkViewer.Thumbnail." + std::to_wstring(hash64(pathKey(path.wstring())));
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
    }
    ~NamedMutex() { if (handle_) CloseHandle(handle_); }
    class Guard {
    public:
        explicit Guard(NamedMutex& mutex) : handle_(mutex.handle_) {
            const DWORD result = handle_ ? WaitForSingleObject(handle_, 150) : WAIT_FAILED;
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
        ~Guard() { if (locked_) ReleaseMutex(handle_); }
        explicit operator bool() const { return locked_; }
    private:
        HANDLE handle_;
        bool locked_ = false;
    };
private:
    HANDLE handle_ = nullptr;
};

struct Record {
    std::string key;
    Metadata metadata;
    uint64_t offset = 0, lastUse = 0;
    uint32_t length = 0, pathBytes = 0, pngBytes = 0, flags = 0, checksum = 0;
    bool valid() const { return flags & kValid; }
};
struct CacheLimits {
    uint64_t fileBytes = kMaxFile;
    uint64_t compactMinBytes = 4 * 1024 * 1024;
};
enum class CacheCode { Ok, Miss, StaleEpoch, Unavailable };
struct CacheResult {
    CacheCode code = CacheCode::Miss;
    Bytes png;
    bool failed = false;
    uint64_t epoch = 0, bytes = 0;
    size_t entries = 0;
};

// 仅缓存 I/O 线程（以及同步自检）使用。每次操作在路径对应的系统互斥锁内
// 打开并核对当前文件。PNG 编解码和 Shell 调用必须放在此类之外。
class DiskCache {
public:
    explicit DiskCache(std::filesystem::path path, CacheLimits limits = {})
        : path_(absolutePath(path)), mutex_(path_), limits_(limits) {}

    CacheResult sync() { return transaction([](File&) { return CacheResult{ CacheCode::Ok }; }); }

    CacheResult lookup(const std::wstring& key, const Metadata& metadata, bool touch,
        uint64_t expectedEpoch = 0) {
        const auto encodedKey = utf8(key);
        return transaction([&](File& file) {
            if (expectedEpoch && expectedEpoch != epoch_) return CacheResult{ CacheCode::StaleEpoch };
            const int slot = find(encodedKey);
            if (slot < 0) return CacheResult{};
            auto& record = records_[slot];
            if (record.metadata != metadata) {
                eraseSlot(file, slot);
                return CacheResult{};
            }
            Bytes blob(record.length);
            file.read(record.offset, blob.data(), blob.size());
            if (crc32(blob.data(), blob.size()) != record.checksum
                || !std::equal(record.key.begin(), record.key.end(), reinterpret_cast<const char*>(blob.data()))) {
                eraseSlot(file, slot);
                return CacheResult{};
            }
            CacheResult result{ CacheCode::Ok };
            result.failed = (record.flags & kFailed) != 0;
            result.png.assign(blob.begin() + record.pathBytes, blob.end());
            if (touch) {
                record.lastUse = nextUse_++;
                writeIndex(file, slot, record);
                commitHeader(file);
            }
            return result;
        });
    }

    CacheResult put(const std::wstring& key, const Metadata& metadata, const Bytes& png,
        bool failed, uint64_t expectedEpoch) {
        const auto encodedKey = utf8(key);
        // 只接受长度受限、已编码的 PNG，或不带 PNG 的失败标记。
        if (encodedKey.empty() || encodedKey.size() > kMaxPath || png.size() > kMaxPng
            || (!failed && png.empty()) || (failed && !png.empty()))
            return { CacheCode::Unavailable };
        Bytes blob(encodedKey.begin(), encodedKey.end());
        blob.insert(blob.end(), png.begin(), png.end());
        const uint32_t checksum = crc32(blob.data(), blob.size());
        return transaction([&](File& file) {
            if (expectedEpoch != epoch_) return CacheResult{ CacheCode::StaleEpoch };
            int slot = find(encodedKey);
            if (slot < 0) slot = freeSlot();
            if (slot < 0) slot = coldest();
            records_[slot] = {}; // 提交或整理之前，磁盘仍保留旧记录。
            uint64_t live = liveBytes();
            while (kDataStart + live + blob.size() > limits_.fileBytes) {
                const int victim = coldest();
                if (victim < 0) throw BadFormat();
                live -= records_[victim].length;
                records_[victim] = {};
            }
            // 字节配额要求淘汰多条记录时，整理会一并发布所有删除。
            // 事务失败必须丢弃内存索引，下次重新读取磁盘现状。
            const uint64_t holes = fileBytes_ - kDataStart - live;
            if (fileBytes_ + blob.size() > limits_.fileBytes
                || (fileBytes_ > limits_.compactMinBytes && holes > live / 2))
                compact(file);
            Record record;
            record.key = encodedKey;
            record.metadata = metadata;
            record.offset = fileBytes_;
            record.length = static_cast<uint32_t>(blob.size());
            record.pathBytes = static_cast<uint32_t>(encodedKey.size());
            record.pngBytes = static_cast<uint32_t>(png.size());
            record.flags = kValid | (failed ? kFailed : 0) | (metadata.exists ? 0 : kMissing);
            record.lastUse = nextUse_++;
            record.checksum = checksum;
            file.write(record.offset, blob.data(), blob.size());
            file.flush(); // 先完整落盘数据块，再发布指向它的索引。
            writeIndex(file, slot, record);
            file.flush();
            records_[slot] = std::move(record);
            fileBytes_ = file.size();
            commitHeader(file);
            return CacheResult{ CacheCode::Ok };
        });
    }

    CacheResult touch(const std::wstring& key, const Metadata& metadata, uint64_t expectedEpoch) {
        const auto encodedKey = utf8(key);
        return transaction([&](File& file) {
            if (expectedEpoch != epoch_) return CacheResult{ CacheCode::StaleEpoch };
            const int slot = find(encodedKey);
            if (slot < 0) return CacheResult{};
            if (records_[slot].metadata != metadata) {
                eraseSlot(file, slot);
                return CacheResult{};
            }
            records_[slot].lastUse = nextUse_++;
            writeIndex(file, slot, records_[slot]);
            commitHeader(file);
            return CacheResult{ CacheCode::Ok };
        });
    }

    CacheResult erase(const std::wstring& key, uint64_t expectedEpoch = 0) {
        const auto encodedKey = utf8(key);
        return transaction([&](File& file) {
            if (expectedEpoch && expectedEpoch != epoch_) return CacheResult{ CacheCode::StaleEpoch };
            const int slot = find(encodedKey);
            if (slot >= 0) eraseSlot(file, slot);
            return CacheResult{ CacheCode::Ok };
        });
    }

    CacheResult clear() {
        return transaction([&](File& file) {
            const uint64_t newEpoch = epoch_ == UINT64_MAX ? freshEpoch() : epoch_ + 1;
            const uint64_t newGeneration = generation_ + 1;
            file.close();
            replaceEmpty(newEpoch, newGeneration);
            file.open(path_, OPEN_EXISTING);
            loaded_ = false;
            refresh(file);
            return CacheResult{ CacheCode::Ok };
        });
    }
    uint64_t compactions() const { return compactions_; }

private:
    std::filesystem::path path_;
    NamedMutex mutex_;
    CacheLimits limits_;
    std::array<Record, kSlots> records_{};
    uint64_t epoch_ = 0, generation_ = 0, nextUse_ = 1, fileBytes_ = 0, compactions_ = 0;
    bool loaded_ = false;

    std::array<unsigned char, kHeaderBytes> header(uint64_t epoch, uint64_t generation,
        uint64_t nextUse) const {
        std::array<unsigned char, kHeaderBytes> data{};
        constexpr char magic[] = "JARKTHM1";
        std::copy_n(magic, 8, data.begin());
        put32(data.data() + 8, kFormatVersion);
        put32(data.data() + 12, kHeaderBytes);
        put32(data.data() + 16, kSlots);
        put32(data.data() + 20, kIndexBytes);
        put64(data.data() + 24, generation);
        put64(data.data() + 32, epoch);
        put64(data.data() + 40, nextUse);
        put32(data.data() + 56, crc32(data.data(), 56));
        return data;
    }
    void writeEmpty(File& file, uint64_t epoch, uint64_t generation) {
        const auto data = header(epoch, generation, 1);
        const Bytes zeroIndex(kSlots * kIndexBytes, 0);
        file.write(0, data.data(), data.size());
        file.write(kHeaderBytes, zeroIndex.data(), zeroIndex.size());
        file.flush();
    }
    std::filesystem::path tempPath() const {
        return std::filesystem::path(path_.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId())
            + L"." + std::to_wstring(freshEpoch()));
    }
    void replaceEmpty(uint64_t epoch, uint64_t generation) {
        const auto temporary = tempPath();
        try {
            { File out(temporary, CREATE_NEW); writeEmpty(out, epoch, generation); }
            if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw IoFailure();
        } catch (...) { DeleteFileW(temporary.c_str()); throw; }
    }
    template<class Function> CacheResult transaction(Function&& operation) {
        try {
            if (path_.empty()) throw IoFailure();
            NamedMutex::Guard guard(mutex_);
            if (!guard) throw IoFailure();
            std::error_code ec;
            std::filesystem::create_directories(path_.parent_path(), ec);
            if (ec) throw IoFailure();
            File file(path_);
            if (file.size() == 0) { writeEmpty(file, freshEpoch(), 1); loaded_ = false; }
            try { refresh(file); }
            catch (const BadFormat&) {
                // 损坏的文件头无法提供可信代次；用新的随机 epoch 重建，
                // 防止先前已经开始的提取结果回填。
                file.close();
                replaceEmpty(freshEpoch(), 1);
                file.open(path_, OPEN_EXISTING);
                loaded_ = false;
                refresh(file);
            }
            auto result = operation(file);
            result.epoch = epoch_;
            result.bytes = fileBytes_;
            result.entries = static_cast<size_t>(std::count_if(records_.begin(), records_.end(),
                [](const Record& r) { return r.valid(); }));
            return result;
        } catch (...) {
            loaded_ = false;
            return { CacheCode::Unavailable };
        }
    }
    void refresh(File& file) {
        const uint64_t bytes = file.size();
        if (bytes < kDataStart || bytes > limits_.fileBytes) throw BadFormat();
        std::array<unsigned char, kHeaderBytes> data{};
        file.read(0, data.data(), data.size());
        constexpr char magic[] = "JARKTHM1";
        if (!std::equal(data.begin(), data.begin() + 8, magic)
            || get32(data.data() + 8) != kFormatVersion || get32(data.data() + 12) != kHeaderBytes
            || get32(data.data() + 16) != kSlots || get32(data.data() + 20) != kIndexBytes
            || get32(data.data() + 56) != crc32(data.data(), 56)) throw BadFormat();
        const auto generation = get64(data.data() + 24), epoch = get64(data.data() + 32);
        const auto nextUse = get64(data.data() + 40);
        if (!epoch || !generation || !nextUse || generation == UINT64_MAX || nextUse == UINT64_MAX)
            throw BadFormat();
        if (loaded_ && generation == generation_ && epoch == epoch_ && bytes == fileBytes_) return;
        records_ = {};
        generation_ = generation; epoch_ = epoch; nextUse_ = nextUse; fileBytes_ = bytes;
        Bytes index(kSlots * kIndexBytes);
        file.read(kHeaderBytes, index.data(), index.size());
        std::unordered_set<std::string> keys;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (uint32_t i = 0; i < kSlots; ++i) {
            const auto* p = index.data() + size_t(i) * kIndexBytes;
            Record r;
            r.offset = get64(p); r.length = get32(p + 8); r.pathBytes = get32(p + 12);
            r.pngBytes = get32(p + 16); r.flags = get32(p + 20);
            r.metadata = { get64(p + 24), get64(p + 32), !(r.flags & kMissing) };
            r.lastUse = get64(p + 40); r.checksum = get32(p + 48);
            if (!r.valid() || (r.flags & ~(kValid | kFailed | kMissing))
                || get32(p + 52) != crc32(p, 52)
                || !r.pathBytes || r.pathBytes > kMaxPath || r.pngBytes > kMaxPng
                || r.length > kMaxBlob || uint64_t(r.pathBytes) + r.pngBytes != r.length
                || ((r.flags & kFailed) ? r.pngBytes != 0 : r.pngBytes == 0)
                || r.offset < kDataStart || r.offset > bytes || r.length > bytes - r.offset
                || !r.lastUse || r.lastUse >= nextUse) continue;
            // 多个索引不能重叠引用同一段数据，否则容量统计可能下溢。
            if (std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                return r.offset < range.second && range.first < r.offset + r.length;
            })) continue;
            r.key.resize(r.pathBytes);
            file.read(r.offset, r.key.data(), r.key.size());
            if (r.key.find('\0') != r.key.npos || !keys.insert(r.key).second) continue;
            ranges.emplace_back(r.offset, r.offset + r.length);
            records_[i] = std::move(r);
        }
        loaded_ = true;
    }
    void writeIndex(File& file, int slot, const Record& record) {
        std::array<unsigned char, kIndexBytes> data{};
        if (record.valid()) {
            put64(data.data(), record.offset);
            put32(data.data() + 8, record.length); put32(data.data() + 12, record.pathBytes);
            put32(data.data() + 16, record.pngBytes); put32(data.data() + 20, record.flags);
            put64(data.data() + 24, record.metadata.mtime); put64(data.data() + 32, record.metadata.size);
            put64(data.data() + 40, record.lastUse); put32(data.data() + 48, record.checksum);
            put32(data.data() + 52, crc32(data.data(), 52));
        }
        file.write(kHeaderBytes + uint64_t(slot) * kIndexBytes, data.data(), data.size());
    }
    void commitHeader(File& file) {
        ++generation_;
        const auto data = header(epoch_, generation_, nextUse_);
        file.write(0, data.data(), data.size());
        file.flush();
    }
    void eraseSlot(File& file, int slot) {
        records_[slot] = {};
        writeIndex(file, slot, records_[slot]);
        commitHeader(file);
    }
    int find(const std::string& key) const {
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].valid() && records_[i].key == key) return static_cast<int>(i);
        return -1;
    }
    int freeSlot() const {
        for (size_t i = 0; i < records_.size(); ++i) if (!records_[i].valid()) return static_cast<int>(i);
        return -1;
    }
    int coldest() const {
        int slot = -1;
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].valid() && (slot < 0 || records_[i].lastUse < records_[slot].lastUse))
                slot = static_cast<int>(i);
        return slot;
    }
    uint64_t liveBytes() const {
        uint64_t bytes = 0;
        for (const auto& record : records_) if (record.valid()) bytes += record.length;
        return bytes;
    }
    void compact(File& file) {
        // 仅复制缓存自身的数据块，丢弃校验失败的记录。
        const auto temporary = tempPath();
        auto updated = records_;
        try {
            {
                File out(temporary, CREATE_NEW);
                writeEmpty(out, epoch_, generation_ + 1);
                uint64_t offset = kDataStart;
                for (size_t i = 0; i < updated.size(); ++i) {
                    auto& r = updated[i];
                    if (!r.valid()) continue;
                    Bytes blob(r.length);
                    file.read(r.offset, blob.data(), blob.size());
                    if (crc32(blob.data(), blob.size()) != r.checksum) { r = {}; continue; }
                    out.write(offset, blob.data(), blob.size());
                    r.offset = offset; offset += blob.size();
                    writeIndex(out, static_cast<int>(i), r);
                }
                const auto data = header(epoch_, generation_ + 1, nextUse_);
                out.write(0, data.data(), data.size());
                out.flush();
            }
            file.close();
            if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw IoFailure();
            file.open(path_, OPEN_EXISTING);
            loaded_ = false;
            refresh(file);
            ++compactions_;
        } catch (...) { DeleteFileW(temporary.c_str()); throw; }
    }
};

cv::Mat failurePlaceholder() {
    cv::Mat image(64, 64, CV_8UC4, cv::Scalar(58, 58, 58, 255));
    for (int y = 12; y < 52; ++y)
        for (int x = 12; x < 52; ++x)
            if (std::abs(x - y) < 2 || std::abs(x + y - 63) < 2)
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(155, 155, 155, 255);
    return image;
}

bool boundedPng(const Bytes& png) {
    constexpr unsigned char signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (png.size() < 33 || png.size() > kMaxPng || !std::equal(std::begin(signature), std::end(signature), png.begin()))
        return false;
    auto big32 = [](const unsigned char* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    };
    if (big32(png.data() + 8) != 13 || png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R')
        return false;
    const auto width = big32(png.data() + 16), height = big32(png.data() + 20);
    return width && height && width <= kEdge && height <= kEdge
        && png[24] == 8 && png[25] == 6; // 只接收本服务写入的 8 位 RGBA 缓存 PNG。
}

cv::Mat decodeCachePng(const Bytes& png) {
    if (!boundedPng(png)) return {};
    try {
        auto image = cv::imdecode(png, cv::IMREAD_UNCHANGED);
        if (image.type() != CV_8UC4 || image.empty() || image.cols > int(kEdge) || image.rows > int(kEdge)) return {};
        return image;
    } catch (...) { return {}; }
}
Bytes encodeCachePng(const cv::Mat& image) {
    Bytes png;
    if (image.empty() || image.type() != CV_8UC4 || image.cols > int(kEdge) || image.rows > int(kEdge)) return png;
    try {
        if (!cv::imencode(".png", image, png, { cv::IMWRITE_PNG_COMPRESSION, 3 }) || !boundedPng(png)) png.clear();
    } catch (...) { png.clear(); }
    return png;
}

// HBITMAP 属于 ISharedBitmap，本函数只借用；不可选入 DC、Detach，
// 更不可对借用句柄调用 DeleteObject。
cv::Mat sharedBitmapImage(ISharedBitmap* shared) {
    HBITMAP bitmap = nullptr;
    SIZE size{};
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    if (!shared || FAILED(shared->GetSharedBitmap(&bitmap)) || !bitmap
        || FAILED(shared->GetSize(&size)) || size.cx <= 0 || size.cy <= 0
        || size.cx > LONG(kEdge) || size.cy > LONG(kEdge)) return {};
    if (FAILED(shared->GetFormat(&alpha))) return {};
    cv::Mat result(size.cy, size.cx, CV_8UC4);
    // 共享缓存位图不能仅凭 GetObject 的 biHeight 推断内存行序；
    // 统一通过 GDI 请求自顶向下的副本，避免系统缓存返回的缩略图上下颠倒。
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return {};
    const int rows = GetDIBits(dc, bitmap, 0, static_cast<UINT>(size.cy), result.data, &info, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != size.cy) return {};
    for (int y = 0; y < result.rows; ++y) {
        auto* row = result.ptr<cv::Vec4b>(y);
        for (int x = 0; x < result.cols; ++x) {
            auto& pixel = row[x];
            if (alpha != WTSAT_ARGB) { pixel[3] = 255; continue; }
            const unsigned a = pixel[3];
            for (int c = 0; c < 3; ++c)
                pixel[c] = a ? static_cast<unsigned char>(std::min(255u, (unsigned(pixel[c]) * 255 + a / 2) / a)) : 0;
        }
    }
    return result;
}

// —— 本地解码兜底 ——
// Shell 链路拿不到缩略图时（无处理器、未安装 JarkThumbnailProvider.dll 等），
// 由解码工作线程用工程内解码器生成缩略图，预览带因此不依赖任何已注册的 Shell 处理器。
// 只在解码线程调用：独立 ImageDatabase 实例，不进应用缓存，也不读窗口/显示器状态。

bool localDecodeEligible(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.rfind(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) || dot + 1 >= path.size())
        return false;
    std::wstring ext = path.substr(dot + 1);
    for (auto& c : ext) if (c >= L'A' && c <= L'Z') c = wchar_t(c + (L'a' - L'A'));
    return ImageDatabase::supportExt.contains(ext) || ImageDatabase::supportRaw.contains(ext)
        || ImageDatabase::videoExt.contains(ext);
}

// 任意解码结果 → 长边不超过 kEdge 的 8UC4 缩略图；失败返回空。
cv::Mat thumbnailImage(cv::Mat image) {
    if (image.empty() || image.channels() < 1 || image.channels() > 4)
        return {};
    try {
        ImageDatabase::convertMatToCV_8U(image); // 与查看器显示同一套深度语义
        if (image.channels() == 1) cv::cvtColor(image, image, cv::COLOR_GRAY2BGRA);
        else if (image.channels() == 3) cv::cvtColor(image, image, cv::COLOR_BGR2BGRA);
        const double edge = (std::max)(image.cols, image.rows);
        if (edge > double(kEdge)) {
            const double scale = double(kEdge) / edge;
            cv::resize(image, image, cv::Size(), scale, scale, cv::INTER_AREA);
        }
        if (image.type() != CV_8UC4 || image.cols < 1 || image.rows < 1
            || image.cols > int(kEdge) || image.rows > int(kEdge)) return {};
        return image;
    } catch (...) { return {}; }
}

cv::Mat decodeLocalThumbnail(ImageDatabase& database, const std::wstring& path) {
    ImageAsset asset;
    try { asset = database.myLoader(path); } catch (...) { return {}; }
    if (ImageDatabase::isDecodeFailed(asset)) return {};
    // 动图取第一帧；EXIF 方向已由各解码分支按查看器同一策略应用。
    const cv::Mat& source = !asset.primaryFrame.empty() ? asset.primaryFrame : asset.frames.front();
    return thumbnailImage(source);
}

struct ShellResult {
    cv::Mat image;
    HRESULT cached = E_FAIL, extracted = E_FAIL;
    bool cacheHit = false;
};
class ShellSession {
public:
    ShellSession() : initialized_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (SUCCEEDED(initialized_))
            CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache_));
    }
    ~ShellSession() {
        cache_.Reset(); // 所有 COM 引用都在初始化它们的工作线程释放。
        if (SUCCEEDED(initialized_)) CoUninitialize();
    }
    ShellResult extract(const std::wstring& path) {
        ShellResult result;
        if (FAILED(initialized_) || !cache_) return result;
        ComPtr<IShellItem> item;
        const auto shellPath = absolutePath(path);
        result.extracted = SHCreateItemFromParsingName(shellPath.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(result.extracted)) return result;
        ComPtr<ISharedBitmap> bitmap;
        WTS_CACHEFLAGS flags{};
        WTS_THUMBNAILID id{};
        result.cached = cache_->GetThumbnail(item.Get(), kEdge, WTS_INCACHEONLY, &bitmap, &flags, &id);
        if (SUCCEEDED(result.cached) && bitmap) {
            result.image = sharedBitmapImage(bitmap.Get());
            if (!result.image.empty()) { result.cacheHit = true; result.extracted = S_OK; return result; }
        }
        bitmap.Reset();
        // WTS_EXTRACT 默认使用系统 surrogate。禁止添加 EXTRACTINPROC、
        // 直接实例化 provider，或回退到查看器的原图解码器。
        result.extracted = cache_->GetThumbnail(item.Get(), kEdge,
            static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_SCALETOREQUESTEDSIZE), &bitmap, &flags, &id);
        if (SUCCEEDED(result.extracted) && bitmap) result.image = sharedBitmapImage(bitmap.Get());
        return result;
    }
private:
    HRESULT initialized_;
    ComPtr<IThumbnailCache> cache_;
};

struct Request {
    std::wstring path, key;
    bool visible = false, touch = false;
    uint64_t generation = 0;
};
struct ShellTask {
    Request request;
    Metadata metadata;
    uint64_t epoch = 0;
};
struct Completion {
    ShellTask task;
    cv::Mat image;
    bool failed = false;
    bool fromShell = true; // false = 本地解码兜底结果，失败时不再重复排队
};
struct MemoryItem {
    Metadata metadata;
    Thumbnail thumbnail;
    uint64_t use = 0;
    bool persisted = false;
};
using Published = std::unordered_map<std::wstring, Thumbnail>;

struct ServiceState {
    explicit ServiceState(std::filesystem::path path, std::shared_ptr<std::atomic<uint64_t>> versions)
        : cachePath(std::move(path)), versions(std::move(versions)) {
        published.store(std::make_shared<const Published>());
        publishedStats.store(std::make_shared<const Stats>());
    }
    const std::filesystem::path cachePath;
    const std::shared_ptr<std::atomic<uint64_t>> versions;
    std::mutex mutex;
    std::condition_variable wake, finished;
    bool stop = false, diskClearPending = false;
    uint64_t generation = 1, epoch = 0, memoryUse = 0, clearSerial = 0;
    unsigned workers = 0;
    std::vector<std::wstring> lastVisible, lastPrefetch;
    std::vector<Request> desired;
    std::deque<Request> requests;
    std::deque<ShellTask> shellQueue;
    std::deque<ShellTask> decodeQueue;
    std::deque<Completion> completions;
    std::unordered_set<std::wstring> invalidations;
    std::unordered_map<std::wstring, MemoryItem> memory;
    std::unordered_set<std::wstring> confirmed;
    std::atomic<std::shared_ptr<const Published>> published;
    std::atomic<std::shared_ptr<const Stats>> publishedStats;
    std::atomic<bool> changed{ false };
    Stats currentStats;

    bool currentLocked(const Request& r) const { return !stop && r.generation == generation; }
    bool current(const Request& r) {
        std::lock_guard lock(mutex);
        return currentLocked(r);
    }
    void publishLocked() {
        auto snapshot = std::make_shared<Published>();
        for (const auto& [key, entry] : memory)
            if (confirmed.contains(key)) snapshot->emplace(key, entry.thumbnail);
        published.store(std::move(snapshot));
        changed.store(true);
    }
    void statsLocked() {
        publishedStats.store(std::make_shared<const Stats>(currentStats));
        changed.store(true);
    }
    void requeueLocked() {
        ++generation;
        requests.clear(); shellQueue.clear(); decodeQueue.clear(); completions.clear();
        for (auto& r : desired) {
            r.generation = generation;
            requests.push_back(r);
        }
        wake.notify_all();
    }
    void trimLocked() {
        while (memory.size() > kMemoryLimit) {
            auto victim = memory.end();
            for (auto i = memory.begin(); i != memory.end(); ++i) {
                const bool visible = std::any_of(desired.begin(), desired.end(), [&](const Request& r) {
                    return r.visible && r.key == i->first;
                });
                if (!visible && (victim == memory.end() || i->second.use < victim->second.use)) victim = i;
            }
            if (victim == memory.end()) break;
            confirmed.erase(victim->first);
            memory.erase(victim);
        }
    }
    // 外部清理改变 epoch 时丢弃旧结果；自己的磁盘清理完成不重复增加 UI 清理序号。
    bool observe(const CacheResult& result, bool ownClear = false) {
        std::lock_guard lock(mutex);
        if (stop) return false;
        const bool available = result.code != CacheCode::Unavailable;
        bool epochChanged = false;
        if (available && result.epoch) {
            epochChanged = epoch && epoch != result.epoch;
            epoch = result.epoch;
            if (epochChanged) {
                if (!ownClear)
                    ++currentStats.clearVersion;
                memory.clear(); confirmed.clear();
                requeueLocked();
                publishLocked();
            }
        }
        const size_t entries = available ? result.entries : currentStats.entries;
        const uint64_t bytes = available ? result.bytes : currentStats.bytes;
        if (epochChanged || available != currentStats.diskAvailable || entries != currentStats.entries || bytes != currentStats.bytes) {
            currentStats.diskAvailable = available;
            currentStats.entries = entries;
            currentStats.bytes = bytes;
            statsLocked();
        }
        return !epochChanged;
    }
};

void workerFinished(const std::shared_ptr<ServiceState>& state) {
    std::lock_guard lock(state->mutex);
    --state->workers;
    state->finished.notify_all();
}

void runShell(const std::shared_ptr<ServiceState>& state) {
    try {
        ShellSession shell;
        for (;;) {
            ShellTask task;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stop || !state->shellQueue.empty(); });
                if (state->stop) break;
                task = std::move(state->shellQueue.front());
                state->shellQueue.pop_front();
                if (!state->currentLocked(task.request)) continue;
            }
            Completion result; result.task = task;
            try { result.image = shell.extract(task.request.path).image; }
            catch (...) { result.image.release(); }
            result.failed = result.image.empty();
            {
                std::lock_guard lock(state->mutex);
                if (state->currentLocked(task.request) && state->completions.size() < kQueueLimit)
                    state->completions.push_back(std::move(result));
                state->wake.notify_all();
            }
        }
    } catch (...) { /* COM 或分配失败不影响主界面。 */ }
    workerFinished(state);
}

uint64_t currentEpoch(const std::shared_ptr<ServiceState>& state) {
    std::lock_guard lock(state->mutex);
    return state->epoch;
}

// Shell 失败后的兜底解码线程：与 Shell 线程分开，慢解码既不挡住系统提取，
// 也不阻塞缓存查询/清理。独立 ImageDatabase 实例只在本线程使用。
void runDecode(const std::shared_ptr<ServiceState>& state) {
    try {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC 兜底解码需要 COM
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // 不抢查看大图的前台解码
        ImageDatabase database;
        for (;;) {
            ShellTask task;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stop || !state->decodeQueue.empty(); });
                if (state->stop) break;
                task = std::move(state->decodeQueue.front());
                state->decodeQueue.pop_front();
                if (!state->currentLocked(task.request)) continue;
            }
            Completion result;
            result.task = task;
            result.fromShell = false;
            try { result.image = decodeLocalThumbnail(database, task.request.path); }
            catch (...) { result.image.release(); }
            result.failed = result.image.empty();
            {
                std::lock_guard lock(state->mutex);
                if (state->currentLocked(task.request) && state->completions.size() < kQueueLimit)
                    state->completions.push_back(std::move(result));
                state->wake.notify_all();
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    } catch (...) { /* 兜底解码失败不影响主界面。 */ }
    workerFinished(state);
}

void persistItem(const std::shared_ptr<ServiceState>& state, DiskCache& disk,
    const Request& request, const MemoryItem& item, uint64_t epoch, bool touch) {
    // 预取不刷热；提取失败只在内存中短暂记住，不能永久遮住后来安装的处理器。
    if (!request.visible || item.thumbnail.failed || !state->current(request)) return;
    if (item.persisted) {
        if (!touch) return;
        const auto result = disk.touch(request.key, item.metadata, epoch);
        if (!state->observe(result) || !state->current(request)) return;
        if (result.code != CacheCode::Miss) return;
        // CPU 缓存命中后，该磁盘记录仍可能已被另一个实例淘汰。
    }
    Bytes png;
    if (!item.thumbnail.failed) {
        png = encodeCachePng(item.thumbnail.image); // 编码时不持有跨进程互斥锁。
        if (png.empty()) return;
    }
    if (!state->current(request)) return;
    const auto result = disk.put(request.key, item.metadata, png, item.thumbnail.failed, epoch);
    if (!state->observe(result) || result.code != CacheCode::Ok) return;
    std::lock_guard lock(state->mutex);
    auto found = state->memory.find(request.key);
    if (state->currentLocked(request) && found != state->memory.end()
        && found->second.thumbnail.version == item.thumbnail.version)
        found->second.persisted = true;
}

void acceptImage(const std::shared_ptr<ServiceState>& state, DiskCache& disk,
    const Request& request, const Metadata& metadata, cv::Mat image, bool failed,
    bool persisted, uint64_t epoch) {
    MemoryItem item;
    item.metadata = metadata;
    item.thumbnail.image = failed ? failurePlaceholder() : std::move(image);
    item.thumbnail.failed = failed;
    item.persisted = persisted;
    {
        std::lock_guard lock(state->mutex);
        if (!state->currentLocked(request) || epoch != state->epoch) return;
        item.thumbnail.version = state->versions->fetch_add(1) + 1;
        item.use = ++state->memoryUse;
        state->memory[request.key] = item;
        state->confirmed.insert(request.key);
        state->trimLocked();
        state->publishLocked();
    }
    persistItem(state, disk, request, item, epoch, false);
}

void processRequest(const std::shared_ptr<ServiceState>& state, DiskCache& disk, const Request& request) {
    if (!state->current(request)) return;
    const Metadata metadata = sourceMetadata(request.path); // 仅读文件属性，不读原图内容。
    MemoryItem memory;
    bool memoryHit = false;
    uint64_t epoch = 0;
    {
        std::lock_guard lock(state->mutex);
        if (!state->currentLocked(request)) return;
        epoch = state->epoch;
        auto found = state->memory.find(request.key);
        if (found != state->memory.end() && found->second.metadata == metadata &&
            !(request.touch && found->second.thumbnail.failed)) {
            found->second.use = ++state->memoryUse;
            memory = found->second;
            memoryHit = true;
            state->confirmed.insert(request.key);
            state->publishLocked();
        } else if (found != state->memory.end()) {
            state->memory.erase(found);
            state->confirmed.erase(request.key);
            state->publishLocked();
        }
    }
    if (memoryHit) {
        persistItem(state, disk, request, memory, epoch, request.touch);
        return;
    }
    auto result = disk.lookup(request.key, metadata, request.visible && request.touch, epoch);
    if (!state->observe(result) || !state->current(request)) return;
    epoch = currentEpoch(state);
    if (result.code == CacheCode::Ok) {
        cv::Mat image = result.failed ? cv::Mat{} : decodeCachePng(result.png);
        if (result.failed || !image.empty()) {
            acceptImage(state, disk, request, metadata, std::move(image), result.failed, true, epoch);
            return;
        }
        state->observe(disk.erase(request.key, epoch)); // 缓存 PNG 损坏也只重新请求 Shell，不调用原图解码器。
    }
    if (!state->current(request)) return;
    if (!metadata.exists) {
        acceptImage(state, disk, request, metadata, {}, true, false, epoch);
        return;
    }
    std::lock_guard lock(state->mutex);
    if (!state->currentLocked(request) || state->shellQueue.size() >= kQueueLimit) return;
    ShellTask task{ request, metadata, epoch };
    // 可见项排在少量预取项前面。
    auto position = request.visible ? std::find_if(state->shellQueue.begin(), state->shellQueue.end(),
        [](const ShellTask& r) { return !r.request.visible; }) : state->shellQueue.end();
    state->shellQueue.insert(position, std::move(task));
    state->wake.notify_all();
}

void processCompletion(const std::shared_ptr<ServiceState>& state, DiskCache& disk, Completion result) {
    const auto& task = result.task;
    if (!state->current(task.request)) return;
    if (!state->observe(disk.sync()) || task.epoch != currentEpoch(state)) return;
    if (sourceMetadata(task.request.path) != task.metadata) {
        // 外部处理器运行时原文件发生变化，重新排队核对，不发布过时图像。
        std::lock_guard lock(state->mutex);
        if (state->currentLocked(task.request) && state->requests.size() < kQueueLimit)
            state->requests.push_front(task.request);
        return;
    }
    if (result.failed && result.fromShell && localDecodeEligible(task.request.path)) {
        // Shell 拿不到缩略图（没有处理器或未安装 provider DLL）：排队本地解码兜底；
        // 成功照常发布并持久化，失败才落占位。可见项排在预取项前面。
        std::lock_guard lock(state->mutex);
        if (!state->currentLocked(task.request) || state->decodeQueue.size() >= kQueueLimit) return;
        auto position = task.request.visible
            ? std::find_if(state->decodeQueue.begin(), state->decodeQueue.end(),
                [](const ShellTask& r) { return !r.request.visible; })
            : state->decodeQueue.end();
        state->decodeQueue.insert(position, task);
        state->wake.notify_all();
        return;
    }
    acceptImage(state, disk, task.request, task.metadata, std::move(result.image), result.failed, false, task.epoch);
}

void runCacheIo(const std::shared_ptr<ServiceState>& state) {
    try {
        DiskCache disk(state->cachePath);
        state->observe(disk.sync());
        auto nextPoll = Clock::now() + std::chrono::seconds(2);
        for (;;) {
            Request request;
            Completion completion;
            std::wstring invalidation;
            enum class Work { None, Clear, Invalidate, Complete, Request } work = Work::None;
            uint64_t clearSerial = 0;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait_until(lock, nextPoll, [&] {
                    return state->stop || state->diskClearPending || !state->invalidations.empty()
                        || !state->completions.empty() || !state->requests.empty();
                });
                if (state->stop) break;
                if (state->diskClearPending) {
                    state->diskClearPending = false;
                    clearSerial = state->clearSerial;
                    work = Work::Clear;
                } else if (!state->invalidations.empty()) {
                    auto first = state->invalidations.begin();
                    invalidation = *first; state->invalidations.erase(first);
                    work = Work::Invalidate;
                } else if (!state->completions.empty()) {
                    completion = std::move(state->completions.front()); state->completions.pop_front();
                    work = Work::Complete;
                } else if (!state->requests.empty()) {
                    request = std::move(state->requests.front()); state->requests.pop_front();
                    work = Work::Request;
                }
            }
            // 持续有请求时也定期检测其它实例的清理。
            if (Clock::now() >= nextPoll) {
                state->observe(disk.sync());
                nextPoll = Clock::now() + std::chrono::seconds(2);
            }
            switch (work) {
            case Work::Clear: {
                const auto result = disk.clear();
                state->observe(result, true);
                std::lock_guard lock(state->mutex);
                if (!state->stop && clearSerial == state->clearSerial) {
                    state->currentStats.clearState = result.code == CacheCode::Ok ? ClearState::Done : ClearState::Failed;
                    state->statsLocked();
                }
                break;
            }
            case Work::Invalidate: state->observe(disk.erase(invalidation)); break;
            case Work::Complete: processCompletion(state, disk, std::move(completion)); break;
            case Work::Request: processRequest(state, disk, request); break;
            default: break;
            }
        }
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->currentStats.diskAvailable = false;
        if (state->currentStats.clearState == ClearState::Pending) state->currentStats.clearState = ClearState::Failed;
        state->statsLocked();
    }
    workerFinished(state);
}

void stopState(std::shared_ptr<ServiceState> state) {
    if (!state) return;
    std::unique_lock lock(state->mutex);
    state->stop = true;
    ++state->generation;
    state->requests.clear(); state->shellQueue.clear(); state->decodeQueue.clear(); state->completions.clear();
    state->wake.notify_all();
    // 同步 Shell/文件系统调用无法安全强制中断。线程只持有自己的状态与局部变量，
    // 不使用 TerminateThread、不无限 join，也不引用查看器的全局对象。
    state->finished.wait_for(lock, std::chrono::milliseconds(400), [&] { return state->workers == 0; });
}

} // namespace

struct ThumbnailService::Impl {
    std::mutex lifecycle;
    std::atomic<std::shared_ptr<ServiceState>> state;
    std::shared_ptr<std::atomic<uint64_t>> versions = std::make_shared<std::atomic<uint64_t>>(0);
};
ThumbnailService::ThumbnailService() : impl_(std::make_unique<Impl>()) {}
ThumbnailService::~ThumbnailService() { shutdown(); }
ThumbnailService& ThumbnailService::instance() {
    static ThumbnailService service;
    return service;
}
void ThumbnailService::initialize(const std::filesystem::path& cacheFile) {
    std::lock_guard lifecycle(impl_->lifecycle);
    stopState(impl_->state.exchange({}));
    auto state = std::make_shared<ServiceState>(absolutePath(cacheFile), impl_->versions);
    impl_->state.store(state);
    auto launch = [&](auto function) {
        { std::lock_guard lock(state->mutex); ++state->workers; }
        try { std::thread([state, function] { function(state); }).detach(); }
        catch (...) { workerFinished(state); throw; }
    };
    try { launch(runCacheIo); launch(runShell); launch(runDecode); }
    catch (...) {
        stopState(impl_->state.exchange({}));
    }
}
void ThumbnailService::shutdown() {
    std::lock_guard lifecycle(impl_->lifecycle);
    stopState(impl_->state.exchange({}));
}
void ThumbnailService::updateRequests(const std::vector<std::wstring>& visible,
    const std::vector<std::wstring>& prefetch) {
    auto state = impl_->state.load();
    if (!state) return;
    const std::vector<std::wstring> shown(visible.begin(), visible.begin() + std::min(visible.size(), kVisibleLimit));
    const std::vector<std::wstring> ahead(prefetch.begin(), prefetch.begin()
        + (shown.empty() ? 0 : std::min(prefetch.size(), kPrefetchLimit)));
    std::lock_guard lock(state->mutex);
    if (state->stop || (shown == state->lastVisible && ahead == state->lastPrefetch)) return;
    std::unordered_set<std::wstring> previousVisible, seen;
    for (const auto& r : state->desired) if (r.visible) previousVisible.insert(r.key);
    state->lastVisible = shown; state->lastPrefetch = ahead;
    state->desired.clear();
    auto add = [&](const std::vector<std::wstring>& paths, bool isVisible) {
        for (const auto& path : paths) {
            auto key = pathKey(path);
            if (key.empty() || !seen.insert(key).second) continue;
            const bool newlyVisible = isVisible && !previousVisible.contains(key);
            if (newlyVisible) state->confirmed.erase(key); // 重新展示时先核对文件属性再复用 CPU 缓存。
            state->desired.push_back({ path, std::move(key), isVisible, newlyVisible, 0 });
        }
    };
    add(shown, true); add(ahead, false);
    state->requeueLocked(); // 可见列表为空时取消旧请求及预取。
    state->publishLocked();
}
Thumbnail ThumbnailService::get(const std::wstring& path) const {
    auto state = impl_->state.load();
    if (!state) return {};
    const auto snapshot = state->published.load();
    const auto found = snapshot->find(pathKey(path));
    return found == snapshot->end() ? Thumbnail{} : found->second;
}
void ThumbnailService::invalidate(const std::wstring& path) {
    auto state = impl_->state.load();
    const auto key = pathKey(path);
    if (!state || key.empty()) return;
    std::lock_guard lock(state->mutex);
    if (state->stop) return;
    state->memory.erase(key); state->confirmed.erase(key);
    if (state->invalidations.size() < kInvalidationLimit) state->invalidations.insert(key);
    else {
        // 有界溢出时清理整个私有缓存，不丢失编辑通知，也不无限堆积队列。
        state->invalidations.clear(); state->memory.clear(); state->confirmed.clear();
        state->diskClearPending = true; ++state->clearSerial;
        state->currentStats.clearState = ClearState::Pending;
        ++state->currentStats.clearVersion;
        state->statsLocked();
    }
    state->requeueLocked();
    state->publishLocked();
}
void ThumbnailService::clear() {
    auto state = impl_->state.load();
    if (!state) return;
    std::lock_guard lock(state->mutex);
    if (state->stop) return;
    state->memory.clear(); state->confirmed.clear(); state->desired.clear();
    state->lastVisible.clear(); state->lastPrefetch.clear(); state->invalidations.clear();
    state->diskClearPending = true; ++state->clearSerial;
    state->currentStats.clearState = ClearState::Pending;
    ++state->currentStats.clearVersion;
    state->requeueLocked();
    state->publishLocked(); state->statsLocked();
}
bool ThumbnailService::consumeChanged() {
    auto state = impl_->state.load();
    return state && state->changed.exchange(false);
}
Stats ThumbnailService::stats() const {
    auto state = impl_->state.load();
    return state ? *state->publishedStats.load() : Stats{};
}

bool runThumbnailCacheWriter(std::ostream& output, const std::filesystem::path& directory, int writerId) {
    if (writerId < 0 || writerId > 1 || directory.empty()) return false;
    DiskCache disk(directory / L"concurrent.thumbnail");
    const auto initial = disk.sync();
    if (initial.code != CacheCode::Ok) return false;
    const Metadata metadata{ 1, 1, true };
    const auto png = encodeCachePng(cv::Mat(16, 16, CV_8UC4, cv::Scalar(60, 90, 130, 255)));
    for (int n = 0; n < 100; ++n) {
        const auto key = L"writer-" + std::to_wstring(writerId) + L"-" + std::to_wstring(n);
        bool saved = false;
        for (int retry = 0; retry < 20 && !saved; ++retry) {
            saved = disk.put(key, metadata, png, false, initial.epoch).code == CacheCode::Ok;
            if (!saved) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!saved) { output << "FAIL concurrent writer " << writerId << '\n'; return false; }
    }
    output << "PASS concurrent writer " << writerId << '\n';
    return true;
}

bool runThumbnailCacheTests(std::ostream& output, const std::filesystem::path& testDirectory) {
    size_t passed = 0, failed = 0;
    auto check = [&](bool condition, const char* name) {
        output << (condition ? "PASS " : "FAIL ") << name << '\n';
        condition ? ++passed : ++failed;
    };
    try {
        if (testDirectory.empty()) throw std::runtime_error("a test output directory is required");
        const auto root = absolutePath(testDirectory) / (L"thumbnail-tests-" + std::to_wstring(freshEpoch()));
        if (!std::filesystem::create_directories(root)) throw std::runtime_error("cannot create unique test directory");
        output << "Cache test directory: " << utf8(root.wstring()) << '\n';
        const Metadata metadata{ 12345678, 54321, true };
        const auto png = encodeCachePng(cv::Mat(12, 16, CV_8UC4, cv::Scalar(25, 50, 100, 128)));
        check(!png.empty() && !decodeCachePng(png).empty(), "bounded BGRA PNG round trip");
        auto key = [](size_t i) { return L"C:\\thumbnail-test\\image-" + std::to_wstring(i) + L".png"; };
        const auto lruPath = root / L"lru.thumbnail";
        DiskCache first(lruPath);
        auto initial = first.sync();
        check(initial.code == CacheCode::Ok && initial.entries == 0, "create explicit fixed 1000-slot index");
        bool filled = true;
        for (size_t i = 0; i < kSlots; ++i)
            filled &= first.put(key(i), metadata, png, false, initial.epoch).code == CacheCode::Ok;
        check(filled && first.sync().entries == 1000, "persist 1000 records");
        check(first.touch(key(0), metadata, initial.epoch).code == CacheCode::Ok, "small LRU touch");
        {
            DiskCache restarted(lruPath);
            auto reopened = restarted.sync();
            check(reopened.entries == 1000 && reopened.epoch == initial.epoch, "restart preserves index and epoch");
            const auto inserted = restarted.put(key(1000), metadata, png, false, reopened.epoch);
            check(inserted.code == CacheCode::Ok && inserted.entries == 1000
                && restarted.lookup(key(1), metadata, false).code == CacheCode::Miss
                && restarted.lookup(key(0), metadata, false).code == CacheCode::Ok,
                "1001st entry evicts coldest; touched record survives restart");
            // first 仍持有旧快照；下次提交必须重载另一个实例的最新索引再选槽。
            check(first.put(key(1001), metadata, png, false, initial.epoch).code == CacheCode::Ok
                && restarted.lookup(key(1000), metadata, false).code == CacheCode::Ok
                && restarted.lookup(key(1001), metadata, false).code == CacheCode::Ok,
                "stale instance merges current index before commit");
        }
        const auto unicodeKey = pathKey(L"C:\\缩略图测试\\日本語-한국어.png");
        // 不只是两个对象：启动两个测试子进程，同时修改同一份临时缓存。
        std::wstring executable(32768, L'\0');
        const DWORD executableLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        executable.resize(executableLength);
        std::array<PROCESS_INFORMATION, 2> writers{};
        bool concurrentOk = executableLength > 0 && executableLength < 32768;
        for (int i = 0; i < 2 && concurrentOk; ++i) {
            const auto reportFile = root / (L"writer-" + std::to_wstring(i) + L".txt");
            std::wstring command = L"\"" + executable + L"\" --probe --thumbnail-writer " + std::to_wstring(i) +
                L" --out-dir \"" + root.wstring() + L"\" --out \"" + reportFile.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            concurrentOk = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &writers[i]) != FALSE;
        }
        for (auto& process : writers) {
            if (!process.hProcess) { concurrentOk = false; continue; }
            if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
                // 只终止本自检创建的超时子进程，不干预任何已有查看器或 Shell 处理器。
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                concurrentOk = false;
            }
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            concurrentOk &= code == 0;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        DiskCache concurrent(root / L"concurrent.thumbnail");
        const Metadata concurrentMetadata{ 1, 1, true };
        check(concurrentOk && concurrent.sync().entries == 200 &&
            concurrent.lookup(L"writer-0-99", concurrentMetadata, false).code == CacheCode::Ok &&
            concurrent.lookup(L"writer-1-99", concurrentMetadata, false).code == CacheCode::Ok,
            "two processes concurrently commit without losing either writer's entries");
        check(first.put(unicodeKey, metadata, png, false, initial.epoch).code == CacheCode::Ok
            && first.lookup(unicodeKey, metadata, false).png == png, "UTF-8 path payload and checksum");

        const auto corruptPath = root / L"corrupt.thumbnail";
        DiskCache corrupt(corruptPath);
        const auto corruptEpoch = corrupt.sync().epoch;
        corrupt.put(key(0), metadata, png, false, corruptEpoch);
        corrupt.put(key(1), metadata, png, false, corruptEpoch);
        {
            File file(corruptPath, OPEN_EXISTING);
            std::array<unsigned char, kIndexBytes> index{};
            file.read(kHeaderBytes, index.data(), index.size());
            put32(index.data() + 8, UINT32_MAX); // 人为构造 CRC 正确但长度非法的索引。
            put32(index.data() + 52, crc32(index.data(), 52));
            file.write(kHeaderBytes, index.data(), index.size()); file.flush();
        }
        {
            DiskCache reopened(corruptPath);
            check(reopened.lookup(key(0), metadata, false).code == CacheCode::Miss
                && reopened.lookup(key(1), metadata, false).code == CacheCode::Ok,
                "malformed record length drops only the bad slot");
        }
        {
            File file(corruptPath, OPEN_EXISTING);
            file.seek(file.size() - 5);
            if (!SetEndOfFile(file.handle)) throw IoFailure();
            file.flush();
        }
        {
            DiskCache reopened(corruptPath);
            check(reopened.sync().entries == 0, "truncated payload rejected before allocation/read");
        }
        {
            File file(corruptPath, OPEN_EXISTING);
            const unsigned char badMagic = 0;
            file.write(0, &badMagic, 1); file.flush();
        }
        {
            DiskCache reopened(corruptPath);
            const auto rebuilt = reopened.sync();
            check(rebuilt.code == CacheCode::Ok && rebuilt.epoch != corruptEpoch && rebuilt.entries == 0,
                "bad header rebuilt with a new epoch");
            reopened.put(key(0), metadata, png, false, rebuilt.epoch);
            {
                File file(corruptPath, OPEN_EXISTING);
                unsigned char damaged = 0;
                file.read(kDataStart, &damaged, 1); damaged ^= 1;
                file.write(kDataStart, &damaged, 1); file.flush();
            }
            check(reopened.lookup(key(0), metadata, false).code == CacheCode::Miss,
                "payload checksum catches silent corruption");
        }
        Bytes hugePng = png;
        if (hugePng.size() > 20) hugePng[16] = 127;
        check(decodeCachePng(hugePng).empty(), "PNG dimension bound checked before decoder");

        const auto epochPath = root / L"epoch.thumbnail";
        DiskCache epochA(epochPath), epochB(epochPath);
        const auto beforeClear = epochA.sync();
        epochB.sync();
        epochA.put(key(0), metadata, png, false, beforeClear.epoch);
        const auto afterClear = epochA.clear();
        check(afterClear.code == CacheCode::Ok && afterClear.epoch != beforeClear.epoch && afterClear.entries == 0,
            "clear atomically persists a new epoch");
        check(epochB.put(key(1), metadata, png, false, beforeClear.epoch).code == CacheCode::StaleEpoch
            && epochA.sync().entries == 0, "old in-flight extraction cannot refill after foreign clear");
        check(epochB.put(key(1), metadata, png, false, afterClear.epoch).code == CacheCode::Ok,
            "new-epoch extraction can commit");

        const auto source = root / L"metadata-source.bin";
        { std::ofstream out(source, std::ios::binary); out << "first"; }
        const auto beforeMetadata = sourceMetadata(source.wstring());
        const auto sourceKey = pathKey(source.wstring());
        epochA.put(sourceKey, beforeMetadata, png, false, afterClear.epoch);
        { std::ofstream out(source, std::ios::binary | std::ios::trunc); out << "changed source size"; }
        const auto afterMetadata = sourceMetadata(source.wstring());
        check(beforeMetadata.exists && beforeMetadata != afterMetadata
            && epochA.lookup(sourceKey, afterMetadata, false).code == CacheCode::Miss,
            "real source metadata change invalidates cache without decoding source");
        check(epochA.put(sourceKey, afterMetadata, {}, true, afterClear.epoch).code == CacheCode::Ok
            && epochB.lookup(sourceKey, afterMetadata, false).failed, "failed placeholder persists and reloads");
        Metadata changedTime = afterMetadata; ++changedTime.mtime;
        check(epochA.lookup(sourceKey, changedTime, false).code == CacheCode::Miss,
            "mtime-only change invalidates failed placeholder");

        cv::Mat noise(128, 128, CV_8UC4);
        uint32_t random = 1234567;
        for (size_t i = 0; i < noise.total() * noise.elemSize(); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            noise.data[i] = static_cast<unsigned char>(random);
        }
        const auto largePng = encodeCachePng(noise);
        CacheLimits smallLimits{ kDataStart + (largePng.size() + 128) * 3, kDataStart + 2000 };
        DiskCache compacted(root / L"compact.thumbnail", smallLimits);
        const auto compactEpoch = compacted.sync().epoch;
        compacted.put(key(99), metadata, largePng, false, compactEpoch);
        bool bounded = !largePng.empty();
        for (int i = 0; i < 12; ++i) {
            const auto r = compacted.put(key(0), metadata, largePng, false, compactEpoch);
            bounded &= r.code == CacheCode::Ok && r.bytes <= smallLimits.fileBytes;
        }
        check(bounded && compacted.compactions() > 0
            && compacted.lookup(key(99), metadata, false).png == largePng,
            "hole compaction atomically retains live blobs and respects byte cap");
        for (int i = 0; i < 10; ++i) {
            const auto r = compacted.put(key(i), metadata, largePng, false, compactEpoch);
            bounded &= r.code == CacheCode::Ok && r.bytes <= smallLimits.fileBytes;
        }
        check(bounded && compacted.sync().entries <= 3
            && compacted.lookup(key(9), metadata, false).code == CacheCode::Ok,
            "byte quota evicts LRU even before 1000 slots are occupied");
        {
            DiskCache reopened(root / L"compact.thumbnail", smallLimits);
            check(reopened.lookup(key(9), metadata, false).png == largePng, "compacted file survives restart");
        }

        const auto blockedParent = root / L"not-a-directory";
        { std::ofstream file(blockedParent); file << 'x'; }
        DiskCache unavailable(blockedParent / L"cache.thumbnail");
        check(unavailable.sync().code == CacheCode::Unavailable
            && unavailable.clear().code == CacheCode::Unavailable,
            "unwritable path reports unavailable and clear failure");
        const auto readOnlyPath = root / L"readonly.thumbnail";
        DiskCache readOnly(readOnlyPath);
        readOnly.sync();
        if (!SetFileAttributesW(readOnlyPath.c_str(), FILE_ATTRIBUTE_READONLY)) throw IoFailure();
        const bool readonlyRejected = readOnly.sync().code == CacheCode::Unavailable
            && readOnly.clear().code == CacheCode::Unavailable;
        SetFileAttributesW(readOnlyPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        check(readonlyRejected, "existing read-only cache is not silently replaced on clear");

        // 用刻意卡住的模拟 Shell 线程验证真实缓存线程仍可工作，不依赖处理器或应用实例。
        const auto workerPath = root / L"workers.thumbnail";
        DiskCache workerSeed(workerPath);
        auto seed = workerSeed.sync();
        workerSeed.put(sourceKey, afterMetadata, png, false, seed.epoch);
        auto versions = std::make_shared<std::atomic<uint64_t>>(0);
        auto state = std::make_shared<ServiceState>(workerPath, versions);
        auto releaseSlowWorker = std::make_shared<std::promise<void>>();
        auto slowFuture = releaseSlowWorker->get_future();
        state->workers = 2;
        std::thread([state] { runCacheIo(state); }).detach();
        std::thread([state, future = std::move(slowFuture)]() mutable {
            future.wait(); workerFinished(state);
        }).detach();
        auto waitFor = [](auto predicate, std::chrono::milliseconds timeout) {
            const auto deadline = Clock::now() + timeout;
            do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            while (Clock::now() < deadline);
            return predicate();
        };
        {
            std::lock_guard lock(state->mutex);
            state->desired.push_back({ source.wstring(), sourceKey, true, true, 0 });
            state->requeueLocked();
        }
        const bool cacheProgress = waitFor([&] { return state->published.load()->contains(sourceKey); }, std::chrono::seconds(3));
        check(cacheProgress, "cache hit proceeds while Shell worker is blocked");
        {
            std::lock_guard lock(state->mutex);
            state->desired.clear(); state->memory.clear(); state->confirmed.clear();
            state->diskClearPending = true; ++state->clearSerial;
            state->currentStats.clearState = ClearState::Pending;
            state->requeueLocked(); state->publishLocked(); state->statsLocked();
        }
        check(waitFor([&] { return state->publishedStats.load()->clearState == ClearState::Done; }, std::chrono::seconds(3))
            && workerSeed.sync().entries == 0, "disk clear proceeds while Shell worker is blocked");
        const auto shutdownStart = Clock::now();
        stopState(state);
        check(Clock::now() - shutdownStart < std::chrono::milliseconds(1000), "bounded shutdown with a stuck worker");
        releaseSlowWorker->set_value();
        check(waitFor([&] { std::lock_guard lock(state->mutex); return state->workers == 0; }, std::chrono::seconds(2)),
            "self-owned worker state completes safely after shutdown");
        Request oldRequest{ source.wstring(), sourceKey, true, true, 1 };
        acceptImage(state, workerSeed, oldRequest, afterMetadata, decodeCachePng(png), false, false, seed.epoch);
        check(state->published.load()->empty() && workerSeed.sync().entries == 0,
            "stopped/old-generation result cannot publish or persist");

        // —— Shell 失败后的本地解码兜底：预览带不依赖任何已注册的 Shell 处理器 ——
        check(localDecodeEligible(L"C:\\t\\a.psd") && localDecodeEligible(L"C:\\t\\a.CR2")
            && localDecodeEligible(L"C:\\t\\a.mp4") && !localDecodeEligible(L"C:\\t\\a.xyz")
            && !localDecodeEligible(L"C:\\t\\noextension"),
            "local decode eligibility by extension");

        const auto localSource = root / L"local-source.png";
        {
            cv::Mat gradient(200, 300, CV_8UC3);
            for (int y = 0; y < gradient.rows; ++y)
                for (int x = 0; x < gradient.cols; ++x)
                    gradient.at<cv::Vec3b>(y, x) = cv::Vec3b(
                        static_cast<uchar>(x), static_cast<uchar>(y), static_cast<uchar>(64));
            std::vector<uchar> pngBytes;
            cv::imencode(".png", gradient, pngBytes);
            std::ofstream out(localSource, std::ios::binary);
            out.write(reinterpret_cast<const char*>(pngBytes.data()), static_cast<std::streamsize>(pngBytes.size()));
        }
        {
            ImageDatabase database;
            const auto image = decodeLocalThumbnail(database, localSource.wstring());
            check(!image.empty() && image.type() == CV_8UC4 && image.cols == 256 && image.rows == 171,
                "local decode shrinks to 256-edge BGRA thumbnail");
        }

        // 全链路：模拟 Shell 全部提取失败（等同未安装 JarkThumbnailProvider.dll）。
        const auto fallbackPath = root / L"fallback.thumbnail";
        auto fallbackVersions = std::make_shared<std::atomic<uint64_t>>(0);
        auto fallbackState = std::make_shared<ServiceState>(fallbackPath, fallbackVersions);
        fallbackState->workers = 3;
        std::thread([state = fallbackState] { runCacheIo(state); }).detach();
        std::thread([state = fallbackState] { runDecode(state); }).detach();
        std::thread([state = fallbackState] {
            for (;;) {
                ShellTask task;
                {
                    std::unique_lock lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->stop || !state->shellQueue.empty(); });
                    if (state->stop) break;
                    task = std::move(state->shellQueue.front());
                    state->shellQueue.pop_front();
                    if (!state->currentLocked(task.request)) continue;
                }
                Completion result;
                result.task = task;
                result.failed = true; // 永远模拟提取失败
                std::lock_guard lock(state->mutex);
                if (state->currentLocked(task.request) && state->completions.size() < kQueueLimit)
                    state->completions.push_back(std::move(result));
                state->wake.notify_all();
            }
            workerFinished(state);
        }).detach();

        const auto brokenSource = root / L"broken.psd";
        { std::ofstream out(brokenSource, std::ios::binary); out << "not a real psd"; }
        const auto localKey = pathKey(localSource.wstring());
        const auto brokenKey = pathKey(brokenSource.wstring());
        {
            std::lock_guard lock(fallbackState->mutex);
            fallbackState->desired.push_back({ localSource.wstring(), localKey, true, true, 0 });
            fallbackState->desired.push_back({ brokenSource.wstring(), brokenKey, true, true, 0 });
            fallbackState->requeueLocked();
        }
        auto publishedOf = [&](const std::wstring& key) -> Thumbnail {
            const auto snapshot = fallbackState->published.load();
            const auto found = snapshot->find(key);
            return found == snapshot->end() ? Thumbnail{} : found->second;
        };
        bool fallbackOk = false;
        waitFor([&] {
            const auto t = publishedOf(localKey);
            fallbackOk = !t.failed && !t.image.empty();
            return fallbackOk || t.failed;
        }, std::chrono::seconds(10));
        const auto localThumb = publishedOf(localKey);
        check(fallbackOk && localThumb.image.type() == CV_8UC4
            && (std::max)(localThumb.image.cols, localThumb.image.rows) == int(kEdge),
            "shell failure falls back to local decode and publishes thumbnail");
        check(waitFor([&] { return publishedOf(brokenKey).failed; }, std::chrono::seconds(10)),
            "undecodable file degrades to placeholder after fallback");
        DiskCache fallbackDisk(fallbackPath);
        const auto localMetadata = sourceMetadata(localSource.wstring());
        check(waitFor([&] {
            return fallbackDisk.lookup(localKey, localMetadata, false).code == CacheCode::Ok;
        }, std::chrono::seconds(5)), "locally decoded thumbnail persists to disk cache");
        check(fallbackDisk.lookup(brokenKey, sourceMetadata(brokenSource.wstring()), false).code == CacheCode::Miss,
            "failed fallback stays memory-only");
        stopState(fallbackState);
    } catch (const std::exception& error) {
        output << "FAIL exception: " << error.what() << '\n'; ++failed;
    } catch (...) { output << "FAIL unknown exception\n"; ++failed; }
    output << "Thumbnail cache tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0;
}

bool probeShellThumbnail(const std::wstring& path, const std::filesystem::path& pngOutput,
    std::ostream& output) {
    try {
        auto promise = std::make_shared<std::promise<ShellResult>>();
        auto future = promise->get_future();
        std::thread([promise, path] {
            ShellResult result;
            try { ShellSession shell; result = shell.extract(path); }
            catch (...) { result.extracted = E_FAIL; }
            promise->set_value(std::move(result));
        }).detach();
        if (future.wait_for(std::chrono::seconds(12)) != std::future_status::ready) {
            output << "Shell thumbnail timed out (12 s); worker remains self-owned, no output file written.\n";
            return false;
        }
        auto result = future.get();
        output << "Shell INCACHEONLY HRESULT=0x" << std::hex << uint32_t(result.cached)
            << " EXTRACT HRESULT=0x" << uint32_t(result.extracted) << std::dec
            << " cacheHit=" << result.cacheHit << '\n';
        if (result.image.empty()) { output << "No Shell thumbnail; service would cache a failure placeholder.\n"; return false; }
        if (!pngOutput.empty()) {
            const auto png = encodeCachePng(result.image);
            if (png.empty()) return false;
            std::ofstream file(pngOutput, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            file.close();
            if (!file) { output << "Cannot write probe PNG.\n"; return false; }
        }
        output << "Shell thumbnail: " << result.image.cols << 'x' << result.image.rows << " BGRA, straight alpha\n";
        return true;
    } catch (const std::exception& error) { output << "Shell probe failed: " << error.what() << '\n'; return false; }
}
} // namespace jark
