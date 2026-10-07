#include "DecodeProbe.h"

#include "FormatSniffer.h"
#include "ImageDatabase.h"
#include "AudioOutput.h"
#include "Localization.h"
#include "MediaDecoder.h"
#include "VectorImage.h"
#include "jarkUtils.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <windows.h>

namespace jark {
namespace {

    struct ProbeResult {
        bool sniffOk = false;
        FileFormat sniffed = FileFormat::Unknown;
        bool decodeOk = false;
        const char* kind = "None";
        int width = 0;
        int height = 0;
        size_t frameCount = 0;
        long long frameDurationMs = 0;
        size_t exifBytes = 0;
        bool tips = false;
        int orientation = 1;
        double elapsedMs = 0.0;
        std::string firstExifLine;
        std::string exifText;
        std::string vectorReport;
        std::string mediaReport;
    };

    std::string utf8(std::wstring_view text) {
        return jarkUtils::wstringToUtf8(text);
    }

    std::span<const uint8_t> readHead(const std::wstring& path, std::vector<uint8_t>& storage, size_t maxBytes) {
        storage.clear();

        std::error_code errorCode;
        const auto size = std::filesystem::file_size(path, errorCode);
        if (errorCode || size == 0)
            return {};

        const auto readSize = static_cast<size_t>(std::min<uintmax_t>(size, maxBytes));
        storage.resize(readSize);

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};

        file.read(reinterpret_cast<char*>(storage.data()), static_cast<std::streamsize>(readSize));
        storage.resize(static_cast<size_t>(file.gcount()));
        return storage;
    }

    std::string firstLineOf(const std::string& text) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end == std::string::npos ? text.size() : end);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        return line;
    }

    // 视频/音频自检：打开 MediaDecoder 并统计能解出的视频帧与音频样本
    std::string buildMediaReport(const std::wstring& path, jark::FileFormat sniffed) {
        if (sniffed != jark::FileFormat::Video)
            return {};

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return {};

        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

        auto decoder = MediaDecoder::open(data);
        if (!decoder)
            return "\n            | media: 无法打开";

        const auto& info = decoder->info();
        std::string report = std::format(
            "\n            | media: video={} {}x{} rot={} fps={:.1f} | audio={} {}Hz {}ch | dur={}ms",
            info.hasVideo, info.width, info.height, info.rotationDegrees, info.frameRate,
            info.hasAudio, info.audioSampleRate, info.audioChannels, info.durationMs);

        const auto begin = std::chrono::steady_clock::now();
        size_t videoFrames = 0;
        size_t audioSamples = 0;
        size_t audioChunks = 0;
        int64_t lastVideoPts = 0;
        int64_t lastAudioPts = 0;

        MediaDecoder::Chunk chunk;
        while (decoder->readNext(chunk)) {
            if (chunk.type == MediaDecoder::Chunk::Type::Video) {
                ++videoFrames;
                lastVideoPts = chunk.ptsMs;
            }
            else if (chunk.type == MediaDecoder::Chunk::Type::Audio) {
                ++audioChunks;
                audioSamples += chunk.audio.size() / MediaDecoder::kOutputChannels;
                lastAudioPts = chunk.ptsMs;
            }
        }

        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();

        report += std::format(
            "\n            | decoded: {} frames (last {}ms), {} audio samples in {} chunks (last {}ms, {:.0f}ms)",
            videoFrames, lastVideoPts, audioSamples, audioChunks, lastAudioPts, elapsedMs);

        return report;
    }

    // 模拟不同缩放级别，验证矢量图的按需光栅化（目标分辨率、滞后策略、耗时）
    std::string buildVectorReport(ImageAsset& imageAsset) {
        const auto& vectorImage = imageAsset.vectorSource;
        if (!vectorImage)
            return {};

        std::string report = std::format("\n            | vector: intrinsic={}x{} initial={}x{}",
            vectorImage->intrinsicWidth, vectorImage->intrinsicHeight,
            vectorImage->rasterWidth, vectorImage->rasterHeight);

        for (const double scale : { 0.25, 1.0, 2.0, 8.0 }) {
            const auto zoom = static_cast<int64_t>(std::llround(scale * ZOOM_BASE));
            const int targetEdge = vectorTargetEdge(imageAsset, zoom, ZOOM_BASE);

            const auto begin = std::chrono::steady_clock::now();
            const bool refreshed = refreshVectorRaster(imageAsset, targetEdge);
            const auto end = std::chrono::steady_clock::now();
            const auto elapsedMs = std::chrono::duration<double, std::milli>(end - begin).count();

            report += std::format("\n            | zoom {:>4.0f}% -> target={} raster={}x{} {} {:.0f}ms",
                scale * 100.0, targetEdge,
                vectorImage->rasterWidth, vectorImage->rasterHeight,
                refreshed ? "rendered" : "kept", elapsedMs);
        }

        return report;
    }

    ProbeResult probeOne(ImageDatabase& imageDatabase, const std::wstring& path) {
        ProbeResult result;

        // 嗅探用于自检的文件头（最多 1MiB，TGA 需要文件尾故尽可能多读）
        std::vector<uint8_t> head;
        const auto headView = readHead(path, head, 1u << 20);
        if (!headView.empty()) {
            result.sniffOk = true;
            result.sniffed = sniffFileFormat(headView);
        }

        const auto begin = std::chrono::steady_clock::now();
        auto imageAsset = imageDatabase.loader(path);
        const auto end = std::chrono::steady_clock::now();
        result.elapsedMs = std::chrono::duration<double, std::milli>(end - begin).count();

        result.decodeOk = imageAsset.format != ImageFormat::None &&
            (!imageAsset.primaryFrame.empty() || !imageAsset.frames.empty());

        const bool isAnimated = imageAsset.format == ImageFormat::Animated;
        result.kind = isAnimated ? "Animated" : "Still";

        const cv::Mat& ref = imageAsset.primaryFrame.empty()
            ? (imageAsset.frames.empty() ? cv::Mat() : imageAsset.frames.front())
            : imageAsset.primaryFrame;
        result.width = ref.empty() ? 0 : ref.cols;
        result.height = ref.empty() ? 0 : ref.rows;

        result.frameCount = imageAsset.frames.empty() ? (imageAsset.primaryFrame.empty() ? 0 : 1) : imageAsset.frames.size();
        for (const auto duration : imageAsset.frameDurations)
            result.frameDurationMs += duration;

        result.orientation = imageAsset.orientation;
        result.exifBytes = imageAsset.exifInfo.size();
        result.firstExifLine = firstLineOf(imageAsset.exifInfo);
        result.exifText = imageAsset.exifInfo;

        // 解码失败时 myLoader 会返回错误提示图，比对像素指针即可识别
        const auto tipsMat = imageDatabase.getErrorTipsMat();
        result.tips = !tipsMat.empty() && !imageAsset.primaryFrame.empty() &&
            imageAsset.primaryFrame.data == tipsMat.data;

        result.vectorReport = buildVectorReport(imageAsset);
        result.mediaReport = buildMediaReport(path, result.sniffed);

        return result;
    }

} // namespace

