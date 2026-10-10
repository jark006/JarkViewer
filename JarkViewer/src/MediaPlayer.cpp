#include "MediaPlayer.h"

#include "AudioSpectrumAnalyzer.h"

#include "AudioOutput.h"
#include "MediaDecoder.h"
#include "jarkUtils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
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

    int64_t elapsedMs(std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - from).count();
    }

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
//
// seek 复用同一条分工：命令用序号发布（见 requestSeek），两个线程各自把自家解码器
// 定位到关键帧；谁负责时钟由"有没有声卡"决定（有就由音频线程重设基线，因为时钟就是
// 声卡的已播样本数），这样时钟永远只有一个写者。
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
    std::atomic<bool> paused{ false };
    std::atomic<bool> videoEof{ false };
    // seek 落点：只交出"覆盖目标时刻"的那一帧，见 acquireFrame
    std::atomic<bool> landingPending{ false };

    // —— 时钟 ——
    // 唯一变量是"播放中累计的毫秒"：有声卡时取已播样本数（voice 一停就冻住，所以
    // 暂停不用另外处理），没有声卡时按墙上时间累计（暂停时停止累计）。
    // 起播与 seek 时重设基线。
    mutable std::mutex clockMutex;
    int64_t clockBaseMs = 0;
    int64_t audioBaseFrames = 0;
    int64_t sysElapsedMs = 0;
    std::chrono::steady_clock::time_point sysSegmentStart;
    bool sysRunning = false;

    // —— seek 命令 ——
    // 先写目标、再递增序号（两个都是 seq_cst），解码线程读到新序号就能安全取走目标。
    // 只保留最新目标 = 拖动时在途的旧请求自动作废，不做累积记录。
    struct SeekCommand {
        int64_t targetMs = 0;
        bool exact = true;
    };

    std::atomic<uint64_t> seekSerial{ 0 };
    std::atomic<int64_t> seekTargetMs{ 0 };
    std::atomic<bool> seekExact{ true };
    uint64_t videoSeenSerial = 0;
    uint64_t audioSeenSerial = 0;
    std::mutex seekMutex;
    std::condition_variable seekSignal;

    // 视频线程专用：落点过滤的界线（< 0 表示不作过滤）
    int64_t dropBeforeMs = -1;
    // 视频线程专用：当前这次 seek 是不是"拖动预览"（只跳到关键帧）
    bool previewMode = false;
    // 视频线程专用：拖动预览只交关键帧那一帧，交出后就停下等下一次 seek。
    // 不这么做的话，解码线程会顺着关键帧继续解、把"未到目标"的帧一帧帧送出来
    // （它们的 PTS 都早于已被重设到目标的时钟，全都算"到点"），画面看起来像在追赶
    bool previewHold = false;
    // 音频线程专用：音轨已解到末尾（线程随后转入"补静音 / 等 seek"）
    bool audioEof = false;
    // 音频线程专用：seek 后第一段音频要裁到的时间戳（< 0 表示不裁）
    int64_t submittedMs = 0;

    bool hasAudio = false;
    int64_t mediaDurationMs = 0;
    int64_t frameStepMs = 33;

    // 实时频谱：分析器在 start() 里建（那时线程还没起）、stop() 里随线程一起收，
    // 指针本身在整个播放会话里是稳定的，所以音频线程可以直接解引用；
    // 开关是原子的，界面想开就开，不必与音频线程同步
    std::unique_ptr<AudioSpectrumAnalyzer> spectrum;
    std::atomic<bool> spectrumEnabled{ false };

    // 时钟：clockBaseMs + 播放中累计
    int64_t clockMs() const {
        std::lock_guard<std::mutex> lock(clockMutex);

        int64_t elapsed = 0;
        if (audio) {
            const int64_t played = audio->playedFrames() - audioBaseFrames;
            elapsed = played > 0 ? played * 1000 / MediaDecoder::kOutputSampleRate : 0;
        }
        else if (sysRunning) {
            elapsed = sysElapsedMs + elapsedMs(sysSegmentStart);
        }
        else {
            elapsed = sysElapsedMs;
        }

        int64_t clock = clockBaseMs + (elapsed > 0 ? elapsed : 0);
        // 上限留一帧余量：最后一帧的时间戳常常略超容器时长（切片的 duration 会短一点），
        // 卡死在时长上会让 hasFinished() 的"时钟 ≥ 最后一帧"永远不成立——播完就停不下来了
        const int64_t upper = mediaDurationMs > 0 ? mediaDurationMs + frameStepMs : 0;
        if (upper > 0 && clock > upper)
            clock = upper;
        return clock;
    }

    // 把时钟搬到目标位置（seek / 起播用）。只有声卡那一侧或视频那一侧会来写，
    // 按 hasAudio 二选一，绝不两个线程同时改同一个基线
    void rebaseClockForAudio(int64_t target) {
        std::lock_guard<std::mutex> lock(clockMutex);
        audioBaseFrames = audio ? audio->playedFrames() : 0;
        clockBaseMs = target;
    }

    void rebaseClockForSystemClock(int64_t target) {
        std::lock_guard<std::mutex> lock(clockMutex);
        clockBaseMs = target;
        sysElapsedMs = 0;
        sysSegmentStart = std::chrono::steady_clock::now();
    }

    // —— seek 命令的发布与取用 ——

    void requestSeek(int64_t target, bool exact) {
        seekTargetMs.store(target);
        seekExact.store(exact);
        seekSerial.fetch_add(1);
        seekSignal.notify_all();
        queueSpaceAvailable.notify_all();
    }

    bool takeSeek(uint64_t& seen, SeekCommand& command) {
        const uint64_t serial = seekSerial.load();
        if (serial == seen)
            return false;
        seen = serial;
        command.targetMs = seekTargetMs.load();
        command.exact = seekExact.load();
        return true;
    }

    // 音轨排空后要补静音到哪：有时长就补到时长；时长未知时补到最后一帧的时间戳
    // （不补的话时钟停在最后一帧之前，hasFinished() 永远不成立、播完也不会停下）
    int64_t silenceUntilMs() const {
        if (mediaDurationMs > 0)
            return mediaDurationMs;
        if (videoDecodeFinished.load())
            return lastFramePtsMs.load();
        return (std::numeric_limits<int64_t>::max)();
    }

    // 把音频摆到 seek 目标上的活儿交给了解码器（MediaDecoder::setSkipBeforeMs）：
    // 这里以前只裁"seek 后的第一块"，而落点常常远在目标之前（视频关键帧上），
    // 于是第二块起就被当成正常音频送出去了 —— 听感就是 seek 之后先冒出半秒到几秒
    // 的旧声音（像在快放）才回到正确位置。
    // 提交一批音频的结果：Ok = 已提交；Seek = 来了新的 seek（回外层处理）；
    // Stop = 停止请求。**提交失败要重试、不能丢**——丢一批就是声音里一个
    // 20ms 的空洞，听感就是"一卡一卡"
    enum class SubmitResult { Ok, Seek, Stop };

    SubmitResult submitAudio(const std::vector<int16_t>& samples, std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (audioSeenSerial != seekSerial.load())
                return SubmitResult::Seek;

            if (!audio)
                return SubmitResult::Ok; // 没有输出设备：静音播放，样本直接丢掉

            if (audio->queuedFrames() >= AudioOutput::kMaxQueuedFrames ||
                audio->queuedBuffers() >= AudioOutput::kMaxQueuedBuffers) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (audio->submit(samples))
                return SubmitResult::Ok;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return SubmitResult::Stop;
    }

    void videoLoop(std::stop_token stopToken) {
        MediaDecoder::Chunk chunk;
        SeekCommand command;

        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (takeSeek(videoSeenSerial, command)) {
                applyVideoSeek(command);
                continue;
            }

            if (videoEof || previewHold) {
                // 解到文件末尾也不退出线程：播放器要停在最后一帧等用户拖回中间，
                // 线程一退出后面的 seek 就没人执行了；拖动预览交出关键帧后同理
                std::unique_lock<std::mutex> lock(seekMutex);
                seekSignal.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return stopRequested.load() || stopToken.stop_requested() ||
                        videoSeenSerial != seekSerial.load();
                });
                continue;
            }

            if (!videoDecoder->readNext(chunk)) {
                videoEof = true;
                videoDecodeFinished.store(true);
                continue;
            }

            if (chunk.type != MediaDecoder::Chunk::Type::Video || chunk.video.empty())
                continue;

            // seek 落点过滤：丢掉"整帧都落在目标之前"的帧，于是队列里的第一帧
            // 就是覆盖目标时刻的那一帧（时间戳 <= 目标），画面真落在松手的位置上。
            // 解码器那边已经按同样的界线在缩放/拷贝之前丢过一遍（省时间），这里是定稿
            if (dropBeforeMs >= 0) {
                if (chunk.ptsMs + frameStepMs <= dropBeforeMs)
                    continue;
                dropBeforeMs = -1;
                videoDecoder->setSkipBeforeMs(-1);
            }

            // 队列满时等播放端取帧腾位置（有通知就醒）；来了新 seek 就立刻出去处理
            std::unique_lock<std::mutex> lock(queueMutex);
            while (!stopToken.stop_requested() && !stopRequested.load() &&
                videoQueue.size() >= kMaxQueuedVideoFrames &&
                videoSeenSerial == seekSerial.load()) {
                queueSpaceAvailable.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (stopToken.stop_requested() || stopRequested.load())
                break;
            if (videoSeenSerial != seekSerial.load())
                continue; // 这一帧作废，回外层处理 seek

            videoQueue.push_back(QueuedFrame{ std::move(chunk.video), chunk.ptsMs });
            decodedFrames.fetch_add(1);
            queueNotEmpty.notify_all();

            // 预览只交这一帧（拖动中每一下都只要一张关键帧），接下来停下等下一次 seek
            if (dropBeforeMs < 0 && previewMode)
                previewHold = true;
        }

        videoDecodeFinished.store(true);
    }

    void applyVideoSeek(const SeekCommand& command) {
        const int64_t target = command.targetMs;
        videoDecoder->seek(target);

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            videoQueue.clear(); // 旧位置的帧全部作废；当前这一帧先留着，画面不黑不跳
        }

        lastFramePtsMs.store(target);
        videoEof.store(false);
        videoDecodeFinished.store(false);
        previewHold = false;
        previewMode = !command.exact;
        // 精确落点要从关键帧向前解码到目标；预览模式不动这个过滤器，
        // 交出来的就是关键帧本身（拖动时快得多，代价是画面与把手差最多一个 GOP）
        dropBeforeMs = command.exact ? target : -1;
        // 解码器侧也在转换之前丢一遍（界线放宽一帧：覆盖目标时刻的那一帧正好在界线之上，
        // 不能被它误杀），落点耗时的大头就在这儿
        videoDecoder->setSkipBeforeMs(command.exact ? target - frameStepMs : -1);
        landingPending.store(true);

        if (!audio)
            rebaseClockForSystemClock(target); // 没有声卡：时钟基线由视频这一侧搬

        queueSpaceAvailable.notify_all();
        JARK_LOG("player: video seek -> {} ms ({})", target, command.exact ? "精确" : "关键帧预览");
    }

    void audioLoop(std::stop_token stopToken) {
        MediaDecoder::Chunk chunk;
        SeekCommand command;

        while (!stopToken.stop_requested() && !stopRequested.load()) {
            if (takeSeek(audioSeenSerial, command)) {
                applyAudioSeek(command.targetMs);
                continue;
            }

            if (audioEof) {
                // 音轨比视频短时补静音。播放时钟由音频队列驱动，音轨一停时钟就停：
                // 视频最后几帧永远等不到"到点"，播放也永远不结束（画面停在末尾、
                // 实况照片回不到静态图）。静音补到媒体总时长为止。
                bool interrupted = false;
                while (!stopToken.stop_requested() && !stopRequested.load() &&
                    submittedMs < silenceUntilMs()) {
                    if (audioSeenSerial != seekSerial.load()) {
                        interrupted = true;
                        break;
                    }

                    const int64_t upper = silenceUntilMs();
                    const int64_t chunkMs = upper == (std::numeric_limits<int64_t>::max)()
                        ? int64_t{ 100 } : (std::min)(upper - submittedMs, int64_t{ 100 });
                    const std::vector<int16_t> silence(
                        static_cast<size_t>(chunkMs) * MediaDecoder::kOutputSampleRate / 1000 *
                        MediaDecoder::kOutputChannels, 0);

                    // 补的静音也喂给频谱（时间戳按已提交到的位置推）：否则尾部这段时间里
                    // 频谱取不到这一段的帧，界面会停在最后一次有声音的样子不动
                    if (spectrum && spectrumEnabled.load())
                        spectrum->push(silence, submittedMs);

                    const SubmitResult result = submitAudio(silence, stopToken);
                    if (result == SubmitResult::Stop)
                        return;
                    if (result == SubmitResult::Seek) {
                        interrupted = true;
                        break;
                    }
                    submittedMs += chunkMs;
                }

                if (interrupted || audioSeenSerial != seekSerial.load())
                    continue; // 来了新的 seek：回外层处理

                if (stopToken.stop_requested() || stopRequested.load())
                    break;

                std::unique_lock<std::mutex> lock(seekMutex);
                seekSignal.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return stopRequested.load() || stopToken.stop_requested() ||
                        audioSeenSerial != seekSerial.load();
                });
                continue;
            }

            if (!audioDecoder->readNext(chunk)) {
                audioEof = true;
                audioDecodeFinished.store(true);
                continue;
            }

            if (chunk.type != MediaDecoder::Chunk::Type::Audio || chunk.audio.empty())
                continue;

            // 目标之前的样本由解码器侧丢掉（setSkipBeforeMs，与视频同一个机制），
            // 这里只兜一下空批：submit 空批会返回失败、把重试循环卡死
            if (chunk.audio.empty())
                continue;

            // 频谱喂的是**马上要提交的这批样本**（时间戳就是这批的起始媒体时间）：
            // 界面按播放位置去取，所以"解码跑在前面"不会让频谱提前（见 AudioSpectrumAnalyzer）
            if (spectrum && spectrumEnabled.load())
                spectrum->push(chunk.audio, chunk.ptsMs);

            // 音频唯一的节流阀是声卡队列：攒满配额就等它播掉一些。
            // 播放时钟就是这个队列的播放位置，余量越足越不怕取帧或解码的抖动
            const SubmitResult result = submitAudio(chunk.audio, stopToken);
            if (result == SubmitResult::Stop)
                break;
            if (result == SubmitResult::Seek)
                continue;

            submittedMs += static_cast<int64_t>(chunk.audio.size()) /
                MediaDecoder::kOutputChannels * 1000 / MediaDecoder::kOutputSampleRate;
        }

        audioDecodeFinished.store(true);
    }

    void applyAudioSeek(int64_t target) {
        if (audio)
            audio->flush(); // 丢掉旧位置排队的音频（保留"在播/暂停"的意图）

        audioDecoder->seek(target);
        // 与视频同一个机制：目标之前的样本由解码器丢掉（含"落点还在目标之前"的整批），
        // 时钟基线随即按目标重设，两边才对得上
        audioDecoder->setSkipBeforeMs(target);
        audioEof = false;
        audioDecodeFinished.store(false);
        submittedMs = target;
        // 频谱历史原地作废：里面全是旧位置的帧，取出来的画面与新的播放位置对不上
        if (spectrum)
            spectrum->reset();

        // 时钟基线交给音频这一侧：已播样本数从"当下读到的值"重新起算，
        // 不依赖 FlushSourceBuffers 会不会把 SamplesPlayed 清零（各版本行为不一致）
        if (audio)
            rebaseClockForAudio(target);
        else if (!videoDecoder)
            // 没有声卡**又没有视频线程**（纯音频 + 声卡创建失败）：这一侧是唯一的时钟写者，
            // 不补这一下，seek 之后时钟还按墙上时间从老位置往前跑，永远追不上目标
            rebaseClockForSystemClock(target);

        JARK_LOG("player: audio seek -> {} ms", target);
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

    // 视频与音频各自探测：**有哪一路就播哪一路**。纯音频文件（mp3/flac/wav…或只有音轨的
    // mp4/mkv）没有视频轨，于是没有视频线程、没有画面，只剩时钟与声音——
    // 播放器窗口照常可用（界面自己画音频占位画面）。
    auto videoDecoder = MediaDecoder::open(data, MediaDecoder::StreamFilter::VideoOnly);
    if (!videoDecoder || !videoDecoder->info().hasVideo)
        videoDecoder.reset();

    auto audioDecoder = MediaDecoder::open(data, MediaDecoder::StreamFilter::AudioOnly);
    if (!audioDecoder || !audioDecoder->info().hasAudio)
        audioDecoder.reset();

    if (!videoDecoder && !audioDecoder) {
        JARK_LOG("MediaPlayer: cannot open media");
        return false;
    }
    impl_->videoDecoder = std::move(videoDecoder);
    impl_->audioDecoder = std::move(audioDecoder);

    // 时长/帧率取现有那一路：纯音频的 frameRate 是 0，单帧时长保持默认（只用于 seek 上界
    // 与时钟余量，音频没有帧可言）
    const MediaInfo& info = impl_->videoDecoder ? impl_->videoDecoder->info() : impl_->audioDecoder->info();
    impl_->hasAudio = impl_->audioDecoder != nullptr;
    impl_->mediaDurationMs = info.durationMs;
    // 分析器建在线程起来之前（这里是唯一一处新建），有音轨才建；
    // 真正算不算由 spectrumEnabled 决定（视频/实况照片那条路不白算）
    impl_->spectrum = impl_->audioDecoder
        ? std::make_unique<AudioSpectrumAnalyzer>(MediaDecoder::kOutputSampleRate)
        : nullptr;
    impl_->spectrumEnabled.store(false);
    impl_->frameStepMs = info.frameRate > 0.0
        ? (std::max)(int64_t{ 1 }, static_cast<int64_t>(std::llround(1000.0 / info.frameRate)))
        : 33;
    impl_->stopRequested.store(false);
    impl_->videoDecodeFinished.store(impl_->videoDecoder == nullptr); // 没有视频线程 = 已经"解完"
    impl_->audioDecodeFinished.store(!impl_->audioDecoder);
    impl_->decodedFrames.store(0);
    impl_->presentedFrames.store(0);
    impl_->lastFramePtsMs.store(0);
    impl_->paused.store(false);
    impl_->videoEof.store(false);
    impl_->landingPending.store(false);
    impl_->dropBeforeMs = -1;
    impl_->previewHold = false;
    impl_->previewMode = false;
    impl_->submittedMs = 0;
    impl_->audioEof = false;
    impl_->videoSeenSerial = 0;
    impl_->audioSeenSerial = 0;
    impl_->seekSerial.store(0);
    impl_->seekTargetMs.store(0);
    impl_->seekExact.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->clockMutex);
        impl_->clockBaseMs = 0;
        impl_->audioBaseFrames = 0;
        impl_->sysElapsedMs = 0;
        impl_->sysRunning = true;
        impl_->sysSegmentStart = std::chrono::steady_clock::now();
    }

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
        info.hasVideo, impl_->hasAudio, info.durationMs);

    if (impl_->videoDecoder) {
        impl_->videoThread = std::jthread([this](std::stop_token stopToken) {
            impl_->videoLoop(stopToken);
        });
    }
    if (impl_->audioDecoder) {
        impl_->audioThread = std::jthread([this](std::stop_token stopToken) {
            impl_->audioLoop(stopToken);
        });
    }

    return true;
}

