#include "MediaPlayer.h"

#include "AudioOutput.h"
#include "MediaDecoder.h"
#include "jarkUtils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace jark {
namespace {

    // 视频队列上限：解码最多领先播放这么多帧
    constexpr size_t kMaxQueuedVideoFrames = 6;

    // 显示提前量：让 UI 定时刷新（约 15.6ms）总能拿到到点的帧
    constexpr int64_t kFrameLeadMs = 20;

    struct QueuedFrame {
        cv::Mat image;
        int64_t ptsMs = 0;
    };

} // namespace

struct MediaPlayer::Impl {
    std::unique_ptr<MediaDecoder> decoder;
    std::unique_ptr<AudioOutput> audio;
    std::jthread decodeThread;

    std::mutex queueMutex;
    std::condition_variable queueSpaceAvailable;
    std::condition_variable queueNotEmpty;
    std::deque<QueuedFrame> videoQueue;
    cv::Mat currentFrame;

    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> decodeFinished{ false };
    std::atomic<int64_t> decodedFrames{ 0 };
    std::atomic<int64_t> presentedFrames{ 0 };
    std::atomic<int64_t> lastFramePtsMs{ 0 };

    std::chrono::steady_clock::time_point startTime;
    bool hasAudio = false;

    int64_t clockMs() const {
        if (audio)
            return audio->playedFrames() * 1000 / MediaDecoder::kOutputSampleRate;

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
    }

    void decodeLoop(std::stop_token stopToken) {
        MediaDecoder::Chunk chunk;

        while (!stopToken.stop_requested() && !stopRequested.load()) {
            // 队列已满时等待播放消耗（音频播放时同时给音频留出空间）
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueSpaceAvailable.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return videoQueue.size() < kMaxQueuedVideoFrames;
                });
            }
            if (stopToken.stop_requested() || stopRequested.load())
                break;

            if (!decoder->readNext(chunk)) {
                decodeFinished.store(true);
                break;
            }

            if (chunk.type == MediaDecoder::Chunk::Type::Audio) {
                // 音频队列满时稍等：解码线程跟着音频播放速度走
                while (!stopToken.stop_requested() && !stopRequested.load() && audio &&
                    audio->queuedFrames() >= AudioOutput::kMaxQueuedFrames) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (audio)
                    audio->submit(chunk.audio);
            }
            else if (chunk.type == MediaDecoder::Chunk::Type::Video && !chunk.video.empty()) {
                std::lock_guard<std::mutex> lock(queueMutex);
                videoQueue.push_back(QueuedFrame{ std::move(chunk.video), chunk.ptsMs });
                decodedFrames.fetch_add(1);
                queueNotEmpty.notify_all();

                // 无音频时按壁钟节流，避免一次性解出整段视频
                if (!hasAudio && videoQueue.size() >= kMaxQueuedVideoFrames) {
                    const int64_t duration = chunk.ptsMs - clockMs();
                    if (duration > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
                }
            }
        }
    }
};

MediaPlayer::MediaPlayer() : impl_(std::make_unique<Impl>()) {}

MediaPlayer::~MediaPlayer() {
    stop();
}

std::unique_ptr<MediaPlayer> MediaPlayer::create() {
    return std::unique_ptr<MediaPlayer>(new MediaPlayer());
}

bool MediaPlayer::start(std::span<const uint8_t> data, float volume) {
    stop();

    auto decoder = MediaDecoder::open(data);
    if (!decoder) {
        JARK_LOG("MediaPlayer: cannot open media");
        return false;
    }

    impl_->decoder = std::move(decoder);
    impl_->hasAudio = impl_->decoder->info().hasAudio;
    impl_->stopRequested.store(false);
    impl_->decodeFinished.store(false);
    impl_->decodedFrames.store(0);
    impl_->presentedFrames.store(0);
    impl_->startTime = std::chrono::steady_clock::now();

    if (impl_->hasAudio) {
        impl_->audio = AudioOutput::create(MediaDecoder::kOutputSampleRate, MediaDecoder::kOutputChannels);
        if (!impl_->audio) {
            JARK_LOG("MediaPlayer: no audio output, falling back to system clock");
            impl_->hasAudio = false;
        }
        else {
            impl_->audio->setVolume(volume);
        }
    }

    JARK_LOG("MediaPlayer started: video={} audio={} duration={}ms",
        impl_->decoder->info().hasVideo, impl_->hasAudio, impl_->decoder->info().durationMs);

    impl_->decodeThread = std::jthread([this](std::stop_token stopToken) {
        impl_->decodeLoop(stopToken);
    });

    return true;
}

void MediaPlayer::stop() {
    if (!impl_->decodeThread.joinable() && !impl_->decoder)
        return;

    impl_->stopRequested.store(true);
    if (impl_->decodeThread.joinable()) {
        impl_->decodeThread.request_stop();
        impl_->queueSpaceAvailable.notify_all();
        impl_->queueNotEmpty.notify_all();
        impl_->decodeThread.join();
    }

    if (impl_->audio)
        impl_->audio->stop();

    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->videoQueue.clear();
    }

    impl_->audio.reset();
    impl_->decoder.reset();
    impl_->currentFrame = cv::Mat();
}

bool MediaPlayer::isRunning() const noexcept {
    return impl_->decoder != nullptr && !impl_->decodeFinished.load();
}

bool MediaPlayer::hasFinished() const noexcept {
    if (!impl_->decoder || !impl_->decodeFinished.load())
        return false;

    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        if (!impl_->videoQueue.empty())
            return false;
    }

    if (impl_->audio && impl_->audio->queuedFrames() > 0)
        return false;

    // 最后一帧显示完毕（给一帧的显示时间）后结束
    const int64_t lastFrameMs = impl_->lastFramePtsMs.load();
    return impl_->clockMs() >= lastFrameMs;
}

bool MediaPlayer::getVideoSize(int& width, int& height) const noexcept {
    if (!impl_->decoder || !impl_->decoder->info().hasVideo)
        return false;

    // 给的是"播放端看到的"尺寸（已按显示旋转），与 acquireFrame() 交出来的帧一致；
    // 直接给编码尺寸会在竖拍视频上把宽高弄反，画面被拉伸
    width = impl_->decoder->info().displayWidth();
    height = impl_->decoder->info().displayHeight();
    return true;
}

bool MediaPlayer::hasAudio() const noexcept {
    return impl_->hasAudio;
}

bool MediaPlayer::acquireFrame(cv::Mat& frame) {
    if (!impl_->decoder)
        return false;

    const int64_t clock = impl_->clockMs();

    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        while (!impl_->videoQueue.empty() && impl_->videoQueue.front().ptsMs <= clock + kFrameLeadMs) {
            impl_->currentFrame = std::move(impl_->videoQueue.front().image);
            impl_->lastFramePtsMs.store(impl_->videoQueue.front().ptsMs);
            impl_->videoQueue.pop_front();
            impl_->presentedFrames.fetch_add(1);
            updated = true;
        }
    }

    if (updated)
        impl_->queueSpaceAvailable.notify_all();

    if (updated && !impl_->currentFrame.empty()) {
        frame = impl_->currentFrame;
        return true;
    }

    return false;
}

int64_t MediaPlayer::positionMs() const noexcept {
    return impl_->clockMs();
}

void MediaPlayer::setVolume(float volume) noexcept {
    if (impl_->audio)
        impl_->audio->setVolume(volume);
}

} // namespace jark
