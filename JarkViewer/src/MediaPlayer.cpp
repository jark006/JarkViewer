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

    // 视频队列上限：解码最多领先播放这么多帧（一帧就是几 MB，这里同时是内存上限）
    constexpr size_t kMaxQueuedVideoFrames = 6;

    // 显示提前量：让 UI 定时刷新（约 15.6ms）总能拿到到点的帧
    constexpr int64_t kFrameLeadMs = 20;

    struct QueuedFrame {
        cv::Mat image;
        int64_t ptsMs = 0;
    };

} // namespace

// 视频与音频各占一个解码器实例 + 一个线程（所以这里有两个 MediaDecoder）。
//
// 为什么不能共用一个线程：视频必须按"播放端取帧"背压（队列只留 6 帧、约 200ms，
// 一帧就是几 MB，不能无上限地解在前面），而音频必须永远跑在播放位置前面（声卡队列
// 空了声音就卡）。两条路抢同一个线程时，视频队列一满，整个循环就卡在等播放端取帧上，
// 音频提交被迫一起停下 —— 于是：
//   视频队满 → 音频提交掉到 1 倍速以下 → 声卡饿死、播放时钟停住 →
//   播放端按时钟判"还没到点"、不再取帧 → 视频队列永远满 —— 自锁成永久卡顿
// （实况照片"画面卡 + 声音一卡一卡"就是这一个故障的两个表现：时钟停住后画面也停）。
// 拆开后视频线程只受队列上限约束、音频线程只受声卡队列约束，互不牵连；音频提前约
// 2 秒提交（AudioOutput::kMaxQueuedFrames），取帧偶发的延迟吃不掉它。
struct MediaPlayer::Impl {
    std::unique_ptr<MediaDecoder> videoDecoder;   // 只解视频
    std::unique_ptr<MediaDecoder> audioDecoder;   // 只解音频（无音轨时为空）
    std::unique_ptr<AudioOutput> audio;

    std::jthread videoThread;
    std::jthread audioThread;

    std::mutex queueMutex;
    std::condition_variable queueSpaceAvailable;
    std::condition_variable queueNotEmpty;
    std::deque<QueuedFrame> videoQueue;
    cv::Mat currentFrame;

    std::atomic<bool> stopRequested{ false };
    std::atomic<bool> videoDecodeFinished{ true };
    std::atomic<bool> audioDecodeFinished{ true };
    std::atomic<int64_t> decodedFrames{ 0 };
    std::atomic<int64_t> presentedFrames{ 0 };
    std::atomic<int64_t> lastFramePtsMs{ 0 };

    std::chrono::steady_clock::time_point startTime;
    bool hasAudio = false;
    int64_t mediaDurationMs = 0;

    int64_t clockMs() const {
        if (audio)
            return audio->playedFrames() * 1000 / MediaDecoder::kOutputSampleRate;

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
    }

    void videoLoop(std::stop_token stopToken) {
        MediaDecoder::Chunk chunk;

        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (!videoDecoder->readNext(chunk))
                break;

            if (chunk.type != MediaDecoder::Chunk::Type::Video || chunk.video.empty())
                continue;

            // 队列满时等播放端取帧腾位置（有通知就醒）
            std::unique_lock<std::mutex> lock(queueMutex);
            while (!stopToken.stop_requested() && !stopRequested.load() &&
                videoQueue.size() >= kMaxQueuedVideoFrames) {
                queueSpaceAvailable.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (stopToken.stop_requested() || stopRequested.load())
                break;

            videoQueue.push_back(QueuedFrame{ std::move(chunk.video), chunk.ptsMs });
            decodedFrames.fetch_add(1);
            queueNotEmpty.notify_all();
        }

        videoDecodeFinished.store(true);
    }

