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
    // 内存始终由本线程释放：XAudio2 的 FlushSourceBuffers 不保证回调，
    // 因此只把「已播完」标记出来，再由提交线程回收，避免悬空指针。
    struct SubmittedBuffer {
        std::vector<int16_t> samples;
        std::atomic<bool> consumed{ false };
    };

} // namespace

class AudioOutputVoiceCallback : public IXAudio2VoiceCallback {
public:
    void setOwner(std::mutex* mutex,
        std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>>* pending) noexcept {
        pendingMutex = mutex;
        pendingBuffers = pending;
    }

    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        if (!context || !pendingMutex || !pendingBuffers)
            return;

        std::lock_guard<std::mutex> lock(*pendingMutex);
        if (const auto it = pendingBuffers->find(context); it != pendingBuffers->end())
            it->second->consumed.store(true);
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
};

struct AudioOutput::Impl {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    IXAudio2SourceVoice* sourceVoice = nullptr;
    AudioOutputVoiceCallback callback;

    std::mutex pendingMutex;
    std::unordered_map<void*, std::shared_ptr<SubmittedBuffer>> pendingBuffers;

    int sampleRate = 48000;
    int channels = 2;
    std::atomic<int64_t> submittedFrames{ 0 };
    bool started = false;

    // 回收已完成（或已冲掉）的缓冲区，只能由提交线程调用
    void reclaimBuffers(bool reclaimAll) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto it = pendingBuffers.begin(); it != pendingBuffers.end();) {
            if (reclaimAll || it->second->consumed.load())
                it = pendingBuffers.erase(it);
            else
                ++it;
        }
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    stop();
    impl_->reclaimBuffers(true);

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

    impl.callback.setOwner(&impl.pendingMutex, &impl.pendingBuffers);
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
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingBuffers.emplace(buffer.get(), buffer);
    }

    const HRESULT hr = impl_->sourceVoice->SubmitSourceBuffer(&audioBuffer);
    if (FAILED(hr)) {
        JARK_LOG("SubmitSourceBuffer failed: {}", hresultToText(hr));
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingBuffers.erase(buffer.get());
        return false;
    }

    impl_->submittedFrames.fetch_add(static_cast<int64_t>(frames));

    if (!impl_->started) {
        impl_->sourceVoice->Start(0);
        impl_->started = true;
    }

    impl_->reclaimBuffers(false);
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

void AudioOutput::setVolume(float volume) noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->sourceVoice->SetVolume(volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
}

void AudioOutput::stop() noexcept {
    if (!impl_->sourceVoice)
        return;

    impl_->sourceVoice->Stop(0);
    impl_->sourceVoice->FlushSourceBuffers();
    impl_->started = false;
    impl_->submittedFrames.store(0);
    impl_->reclaimBuffers(true);
}

} // namespace jark
