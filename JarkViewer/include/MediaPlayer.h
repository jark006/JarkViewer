#pragma once

// 实时媒体播放器：以音频播放位置为主时钟驱动视频帧，一次播完。
// 面向实况照片/视频预览，按需求不提供暂停与逐帧控制。

#include <cstdint>
#include <memory>
#include <span>

#include <opencv2/opencv.hpp>

namespace jark {

struct MediaInfo;

// 内存中的视频源（实况照片提取出的视频、或直接打开的视频文件）
// 单独定义便于 ImageAsset 只持有 shared_ptr 而不依赖 ffmpeg 头文件。
struct VideoSource {
    std::vector<uint8_t> data;
};

class MediaPlayer {
public:
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    static std::unique_ptr<MediaPlayer> create();

    // 开始播放（数据需在播放期间保持有效由内部拷贝保证）
    bool start(std::span<const uint8_t> data, float volume = 1.0f);

    void stop();

    bool isRunning() const noexcept;
    bool hasFinished() const noexcept;

    // 视频帧的显示尺寸（未处理旋转前的解码尺寸）
    bool getVideoSize(int& width, int& height) const noexcept;

    bool hasAudio() const noexcept;

    // 取当前应当显示的最新帧；返回 false 表示沿用上一帧
    bool acquireFrame(cv::Mat& frame);

    // 已播放时长（毫秒）
    int64_t positionMs() const noexcept;

    // 音量 0.0~1.0
    void setVolume(float volume) noexcept;

private:
    MediaPlayer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jark
