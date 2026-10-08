#pragma once

#include <opencv2/core/mat.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace jark {

enum class ClearState { Idle, Pending, Done, Failed };

struct Thumbnail {
    // 自顶向下排列的 CV_8UC4 BGRA，使用直通 alpha。浅拷贝共享只读像素，
    // 调用方不得原地修改；需要可写图像时先 clone()。
    cv::Mat image;
    uint64_t version = 0;
    bool failed = false;
};

struct Stats {
    // 本程序持久缓存的记录数和文件总字节数，不包含 Windows 系统缩略图缓存。
    size_t entries = 0;
    uint64_t bytes = 0;
    bool diskAvailable = true;
    ClearState clearState = ClearState::Idle;
    // 本地清理立即递增；外部清理 epoch 变化也递增，普通缩略图完成不变。
    uint64_t clearVersion = 0;
};

class ThumbnailService {
public:
    // 同时支持 jark::Stats 和 ThumbnailService::Stats 等两种写法。
    using Thumbnail = jark::Thumbnail;
    using Stats = jark::Stats;
    using ClearState = jark::ClearState;

    static ThumbnailService& instance();
    // 传入 GlobalVar::settingPath.parent_path() / L"JarkViewer.thumbnail"。
    // 服务不访问查看器全局变量、应用的图像缓存、窗口句柄或 GPU 对象；
    // Shell 提取失败时（无处理器、未安装 JarkThumbnailProvider.dll 等）由独立
    // 解码线程用私有 ImageDatabase 实例兜底，预览带不依赖任何 Shell 处理器注册。
    void initialize(const std::filesystem::path& cacheFile);
    // 最多等待工作线程 400 ms；同步 Shell 调用阻塞时由线程自持状态，
    // 返回后的旧结果不会发布或发起新写入。已开始的磁盘事务可能稍后结束。
    void shutdown();
    void updateRequests(const std::vector<std::wstring>& visible,
        const std::vector<std::wstring>& prefetch = {});
    // 纯内存快照查询，不做文件/COM I/O，也不获取工作队列互斥锁。
    Thumbnail get(const std::wstring& path) const;
    void invalidate(const std::wstring& path);
    void clear();
    bool consumeChanged();
    Stats stats() const;

    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;

private:
    ThumbnailService();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 只操作 testDirectory 下新建的唯一子目录，保留文件便于排障。
// 兜底解码自检会解码测试目录内自建的测试图，但不触碰应用实例的缓存。
bool runThumbnailCacheTests(std::ostream& output,
    const std::filesystem::path& testDirectory);
// 仅供上述自检启动的两个子进程，共享临时目录验证真实跨进程写入。
bool runThumbnailCacheWriter(std::ostream& output,
    const std::filesystem::path& testDirectory, int writerId);
// 最多等待 Shell 12 秒；只有成功取到缩略图才写 PNG。
bool probeShellThumbnail(const std::wstring& path,
    const std::filesystem::path& pngOutput, std::ostream& output);

} // namespace jark
