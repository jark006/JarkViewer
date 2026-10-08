#include "DecodeProbe.h"

#include "FormatSniffer.h"
#include "ImageDatabase.h"
#include "AudioOutput.h"
#include "ImageAnnotator.h"
#include "Localization.h"
#include "BatchProcessor.h"
#include "CanvasRenderer.h"
#include "InfoScreen.h"
#include "NavigationOverlay.h"
#include "ThumbnailService.h"
#include <sstream>
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
        PlaceholderKind placeholder = PlaceholderKind::None;
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

        // 解码失败时 myLoader 返回空帧 + 占位类型（界面层才会渲染成画面）
        result.placeholder = imageAsset.placeholder;

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
        const uint32_t sampleIds[] = { 1, 2, 28, 39, 41, 54, 124, 126, 127, 129, 146, 149, 151, 156, 165 }; // 含新增导航/缓存/占位界面文案
        const uint32_t wideIds[] = { 1, 13, 30, 49 };            // 窗口标题/窗口创建失败/删除到回收站/批量无图提示

        const uint32_t savedLanguage = GlobalVar::settingParameter.UI_LANG;

        for (size_t index = 0; index < jark::kLanguageCount; ++index) {
            GlobalVar::settingParameter.UI_LANG = static_cast<uint32_t>(index);
            const auto language = static_cast<jark::Language>(index);

            report += std::format("\n[{}] ", jark::languageDisplayName(language));
            for (const uint32_t id : sampleIds)
                report += std::format("{} | ", getUIString(id));
            report += "\n     宽字符: ";
            for (const uint32_t id : wideIds)
                report += std::format("{} | ", jarkUtils::wstringToUtf8(getUIStringW(id).c_str()));
            report += std::format("\n     EXIF 标签文案：{}", jark::prefersChineseResources() ? "中文" : "英文");
        }

        GlobalVar::settingParameter.UI_LANG = savedLanguage;
        return report;
    }

    // 批量处理自检：对给定文件执行一次批量任务并输出结果
    std::string runBatchTest(const std::vector<std::wstring>& files, const jark::BatchOptions& options) {
        std::string report = std::format("批量任务: {} 个文件, 任务类型 {}, 输出格式 {}, 质量 {}\n",
            files.size(), static_cast<int>(options.task),
            jarkUtils::wstringToUtf8(options.outputExtension.empty() ?
                std::wstring(L"(保持原格式)") : options.outputExtension), options.jpegQuality);

        if (options.task == jark::BatchTask::Scale)
            report += std::format("缩放: 方式 {}, 算法 {}, 百分比 {}%, 宽 {}, 高 {}, 长边 {}\n",
                static_cast<int>(options.scaleMode), static_cast<int>(options.scaleAlgorithm),
                options.scalePercent, options.scaleWidth, options.scaleHeight, options.scaleMaxEdge);

        const auto begin = std::chrono::steady_clock::now();
        const auto result = jark::runBatch(files, options);
        const auto elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();

        report += std::format("成功 {} / 跳过 {} / 失败 {}，用时 {:.0f}ms\n",
            result.succeeded, result.skipped, result.failed, elapsedMs);
        for (const auto& message : result.messages)
            report += std::format("  {}\n", jarkUtils::wstringToUtf8(message));

        return report;
    }

    // 标注自检：在固定尺寸的合成底图上画一遍全部标注类型，用像素断言验证；
    // 传入真实图片时另外输出一张标注结果图，便于人眼确认。
    std::string runAnnotateTest(const std::vector<std::wstring>& files, const std::wstring& outDir) {
        const cv::Vec4b red(0, 0, 255, 255);       // 0xFFFF0000
        const cv::Vec4b background(40, 40, 40, 255);

        std::string report;
        int passed = 0;
        int failed = 0;
        auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("  [{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        auto pixelNear = [](const cv::Mat& canvas, int x, int y, const cv::Vec4b& expected, int tolerance = 24) {
            if (canvas.empty() || x < 0 || y < 0 || x >= canvas.cols || y >= canvas.rows)
                return false;
            const cv::Vec4b value = canvas.at<cv::Vec4b>(y, x);
            return std::abs(value[0] - expected[0]) <= tolerance &&
                std::abs(value[1] - expected[1]) <= tolerance &&
                std::abs(value[2] - expected[2]) <= tolerance;
            };

        cv::Mat base(600, 800, CV_8UC4, background);
        cv::Mat noise(120, 160, CV_8UC4);
        cv::randu(noise, cv::Scalar(0, 0, 0, 255), cv::Scalar(255, 255, 255, 255));
        noise.copyTo(base(cv::Rect(560, 40, 160, 120)));

        jark::AnnotatorDocument document(base);
        jark::AnnoStyle style;
        style.color = 0xFFFF0000;
        style.width = 6;

        document.begin(jark::AnnoTool::Rect, style, { 100, 100 });
        document.update({ 300, 200 });
        document.commit();
        check(pixelNear(document.flatten(), 200, 100, red), "矩形：上边中点着色");
        check(pixelNear(document.flatten(), 200, 150, background), "矩形：内部保持空心");
        check(document.annotations().size() == 1, "矩形：提交后标注数 +1");

        style.filled = true;
        document.begin(jark::AnnoTool::Rect, style, { 340, 100 });
        document.update({ 440, 200 });
        document.commit();
        check(pixelNear(document.flatten(), 390, 150, red), "矩形：填充后内部着色");
        style.filled = false;

        document.begin(jark::AnnoTool::Ellipse, style, { 200, 300 });
        document.update({ 400, 400 });
        document.commit();
        check(pixelNear(document.flatten(), 300, 300, red), "椭圆：顶点着色");
        check(pixelNear(document.flatten(), 300, 350, background), "椭圆：内部保持空心");

        document.begin(jark::AnnoTool::Line, style, { 100, 450 });
        document.update({ 300, 450 });
        document.commit();
        check(pixelNear(document.flatten(), 200, 450, red), "直线：中点着色");

        document.begin(jark::AnnoTool::Arrow, style, { 350, 400 });
        document.update({ 550, 500 });
        document.commit();
        check(pixelNear(document.flatten(), 360, 405, red), "箭头：起点附近着色");
        check(pixelNear(document.flatten(), 540, 495, red), "箭头：箭头附近着色");

        document.begin(jark::AnnoTool::Pen, style, { 100, 550 });
        document.update({ 150, 520 });
        document.update({ 200, 550 });
        document.commit();
        check(pixelNear(document.flatten(), 100, 550, red), "画笔：轨迹起点着色");
        check(pixelNear(document.flatten(), 200, 550, red), "画笔：轨迹终点着色");

        // 马赛克：区域内标准差应明显下降，且同一块内像素一致
        cv::Mat before = document.flatten();
        cv::Mat beforeRegion = before(cv::Rect(560, 40, 160, 120));
        cv::Scalar meanBefore, stddevBefore;
        cv::meanStdDev(beforeRegion, meanBefore, stddevBefore);

        jark::Annotation mosaic;
        mosaic.tool = jark::AnnoTool::Mosaic;
        mosaic.points = { { 560, 40 }, { 720, 160 } };
        mosaic.mosaicBlock = 16;
        document.begin(jark::AnnoTool::Mosaic, style, { 560, 40 });
        document.update({ 720, 160 });
        document.commit();

        cv::Mat after = document.flatten();
        cv::Mat afterRegion = after(cv::Rect(560, 40, 160, 120));
        cv::Scalar meanAfter, stddevAfter;
        cv::meanStdDev(afterRegion, meanAfter, stddevAfter);
        check(stddevAfter[0] < stddevBefore[0] * 0.6, std::format("马赛克：标准差 {:.1f} -> {:.1f}",
            stddevBefore[0], stddevAfter[0]));
        check(pixelNear(after, 562, 42, cv::Vec4b(after.at<cv::Vec4b>(42, 562)), 0), "马赛克：块内像素一致");

        style.fontSize = 48;
        document.begin(jark::AnnoTool::Text, style, { 560, 200 });
        document.setText("Ag中1");
        document.commit();
        style.fontSize = 40;
        {
            const cv::Mat flattened = document.flatten();
            int changed = 0;
            int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
            for (int y = 150; y < 400 && y < flattened.rows; ++y) {
                for (int x = 400; x < 900 && x < flattened.cols; ++x) {
                    if (pixelNear(flattened, x, y, background, 12))
                        continue;

                    minX = (std::min)(minX, x);
                    minY = (std::min)(minY, y);
                    maxX = (std::max)(maxX, x);
                    maxY = (std::max)(maxY, y);

                    if (x >= 560 && x < 560 + 132 && y >= 200 && y < 200 + 53)
                        ++changed;
                }
            }
            check(changed > 50, std::format("文字：包围盒内着色像素 {} 个（实际范围 {}x{}..{}x{}）",
                changed, minX, minY, maxX, maxY));

            if (!outDir.empty()) {
                std::error_code errorCode;
                std::filesystem::create_directories(outDir, errorCode);
                cv::imwrite((std::filesystem::path(outDir) / "synthetic.png").string(),
                    document.flatten()(cv::Rect(500, 150, 300, 150)));
            }
        }

        // 撤销 / 重做
        {
            const size_t beforeUndo = document.annotations().size();
            document.undo();
            check(document.annotations().size() == beforeUndo - 1, "撤销：标注数 -1");
            check(pixelNear(document.flatten(), 560, 200, background), "撤销：文字已消失");
            document.redo();
            check(document.annotations().size() == beforeUndo, "重做：标注数恢复");

            while (document.canUndo())
                document.undo();
            check(!document.canUndo() && document.canRedo(), "撤销到底：栈状态正确");
            document.redo();
            check(document.annotations().size() == 1, "重做一步：回到第一个标注");
        }

        // 裁剪：先重新画满，再裁到指定矩形
        {
            document.applyEdit(base); // 清空重来，保证裁剪内容可预测
            document.begin(jark::AnnoTool::Rect, style, { 100, 100 });
            document.update({ 300, 200 });
            document.commit();

            const bool ok = document.cropTo({ 50, 60, 400, 300 });
            check(ok && document.width() == 400 && document.height() == 300,
                std::format("裁剪：尺寸 -> {}x{}", document.width(), document.height()));
            check(pixelNear(document.flatten(), 150, 40, red), "裁剪：标注随内容平移");
            check(pixelNear(document.flatten(), 150, 90, background), "裁剪：内部仍空心");
            check(pixelNear(document.flatten(), 250, 90, red), "裁剪：右边缘保留");
            check(!document.canUndo(), "裁剪：历史已重置");
        }

        // 编码：png 保留 alpha，jpg 铺白底
        {
            std::vector<uint8_t> png;
            std::vector<uint8_t> jpg;
            check(jark::encodeAnnotatedImage(document.flatten(), L"png", png) && png.size() > 100, "编码：png 成功");
            check(jark::encodeAnnotatedImage(document.flatten(), L"jpg", jpg) && jpg.size() > 100, "编码：jpg 成功");

            cv::Mat pngBack = cv::imdecode(png, cv::IMREAD_UNCHANGED);
            cv::Mat jpgBack = cv::imdecode(jpg, cv::IMREAD_UNCHANGED);
            check(pngBack.type() == CV_8UC4, "编码：png 保留 4 通道");
            check(jpgBack.type() == CV_8UC3, "编码：jpg 为 3 通道");
        }

        // 透明通道：透明底图另存为 png 时透明区域必须仍然是透的（jpg 不支持，铺白底）
        {
            cv::Mat alphaBase(120, 160, CV_8UC4, cv::Scalar(0, 0, 0, 0));
            alphaBase(cv::Rect(40, 20, 80, 80)) = cv::Scalar(0, 200, 0, 255); // 不透明绿块
            jark::AnnotatorDocument transparent(alphaBase);

            std::vector<uint8_t> alphaPng;
            check(jark::encodeAnnotatedImage(transparent.flatten(), L"png", alphaPng), "透明：png 编码成功");
            cv::Mat alphaBack = cv::imdecode(alphaPng, cv::IMREAD_UNCHANGED);
            check(alphaBack.type() == CV_8UC4 && alphaBack.at<cv::Vec4b>(10, 10)[3] == 0,
                "透明：png 透明区域保持透明");
            check(alphaBack.type() == CV_8UC4 && alphaBack.at<cv::Vec4b>(60, 80)[3] == 255,
                "透明：png 不透明区域保持不透明");

            std::vector<uint8_t> alphaJpg;
            check(jark::encodeAnnotatedImage(transparent.flatten(), L"jpg", alphaJpg), "透明：jpg 编码成功");
            cv::Mat alphaJpgBack = cv::imdecode(alphaJpg, cv::IMREAD_UNCHANGED);
            check(alphaJpgBack.type() == CV_8UC3 && alphaJpgBack.at<cv::Vec3b>(10, 10)[0] > 240,
                "透明：jpg 透明区域铺白底");
        }

        // 真实图片：画一遍全部标注并保存，便于人眼确认
        if (!files.empty()) {
            const auto& path = files.front();
            cv::Mat source = cv::imread(jarkUtils::wstringToUtf8(path), cv::IMREAD_UNCHANGED);
            if (source.empty()) {
                report += std::format("  [跳过] 无法读取 {}\n", jarkUtils::wstringToUtf8(path));
            }
            else {
                jark::AnnotatorDocument visual(source);
                const int w = visual.width();
                const int h = visual.height();
                const int margin = std::max(10, std::min(w, h) / 12);

                jark::AnnoStyle s;
                s.width = std::max(2, std::min(w, h) / 150);
                s.fontSize = std::max(14, std::min(w, h) / 18);

                s.color = 0xFFFF3B30;
                visual.begin(jark::AnnoTool::Rect, s, { margin, margin });
                visual.update({ w / 2, h / 3 });
                visual.commit();

                s.color = 0xFF34C759;
                visual.begin(jark::AnnoTool::Ellipse, s, { w / 2, margin });
                visual.update({ w - margin, h / 2 });
                visual.commit();

                s.color = 0xFFFFCC00;
                visual.begin(jark::AnnoTool::Arrow, s, { w / 4, h * 3 / 4 });
                visual.update({ w * 3 / 4, h / 2 });
                visual.commit();

                s.color = 0xFF0A84FF;
                visual.begin(jark::AnnoTool::Pen, s, { margin, h - margin });
                for (int i = 1; i <= 20; ++i) {
                    const int x = margin + (w - 2 * margin) * i / 20;
                    const int y = h - margin - static_cast<int>(margin * 2 * std::sin(i * 0.5));
                    visual.update({ x, y });
                }
                visual.commit();

                s.color = 0xFFFFFFFF;
                visual.begin(jark::AnnoTool::Text, s, { margin, h / 2 });
                visual.setText("JarkViewer 标注 Annotate 123");
                visual.commit();

                const cv::Mat result = visual.flatten();
                check(!result.empty() && result.size() == source.size(), "真实图片：标注后尺寸不变");

                if (!outDir.empty()) {
                    std::error_code errorCode;
                    std::filesystem::create_directories(outDir, errorCode);
                    const auto outPath = std::filesystem::path(outDir) / "annotated.png";
                    std::vector<uint8_t> encoded;
                    if (jark::encodeAnnotatedImage(result, L"png", encoded)) {
                        std::ofstream out(outPath, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(encoded.data()),
                            static_cast<std::streamsize>(encoded.size()));
                        report += std::format("  已输出 {}\n", jarkUtils::wstringToUtf8(outPath.wstring()));
                    }
                }
            }
        }

        report = std::format("---- 标注自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    std::string runNavigationTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };
        for (int rotation = 0; rotation < 4; ++rotation) {
            ViewState view{ 1000, 600, 2 << 16, 1 << 16, 0, 0, rotation };
            const cv::Size canvasSize(801, 603);
            auto geometry = imageGeometry(view, canvasSize, { 3000, 1800 });
            check(geometry.nominalSize == (rotation & 1 ? cv::Size(600, 1000) : cv::Size(1000, 600)),
                "旋转后的名义尺寸不受 SVG 位图分辨率影响");
            for (const auto target : { cv::Point2d(0, 0), cv::Point2d(0.4, 0.7), cv::Point2d(1, 1) }) {
                const cv::Point slide = navigationSlide(geometry, canvasSize, target);
                view.slideX = slide.x;
                view.slideY = slide.y;
                const auto next = imageGeometry(view, canvasSize);
                const double halfX = canvasSize.width / next.renderedSize.width / 2;
                const double halfY = canvasSize.height / next.renderedSize.height / 2;
                check(std::abs(next.visible.x + next.visible.width / 2 - std::clamp(target.x, halfX, 1 - halfX)) < 0.002 &&
                    std::abs(next.visible.y + next.visible.height / 2 - std::clamp(target.y, halfY, 1 - halfY)) < 0.002,
                    "鸟瞰定位落点与可见区域中心一致（包括两端夹取）");
                check(next.origin.x <= 0 && next.origin.y <= 0 &&
                    next.origin.x + next.renderedSize.width >= canvasSize.width &&
                    next.origin.y + next.renderedSize.height >= canvasSize.height, "鸟瞰定位不露白边");
            }
        }
        const ViewState small{ 200, 100, 1 << 16, 1 << 16, 0, 0, 0 };
        const auto fitted = imageGeometry(small, { 800, 600 });
        check(fitted.visible == cv::Rect2d(0, 0, 1, 1), "完整可见时框覆盖整图");
        check(navigationSlide(fitted, { 800, 600 }, { 0, 1 }) == cv::Point(0, 0), "小图不产生多余平移");
        check(imageGeometry({}, { 800, 600 }).scale == 0, "空图/零缩放不除零");

        // 比较鸟瞰定位后的主画布像素，覆盖旋转采样与几何取整。
        cv::Mat source(120, 200, CV_8UC4);
        for (int y = 0; y < source.rows; ++y)
            for (int x = 0; x < source.cols; ++x)
                source.at<cv::Vec4b>(y, x) = { static_cast<uchar>(x), static_cast<uchar>(y), 160, 255 };
        for (int rotation = 0; rotation < 4; ++rotation) {
            ViewState view{ 200, 120, 1 << 16, 1 << 16, 0, 0, rotation };
            const auto slide = navigationSlide(imageGeometry(view, { 80, 60 }), { 80, 60 }, { 0.6, 0.7 });
            view.slideX = slide.x;
            view.slideY = slide.y;
            const auto geometry = imageGeometry(view, { 80, 60 });
            cv::Mat rotated;
            if (rotation == 0) rotated = source;
            else cv::rotate(source, rotated, rotation == 1 ? cv::ROTATE_90_COUNTERCLOCKWISE :
                (rotation == 2 ? cv::ROTATE_180 : cv::ROTATE_90_CLOCKWISE));
            cv::Mat canvas(60, 80, CV_8UC4);
            drawImageToCanvas(source, canvas, view);
            check(canvas.at<cv::Vec4b>(30, 40) == rotated.at<cv::Vec4b>(30 - geometry.origin.y, 40 - geometry.origin.x),
                "定位后的中心像素与旋转图像一致");
        }
        ui::NavigationOverlay overlay;
        overlay.sync({ 1000, 600, 2 << 16, 1 << 16, 0, 0, 0 }, { 1280, 800 }, 1, true, false, 1, 0);
        check(overlay.mouseDown({ 1125, 718 }, 1).handled && overlay.ownsGesture(), "鸟瞰按下取得手势");
        check(overlay.mouseMove({ -80, -60 }, false).slide.has_value(), "拖出客户区仍追踪鸟瞰拖动");
        overlay.cancel();
        check(!overlay.ownsGesture() && overlay.mouseUp(1).handled, "失捕获取消后吞掉残余抬起");
        check(!overlay.mouseDown({ 500, 400 }, 1).handled, "非浮层区域不拦截主图");
        return std::format("---- navigation: {} ok, {} failed ----\n", passed, failed) + report;
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
    bool batchTest = false;
    bool annotateTest = false;
    bool navigationTest = false;
    bool thumbnailTest = false;
    bool shellThumbnailTest = false;
    int thumbnailWriter = -1;
    std::wstring annotateOutDir;
    jark::BatchOptions batchOptions;
    bool outputFormatGiven = false;

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
        if (argv[i] == L"--batch") {
            batchTest = true;
            continue;
        }
        if (argv[i] == L"--navigation-test") {
            navigationTest = true;
            continue;
        }
        if (argv[i] == L"--thumbnail-writer" && i + 1 < argv.size()) {
            thumbnailWriter = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--thumbnail-test") {
            thumbnailTest = true;
            continue;
        }
        if (argv[i] == L"--shell-thumbnail") {
            shellThumbnailTest = true;
            continue;
        }
        if (argv[i] == L"--annotate") {
            annotateTest = true;
            continue;
        }
        if (argv[i] == L"--annotate-out" && i + 1 < argv.size()) {
            annotateOutDir = argv[++i];
            continue;
        }
        if (argv[i] == L"--out-dir" && i + 1 < argv.size()) {
            batchOptions.outputDirectory = argv[++i];
            continue;
        }
        if (argv[i] == L"--to" && i + 1 < argv.size()) {
            batchOptions.outputExtension = argv[++i];
            outputFormatGiven = true;
            continue;
        }
        if (argv[i] == L"--scale-percent" && i + 1 < argv.size()) {
            batchOptions.task = jark::BatchTask::Scale;
            batchOptions.scaleMode = jark::ScaleMode::Percent;
            batchOptions.scalePercent = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--scale-width" && i + 1 < argv.size()) {
            batchOptions.task = jark::BatchTask::Scale;
            batchOptions.scaleMode = jark::ScaleMode::Width;
            batchOptions.scaleWidth = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--scale-height" && i + 1 < argv.size()) {
            batchOptions.task = jark::BatchTask::Scale;
            batchOptions.scaleMode = jark::ScaleMode::Height;
            batchOptions.scaleHeight = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--scale-size" && i + 1 < argv.size()) {
            const std::wstring value = argv[++i];
            const size_t separator = value.find_first_of(L"xX*");
            if (separator != std::wstring::npos) {
                batchOptions.task = jark::BatchTask::Scale;
                batchOptions.scaleMode = jark::ScaleMode::Stretch;
                batchOptions.scaleWidth = ::_wtoi(value.substr(0, separator).c_str());
                batchOptions.scaleHeight = ::_wtoi(value.substr(separator + 1).c_str());
            }
            continue;
        }
        if (argv[i] == L"--max-edge" && i + 1 < argv.size()) {
            batchOptions.task = jark::BatchTask::Scale;
            batchOptions.scaleMode = jark::ScaleMode::LongEdge;
            batchOptions.scaleMaxEdge = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--scale-algo" && i + 1 < argv.size()) {
            batchOptions.scaleAlgorithm = static_cast<jark::ScaleAlgorithm>(
                std::clamp(::_wtoi(argv[++i].c_str()), 0, 5));
            continue;
        }
        if (argv[i] == L"--quality" && i + 1 < argv.size()) {
            batchOptions.jpegQuality = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--rotate" && i + 1 < argv.size()) {
            batchOptions.task = jark::BatchTask::Rotate;
            batchOptions.rotationDegrees = ::_wtoi(argv[++i].c_str());
            continue;
        }
        if (argv[i] == L"--flip-h") {
            batchOptions.task = jark::BatchTask::Rotate;
            batchOptions.flipHorizontal = true;
            continue;
        }
        if (argv[i] == L"--flip-v") {
            batchOptions.task = jark::BatchTask::Rotate;
            batchOptions.flipVertical = true;
            continue;
        }
        if (argv[i] == L"--gray") {
            batchOptions.applyAdjustments = true;
            batchOptions.colorMode = 1;
            continue;
        }
        if (argv[i] == L"--invert") {
            batchOptions.applyAdjustments = true;
            batchOptions.invertColors = true;
            continue;
        }
        if (argv[i] == L"--rename") {
            batchOptions.task = jark::BatchTask::Rename;
            if (i + 1 < argv.size() && argv[i + 1].starts_with(L"--") == false) {
                batchOptions.renamePrefix = argv[++i];
            }
            continue;
        }
        if (argv[i] == L"--overwrite") {
            batchOptions.overwrite = true;
            continue;
        }
        if (argv[i] == L"--out" && i + 1 < argv.size()) {
            reportPath = argv[++i];
            continue;
        }
        targets.push_back(argv[i]);
    }

    // 标注自检不需要输入文件（用合成底图断言），用真实图片只是额外输出可视化结果
    if (targets.empty() && !annotateTest && !languageTest && !navigationTest && !thumbnailTest && thumbnailWriter < 0) {
        std::println("usage: JarkViewer.exe --probe <file> [<file>...] [--out <report>]");
        return 2;
    }

    std::ofstream report(reportPath, std::ios::binary | std::ios::trunc);
    auto emit = [&](std::string_view line) {
        std::println("{}", line);
        if (report.is_open())
            report.write(line.data(), static_cast<std::streamsize>(line.size())) << '\n';
    };

    if (navigationTest) {
        const auto text = runNavigationTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (thumbnailTest || shellThumbnailTest || thumbnailWriter >= 0) {
        // probe 在 GUI 设置初始化前运行，明确使用独立测试目录，不迁移或清理用户缓存。
        const std::filesystem::path directory = batchOptions.outputDirectory.empty()
            ? std::filesystem::temp_directory_path() / L"JarkViewer-thumbnail-tests"
            : std::filesystem::path(batchOptions.outputDirectory);
        std::filesystem::create_directories(directory);
        std::ostringstream output;
        bool ok = true;
        if (thumbnailWriter >= 0)
            ok = !batchOptions.outputDirectory.empty() && runThumbnailCacheWriter(output, directory, thumbnailWriter);
        else if (thumbnailTest)
            ok = runThumbnailCacheTests(output, directory);
        else {
            size_t index = 0;
            for (const auto& path : targets)
                ok = probeShellThumbnail(path, directory / std::format(L"shell-{}.png", index++), output) && ok;
        }
        emit(output.str());
        return ok ? 0 : 1;
    }

    if (languageTest) {
        emit(runLanguageTest());
        return 0;
    }

    if (annotateTest) {
        const auto text = runAnnotateTest(targets, annotateOutDir);
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }

    if (batchTest) {
        if (targets.empty()) {
            std::println("usage: JarkViewer.exe --probe --batch <file...> [--out-dir 目录] [--to 格式] [--quality N] [--rotate 90|180|270] [--flip-h|--flip-v] [--gray] [--invert] [--rename 前缀] [--overwrite]\n"
                "       缩放: [--scale-percent N | --scale-width N | --scale-height N | --scale-size 宽x高 | --max-edge N] [--scale-algo 0..5]");
            return 2;
        }
        // 缩放任务不指定 --to 时保持原格式（与界面一致）
        if (!outputFormatGiven && batchOptions.task == jark::BatchTask::Scale)
            batchOptions.outputExtension.clear();

        const auto text = runBatchTest(targets, batchOptions);
        emit(text);
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
        const bool filled = result.decodeOk;
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
        if (result.placeholder != PlaceholderKind::None)
            line += std::format(" placeholder={}", placeholderName(result.placeholder));

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
