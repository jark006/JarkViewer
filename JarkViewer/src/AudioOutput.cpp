#include "AudioOutput.h"

#include "jarkUtils.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")

namespace jark {
namespace {

    std::string hresultToText(HRESULT hr) {
        return std::format("0x{:08X}", static_cast<unsigned>(hr));
    }

    // 已提交给 XAudio2 的缓冲区。
    // 内存始终由提交线程释放（回调线程只做标记），而且**只能**在 OnBufferEnd 到过之后释放：
    // 这份内存此刻正在被音频线程读（XAudio2 是照着 pAudioData 直接取样的，不拷贝）。
    struct SubmittedBuffer {
        std::vector<int16_t> samples;
        std::atomic<bool> consumed{ false };
    };

} // namespace

class AudioOutputVoiceCallback : public IXAudio2VoiceCallback {
public:
    void setOwner(std::mutex* mutex,
        std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>>* pending,
        std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>>* retired) noexcept {
        pendingMutex = mutex;
        pendingBuffers = pending;
        retiredBuffers = retired;
    }

    // 音频线程回调：此刻 XAudio2 已经不再读这块内存（缓冲区被取走/播完才会回调），
    // 所以这里可以安全地放它走。还在排队的只做个记号（由提交线程回收，免得
    // 在音频线程里做分配/释放）；已经被冲掉的直接就地释放。
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        if (!context || !pendingMutex || !pendingBuffers || !retiredBuffers)
            return;

        std::lock_guard<std::mutex> lock(*pendingMutex);
        if (const auto it = pendingBuffers->find(context); it != pendingBuffers->end()) {
            it->second->consumed.store(true);
            return;
        }
        retiredBuffers->erase(context);
    }

    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}

private:
    std::mutex* pendingMutex = nullptr;
    std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>>* pendingBuffers = nullptr;
    std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>>* retiredBuffers = nullptr;
};

struct AudioOutput::Impl {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    IXAudio2SourceVoice* sourceVoice = nullptr;
    AudioOutputVoiceCallback callback;

    std::mutex pendingMutex;
    // 还挂在 voice 队列里、可能仍在被取样的缓冲区（背压按这个计数）
    std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>> pendingBuffers;
    // 已经被 Stop/FlushSourceBuffers 拿掉、但还没收到 OnBufferEnd 的缓冲区。
    // 它们不再计入队列长度（冲掉就等于不播了），但内存还必须留着——
    // FlushSourceBuffers **不会**移除正在播的那一个（文档原文："If the voice is started,
    // the buffer that is currently playing is not removed from the queue"），它的数据
    // 此刻仍在被音频线程读。实测无条件释放会崩在 XAudio2 混音线程里：
    //   ucrtbase!memcpy ← LEAPCORE::CSWVoice::Process ← CGraphManager::GraphThreadProc
    //   OAPIPELINE::MatrixMixFromInt16DiagonalAvx ← LEAPFX::CAudioSRC::Process
    // 读的正是已回收的 pAudioData。所以只等 OnBufferEnd（文档保证每个被取走/播完的
    // 缓冲区都会回调一次），实在等不到的由引擎销毁时的 releaseAllBuffers() 兜底。
    std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>> retiredBuffers;

    int sampleRate = 48000;
    int channels = 2;
    std::atomic<int64_t> submittedFrames{ 0 };
    bool started = false;
    // 提交时是否允许自动起播：暂停期间提交（解码线程会把队列填到配额）不能让 voice 自己跑起来
    bool autoStart = true;
    bool submitFailureLogged = false;

    // 回收「XAudio2 已经明确用完」（OnBufferEnd 到过）的缓冲区，只能由提交线程调用
    void reclaimBuffers() {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto it = pendingBuffers.begin(); it != pendingBuffers.end();) {
            if (it->second->consumed.load())
                it = pendingBuffers.erase(it);
            else
                ++it;
        }
    }

    // 冲队列：把还在排队的缓冲区挪进「已退休」，等各自的 OnBufferEnd 上门再释放
    void retirePendingBuffers() {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto& entry : pendingBuffers)
            retiredBuffers.emplace(entry.first, std::move(entry.second));
        pendingBuffers.clear();
    }

    // 最后清账：只有 XAudio2 引擎整个销毁之后才允许这么干（那时音频线程已经不存在，
    // 没人再读这些缓冲区）。漏掉这一步会有清理不掉的尾巴，但绝不会悬空。
    void releaseAllBuffers() {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingBuffers.clear();
        retiredBuffers.clear();
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    stop();

    if (impl_->sourceVoice) {
        impl_->sourceVoice->DestroyVoice();
        impl_->sourceVoice = nullptr;
    }
    if (impl_->masteringVoice) {
        impl_->masteringVoice->DestroyVoice();
        impl_->masteringVoice = nullptr;
    }
    if (impl_->engine) {
        impl_->engine->Release();
        impl_->engine = nullptr;
    }

    // 顺序不能提前：引擎（音频线程）还在的时候释放，读的就是已回收的内存
    impl_->releaseAllBuffers();
}