namespace {

    // 音频输出自检：把文件的音频提交给 XAudio2（音量 0，不发声），
    // 观察播放时钟是否按采样率推进——用于无人耳参与时验证音频链路。
    // 语言自检：逐一切换语言并打印若干条文案，验证字符串表与回退逻辑
    std::string runLanguageTest() {
        std::string report;
        const uint32_t sampleIds[] = { 1, 2, 28, 39, 41, 54 };   // 设置/常规/语言/路径/分辨率/优先1:1
        const uint32_t wideIds[] = { 1, 13, 30 };                // 窗口标题/窗口创建失败/删除到回收站

        const uint32_t savedLanguage = GlobalVar::settingParameter.UI_LANG;

        for (size_t index = 0; index < jark::kLanguageCount; ++index) {
            GlobalVar::settingParameter.UI_LANG = static_cast<uint32_t>(index);
            const auto language = static_cast<jark::Language>(index);

            report += std::format("\n[{}] ", jark::languageDisplayName(language));
            for (const uint32_t id : sampleIds)
                report += std::format("{} | ", getUIString(id));
            report += "\n     宽字符: ";
            for (const uint32_t id : wideIds)
                report += std::format("{} | ", jarkUtils::wstringToUtf8(getUIStringW(id)));
            report += std::format("\n     资源图使用{}文案", jark::prefersChineseResources() ? "中文" : "英文");
        }

        GlobalVar::settingParameter.UI_LANG = savedLanguage;
        return report;
    }

