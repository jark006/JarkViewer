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

    // 已提交但尚未播完的缓冲区个数。XAudio2 对单个 source voice 最多只接受
    // XAUDIO2_MAX_QUEUED_BUFFERS(64) 个排队缓冲区，再多 SubmitSourceBuffer 直接失败
    // （XAUDIO2_E_INVALID_CALL）——按"缓冲区个数"限流才拦得住，按样本数拦不住：
    // 一个 21ms 的音频批 × 64 就已经到顶了。调用会顺手回收已播完的缓冲区记录。
    // 只能由提交音频的那个线程调用（与 submit 同源）。
    size_t queuedBuffers();

    // 音量 0.0 ~ 1.0
    void setVolume(float volume) noexcept;

    // 暂停：voice Stop() 保留当前采样位置与已排队的缓冲，恢复时从原位继续。
    // **不能**丢掉队列重来——那样"暂停再继续"会有一段静音空洞，反复暂停还会累积偏移。
    void pause() noexcept;
    void resume() noexcept;

    // 丢弃已排队的音频（seek 用）：保留"是否在播放"的意图，之后提交的样本照常播。
    // 只能由提交音频的那个线程调用（与 submit 同源：它要回收被冲掉的缓冲区）。
    void flush() noexcept;

    void stop() noexcept;

    // 最多缓存 2 秒音频，避免解码过快占用内存
    static constexpr int64_t kMaxQueuedFrames = 96000;

    // 排队缓冲区个数上限，留给 64 的硬限制一段余量（提交失败会整段声音丢失）
    static constexpr size_t kMaxQueuedBuffers = 48;

private:
    AudioOutput();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jark
