#include "AudioSpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>

namespace jark {
namespace {

    // Hann 窗的相干增益是 0.5：加窗后正弦的峰值幅度被削掉一半，算幅度时要补回来，
    // 否则"满幅正弦"永远显示成 -6dB、频谱整体偏矮（自检里那条"峰值段电平"会跟着偏）
    constexpr double kWindowGain = 0.5;

    // 幅度 → 电平的 dB 区间：0 dBFS（满幅正弦）映射到 1.0，-72dB 及以下映射到 0。
    // 不做自动增益（AGC）：AGC 会让"静音也是满格跳动"，而且自检没法钉住数值。
    constexpr double kFloorDb = -72.0;
    constexpr double kCeilDb = -6.0;

    // 每帧的衰减量（跳距 10.7ms，0.045 大约 0.24 秒从满格落到 0）：
    // 峰值立刻跟上去、落下要看得见——少了它就是一片闪烁的噪点
    constexpr float kDecayPerFrame = 0.045f;

    // 一阶高通（直流阻断）的极点：48kHz 下转折约 40Hz，正好在最下面那一段之下——
    // 直流、低频隆隆声都被压掉，而 40Hz 以上的内容不受影响
    constexpr float kDcPole = 0.995f;

    float levelFromDb(double db) {
        const double level = (db - kFloorDb) / (kCeilDb - kFloorDb);
        return static_cast<float>(std::clamp(level, 0.0, 1.0));
    }

} // namespace

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer(int sampleRate, int bandCount)
    : sampleRate_(sampleRate > 0 ? sampleRate : 48000)
    , bandCount_(std::clamp(bandCount, 1, 256)) {

    // Hann 窗（周期性形式，按 N 归一化）
    window_.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window_[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 * i / kFftSize)));

    // 对数频段 → FFT bin 区间。**相邻段不许共用同一个 bin**：两边都按四舍五入取边界时，
    // 同一个 bin 会同时属于前后两段，一段有声音时两段一起亮，而且最强的那一段会往前偏
    // 半档（自检里 440Hz 落到第 10 段、而含 440Hz 的是第 11 段，就是这么来的）。
    // 低段仍然会挤在一起（40~100Hz 只有两三个 bin 可用），那是分辨率的下限，不是 bug。
    const double ratio = kHighHz / kLowHz;
    const int maxBin = kFftSize / 2 - 1;
    bandLowBin_.resize(bandCount_);
    bandHighBin_.resize(bandCount_);
    for (int band = 0; band < bandCount_; ++band) {
        const double lowHz = kLowHz * std::pow(ratio, static_cast<double>(band) / bandCount_);
        const double highHz = kLowHz * std::pow(ratio, static_cast<double>(band + 1) / bandCount_);
        const int lowBin = std::clamp(
            static_cast<int>(std::lround(lowHz * kFftSize / sampleRate_)), 0, maxBin);
        const int highBin = std::clamp(
            static_cast<int>(std::lround(highHz * kFftSize / sampleRate_)) - 1, lowBin, maxBin);
        bandLowBin_[band] = lowBin;
        bandHighBin_[band] = highBin;
    }

    previous_.assign(bandCount_, 0.0f);
    history_.assign(static_cast<size_t>(kHistoryFrames) * bandCount_, 0.0f);
    historyPtsMs_.assign(kHistoryFrames, 0);
    pending_.reserve(kFftSize * 4);
}

void AudioSpectrumAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    pendingOffset_ = 0;
    pendingPtsMs_ = 0;
    hasPendingPts_ = false;
    historyWrite_ = 0;
    historyCount_ = 0;
    dcPrevIn_ = 0.0f;
    dcPrevOut_ = 0.0f;
    std::fill(previous_.begin(), previous_.end(), 0.0f);
}

void AudioSpectrumAnalyzer::push(std::span<const int16_t> samples, int64_t ptsMs) {
    if (samples.empty())
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!hasPendingPts_) {
        pendingPtsMs_ = ptsMs;
        hasPendingPts_ = true;
    }

    // 下混成单声道并归一化到 [-1,1]（左右声道的相位差不该影响频谱），再过一阶高通
    // 去掉直流与亚声频：很多素材带几十 LSB 的直流偏置，它的谱泄漏全落在最低那一两个
    // bin 上，而低频段又都挤在那几个 bin 里——不去掉的话最低几根条会永远亮着
    const size_t sampleCount = samples.size() / 2;
    const size_t base = pending_.size();
    pending_.resize(base + sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        const int32_t left = samples[i * 2];
        const int32_t right = samples[i * 2 + 1];
        const float mixed = static_cast<float>(left + right) * (0.5f / 32768.0f);
        const float filtered = mixed - dcPrevIn_ + kDcPole * dcPrevOut_;
        dcPrevIn_ = mixed;
        dcPrevOut_ = filtered;
        pending_[base + i] = filtered;
    }

    while (pending_.size() - pendingOffset_ >= static_cast<size_t>(kFftSize)) {
        const int64_t framePtsMs = pendingPtsMs_ +
            static_cast<int64_t>(pendingOffset_ * 1000 / static_cast<size_t>(sampleRate_));
        analyzeFrame(std::span<const float>(pending_.data() + pendingOffset_, kFftSize), framePtsMs);
        pendingOffset_ += kHopSize;
    }

    // 消费掉的前缀不留着（每次 push 顶多留下一窗不到）
    if (pendingOffset_ > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pendingOffset_));
        pendingPtsMs_ += static_cast<int64_t>(pendingOffset_ * 1000 / static_cast<size_t>(sampleRate_));
        pendingOffset_ = 0;
    }
}

