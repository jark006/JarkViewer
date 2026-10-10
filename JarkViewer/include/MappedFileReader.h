#pragma once

// 整文件只读内存映射：给需要"整段数据在手"的解码器用（ffmpeg 的自定义 AVIO、文件头嗅探）。
//
// 为什么不用 std::vector 读进来：映射是按需分页的，几个 GB 的视频不会真占掉那么多内存，
// 也不需要那一次全量拷贝；对播放器来说 seek 更是只移动一个指针（见 MediaDecoder::seek）。
//
// 生命周期由调用方掌管：`MediaDecoder::open()` 只持有 span 的指针、不拷贝数据，
// 所以映射必须活得比解码器久（播放器里是 shared_ptr 持有）。

#include <cstdint>
#include <span>
#include <string_view>

#include <windows.h>

namespace jark {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept {
        return handle_;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle == handle_) {
            return;
        }
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    explicit operator bool() const noexcept {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = nullptr;
};

class MappedFileReader {
public:
    explicit MappedFileReader(std::wstring_view path) {
        std::wstring filePath(path);
        hFile.reset(CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));

        if (!hFile)
            return;

        hMapping.reset(CreateFileMappingW(hFile.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!hMapping)
            return;

        void* view = MapViewOfFile(hMapping.get(), FILE_MAP_READ, 0, 0, 0);
        if (!view)
            return;

        LARGE_INTEGER size;
        if (!GetFileSizeEx(hFile.get(), &size)) {
            UnmapViewOfFile(view);
            return;
        }

        data_ = static_cast<const uint8_t*>(view);
        size_ = static_cast<size_t>(size.QuadPart);
    }

    ~MappedFileReader() {
        if (data_) UnmapViewOfFile(const_cast<void*>(static_cast<const void*>(data_)));
    }

    MappedFileReader(const MappedFileReader&) = delete;
    MappedFileReader& operator=(const MappedFileReader&) = delete;

    [[nodiscard]] std::span<const uint8_t> view() const noexcept {
        return { data_, size_ };
    }

    // 映射没成功（文件不存在、被独占、映射失败）
    bool isOpen() const noexcept {
        return data_ != nullptr;
    }

    bool isEmpty() const noexcept {
        return data_ == nullptr || size_ < 16;
    }

    size_t size() const noexcept {
        return size_;
    }

private:
    UniqueHandle hFile;
    UniqueHandle hMapping;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace jark
