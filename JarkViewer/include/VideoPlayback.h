#pragma once

// 单视频播放器的引擎与状态机（**不依赖窗口**）。
//
// 界面（VideoPlayerApp）只做三件事：把输入翻译成这里的调用、把这里的当前帧画到画布、
// 按这里的状态画底部条带。`--probe --video-test` 驱动的也是这一份逻辑，
// 所以自检里过的语义就是界面里跑的语义。
//
// 与看图那条路的区别：整文件内存映射（不把视频读进内存、没有 256 MiB 上限），
// 播放/暂停/精确 seek/单帧步进/音量都由这里管，不碰 ImageDatabase / 缩略图 / EXIF。

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/opencv.hpp>

namespace jark {

class MediaPlayer;
class MappedFileReader;

class VideoPlayback {
public:
    // 打开失败的种类（界面按它选 InfoScreen 占位画面）
    enum class Error {
        None,
        FileMissing,   // 打不开：不存在、被独占、映射失败
        DecodeFailed,  // 打开了但没有可解的视频流
    };

    VideoPlayback();
    ~VideoPlayback();

    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;

    // 打开文件并立即开始播放（音量见 setVolumePercent，默认 50%）
    bool open(const std::wstring& path);
    void close();

    bool isOpen() const noexcept { return player_ != nullptr && error_ == Error::None; }
    Error error() const noexcept { return error_; }
    const std::wstring& fileName() const noexcept { return fileName_; }

    // —— 取画面 ——
    // 每帧调用一次；返回 true 表示这一帧与上一帧不同（调用方需要重绘）
    bool takeFrame(cv::Mat& frame);

    // —— 播放控制 ——
    bool isPlaying() const noexcept;   // 正在播（未暂停、未停在结尾）
    bool isPaused() const noexcept;
    bool atEnd() const noexcept { return atEnd_; }
    void togglePlay();                 // 空格：播放/暂停；结尾暂停态下 = 从头重播
    void pause();
    void resume();

    void seekTo(int64_t ms);           // 跳转（保持跳前的播放/暂停状态）
    void nudge(int64_t deltaMs);       // ±5 秒
    void stepFrame(int direction);     // 暂停中单帧前进/后退

    // —— 拖动进度条 ——
    // 按下/移动/松手。移动期间只出关键帧预览（节流，只保留最新目标），
    // 松手才做精确跳转；拖动前在播放的话，等精确落位之后再继续出声。
    void beginScrub(int64_t ms);
    void updateScrub(int64_t ms);
    void endScrub(int64_t ms);
    bool isScrubbing() const noexcept { return scrubbing_; }
    // 拖动中把手该停在哪（没在拖动时就是播放位置）
    int64_t scrubTargetMs() const noexcept { return scrubbing_ ? scrubTargetMs_ : positionMs(); }

    // —— 音量：0~100，5 一档，初始 50。不落盘（每次启动都是 50）——
    int volumePercent() const noexcept { return volumePercent_; }
    void adjustVolume(int deltaPercent);

    // —— 只读信息（条带绘制用）——
    int64_t positionMs() const noexcept;
    // 当前已显示帧的时间戳（自检里用它判断"落位到没到"）
    int64_t framePtsMs() const noexcept;
    int64_t durationMs() const noexcept { return durationMs_; }
    bool hasKnownDuration() const noexcept { return durationMs_ > 0; }
    int videoWidth() const noexcept { return videoWidth_; }
    int videoHeight() const noexcept { return videoHeight_; }
    bool hasAudio() const noexcept { return hasAudio_; }

    static constexpr int kDefaultVolumePercent = 50;
    static constexpr int kVolumeStepPercent = 5;
    static constexpr int64_t kNudgeMs = 5000;

private:
    // 拖动中预览的最小间隔：比这更密没有意义（一次关键帧预览本身就要几十毫秒），
    // 密了只会让鼠标事件排队，松手后画面还在后面追
    static constexpr int64_t kPreviewIntervalMs = 60;

    void updateAtEnd();

    std::wstring path_;
    std::wstring fileName_;
    std::shared_ptr<MappedFileReader> mapping_;   // 必须活得比 player_ 久（解码器只持指针）
    std::unique_ptr<MediaPlayer> player_;
    Error error_ = Error::None;

    int64_t durationMs_ = 0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    bool hasAudio_ = false;

    int volumePercent_ = kDefaultVolumePercent;
    bool atEnd_ = false;

    bool scrubbing_ = false;
    int64_t scrubTargetMs_ = 0;
    int64_t lastPreviewAtMs_ = 0;      // 上次发出预览的时刻（只用来节流）
    bool pendingPreview_ = false;      // 被节流挡下的目标还没发出去（见 takeFrame 里的补发）
    bool resumeAfterScrub_ = false;    // 拖动前在播放：等精确落位后继续出声

    // 松手后的"等落位"：以**画面已经换成目标附近的那一帧**为准（pts 判定），
    // 不能只看"落点在途"那个标志——它由解码线程置位，很可能在本帧取帧之前
    // 就已经被消费掉了，那样会一直等到超时才恢复播放
    int64_t landingMinPtsMs_ = -1;     // >= 0 表示还没落位（值 = 判定阈值）
    int64_t landingWaitStartedMs_ = 0; // 兜底：落点迟迟不来（目标附近没帧）也要恢复播放
};

} // namespace jark
