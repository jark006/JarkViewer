#pragma once

// 单媒体播放器的引擎与状态机（**不依赖窗口**）。
//
// 界面（VideoPlayerApp）只做三件事：把输入翻译成这里的调用、把这里的当前帧画到画布、
// 按这里的状态画底部条带。`--probe --video-test` 驱动的也是这一份逻辑，
// 所以自检里过的语义就是界面里跑的语义。
//
// 与看图那条路的区别：整文件内存映射（不把媒体读进内存、没有 256 MiB 上限），
// 播放/暂停/精确 seek/单帧步进/音量都由这里管，不碰 ImageDatabase / 缩略图 / EXIF。
//
// 纯音频文件（mp3/flac/wav…）也走这里：没有视频轨时 hasVideo() 为假、取不到帧，
// 界面改画音频占位画面，其余（时钟、暂停、seek、音量、拖动）完全一样。

#include <cstdint>
#include <memory>
#include <span>
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
        DecodeFailed,  // 打开了但没有可解的音视频流
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

    // —— 信息面板（界面按 I / Tab 打开）——
    // 打开文件时就用一个一次性解码器把容器/流信息读出来，纯文本交给界面画；
    // 打不开时为空串（失败占位画面没有信息可给）
    const std::string& infoText() const noexcept { return infoText_; }

    // —— 取画面 ——
    // 每帧调用一次；返回 true 表示这一帧与上一帧不同（调用方需要重绘）。
    // 纯音频文件永远返回 false（没有帧可取），但它同时还负责补发拖动预览、恢复播放、
    // 判结尾——所以**每帧都必须调**，不能因为"上次没帧"就跳过。
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
    // 暂停中单帧前进/后退；纯音频没有帧，退化成 ±kNudgeMs（键位语义因此不用改）
    void stepFrame(int direction);

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
    // 有没有视频轨：为假时界面画音频占位画面（纯音频文件，或只有音轨的容器）
    bool hasVideo() const noexcept { return hasVideo_; }

    // —— 实时频谱（音频画面那列跳动的条）——
    // 打开纯音频文件时自动开启；取的是"当前播放位置处"的那一帧，不是最新算出来的
    // （解码线程跑在播放前面约 2 秒，用最新那帧会早出一两秒）。
    // 定义在 .cpp 里：这里只有 MediaPlayer 的前置声明，内联写会用到不完整类型
    bool readSpectrum(std::span<float> out) const noexcept;

    static constexpr int kDefaultVolumePercent = 50;
    static constexpr int kVolumeStepPercent = 5;
    static constexpr int64_t kNudgeMs = 5000;

private:
    // 拖动中预览的最小间隔：比这更密没有意义（一次关键帧预览本身就要几十毫秒），
    // 密了只会让鼠标事件排队，松手后画面还在后面追
    static constexpr int64_t kPreviewIntervalMs = 60;

    void updateAtEnd();
    std::string buildInfoText() const;

    std::wstring path_;
    std::wstring fileName_;
    std::string infoText_;
    std::shared_ptr<MappedFileReader> mapping_;   // 必须活得比 player_ 久（解码器只持指针）
    std::unique_ptr<MediaPlayer> player_;
    Error error_ = Error::None;

    int64_t durationMs_ = 0;
    int videoWidth_ = 0;
    int videoHeight_ = 0;
    bool hasAudio_ = false;
    bool hasVideo_ = false;

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
