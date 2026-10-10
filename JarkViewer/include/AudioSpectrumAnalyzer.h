#pragma once

// 实时频谱：播放器音频画面上那列"跟着声音跳"的条。
//
// 数据来源是**解码后的 PCM**（MediaDecoder 交出的 48kHz 立体声 int16，与提交给 XAudio2 的
// 是同一批样本），不接声卡回环——回环要额外设备与权限，而且这里能看到"还没播到"的样本，
// 正好用得上，见下面那条时间戳。
//
// **必须带媒体时间戳**：音频线程永远跑在播放位置前面（队列提前约 2 秒），若直接画
// "最新算出来的那一帧"，频谱会比听到的声音早一两秒（音乐上很明显）。所以每一帧按 ptsMs
// 存进环形缓冲，界面用自己的播放时钟（MediaPlayer::positionMs）去取对应那一帧。
//
// 纯计算，不依赖窗口；push 在解码线程、readAt 在界面线程，内部用一把锁护住环形缓冲。

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace jark {

class AudioSpectrumAnalyzer {
public:
    static constexpr int kDefaultBandCount = 28;
    static constexpr int kFftSize = 1024;      // 48kHz 下约 21ms 的窗
    static constexpr int kHopSize = 512;       // 跳距约 10.7ms（≈94 帧/秒）
    static constexpr int kHistoryFrames = 512; // 10.7ms × 512 ≈ 5.5s，比音频队列（约 2s）宽

    // bandCount 段按对数从 kLowHz 排到 kHighHz
    static constexpr double kLowHz = 40.0;
    static constexpr double kHighHz = 16000.0;

    explicit AudioSpectrumAnalyzer(int sampleRate, int bandCount = kDefaultBandCount);

    // 清空历史（seek 之后旧位置的帧全部作废）
    void reset();

    // 喂一批交错立体声 int16；ptsMs 是这批**第一个样本**的媒体时间
    void push(std::span<const int16_t> samples, int64_t ptsMs);

    // 取"播放位置 positionMs 处"那一帧（ptsMs <= positionMs 里最新的一帧）。
    // 还没有可用数据（喂得不够 / 位置早于所有帧）返回 false，out 不保证被写。
    bool readAt(int64_t positionMs, std::span<float> out) const;

    int bandCount() const noexcept { return bandCount_; }
    int sampleRate() const noexcept { return sampleRate_; }
    // 第 band 段的标称频率范围与中心频率（Hz），自检用它把某个频率换算成段号。
    // 注意低段的实际 bin 会聚拢（40~100Hz 只有两三个 bin 可用），标称范围比实际能分辨的窄
    void bandRangeHz(int band, double& lowHz, double& highHz) const;
    double bandCenterHz(int band) const;

private:
    void analyzeFrame(std::span<const float> frame, int64_t ptsMs);
    void storeFrame(const float* levels, int64_t ptsMs);

    int sampleRate_ = 0;
    int bandCount_ = 0;

    std::vector<float> window_;      // Hann 窗
    std::vector<int> bandLowBin_;    // 每段的 FFT bin 区间（含两端）
    std::vector<int> bandHighBin_;

    std::vector<float> pending_;     // 还没凑够一窗的单声道样本
    size_t pendingOffset_ = 0;       // pending_ 里已消费到的位置
    int64_t pendingPtsMs_ = 0;       // pending_[0] 的媒体时间
    bool hasPendingPts_ = false;

    std::vector<float> previous_;    // 上一帧的电平（做衰减用）

    // 一阶高通（直流阻断）的状态：进/出的上一个样本
    float dcPrevIn_ = 0.0f;
    float dcPrevOut_ = 0.0f;

    mutable std::mutex mutex_;       // 护住下面这块环形缓冲与 pending_
    std::vector<float> history_;     // kHistoryFrames × bandCount_
    std::vector<int64_t> historyPtsMs_;
    int historyWrite_ = 0;
    int historyCount_ = 0;
};

} // namespace jark