    // 把一批音频交给声卡：配额满就等它播掉一些，提交失败就重试。
    // **不能失败即丢**——丢一批就是声音里一个 20ms 的空洞，听感就是"一卡一卡"。
    bool submitAudio(const std::vector<int16_t>& samples, std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (!audio)
                return true; // 没有输出设备：静音播放，样本直接丢掉

            if (audio->queuedFrames() >= AudioOutput::kMaxQueuedFrames ||
                audio->queuedBuffers() >= AudioOutput::kMaxQueuedBuffers) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (audio->submit(samples))
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    void audioLoop(std::stop_token stopToken) {
        MediaDecoder::Chunk chunk;
        int64_t submittedMs = 0;

        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (!audioDecoder->readNext(chunk)) {
                // 音轨比视频短时补静音。播放时钟由音频队列驱动，音轨一停时钟就停：
                // 视频最后几帧永远等不到"到点"，播放也永远不结束（画面停在末尾、
                // 实况照片回不到静态图）。静音补到媒体总时长为止。
                while (!stopToken.stop_requested() && !stopRequested.load() &&
                    submittedMs < mediaDurationMs) {
                    const int64_t chunkMs = (std::min)(mediaDurationMs - submittedMs, int64_t{ 100 });
                    const std::vector<int16_t> silence(
                        static_cast<size_t>(chunkMs) * MediaDecoder::kOutputSampleRate / 1000 *
                        MediaDecoder::kOutputChannels, 0);
                    if (!submitAudio(silence, stopToken))
                        break;
                    submittedMs += chunkMs;
                }
                break;
            }

            if (chunk.type != MediaDecoder::Chunk::Type::Audio || chunk.audio.empty())
                continue;

            // 音频唯一的节流阀是声卡队列：攒满配额就等它播掉一些。
            // 播放时钟就是这个队列的播放位置，余量越足越不怕取帧或解码的抖动
            if (!submitAudio(chunk.audio, stopToken))
                break;

            submittedMs += static_cast<int64_t>(chunk.audio.size()) /
                MediaDecoder::kOutputChannels * 1000 / MediaDecoder::kOutputSampleRate;
        }

        audioDecodeFinished.store(true);
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

    auto videoDecoder = MediaDecoder::open(data, MediaDecoder::StreamFilter::VideoOnly);
    if (!videoDecoder || !videoDecoder->info().hasVideo) {
        JARK_LOG("MediaPlayer: cannot open media");
        return false;
    }
    impl_->videoDecoder = std::move(videoDecoder);

    if (auto audioDecoder = MediaDecoder::open(data, MediaDecoder::StreamFilter::AudioOnly);
        audioDecoder && audioDecoder->info().hasAudio) {
        impl_->audioDecoder = std::move(audioDecoder);
    }

    impl_->hasAudio = impl_->audioDecoder != nullptr;
    impl_->mediaDurationMs = impl_->videoDecoder->info().durationMs;
    impl_->stopRequested.store(false);
    impl_->videoDecodeFinished.store(false);
    impl_->audioDecodeFinished.store(!impl_->audioDecoder);
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
        impl_->videoDecoder->info().hasVideo, impl_->hasAudio, impl_->videoDecoder->info().durationMs);

    impl_->videoThread = std::jthread([this](std::stop_token stopToken) {
        impl_->videoLoop(stopToken);
    });
    if (impl_->audioDecoder) {
        impl_->audioThread = std::jthread([this](std::stop_token stopToken) {
            impl_->audioLoop(stopToken);
        });
    }

    return true;
}

void MediaPlayer::stop() {
    if (!impl_->videoThread.joinable() && !impl_->videoDecoder)
        return;

    impl_->stopRequested.store(true);
    if (impl_->videoThread.joinable()) {
        impl_->videoThread.request_stop();
        impl_->queueSpaceAvailable.notify_all();
        impl_->queueNotEmpty.notify_all();
    }
    if (impl_->audioThread.joinable())
        impl_->audioThread.request_stop();

    if (impl_->videoThread.joinable())
        impl_->videoThread.join();
    if (impl_->audioThread.joinable())
        impl_->audioThread.join();

    if (impl_->audio)
        impl_->audio->stop();

    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->videoQueue.clear();
    }

    impl_->audio.reset();
    impl_->audioDecoder.reset();
    impl_->videoDecoder.reset();
    impl_->currentFrame = cv::Mat();
}

bool MediaPlayer::isRunning() const noexcept {
    return impl_->videoDecoder != nullptr && !impl_->videoDecodeFinished.load();
}

bool MediaPlayer::hasFinished() const noexcept {
    if (!impl_->videoDecoder || !impl_->videoDecodeFinished.load() || !impl_->audioDecodeFinished.load())
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
    if (!impl_->videoDecoder || !impl_->videoDecoder->info().hasVideo)
        return false;

    // 给的是"播放端看到的"尺寸（已按显示旋转），与 acquireFrame() 交出来的帧一致；
    // 直接给编码尺寸会在竖拍视频上把宽高弄反，画面被拉伸
    width = impl_->videoDecoder->info().displayWidth();
    height = impl_->videoDecoder->info().displayHeight();
    return true;
}

bool MediaPlayer::hasAudio() const noexcept {
    return impl_->hasAudio;
}

bool MediaPlayer::acquireFrame(cv::Mat& frame) {
    if (!impl_->videoDecoder)
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
