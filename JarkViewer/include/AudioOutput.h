#pragma once

// XAudio2 音频输出：接收 48kHz 立体声 16 位 PCM。
// 同时提供播放时钟（已播放样本数），供视频帧同步使用。

#include <cstdint>
#include <memory>
#include <span>

namespace jark {

class AudioOutput {
public:
    ~AudioOutput();

    // 打开默认输出设备；失败返回 nullptr（此时仍可无声播放）
    static std::unique_ptr<AudioOutput> create(int sampleRate = 48000, int channels = 2);

    // 提交一批交错样本（内部拷贝一份排队）；失败返回 false
    bool submit(std::span<const int16_t> samples);

    // 已播放的样本帧数（以每声道一个样本为一帧），作为播放时钟
    int64_t playedFrames() const noexcept;

    // 已提交但尚未播完的样本帧数
    int64_t queuedFrames() const noexcept;

    // 音量 0.0 ~ 1.0
    void setVolume(float volume) noexcept;

    void stop() noexcept;

    // 最多缓存 2 秒音频，避免解码过快占用内存
    static constexpr int64_t kMaxQueuedFrames = 96000;

private:
    AudioOutput();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jark