void AudioSpectrumAnalyzer::analyzeFrame(std::span<const float> frame, int64_t ptsMs) {
    cv::Mat input(1, kFftSize, CV_32FC1);
    for (int i = 0; i < kFftSize; ++i)
        input.at<float>(0, i) = frame[i] * window_[i];

    cv::Mat spectrum;
    cv::dft(input, spectrum, cv::DFT_COMPLEX_OUTPUT); // 未归一化：|X[k]| = A·N/2（正弦）

    std::vector<float> levels(static_cast<size_t>(bandCount_));
    for (int band = 0; band < bandCount_; ++band) {
        double peak = 0.0;
        for (int bin = bandLowBin_[band]; bin <= bandHighBin_[band]; ++bin) {
            const cv::Vec2f value = spectrum.at<cv::Vec2f>(0, bin);
            peak = (std::max)(peak, static_cast<double>(std::hypot(value[0], value[1])));
        }

        // 单边谱 ×2、再补回加窗损失，得到该段正弦的峰值幅度（1.0 = 满幅）
        const double magnitude = peak * 2.0 / (kFftSize * kWindowGain);
        const double db = 20.0 * std::log10(magnitude + 1e-12);
        const float level = levelFromDb(db);

        // 快起慢落：低了就按固定斜率往下走，高了立刻跟上去
        levels[band] = (std::max)(level, previous_[band] - kDecayPerFrame);
        previous_[band] = levels[band];
    }

    storeFrame(levels.data(), ptsMs);
}

void AudioSpectrumAnalyzer::storeFrame(const float* levels, int64_t ptsMs) {
    float* slot = history_.data() + static_cast<size_t>(historyWrite_) * bandCount_;
    std::copy_n(levels, bandCount_, slot);
    historyPtsMs_[historyWrite_] = ptsMs;
    historyWrite_ = (historyWrite_ + 1) % kHistoryFrames;
    historyCount_ = (std::min)(historyCount_ + 1, kHistoryFrames);
}

bool AudioSpectrumAnalyzer::readAt(int64_t positionMs, std::span<float> out) const {
    if (out.size() < static_cast<size_t>(bandCount_))
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (historyCount_ == 0)
        return false;

    // 取 pts <= 播放位置里最新的一帧。全量扫一遍（最多 512 帧）比维护有序游标简单得多，
    // 一帧的成本可以忽略。若一帧都不比位置早（刚 seek 完、位置早于缓冲区里最早的帧），
    // 退回最早的那一帧——宁可显示"稍微旧一点"的一帧，也不要空着闪一下
    int best = -1;
    int64_t bestPts = 0;
    int oldest = -1;
    int64_t oldestPts = 0;
    for (int i = 0; i < historyCount_; ++i) {
        const int index = (historyWrite_ - historyCount_ + i + kHistoryFrames * 2) % kHistoryFrames;
        const int64_t pts = historyPtsMs_[index];
        if (pts <= positionMs && (best < 0 || pts > bestPts)) {
            best = index;
            bestPts = pts;
        }
        if (oldest < 0 || pts < oldestPts) {
            oldest = index;
            oldestPts = pts;
        }
    }
    if (best < 0)
        best = oldest;

    std::copy_n(history_.data() + static_cast<size_t>(best) * bandCount_, bandCount_, out.data());
    return true;
}

void AudioSpectrumAnalyzer::bandRangeHz(int band, double& lowHz, double& highHz) const {
    lowHz = 0.0;
    highHz = 0.0;
    if (band < 0 || band >= bandCount_)
        return;

    const double ratio = kHighHz / kLowHz;
    lowHz = kLowHz * std::pow(ratio, static_cast<double>(band) / bandCount_);
    highHz = kLowHz * std::pow(ratio, static_cast<double>(band + 1) / bandCount_);
}

double AudioSpectrumAnalyzer::bandCenterHz(int band) const {
    if (band < 0 || band >= bandCount_)
        return 0.0;

    const double ratio = kHighHz / kLowHz;
    const double lowHz = kLowHz * std::pow(ratio, static_cast<double>(band) / bandCount_);
    const double highHz = kLowHz * std::pow(ratio, static_cast<double>(band + 1) / bandCount_);
    return std::sqrt(lowHz * highHz);
}

} // namespace jark