    std::string runAudioTest(const std::wstring& path) {
        std::string report;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return std::format("cannot open {}", jarkUtils::wstringToUtf8(path));

        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

        auto decoder = MediaDecoder::open(data);
        if (!decoder || !decoder->info().hasAudio)
            return std::format("no audio stream in {}", jarkUtils::wstringToUtf8(path));

        auto output = AudioOutput::create();
        if (!output)
            return "AudioOutput::create failed (no output device?)";

        output->setVolume(0.0f); // 静音，只看时钟

        std::vector<int16_t> pending;
        MediaDecoder::Chunk chunk;
        bool submittedAll = false;
        const auto begin = std::chrono::steady_clock::now();
        int64_t lastPlayed = 0;

        while (true) {
            const auto elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();

            if (!submittedAll) {
                while (output->queuedFrames() < AudioOutput::kMaxQueuedFrames && decoder->readNext(chunk)) {
                    if (chunk.type == MediaDecoder::Chunk::Type::Audio)
                        output->submit(chunk.audio);
                }
                if (chunk.type == MediaDecoder::Chunk::Type::End)
                    submittedAll = true;
            }

            const int64_t played = output->playedFrames();
            if (played != lastPlayed) {
                report += std::format("[{:>6.0f}ms] played={} frames ({:.2f}s), queued={}\n",
                    elapsedMs, played, static_cast<double>(played) / 48000.0, output->queuedFrames());
                lastPlayed = played;
            }

            if (elapsedMs > 1500.0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const auto totalMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        const int64_t played = output->playedFrames();
        const double expectedFrames = totalMs / 1000.0 * 48000.0;
        const double ratio = expectedFrames > 0 ? played / expectedFrames : 0.0;

        report += std::format("[audio-test] 用时 {:.0f}ms, 已播放 {} 帧 ({:.2f}s), 期望约 {:.0f} 帧, 比值 {:.3f}",
            totalMs, played, played / 48000.0, expectedFrames, ratio);

        return report;
    }

} // namespace

int runDecodeProbe(const std::vector<std::wstring>& argv) {
    // GUI 子系统默认没有控制台，附着到父进程控制台以便直接看到结果
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        ::SetConsoleOutputCP(CP_UTF8);
    }

    std::vector<std::wstring> targets;
    std::wstring reportPath = L"decode-probe.txt";
    bool fullExif = false;
    bool audioTest = false;
    bool languageTest = false;

    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] == L"--probe")
            continue;
        if (argv[i] == L"--full") {
            fullExif = true;
            continue;
        }
        if (argv[i] == L"--audio-test") {
            audioTest = true;
            continue;
        }
        if (argv[i] == L"--lang-test") {
            languageTest = true;
            continue;
        }
        if (argv[i] == L"--out" && i + 1 < argv.size()) {
            reportPath = argv[++i];
            continue;
        }
        targets.push_back(argv[i]);
    }

    if (targets.empty()) {
        std::println("usage: JarkViewer.exe --probe <file> [<file>...] [--out <report>]");
        return 2;
    }

    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    auto emit = [&](std::string_view line) {
        std::println("{}", line);
        if (report.is_open())
            report.write(line.data(), static_cast<std::streamsize>(line.size())) << '\n';
    };

    if (languageTest) {
        emit(runLanguageTest());
        return 0;
    }

    if (audioTest) {
        const auto text = runAudioTest(targets.front());
        emit(text);
        return text.find("比值") == std::string::npos ? 1 : 0;
    }

    ImageDatabase imageDatabase;
    size_t okCount = 0;
    size_t failCount = 0;

    for (const auto& target : targets) {
        const auto result = probeOne(imageDatabase, target);
        const bool filled = result.decodeOk && !result.tips;
        filled ? ++okCount : ++failCount;

        std::string status = filled
            ? std::format("[{:>8}]", result.kind)
            : std::string("[  FAILED]");

        auto line = std::format("{} sniff={:<8} {}x{} frames={} ori={} {}ms exif={}B",
            status,
            fileFormatName(result.sniffed),
            result.width,
            result.height,
            result.frameCount,
            result.orientation,
            static_cast<int>(result.elapsedMs),
            result.exifBytes);

        if (filled && result.frameCount > 1)
            line += std::format(" total={}ms", result.frameDurationMs);
        if (result.tips)
            line += " TIPS";

        line += " | " + utf8(target);
        if (fullExif) {
            for (const auto& exifLine : std::views::split(result.exifText, '\n'))
                line += std::format("\n            | {:.{}}", std::string_view(exifLine), 200);
        }
        else if (!result.firstExifLine.empty()) {
            line += std::format("\n            | {}", result.firstExifLine);
        }

        // 矢量图：模拟若干缩放级别，验证按需光栅化的目标分辨率与耗时
        if (filled && !result.vectorReport.empty())
            line += result.vectorReport;
        if (!result.mediaReport.empty())
            line += result.mediaReport;

        emit(line);
    }

    emit(std::format("---- {} ok, {} failed, {} total ----", okCount, failCount, targets.size()));
    return failCount == 0 ? 0 : 1;
}

} // namespace jark
