#pragma once

// 实时媒体播放器：以音频播放位置为主时钟驱动视频帧。
// 面向实况照片（一次播完，只用 start/stop/acquireFrame/hasFinished）与
// 独立播放器（暂停、seek、单帧步进、音量，见下半部分的控制接口）。
// 只有音轨的文件（mp3/flac/wav…）同样是合法的输入：没有视频帧可取，时钟与声音照常。

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <opencv2/opencv.hpp>

namespace jark {

struct MediaInfo;

// 内存中的视频源（实况照片提取出的视频、或直接打开的视频文件）
// 单独定义便于 ImageAsset 只持有 shared_ptr 而不依赖 ffmpeg 头文件。
struct VideoSource {
    std::vector<uint8_t> data;
    // 原始容器扩展名（mp4/mov…），只用于「导出视频」时的默认落盘格式；
    // 数据本身与它无关，播放器一律当内存字节流解复用。
    std::wstring extension;
};

class MediaPlayer {
public:
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    static std::unique_ptr<MediaPlayer> create();

    // 开始播放。**不拷贝数据**：data 指向的内存必须活到 stop() / 析构
    // （播放器窗口用 MappedFileReader 的整文件映射，实况照片用 ImageAsset::videoSource 的字节）
    bool start(std::span<const uint8_t> data, float volume = 1.0f);

    void stop();

    bool isRunning() const noexcept;
    bool hasFinished() const noexcept;

    // 视频帧的显示尺寸（已按显示旋转，与 acquireFrame 交出来的帧一致）
    bool getVideoSize(int& width, int& height) const noexcept;

    bool hasAudio() const noexcept;

    // 取当前应当显示的最新帧；返回 false 表示沿用上一帧
    bool acquireFrame(cv::Mat& frame);

    // 已播放时长（毫秒）
    int64_t positionMs() const noexcept;

    // 音量 0.0~1.0
    void setVolume(float volume) noexcept;

    // —— 实时频谱（只有纯音频的播放器画面会用到）——
    // 开着时音频线程顺带对解码出的样本做 FFT（约几毫秒/秒的代价）；
    // 关着时 readSpectrum 一律返回 false。**不影响出声**，与音量无关。
    void setSpectrumEnabled(bool enabled) noexcept;
    // 取"当前播放位置处"那一帧频谱（每段 0~1）。没有音频、没开启、还没有数据时返回 false
    bool readSpectrum(std::span<float> out) const noexcept;

    // —— 播放控制（独立播放器用；实况照片那条路只用上面那组）——

    // 暂停/恢复：声卡 voice 停在原地（位置保留），时钟跟着停，队列不动
    void pause();
    void resume();
    bool isPaused() const noexcept;

    // seek 的落点方式
    enum class SeekMode {
        // 精确落点：从关键帧向前解码到目标（跳转、±5s、单帧步进、拖动松手）
        Exact,
        // 只跳到目标之前的关键帧：拖动中的预览。快（不动解码器往前补），
        // 但画面与把手最多差一个 GOP——这是所有播放器 scrub 的固有现象
        KeyFrame,
    };

    // 请求跳到指定位置（毫秒）。**异步**：命令交给解码线程执行，两个解码器各自
    // 定位到关键帧（Exact 时视频侧丢掉 PTS 早于目标的帧、音频侧裁掉目标之前的样本，
    // 所以落点是精确的）。拖动进度条时连续调用是安全的：在途的旧目标会被最新一次
    // 覆盖，不做累积。
    // 目标会被夹到 [0, 时长]，时长未知时不夹上界。
    bool seek(int64_t ms, SeekMode mode = SeekMode::Exact);

    // 上一次精确 seek 的那一帧还没交出来（acquireFrame 交出落点帧后清掉）。
    // 拖动松手后要等它落位再恢复播放，否则会先按错位置出一小段声
    bool seekLandingPending() const noexcept;

    // 暂停中单帧前进/后退（direction = +1 / -1）：以当前帧时间戳移一帧，
    // 内部走同一条精确 seek 路径。播放中调用会先暂停。
    bool stepFrame(int direction);

    // 视频总时长；未知（<= 0）时进度条不可拖不可点
    int64_t durationMs() const noexcept;
    // 单帧时长（毫秒），按时长帧率推算；拿不到帧率时按 30fps 估
    double frameDurationMs() const noexcept;
    bool hasVideo() const noexcept;
    // 当前已显示帧的时间戳（单帧步进以它为基准）
    int64_t currentFramePtsMs() const noexcept;

private:
    MediaPlayer();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jark
