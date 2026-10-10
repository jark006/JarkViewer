#include "VideoPlayback.h"

#include "MappedFileReader.h"
#include "MediaDecoder.h"
#include "MediaPlayer.h"
#include "jarkUtils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>

namespace jark {
namespace {

    int64_t steadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

} // namespace

VideoPlayback::VideoPlayback() = default;

VideoPlayback::~VideoPlayback() {
    close();
}

bool VideoPlayback::open(const std::wstring& path) {
    close();

    path_ = path;
    fileName_ = std::filesystem::path(path).filename().wstring();
    if (fileName_.empty())
        fileName_ = path;

    // 整文件映射（不是读进内存）：几 GB 的视频也不会真占掉那么多内存，
    // 而且解码器的 seek 只是移动映射里的一个指针
    auto mapping = std::make_shared<MappedFileReader>(path);
    if (!mapping->isOpen() || mapping->isEmpty()) {
        JARK_LOG("播放器: 打不开 {}", jarkUtils::wstringToUtf8(path));
        error_ = Error::FileMissing;
        return false;
    }
    mapping_ = std::move(mapping);

    const float volume = static_cast<float>(volumePercent_) / 100.0f;
    auto player = MediaPlayer::create();
    if (!player || !player->start(mapping_->view(), volume)) {
        JARK_LOG("播放器: 没有可播放的视频流 {}", jarkUtils::wstringToUtf8(path));
        error_ = Error::DecodeFailed;
        mapping_.reset();   // 解码器已经不持指针了，映射可以立即释放
        return false;
    }
    player_ = std::move(player);

    player_->getVideoSize(videoWidth_, videoHeight_);
    durationMs_ = player_->durationMs();
    hasAudio_ = player_->hasAudio();
    hasVideo_ = player_->hasVideo();
    // 只有纯音频画面才需要实时频谱（视频画面被帧占着），别让视频/实况那条路白算 FFT
    player_->setSpectrumEnabled(!hasVideo_);
    infoText_ = buildInfoText();
    atEnd_ = false;
    landingMinPtsMs_ = -1;
    resumeAfterScrub_ = false;
    scrubbing_ = false;

    JARK_LOG("播放器: 已打开 {} {} 时长 {}ms 音频={} 音量 {}%",
        jarkUtils::wstringToUtf8(fileName_),
        hasVideo_ ? std::format("显示 {}x{}", videoWidth_, videoHeight_) : std::string("纯音频"),
        durationMs_, hasAudio_, volumePercent_);
    return true;
}

void VideoPlayback::close() {
    if (player_)
        player_->stop();
    player_.reset();
    mapping_.reset(); // 必须在 player_ 之后：解码器只持映射里的指针

    path_.clear();
    fileName_.clear();
    infoText_.clear();
    error_ = Error::None;
    durationMs_ = 0;
    videoWidth_ = 0;
    videoHeight_ = 0;
    hasAudio_ = false;
    hasVideo_ = false;
    atEnd_ = false;
    scrubbing_ = false;
    resumeAfterScrub_ = false;
    landingMinPtsMs_ = -1;
    lastPreviewAtMs_ = 0;
}

bool VideoPlayback::takeFrame(cv::Mat& frame) {
    if (!player_)
        return false;

    const bool updated = player_->acquireFrame(frame);

    // 拖动中被节流挡下的那一下目标：窗口过去了就补发（停手不再发 mousemove 也要落到目标上）
    if (scrubbing_ && pendingPreview_) {
        const int64_t now = steadyNowMs();
        if (now - lastPreviewAtMs_ >= kPreviewIntervalMs) {
            lastPreviewAtMs_ = now;
            pendingPreview_ = false;
            player_->seek(scrubTargetMs_, MediaPlayer::SeekMode::KeyFrame);
        }
    }

    // 松手后的精确落位：画面换成目标附近的那一帧才算落地（精确落点要从关键帧向前
    // 解码到目标，长 GOP 上要几百毫秒，这段时间画面保持拖动预览帧、进度条已在目标位）。
    // 纯音频没有帧，endScrub 里就不进这个等待（见那里）。
    if (landingMinPtsMs_ >= 0) {
        if (updated && player_->currentFramePtsMs() >= landingMinPtsMs_)
            landingMinPtsMs_ = -1;
        else if (steadyNowMs() - landingWaitStartedMs_ > 4000)
            landingMinPtsMs_ = -1; // 兜底：目标附近没有帧时也要恢复播放
    }

    // 恢复播放只发生在"松手之后的精确落位完成"这一刻。拖动期间绝不能走这条：
    // 拖动中本来就该停着（时钟与声音都停），一恢复画面就跟着往前跑了
    if (resumeAfterScrub_ && !scrubbing_ && landingMinPtsMs_ < 0) {
        resumeAfterScrub_ = false;
        player_->resume();
    }

    updateAtEnd();
    return updated;
}

void VideoPlayback::updateAtEnd() {
    if (!player_ || atEnd_ || scrubbing_)
        return;

    // 播完自动暂停：停在最后一帧（不循环、不回退、不关窗）
    if (player_->hasFinished()) {
        player_->pause();
        atEnd_ = true;
        JARK_LOG("播放器: 播完，停在最后一帧");
    }
}

bool VideoPlayback::isPlaying() const noexcept {
    return player_ && !atEnd_ && !player_->isPaused();
}

bool VideoPlayback::isPaused() const noexcept {
    return !isPlaying();
}

void VideoPlayback::togglePlay() {
    if (!player_)
        return;

    if (atEnd_) {
        // 结尾暂停态：从头重播（否则这一下按下去毫无反应，像坏了）
        atEnd_ = false;
        player_->seek(0);
        player_->resume();
        return;
    }

    if (player_->isPaused())
        player_->resume();
    else
        player_->pause();
}

void VideoPlayback::pause() {
    if (player_)
        player_->pause();
}

void VideoPlayback::resume() {
    if (!player_)
        return;

    if (atEnd_) {
        togglePlay();
        return;
    }
    player_->resume();
}

void VideoPlayback::seekTo(int64_t ms) {
    if (!player_)
        return;

    atEnd_ = false;                                         // 从结尾拖回来要能继续播
    player_->seek(ms, MediaPlayer::SeekMode::Exact);
}

void VideoPlayback::nudge(int64_t deltaMs) {
    if (!player_)
        return;

    seekTo(player_->positionMs() + deltaMs);
}

void VideoPlayback::stepFrame(int direction) {
    if (!player_)
        return;

    atEnd_ = false;

    // 纯音频没有帧可步进：同一个键退化成 ±kNudgeMs 跳转，键位语义（暂停中按住左右）
    // 在音频上仍然有个说得过去的行为，界面那边的分支就不用为音频另写一套
    if (!hasVideo_) {
        nudge(direction >= 0 ? kNudgeMs : -kNudgeMs);
        return;
    }

    player_->stepFrame(direction); // 内含 pause()：单帧步进只在暂停态有意义
}

void VideoPlayback::beginScrub(int64_t ms) {
    if (!player_)
        return;

    scrubbing_ = true;
    pendingPreview_ = false;
    // 拖动前在播放的话，松手并精确落位后要接着播。用 || 保留上一次还没兑现的意图：
    // 连着拖两次（第一次的落位还没到）时，第二次松开不能把"该继续播"这件事丢掉
    resumeAfterScrub_ = resumeAfterScrub_ || isPlaying();
    player_->pause();          // 拖动期间时钟与声音都停住
    lastPreviewAtMs_ = 0;      // 第一下立刻出预览
    updateScrub(ms);
}

void VideoPlayback::updateScrub(int64_t ms) {
    if (!player_ || !scrubbing_)
        return;

    scrubTargetMs_ = (std::max)(int64_t{ 0 }, ms);
    if (durationMs_ > 0)
        scrubTargetMs_ = (std::min)(scrubTargetMs_, durationMs_);

    // 节流：一次拖动能产生几百个移动事件，密了只是让解码线程忙着追；
    // 更要紧的是**只保留最新目标**（MediaPlayer 侧天然如此，在途的旧目标会被覆盖），
    // 所以这里丢掉中间的事件不需要补偿、也不做累积记录——
    // 但"被挡下的这一下"要记着，等窗口过去由 takeFrame 补发：用户停手不再发
    // mousemove 时，最后那一下目标不能被丢在半路上
    const int64_t now = steadyNowMs();
    if (lastPreviewAtMs_ != 0 && now - lastPreviewAtMs_ < kPreviewIntervalMs) {
        pendingPreview_ = true;
        return;
    }
    lastPreviewAtMs_ = now;
    pendingPreview_ = false;

    player_->seek(scrubTargetMs_, MediaPlayer::SeekMode::KeyFrame);
}

void VideoPlayback::endScrub(int64_t ms) {
    if (!player_ || !scrubbing_)
        return;

    scrubbing_ = false;
    scrubTargetMs_ = (std::max)(int64_t{ 0 }, ms);
    if (durationMs_ > 0)
        scrubTargetMs_ = (std::min)(scrubTargetMs_, durationMs_);

    atEnd_ = false;
    player_->seek(scrubTargetMs_, MediaPlayer::SeekMode::Exact);

    // 落位判定阈值：目标之前一帧半以内的画面都算"已经落到目标上"。
    // 纯音频没有"落点帧"这个概念（音频侧 seek 由 applyAudioSeek 即时重设时钟基线、
    // 解码器丢掉目标之前的样本），等一个永远不来的帧只会白等 4 秒才恢复播放
    landingMinPtsMs_ = hasVideo_
        ? scrubTargetMs_ - static_cast<int64_t>(player_->frameDurationMs() * 1.5)
        : -1;
    landingWaitStartedMs_ = steadyNowMs();
    // 拖动前是暂停的：保持暂停，只把画面换到目标位置
}

void VideoPlayback::adjustVolume(int deltaPercent) {
    const int before = volumePercent_;
    volumePercent_ = std::clamp(volumePercent_ + deltaPercent, 0, 100);
    if (player_ && volumePercent_ != before)
        player_->setVolume(static_cast<float>(volumePercent_) / 100.0f);
}

int64_t VideoPlayback::positionMs() const noexcept {
    return player_ ? player_->positionMs() : 0;
}

int64_t VideoPlayback::framePtsMs() const noexcept {
    return player_ ? player_->currentFramePtsMs() : 0;
}

bool VideoPlayback::readSpectrum(std::span<float> out) const noexcept {
    return player_ && player_->readSpectrum(out);
}

// 信息面板的文本：用**一次性**的 Both 解码器读一遍容器与两条流的信息
// （正在播的那两个实例各只解一路，拿不到另一路；Both 只解复用不切换播的那一份）
std::string VideoPlayback::buildInfoText() const {
    if (!mapping_)
        return {};

    auto decoder = MediaDecoder::open(mapping_->view(), MediaDecoder::StreamFilter::Both);
    if (!decoder)
        return {};

    return mediaInfoText(path_, decoder->info());
}

} // namespace jark