std::unique_ptr<AudioOutput> AudioOutput::create(int sampleRate, int channels) {
    auto output = std::unique_ptr<AudioOutput>(new AudioOutput());
    auto& impl = *output->impl_;

    impl.sampleRate = sampleRate;
    impl.channels = channels;

    HRESULT hr = XAudio2Create(&impl.engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !impl.engine) {
        JARK_LOG("XAudio2Create failed: {}", hresultToText(hr));
        return nullptr;
    }

    hr = impl.engine->CreateMasteringVoice(&impl.masteringVoice, static_cast<UINT32>(channels),
        static_cast<UINT32>(sampleRate));
    if (FAILED(hr) || !impl.masteringVoice) {
        JARK_LOG("CreateMasteringVoice failed: {}", hresultToText(hr));
        return nullptr;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * sizeof(int16_t));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    impl.callback.setOwner(&impl.pendingMutex, &impl.pendingBuffers, &impl.retiredBuffers);
    hr = impl.engine->CreateSourceVoice(&impl.sourceVoice, &format, 0,
        XAUDIO2_DEFAULT_FREQ_RATIO, &impl.callback);
    if (FAILED(hr) || !impl.sourceVoice) {
        JARK_LOG("CreateSourceVoice failed: {}", hresultToText(hr));
        return nullptr;
    }

    JARK_LOG("audio output ready: {}Hz {}ch", sampleRate, channels);
    return output;
}

bool AudioOutput::submit(std::span<const int16_t> samples) {
    if (!impl_->sourceVoice || samples.empty())
        return false;

    const size_t frames = samples.size() / static_cast<size_t>(impl_->channels);
    if (frames == 0)
        return false;

    auto buffer = std::make_shared<SubmittedBuffer>();
    buffer->samples.assign(samples.begin(), samples.end());

    XAUDIO2_BUFFER audioBuffer{};
    audioBuffer.Flags = 0;
    audioBuffer.AudioBytes = static_cast<UINT32>(buffer->samples.size() * sizeof(int16_t));
    audioBuffer.pAudioData = reinterpret_cast<const BYTE*>(buffer->samples.data());
    audioBuffer.pContext = buffer.get();

    {
        // 键是 SubmittedBuffer 自己的地址：只要「还没被回收的缓冲区」不被释放，
        // 这个地址就不会被新对象复用，emplace 也就不会因为撞键而丢掉新缓冲区
        // （丢了的话新缓冲区在 submit 返回时就被销毁，而 pAudioData 已经交给 XAudio2 了）
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingBuffers.emplace(buffer.get(), buffer);
    }

    const HRESULT hr = impl_->sourceVoice->SubmitSourceBuffer(&audioBuffer);
    if (FAILED(hr)) {
        // 调用方会重试，日志只留第一次，免得刷屏
        if (!impl_->submitFailureLogged) {
            impl_->submitFailureLogged = true;
            JARK_LOG("SubmitSourceBuffer failed: {}", hresultToText(hr));
        }
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingBuffers.erase(buffer.get());
        return false;
    }

    impl_->submittedFrames.fetch_add(static_cast<int64_t>(frames));

    if (!impl_->started && impl_->autoStart) {
        impl_->sourceVoice->Start(0);
        impl_->started = true;
    }

    impl_->reclaimBuffers();
    return true;
}

int64_t AudioOutput::playedFrames() const noexcept {
    if (!impl_->sourceVoice)
        return 0;

    XAUDIO2_VOICE_STATE state{};
    impl_->sourceVoice->GetState(&state);
    return static_cast<int64_t>(state.SamplesPlayed);
}

int64_t AudioOutput::queuedFrames() const noexcept {
    const int64_t queued = impl_->submittedFrames.load() - playedFrames();
    return queued > 0 ? queued : 0;
}

size_t AudioOutput::queuedBuffers() {
    // 先把已经播完的记号清掉，剩下的才是真正还挂在 XAudio2 队列里的
    impl_->reclaimBuffers();

    std::lock_guard<std::mutex> lock(impl_->pendingMutex);
    return impl_->pendingBuffers.size();
}

void AudioOutput::setVolume(float volume) noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->sourceVoice->SetVolume(volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
}

void AudioOutput::pause() noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->autoStart = false;
    if (impl_->started) {
        impl_->sourceVoice->Stop(0); // 位置保留：Start 之后从同一个采样点继续
        impl_->started = false;
    }
}

void AudioOutput::resume() noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->autoStart = true;
    if (!impl_->started) {
        impl_->sourceVoice->Start(0);
        impl_->started = true;
    }
}

void AudioOutput::flush() noexcept {
    if (!impl_->sourceVoice)
        return;

    const bool wasPlaying = impl_->started;

    impl_->sourceVoice->Stop(0);
    impl_->sourceVoice->FlushSourceBuffers();
    impl_->started = false;
    impl_->submittedFrames.store(0);
    // 冲掉的只是「不再播」的记账；内存要等各自的 OnBufferEnd（见 retiredBuffers）
    impl_->retirePendingBuffers();

    // 暂停中冲队列不能顺手把播放恢复起来，否则暂停态 seek 会自己出声
    if (wasPlaying && impl_->autoStart) {
        impl_->sourceVoice->Start(0);
        impl_->started = true;
    }
}

void AudioOutput::stop() noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->sourceVoice->Stop(0);
    impl_->sourceVoice->FlushSourceBuffers();
    impl_->started = false;
    impl_->autoStart = true; // 停掉之后回到默认语义：再提交就播
    impl_->submittedFrames.store(0);
    impl_->retirePendingBuffers(); // 同上：剩下的等 OnBufferEnd，或等析构时引擎销毁
}

} // namespace jark
