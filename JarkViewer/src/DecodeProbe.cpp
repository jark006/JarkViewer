#include "DecodeProbe.h"

#include "FormatSniffer.h"
#include "ImageDatabase.h"
#include "VectorImage.h"
#include "jarkUtils.h"

#include <algorithm>
#include <chrono>
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
        double elapsedMs = 0.0;
        std::string firstExifLine;
        std::string exifText;
        std::string vectorReport;
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

        result.exifBytes = imageAsset.exifInfo.size();
        result.firstExifLine = firstLineOf(imageAsset.exifInfo);
        result.exifText = imageAsset.exifInfo;

        // 解码失败时 myLoader 会返回错误提示图，比对像素指针即可识别
        const auto tipsMat = imageDatabase.getErrorTipsMat();
        result.tips = !tipsMat.empty() && !imageAsset.primaryFrame.empty() &&
            imageAsset.primaryFrame.data == tipsMat.data;

        result.vectorReport = buildVectorReport(imageAsset);

        return result;
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

    for (size_t i = 1; i < argv.size(); ++i) {
        if (argv[i] == L"--probe")
            continue;
        if (argv[i] == L"--full") {
            fullExif = true;
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

        auto line = std::format("{} sniff={:<8} {}x{} frames={} {}ms exif={}B",
            status,
            fileFormatName(result.sniffed),
            result.width,
            result.height,
            result.frameCount,
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

        emit(line);
    }

    emit(std::format("---- {} ok, {} failed, {} total ----", okCount, failCount, targets.size()));
    return failCount == 0 ? 0 : 1;
}

} // namespace jark