void MediaPlayer::stop() {
    // 判据是"还有没有媒体"，**不能**只看视频那一侧：纯音频文件既没有视频线程也没有
    // 视频解码器，照旧写法会在换片/析构时直接返回，音频线程继续跑、声音停不下来
    if (!impl_->videoDecoder && !impl_->audioDecoder)
        return;

    impl_->stopRequested.store(true);
    if (impl_->videoThread.joinable()) {
        impl_->videoThread.request_stop();
        impl_->queueSpaceAvailable.notify_all();
        impl_->queueNotEmpty.notify_all();
    }
    if (impl_->audioThread.joinable())
        impl_->audioThread.request_stop();
    impl_->seekSignal.notify_all();

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
    impl_->spectrumEnabled.store(false);
    impl_->spectrum.reset();
    impl_->currentFrame = cv::Mat();
}

bool MediaPlayer::isRunning() const noexcept {
    return impl_->videoDecoder != nullptr && !impl_->videoDecodeFinished.load();
}

bool MediaPlayer::hasFinished() const noexcept {
    if (!impl_->videoDecoder && !impl_->audioDecoder)
        return false;
    if (!impl_->videoDecodeFinished.load() || !impl_->audioDecodeFinished.load())
        return false;

    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        if (!impl_->videoQueue.empty())
            return false;
    }

    if (impl_->audio && impl_->audio->queuedFrames() > 0)
        return false;

    // 最后一帧显示完毕（给一帧的显示时间）后结束。
    // 纯音频没有帧时间戳：判据就是媒体时长——音频线程解到 EOF 后按 silenceUntilMs()
    // 把静音补到时长，队列排空时钟正好压在结尾；时长未知（0）时压住它的其实是上面的
    // "解码已结束 + 队列已播空"，不会在半路饿一下队列时误判成结尾。
    const int64_t endMs = impl_->videoDecoder ? impl_->lastFramePtsMs.load() : impl_->mediaDurationMs;
    return impl_->clockMs() >= endMs;
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

        // seek 落点：只取"覆盖目标时刻"的那一帧。取多帧的话，暂停态下会一路弹到
        // 队列里最新的一帧（目标之后一两百毫秒），画面就落在松手位置的后面了
        const bool landingOnly = impl_->landingPending.load();

        while (!impl_->videoQueue.empty() && impl_->videoQueue.front().ptsMs <= clock + kFrameLeadMs) {
            impl_->currentFrame = std::move(impl_->videoQueue.front().image);
            impl_->lastFramePtsMs.store(impl_->videoQueue.front().ptsMs);
            impl_->videoQueue.pop_front();
            impl_->presentedFrames.fetch_add(1);
            updated = true;

            if (landingOnly) {
                impl_->landingPending.store(false);
                JARK_LOG("player: 落点帧已交出 pts={}ms 时钟={}ms", impl_->lastFramePtsMs.load(), clock);
                break;
            }
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

void MediaPlayer::setSpectrumEnabled(bool enabled) noexcept {
    impl_->spectrumEnabled.store(enabled && impl_->spectrum != nullptr);
}

bool MediaPlayer::readSpectrum(std::span<float> out) const noexcept {
    if (!impl_->spectrum || !impl_->spectrumEnabled.load())
        return false;

    // 用**播放时钟**取，不是"最新那一帧"：音频线程提前约 2 秒解码，
    // 拿最新的会让频谱比听到的声音早出这么多
    return impl_->spectrum->readAt(impl_->clockMs(), out);
}

void MediaPlayer::pause() {
    if ((!impl_->videoDecoder && !impl_->audioDecoder) || impl_->paused.exchange(true))
        return;

    if (impl_->audio) {
        impl_->audio->pause(); // voice 停在原采样点，时钟跟着停
    }
    else {
        std::lock_guard<std::mutex> lock(impl_->clockMutex);
        if (impl_->sysRunning) {
            impl_->sysElapsedMs += elapsedMs(impl_->sysSegmentStart);
            impl_->sysRunning = false;
        }
    }
}

void MediaPlayer::resume() {
    if ((!impl_->videoDecoder && !impl_->audioDecoder) || !impl_->paused.exchange(false))
        return;

    if (impl_->audio) {
        impl_->audio->resume();
    }
    else {
        std::lock_guard<std::mutex> lock(impl_->clockMutex);
        impl_->sysSegmentStart = std::chrono::steady_clock::now();
        impl_->sysRunning = true;
    }
}

bool MediaPlayer::isPaused() const noexcept {
    return impl_->paused.load();
}

bool MediaPlayer::seek(int64_t ms, SeekMode mode) {
    if (!impl_->videoDecoder && !impl_->audioDecoder)
        return false;

    int64_t target = ms > 0 ? ms : 0;
    if (impl_->mediaDurationMs > 0) {
        // 有视频时目标夹到最后一帧的位置：再往后就只剩"目标之后没有帧"的空档，
        // 拖到最右端反而看不到结尾画面；纯音频没有帧，直接夹到时长
        const int64_t upper = impl_->videoDecoder
            ? (std::max)(int64_t{ 0 }, impl_->mediaDurationMs - impl_->frameStepMs)
            : impl_->mediaDurationMs;
        target = (std::min)(target, upper);
    }

    impl_->requestSeek(target, mode == SeekMode::Exact);
    return true;
}

bool MediaPlayer::seekLandingPending() const noexcept {
    return impl_->landingPending.load();
}

bool MediaPlayer::stepFrame(int direction) {
    if (!impl_->videoDecoder)
        return false;
    if (direction == 0)
        return false;

    pause();

    const int64_t step = impl_->frameStepMs > 0 ? impl_->frameStepMs : 33;
    const int64_t target = impl_->lastFramePtsMs.load() + (direction > 0 ? step : -step);
    return seek(target);
}

int64_t MediaPlayer::durationMs() const noexcept {
    return impl_->mediaDurationMs;
}

double MediaPlayer::frameDurationMs() const noexcept {
    return static_cast<double>(impl_->frameStepMs);
}

bool MediaPlayer::hasVideo() const noexcept {
    return impl_->videoDecoder != nullptr && impl_->videoDecoder->info().hasVideo;
}

int64_t MediaPlayer::currentFramePtsMs() const noexcept {
    return impl_->lastFramePtsMs.load();
}

} // namespace jark
