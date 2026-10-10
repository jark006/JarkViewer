#include "DecodeProbe.h"

#include "FormatSniffer.h"
#include "ImageDatabase.h"
#include "ColorManager.h"
#include "AudioOutput.h"
#include "AudioSpectrumAnalyzer.h"
#include "lcms2.h"
#include "ImageAnnotator.h"
#include "Localization.h"
#include "BatchProcessor.h"
#include "CanvasRenderer.h"
#include "ImageResampler.h"
#include "InfoScreen.h"
#include "NavigationOverlay.h"
#include "ThumbnailService.h"
#include <sstream>
#include "MediaDecoder.h"
#include "MediaPlayer.h"
#include "VectorImage.h"
#include "VideoPlayback.h"
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
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

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

    // 进程内抓崩溃现场：异常时自己写一份完整内存 dump。
    // 为什么不用工具 attach：这里的崩溃都是时序竞态（正常跑约五次一崩，attach cdb 之后
    // 基本不复现），让进程自己在异常处理里落盘才不会把竞态关掉。
    // 用 JARKVIEWER_CRASH_DUMP=<文件路径> 打开；只在 --probe 里装，不影响正常使用。
    std::wstring crashDumpPath;

    LONG WINAPI crashDumpFilter(EXCEPTION_POINTERS* exceptionPointers) {
        if (crashDumpPath.empty())
            return EXCEPTION_CONTINUE_SEARCH;

        HANDLE file = CreateFileW(crashDumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return EXCEPTION_CONTINUE_SEARCH;

        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = exceptionPointers;
        exceptionInfo.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithUnloadedModules),
            &exceptionInfo, nullptr, nullptr);
        CloseHandle(file);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void installCrashDumpHandler() {
        wchar_t buffer[MAX_PATH * 2] = {};
        if (GetEnvironmentVariableW(L"JARKVIEWER_CRASH_DUMP", buffer, static_cast<DWORD>(std::size(buffer))) == 0)
            return;

        crashDumpPath = buffer;
        SetUnhandledExceptionFilter(crashDumpFilter);
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

    // 视频/音频自检：打开 MediaDecoder 并统计能解出的视频帧与音频样本。
    // decodeAll=false 只报头部信息（实况照片里的视频用它：尺寸/旋转一眼可见，
    // 又不至于把整段视频解一遍拖慢自检）
    std::string buildMediaReport(std::span<const uint8_t> data, bool decodeAll = true) {
        auto decoder = MediaDecoder::open(data);
        if (!decoder)
            return "\n            | media: 无法打开";

        const auto& info = decoder->info();
        std::string report = std::format(
            "\n            | media: video={} 显示 {}x{} (编码 {}x{}) rot={} fps={:.1f} | audio={} {}Hz {}ch | dur={}ms",
            info.hasVideo, info.displayWidth(), info.displayHeight(), info.width, info.height,
            info.rotationDegrees, info.frameRate,
            info.hasAudio, info.audioSampleRate, info.audioChannels, info.durationMs);

        const auto begin = std::chrono::steady_clock::now();
        size_t videoFrames = 0;
        size_t audioSamples = 0;
        size_t audioChunks = 0;
        int64_t lastVideoPts = 0;
        int64_t lastAudioPts = 0;
        int firstFrameWidth = 0;
        int firstFrameHeight = 0;

        MediaDecoder::Chunk chunk;
        while (decoder->readNext(chunk)) {
            if (chunk.type == MediaDecoder::Chunk::Type::Video) {
                if (firstFrameWidth == 0 && !chunk.video.empty()) {
                    firstFrameWidth = chunk.video.cols;
                    firstFrameHeight = chunk.video.rows;
                    if (!decodeAll)
                        break; // 实况照片只核对首帧尺寸，不把整段解完
                }
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

        // 不变式：帧与对外报的显示尺寸必须是**同一个宽高比**。帧可能被缩到长边上限以内
        // （那是采样密度，不影响尺寸），但宽高比不一致就说明名义尺寸与帧的方向/形状对不上，
        // 绘制端会把画面拉伸——竖拍视频（编码尺寸是横的、帧已旋转）出过这个问题。
        if (firstFrameWidth > 0 && info.displayWidth() > 0 && info.displayHeight() > 0) {
            constexpr double tolerance = 0.01;
            const double frameAspect = static_cast<double>(firstFrameWidth) / firstFrameHeight;
            const double displayAspect = static_cast<double>(info.displayWidth()) / info.displayHeight();
            const bool aspectMatches = std::abs(frameAspect - displayAspect) <= tolerance;
            report += std::format(" | 首帧 {}x{} 与显示尺寸 {}x{} {}",
                firstFrameWidth, firstFrameHeight, info.displayWidth(), info.displayHeight(),
                aspectMatches ? "宽高比一致" : "!! 宽高比不一致，绘制端会按错误的宽高拉伸");
        }

        return report;
    }

    // 视频/音频文件：读盘后交给上面的解码统计（音频打印 video=false + 音频轨信息）
    std::string buildMediaReport(const std::wstring& path, jark::FileFormat sniffed) {
        if (sniffed != jark::FileFormat::Video && sniffed != jark::FileFormat::Audio)
            return {};

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return {};

        const auto size = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint8_t> data(size);
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        return buildMediaReport(data);
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
        // 实况照片/动图的视频不在文件里独立存在：也报一遍它的尺寸，
        // 否则"竖拍视频被按编码尺寸当横的"这类问题在自检里看不见
        if (result.mediaReport.empty() && imageAsset.videoSource && !imageAsset.videoSource->data.empty())
            result.mediaReport = buildMediaReport(imageAsset.videoSource->data, false);

        return result;
    }

} // namespace

namespace {

    // 音频输出自检：把文件的音频提交给 XAudio2（音量 0，不发声），
    // 观察播放时钟是否按采样率推进——用于无人耳参与时验证音频链路。
    // 语言自检：逐一切换语言并打印若干条文案，验证字符串表与回退逻辑
    std::string runLanguageTest() {
        std::string report;
        const uint32_t sampleIds[] = { 1, 2, 28, 39, 41, 54, 124, 126, 127, 129, 146, 149, 151, 156, 165, 190, 191, 192, 193 }; // 含新增导航/缓存/占位界面/音频画面/信息面板文案
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
        // 面板右上角的 ✕：按下即请求收起（上层写进 settingParameter.hideNavigator），且不进入拖动
        const cv::Rect2f closeRect = overlay.closeRect();
        const cv::Point closeCenter{
            static_cast<int>(closeRect.x + closeRect.width / 2),
            static_cast<int>(closeRect.y + closeRect.height / 2) };
        const auto closeEvent = overlay.mouseDown(closeCenter, 1);
        check(closeEvent.handled && closeEvent.closeNavigator && !closeEvent.slide.has_value(),
            "鸟瞰关闭按钮按下请求收起且不触发拖动");
        check(overlay.mouseUp(1).handled, "关闭按钮的抬起仍由浮层收尾");

        // 预览带：整块控件区域都是触发区；展开后当前图片严格水平居中，两侧不够就留空
        ui::NavigationOverlay stripOverlay;
        std::vector<std::wstring> stripFiles;
        for (int i = 0; i < 24; ++i)
            stripFiles.push_back(L"C:\\fake-folder\\image-" + std::to_wstring(i) + L".png");
        stripOverlay.setDirectory(stripFiles, 12);
        stripOverlay.sync({ 1000, 600, 2 << 16, 1 << 16, 0, 0, 0 }, { 1280, 800 }, 1, true, false, 1, 12);
        check(!stripOverlay.mouseMove({ 640, 600 }, false).handled, "预览带区域之外不拦截主图");
        const auto stripEnter = stripOverlay.mouseMove({ 640, 740 }, false);
        check(stripEnter.handled && stripEnter.redraw && stripOverlay.stripVisible(),
            "进入预览带区域立即展开（无需移到最底部）");
        check(stripOverlay.mouseDown({ 640, 740 }, 1).selected == 12, "展开后当前图片位于控件水平正中");
        check(stripOverlay.mouseUp(1).handled, "预览带吞掉对应的抬起");

        // 靠近列表开头：往前没有图片的位置留空，当前图片仍然居中
        ui::NavigationOverlay stripEdgeOverlay;
        std::vector<std::wstring> fewFiles(stripFiles.begin(), stripFiles.begin() + 5);
        stripEdgeOverlay.setDirectory(fewFiles, 0);
        stripEdgeOverlay.sync({ 1000, 600, 2 << 16, 1 << 16, 0, 0, 0 }, { 1280, 800 }, 1, true, false, 1, 0);
        stripEdgeOverlay.mouseMove({ 640, 740 }, false);
        check(stripEdgeOverlay.mouseDown({ 132, 740 }, 1).selected == -1, "往前没有图片的位置留空且不可点");
        check(stripEdgeOverlay.mouseUp(1).handled, "留空位置上的按下仍由预览带收尾");
        check(stripEdgeOverlay.mouseDown({ 640, 740 }, 1).selected == 0, "数量不足时当前图片仍在正中");
        stripEdgeOverlay.mouseUp(1);

        // 悬停鸟瞰面板不应展开预览带（否则面板会被顶上去，难以操作）
        ui::NavigationOverlay panelOverlay;
        panelOverlay.setDirectory(stripFiles, 12);
        panelOverlay.sync({ 1000, 600, 2 << 16, 1 << 16, 0, 0, 0 }, { 1280, 800 }, 1, true, false, 1, 12);
        check(panelOverlay.mouseMove({ 1125, 718 }, false).handled && !panelOverlay.stripVisible(),
            "悬停鸟瞰面板不会展开预览带");
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

    // 实时播放自检：把"播放卡不卡"变成数字。
    // 主循环每帧调一次 MediaPlayer::acquireFrame()，这里照做（定时轮询），
    // 统计交付间隔、交付率、播放时钟是否按 1 倍速推进、时钟最长停滞——
    // "画面卡顿 + 声音一卡一卡"在界面上只能靠眼睛看，看不出是解码、队列还是时钟的问题。
    // 关键不变式：**播放时钟必须按 1 倍速推进**。音频时钟一停，取帧判定就不再满足，
    // 画面也跟着停——两者是同一个故障，不是两个。
    std::string runPlaybackTest(const std::vector<std::wstring>& targets) {
        std::string report;
        int passed = 0, failed = 0, skipped = 0;
        const auto check = [&](bool ok, const std::string& name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        for (const auto& path : targets) {
            report += std::format("---- {} ----\n", utf8(std::filesystem::path(path).filename().wstring()));

            std::vector<uint8_t> container;
            {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    report += "无法读取该文件\n";
                    ++failed;
                    continue;
                }
                const auto size = static_cast<size_t>(file.tellg());
                file.seekg(0);
                container.resize(size);
                file.read(reinterpret_cast<char*>(container.data()), static_cast<std::streamsize>(size));
            }

            // 实况照片/动图的视频字节藏在容器里，要按查看器的方式取出来；
            // 直接打开的视频文件本身就是媒体数据。mediaBytes 要活得比播放器久
            // （MediaDecoder 只持指针不拷贝）。
            // 注意不能用"直接拿整个文件去解"来试探：JPEG 单帧会被 FFmpeg 当 MJPEG 视频收下，
            // 于是实况照片的静态图被误当成整段视频（时长 0、无音频）。按嗅探结果路由。
            std::vector<uint8_t> head;
            const bool isVideoFile = !readHead(path, head, 1u << 20).empty() &&
                sniffFileFormat(head) == FileFormat::Video;

            std::vector<uint8_t> mediaBytes;
            if (isVideoFile) {
                mediaBytes = std::move(container);
            }
            else {
                ImageDatabase database;
                auto asset = database.loader(path);
                if (asset.videoSource && !asset.videoSource->data.empty())
                    mediaBytes = asset.videoSource->data;
            }

            auto probeDecoder = MediaDecoder::open(mediaBytes);
            if (!probeDecoder || !probeDecoder->info().hasVideo) {
                // 查看器自己也取不出视频流（如 HEIC 里靠 trailer 塞的实况照片）：
                // 这不是播放性能问题，记一笔跳过、但把文件名留在报告里
                report += "跳过：没有可播放的媒体流\n";
                ++skipped;
                continue;
            }

            const auto info = probeDecoder->info();
            probeDecoder.reset();

            if (info.durationMs <= 0 || info.frameRate <= 0.0) {
                report += std::format("时长/帧率不可知（时长 {}ms 帧率 {:.2f}），无法判定播放节奏\n",
                    info.durationMs, info.frameRate);
                ++failed;
                continue;
            }

            // 预扫一遍：期望帧数必须来自"这段媒体到底有多少帧"，不能按 fps×时长估算
            // （估出来的数会随帧率有零头，交付率就没法当断言用）；顺便看清楚音轨到底有多长——
            // 播放时钟由音频队列驱动，音轨比视频短的话时钟会提前停住
            size_t expectedFrames = 0;
            int64_t videoEndMs = 0;
            int64_t audioEndMs = 0;
            {
                MediaDecoder::Chunk chunk;
                if (auto scan = MediaDecoder::open(mediaBytes, MediaDecoder::StreamFilter::VideoOnly)) {
                    while (scan->readNext(chunk)) {
                        if (chunk.type == MediaDecoder::Chunk::Type::Video && !chunk.video.empty()) {
                            ++expectedFrames;
                            videoEndMs = chunk.ptsMs;
                        }
                    }
                }
                if (auto scan = MediaDecoder::open(mediaBytes, MediaDecoder::StreamFilter::AudioOnly)) {
                    while (scan->readNext(chunk)) {
                        if (chunk.type == MediaDecoder::Chunk::Type::Audio && !chunk.audio.empty()) {
                            audioEndMs = chunk.ptsMs + static_cast<int64_t>(chunk.audio.size()) /
                                MediaDecoder::kOutputChannels * 1000 / MediaDecoder::kOutputSampleRate;
                        }
                    }
                }
            }
            report += std::format("预扫: {} 帧 视频结束 {}ms 音频结束 {}ms\n",
                expectedFrames, videoEndMs, audioEndMs);

            const double frameMs = info.frameRate > 0.0 ? 1000.0 / info.frameRate : 40.0;
            report += std::format("媒体: 显示 {}x{} fps={:.2f} 音频={} 时长={}ms 单帧={:.1f}ms\n",
                info.displayWidth(), info.displayHeight(), info.frameRate, info.hasAudio, info.durationMs, frameMs);

            auto player = MediaPlayer::create();
            if (!player || !player->start(mediaBytes, 0.0f)) {
                report += "播放启动失败\n";
                ++failed;
                continue;
            }

            std::vector<double> gaps;
            double lastDeliver = -1.0;
            int64_t lastClock = 0;
            double lastClockChange = 0.0;
            double maxStall = 0.0;
            double maxGap = 0.0;
            double totalGap = 0.0;
            int frames = 0;

            const auto begin = std::chrono::steady_clock::now();
            const auto elapsedMs = [&] {
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count();
            };
            const double deadline = info.durationMs * 2.0 + 3000.0;

            while (true) {
                const double now = elapsedMs();

                cv::Mat frame;
                if (player->acquireFrame(frame)) {
                    if (lastDeliver >= 0.0) {
                        gaps.push_back(now - lastDeliver);
                        maxGap = (std::max)(maxGap, gaps.back());
                        totalGap += gaps.back();
                    }
                    lastDeliver = now;
                    ++frames;
                }

                // 时钟停滞算到播放结束为止：结束后时钟本来就不再前进
                if (now <= info.durationMs + 200.0) {
                    const int64_t clock = player->positionMs();
                    if (clock != lastClock) {
                        lastClock = clock;
                        lastClockChange = now;
                    }
                    else {
                        maxStall = (std::max)(maxStall, now - lastClockChange);
                    }
                }

                if (player->hasFinished())
                    break;
                if (now > deadline)
                    break;

                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }

            const double totalMs = elapsedMs();
            const int64_t clockEnd = player->positionMs();
            const double clockRatio = info.durationMs > 0
                ? static_cast<double>(clockEnd) / info.durationMs : 0.0;
            player->stop();

            const double delivered = expectedFrames > 0
                ? static_cast<double>(frames) / static_cast<double>(expectedFrames) : 0.0;

            // 交付间隔：中位数（偶数个取偏小的那个）——卡顿看不出来就靠它
            std::vector<double> sorted = gaps;
            std::sort(sorted.begin(), sorted.end());
            const double medianGap = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];

            report += std::format(
                "结果: 交付 {}/{} 帧 ({:.0f}%) 间隔 中位 {:.1f}ms 平均 {:.1f}ms 最大 {:.1f}ms | "
                "时钟 {:.3f} 倍速 最大停滞 {:.0f}ms | 播放用时 {:.0f}ms (媒体 {}ms)\n",
                frames, expectedFrames, delivered * 100.0,
                medianGap, gaps.empty() ? 0.0 : totalGap / gaps.size(), maxGap,
                clockRatio, maxStall, totalMs, info.durationMs);

            check(delivered >= 0.85,
                std::format("交付率 {:.0f}% ≥ 85%", delivered * 100.0));
            check(medianGap <= (std::max)(1.6 * frameMs, 50.0),
                std::format("交付间隔中位数 {:.1f}ms ≤ {:.0f}ms（约 1.6 帧）", medianGap, (std::max)(1.6 * frameMs, 50.0)));
            check(maxGap <= (std::max)(6.0 * frameMs, 250.0),
                std::format("最大交付间隔 {:.0f}ms ≤ {:.0f}ms", maxGap, (std::max)(6.0 * frameMs, 250.0)));
            check(totalMs <= info.durationMs * 1.2 + 500.0,
                std::format("总用时 {:.0f}ms ≤ {}ms（不能慢放）", totalMs, static_cast<int64_t>(info.durationMs * 1.2 + 500.0)));

            // 音频时钟直接决定声卡是否饿死：它停下就是"声音一卡一卡 + 画面跟着停"
            if (info.hasAudio) {
                check(std::abs(clockRatio - 1.0) <= 0.1,
                    std::format("播放时钟 {:.3f} 倍速（应在 1.0 附近）", clockRatio));
                check(maxStall <= 100.0,
                    std::format("时钟最大停滞 {:.0f}ms ≤ 100ms", maxStall));
            }
        }

        report += std::format("---- playback: {} ok, {} failed, {} skipped ----\n", passed, failed, skipped);
        return report;
    }

    // 独立视频播放器自检：暂停、精确 seek、单帧步进、播完停在最后一帧。
    // 这些语义在界面上只能靠眼睛看"顺不顺"，看不出落点对不对、时钟有没有偷跑：
    // 暂停后再读一次位置才知道时钟是不是真停了，seek 之后才知道落点偏了几帧。
    // 断言直接打在 MediaPlayer 上（播放器界面只是它的状态机 + 绘制），所以这里过的
    // 就是界面里跑的那一份逻辑。
    std::string runVideoTest(const std::vector<std::wstring>& targets) {
        std::string report;
        int passed = 0, failed = 0, skipped = 0;
        const auto check = [&](bool ok, const std::string& name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };
        const auto abs64 = [](int64_t value) { return value < 0 ? -value : value; };
        const auto elapsedMsSince = [](std::chrono::steady_clock::time_point begin) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin).count();
        };

        for (const auto& path : targets) {
            report += std::format("---- {} ----\n", utf8(std::filesystem::path(path).filename().wstring()));

            std::vector<uint8_t> mediaBytes;
            {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    report += "无法读取该文件\n";
                    ++failed;
                    continue;
                }
                const auto size = static_cast<size_t>(file.tellg());
                file.seekg(0);
                mediaBytes.resize(size);
                file.read(reinterpret_cast<char*>(mediaBytes.data()), static_cast<std::streamsize>(size));
            }

            auto player = MediaPlayer::create();
            if (!player || !player->start(mediaBytes, 0.0f)) {
                report += "启动失败：没有可播放的媒体流（视频轨与音频轨都没有）\n";
                ++skipped;
                continue;
            }

            int videoWidth = 0;
            int videoHeight = 0;
            player->getVideoSize(videoWidth, videoHeight);
            const int64_t duration = player->durationMs();
            const int64_t frameMs = static_cast<int64_t>(std::lround(player->frameDurationMs()));

            // 信息面板（播放器按 I / Tab）的文本：标签来自多语言表、值来自解码器。
            // 这里钉住"该有的字段在、不存在的轨道不出现"——面板上少一行/多一行都只能靠眼睛看
            {
                auto infoDecoder = MediaDecoder::open(mediaBytes, MediaDecoder::StreamFilter::Both);
                const std::string infoText = infoDecoder ? mediaInfoText(path, infoDecoder->info()) : std::string();
                const std::string fileName = utf8(std::filesystem::path(path).filename().wstring());

                check(!infoText.empty(), "信息面板：文本非空");
                check(infoText.find(fileName) != std::string::npos,
                    std::format("信息面板：含文件名 {}", fileName));
                check(infoText.find(getUIString(191)) != std::string::npos, "信息面板：含时长行");
                check(infoText.find(getUIString(192)) != std::string::npos, "信息面板：含格式行");
                const bool hasVideoLine = infoText.find(getUIString(164)) != std::string::npos;
                report += "            | 信息面板: " +
                    infoText.substr(0, infoText.find_last_not_of('\n') + 1) + "\n";
                const bool hasAudioLine = infoText.find(getUIString(190)) != std::string::npos;
                check(hasVideoLine == player->hasVideo(),
                    player->hasVideo() ? "信息面板：有视频轨时有视频行" : "信息面板：没有视频轨就没有视频行");
                check(hasAudioLine == player->hasAudio(),
                    player->hasAudio() ? "信息面板：有音频轨时有音频行" : "信息面板：没有音频轨就没有音频行");
                if (player->hasAudio())
                    check(infoText.find("Hz") != std::string::npos, "信息面板：音频行带采样率");
                if (player->hasVideo())
                    check(infoText.find("fps") != std::string::npos, "信息面板：视频行带帧率");
            }

            // —— 纯音频文件（mp3/flac/wav…）：没有视频轨，一帧都取不到 ——
            // 下面那套视频断言（落点帧、单帧步进、拖动预览只交一帧）在这里无从谈起，
            // 但"时钟按 1 倍速推进 / 暂停真的停 / 精确 seek 落在目标上 / 播完要停住 / 从头重播"
            // 一条都不能少：它们是播放器状态机的骨架，在音频上坏了同样是坏。
            if (!player->hasVideo()) {
                report += std::format("媒体: 纯音频 时长={}ms 音频={}\n", duration, player->hasAudio());
                check(player->hasAudio(), "取到音频轨");
                check(player->frameDurationMs() > 0.0, "单帧时长有默认值（音频用它算 seek 余量）");

                // 与视频同一个取帧节奏：主循环就是这么转的，音频虽无帧可取，
                // 但 takeFrame/acquireFrame 是驱动时钟观测的那一条路
                const auto pumpClock = [&](int milliseconds) {
                    const auto begin = std::chrono::steady_clock::now();
                    while (elapsedMsSince(begin) < milliseconds) {
                        cv::Mat frame;
                        player->acquireFrame(frame);
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                };

                // 1) 起播：时钟按 1 倍速推进（上界严格——声卡不可能播得比媒体时间还快）
                pumpClock(1000);
                const int64_t audioPos1 = player->positionMs();
                check(audioPos1 >= 400, std::format("播放 1 秒后位置 {}ms ≥ 400ms（时钟在推进）", audioPos1));
                check(audioPos1 <= 1400, std::format("播放 1 秒后位置 {}ms ≤ 1400ms（没有偷跑）", audioPos1));

                // 2) 暂停：时钟必须停住（声卡 voice 停在原采样点）
                player->pause();
                pumpClock(150);
                const int64_t audioPaused0 = player->positionMs();
                pumpClock(500);
                const int64_t audioPaused1 = player->positionMs();
                check(player->isPaused(), "暂停后 isPaused() 为真");
                check(abs64(audioPaused1 - audioPaused0) <= 20,
                    std::format("暂停 500ms 后位置只动了 {}ms", audioPaused1 - audioPaused0));

                // 3) 恢复：从原位置继续，不是从头开始
                player->resume();
                pumpClock(500);
                const int64_t audioResumed = player->positionMs();
                check(!player->isPaused(), "恢复后 isPaused() 为假");
                check(audioResumed >= audioPaused1 + 200,
                    std::format("恢复 500ms 后位置 {}ms ≥ {}ms（从原位继续）", audioResumed, audioPaused1 + 200));

                if (duration <= 0) {
                    report += "时长不可知：跳过 seek / 结尾断言\n";
                    ++skipped;
                    player->stop();
                    continue;
                }

                // 4) 暂停中精确 seek：音频没有帧时间戳，落点只能对着播放时钟量。
                //    容差 120ms = 一帧的量级 + 一批提交的样本（解码器按目标丢样本、整批丢弃，
                //    剩下的零头由时钟基线补偿）——比视频那条"落点帧"断言宽，但仍能抓住
                //    "落到关键帧上"那种几秒级的偏差
                player->pause();
                const int64_t audioTarget = duration / 2;
                player->seek(audioTarget);
                int64_t audioLanding = 0;
                {
                    const auto begin = std::chrono::steady_clock::now();
                    do {
                        audioLanding = player->positionMs();
                        if (abs64(audioLanding - audioTarget) <= 120)
                            break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    } while (elapsedMsSince(begin) < 2000);
                }
                check(abs64(audioLanding - audioTarget) <= 120,
                    std::format("暂停中 seek 到 {}ms，落点 {}ms（偏差 ≤ 120ms）", audioTarget, audioLanding));
                check(player->isPaused(), "seek 之后仍是暂停态");

                // 5) 播放中 seek：落到目标附近并继续前进。
                //    seek 是**异步**的（命令交给解码线程），目标生效前读到的是老位置——
                //    所以先 pump 一段再采样基准，别拿"刚发完命令"的位置当起点
                //    （视频那条第 6 步同理，这里踩过一次）
                player->resume();
                const int64_t audioQuarter = duration / 4;
                player->seek(audioQuarter);
                pumpClock(600);
                const int64_t audioAfterSeek = player->positionMs();
                check(audioAfterSeek >= audioQuarter - 20 && audioAfterSeek <= audioQuarter + 1000,
                    std::format("播放中 seek 到 {}ms，600ms 后位置 {}ms", audioQuarter, audioAfterSeek));
                pumpClock(300);
                check(player->positionMs() > audioAfterSeek,
                    std::format("seek 之后继续前进（{}ms → {}ms）", audioAfterSeek, player->positionMs()));

                // 6) 播完停在结尾：hasFinished() 为真（时钟压在媒体时长上、音频队列已播空）
                player->seek((std::max)(int64_t{ 0 }, duration - 300));
                {
                    const auto begin = std::chrono::steady_clock::now();
                    while (!player->hasFinished() && elapsedMsSince(begin) < 10000)
                        pumpClock(20);
                }
                check(player->hasFinished(), "播到结尾后 hasFinished() 为真");
                const int64_t audioEndPos = player->positionMs();
                check(audioEndPos >= duration - 200,
                    std::format("结尾位置 {}ms 落在媒体末尾（≥ {}ms）", audioEndPos, duration - 200));

                // 7) 拖回开头：这是"结尾暂停态按空格从头播"的地基。先暂停再 seek——
                //    暂停时时钟冻住，落点就是 seek 的目标本身，不用去猜"这会儿又播到哪了"
                player->pause();
                player->seek(0);
                pumpClock(400);
                check(player->positionMs() <= 120,
                    std::format("seek(0) 后位置 {}ms 回到开头", player->positionMs()));
                player->resume();
                pumpClock(400);
                check(player->positionMs() >= 150,
                    std::format("从头继续播放 400ms 后位置 {}ms ≥ 150ms", player->positionMs()));

                player->stop();

                // 8) 界面那一层的状态机（VideoPlayback）在纯音频上也要能开、能拖：
                //    拖动中时钟停在把手位置、松手后恢复播放（没有帧可等，不该白等"落位"）
                {
                    jark::VideoPlayback audioScrub;
                    if (!audioScrub.open(path)) {
                        report += "VideoPlayback 无法打开该音频\n";
                        ++failed;
                        continue;
                    }

                    const auto pumpAudioScrub = [&](int milliseconds) {
                        const auto begin = std::chrono::steady_clock::now();
                        while (elapsedMsSince(begin) < milliseconds) {
                            cv::Mat frame;
                            audioScrub.takeFrame(frame); // 纯音频永远返回 false，但必须照常调
                            std::this_thread::sleep_for(std::chrono::milliseconds(3));
                        }
                    };

                    pumpAudioScrub(500);
                    check(!audioScrub.hasVideo(), "VideoPlayback: 纯音频 hasVideo() 为假");
                    check(audioScrub.isPlaying(), "VideoPlayback: 打开即播放");
                    check(audioScrub.positionMs() >= 150,
                        std::format("VideoPlayback: 起播 500ms 后位置 {}ms", audioScrub.positionMs()));

                    const int64_t scrubbedTo = duration * 2 / 5;
                    audioScrub.beginScrub(scrubbedTo);
                    check(audioScrub.isScrubbing(), "VideoPlayback: 按下进度条进入拖动状态");
                    check(!audioScrub.isPlaying(), "VideoPlayback: 拖动中暂停（时钟与声音都停）");
                    check(abs64(audioScrub.scrubTargetMs() - scrubbedTo) <= 20, "VideoPlayback: 把手停在按下位置");

                    const auto settleBegin = std::chrono::steady_clock::now();
                    while (abs64(audioScrub.positionMs() - scrubbedTo) > 120 &&
                        elapsedMsSince(settleBegin) < 1500)
                        pumpAudioScrub(20);
                    check(abs64(audioScrub.positionMs() - scrubbedTo) <= 120,
                        std::format("VideoPlayback: 拖动到 {}ms，时钟停在把手位置（{}ms）",
                            scrubbedTo, audioScrub.positionMs()));

                    audioScrub.endScrub(scrubbedTo);
                    pumpAudioScrub(100);
                    check(!audioScrub.isScrubbing(), "VideoPlayback: 松手后退出拖动状态");

                    const int64_t landedAudio = audioScrub.positionMs();
                    pumpAudioScrub(500);
                    check(audioScrub.isPlaying(), "VideoPlayback: 松手后恢复播放（拖动前是在播的）");
                    check(audioScrub.positionMs() > landedAudio + 100,
                        std::format("VideoPlayback: 松手后时钟继续前进（{}ms → {}ms）",
                            landedAudio, audioScrub.positionMs()));
                    audioScrub.close();
                }
                continue;
            }

            report += std::format("媒体: 显示 {}x{} 时长={}ms 单帧={}ms 音频={}\n",
                videoWidth, videoHeight, duration, frameMs, player->hasAudio());

            check(player->hasVideo(), "取到视频轨");
            check(videoWidth > 0 && videoHeight > 0, "显示尺寸有效");
            check(frameMs > 0, std::format("单帧时长 {}ms 有效", frameMs));

            // 像主循环那样定时取帧：视频队列靠取帧腾位置，不取帧解码线程会一直等着
            int64_t deliveredPts = -1;
            int deliveries = 0;
            const auto pump = [&](int milliseconds) {
                const auto begin = std::chrono::steady_clock::now();
                while (true) {
                    cv::Mat frame;
                    if (player->acquireFrame(frame)) {
                        deliveredPts = player->currentFramePtsMs();
                        ++deliveries;
                    }
                    if (elapsedMsSince(begin) >= milliseconds)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            };

            // 1) 起播：时钟按 1 倍速推进。
            // 下界给得宽（这里量的是"时钟有没有在走"，解码被别的活挤到时声卡会饿一小会儿）；
            // 上界严格，声卡不可能播得比媒体时间还快
            pump(1000);
            const int64_t pos1 = player->positionMs();
            check(pos1 >= 400, std::format("播放 1 秒后位置 {}ms ≥ 400ms（时钟在推进）", pos1));
            check(pos1 <= 1400, std::format("播放 1 秒后位置 {}ms ≤ 1400ms（没有偷跑）", pos1));

            // 2) 暂停：时钟必须停住（声卡 voice 停在原采样点 / 系统时钟停止累计）
            player->pause();
            pump(150);
            const int64_t paused0 = player->positionMs();
            pump(500);
            const int64_t paused1 = player->positionMs();
            check(player->isPaused(), "暂停后 isPaused() 为真");
            check(abs64(paused1 - paused0) <= 20,
                std::format("暂停 500ms 后位置只动了 {}ms", paused1 - paused0));

            // 3) 恢复：从原位置继续，不是从头开始
            player->resume();
            pump(500);
            const int64_t resumed = player->positionMs();
            check(!player->isPaused(), "恢复后 isPaused() 为假");
            check(resumed >= paused1 + 200,
                std::format("恢复 500ms 后位置 {}ms ≥ {}ms（从原位继续）", resumed, paused1 + 200));

            if (duration <= 0) {
                report += "时长不可知：跳过 seek / 步进 / 结尾断言\n";
                ++skipped;
                player->stop();
                continue;
            }

            // 4) 暂停中精确 seek 到中点：落点误差不超过一帧，且交出的正是覆盖目标时刻的那一帧
            player->pause();
            const int64_t target = duration / 2;
            deliveredPts = -1; // 清掉上一步的帧号，不然"没交出帧"会被误读成"交出的帧不对"
            player->seek(target);
            int64_t landingMs = 0;
            {
                const auto begin = std::chrono::steady_clock::now();
                // 长 GOP 的精确落点要从关键帧一路解到目标（4K + 250 帧 GOP 实测要几秒），
                // 这里等够；界面侧这段时间显示的是拖动的预览帧。
                //
                // 判据用"交出的帧时间戳落在目标附近"，不能用"seek 之后第一次交出帧"：
                // seek 只是发布目标、两个解码线程各自动作，在那之前时钟还在旧位置、
                // 队列里也还留着旧位置的帧，取帧照样会把它们交出来（机器忙时这段窗口
                // 会被拉长，压测里真出现过第一帧还是旧位置的 1760ms、而目标是 3000ms）。
                // 记的必须是落点帧本身：落点帧之后还可能有一帧落在 clock+20 以内被一起取走。
                const int64_t landingFloor = target - 4 * frameMs;
                while (elapsedMsSince(begin) < 15000) {
                    cv::Mat frame;
                    if (player->acquireFrame(frame)) {
                        deliveredPts = player->currentFramePtsMs();
                        ++deliveries;
                        if (deliveredPts >= landingFloor)
                            break;
                    }
                    else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                }
                landingMs = elapsedMsSince(begin);
            }
            const int64_t seekPos = player->positionMs();
            check(abs64(seekPos - target) <= frameMs + 20,
                std::format("暂停中 seek 到 {}ms，落点 {}ms（偏差 ≤ {}ms）", target, seekPos, frameMs + 20));
            report += std::format("落点耗时 {}ms\n", landingMs);
            check(deliveredPts >= 0 && deliveredPts <= target && deliveredPts > target - 4 * frameMs,
                std::format("交出的帧时间戳 {}ms 覆盖目标时刻（应落在 ({}ms, {}ms]）",
                    deliveredPts, target - 4 * frameMs, target));

            // 5) 单帧步进：前进一帧、再退回原位（暂停态下画面必须跟着换）
            const int64_t before = player->currentFramePtsMs();
            player->stepFrame(1);
            pump(500);
            const int64_t stepped = player->currentFramePtsMs();
            const int64_t stepTolerance = (std::max)(int64_t{ 2 }, frameMs / 2);
            check(abs64(stepped - before - frameMs) <= stepTolerance,
                std::format("单帧前进：{}ms → {}ms（应 +{}ms）", before, stepped, frameMs));

            player->stepFrame(-1);
            pump(500);
            const int64_t back = player->currentFramePtsMs();
            check(abs64(back - before) <= stepTolerance,
                std::format("单帧后退：{}ms → {}ms（应回到 {}ms）", stepped, back, before));

            // 6) 播放中 seek：落到目标附近并继续前进（不能弹回开头）
            player->resume();
            const int64_t quarter = duration / 4;
            player->seek(quarter);
            pump(600);
            const int64_t afterSeek = player->positionMs();
            check(afterSeek >= quarter - 20 && afterSeek <= quarter + 1000,
                std::format("播放中 seek 到 {}ms，600ms 后位置 {}ms", quarter, afterSeek));
            pump(300);
            check(player->positionMs() > afterSeek,
                std::format("seek 之后继续前进（{}ms → {}ms）", afterSeek, player->positionMs()));

            // 7) 播完自动停在最后一帧（不循环、不回退、不关窗）
            player->seek((std::max)(int64_t{ 0 }, duration - 800));
            {
                const auto begin = std::chrono::steady_clock::now();
                while (!player->hasFinished() && elapsedMsSince(begin) < 6000)
                    pump(20);
            }
            check(player->hasFinished(), "播到结尾后 hasFinished() 为真");
            const int64_t endPos = player->positionMs();
            check(endPos >= duration - 2 * frameMs - 50,
                std::format("结尾位置 {}ms 落在媒体末尾（≥ {}ms）", endPos, duration - 2 * frameMs - 50));
            check(player->currentFramePtsMs() > duration - 2000,
                std::format("停在最后一帧（帧时间戳 {}ms）", player->currentFramePtsMs()));

            // 8) 拖回开头：这是"结尾暂停态按空格从头播"的地基
            player->pause();
            player->seek(0);
            pump(400);
            check(player->positionMs() <= frameMs + 30,
                std::format("seek(0) 后位置 {}ms 回到开头", player->positionMs()));
            player->resume();
            pump(400);
            check(player->positionMs() >= 150,
                std::format("从头继续播放 400ms 后位置 {}ms ≥ 150ms", player->positionMs()));

            player->stop();

            // 9) 拖动进度条：走界面上真正用的那份状态机（VideoPlayback）。
            //    拖动中只出关键帧预览（一帧、时钟停在把手位置、画面不许"追赶"），
            //    松手精确落位之后才恢复播放——这三条只在界面上靠眼睛看会很含糊
            {
                jark::VideoPlayback scrub;
                if (!scrub.open(path)) {
                    report += "VideoPlayback 无法打开该文件\n";
                    ++failed;
                    continue;
                }

                int64_t scrubDeliveredPts = -1;
                int deliveries = 0;
                const auto pumpScrub = [&](int milliseconds) {
                    const auto begin = std::chrono::steady_clock::now();
                    while (true) {
                        cv::Mat frame;
                        if (scrub.takeFrame(frame)) {
                            scrubDeliveredPts = scrub.framePtsMs();
                            ++deliveries;
                        }
                        if (elapsedMsSince(begin) >= milliseconds)
                            break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    }
                };

                pumpScrub(700);
                check(scrub.isPlaying(), "打开即播放");
                check(scrub.positionMs() > 400, std::format("起播 700ms 后位置 {}ms", scrub.positionMs()));

                // 把手位置留在媒体中段：拖到结尾会撞上"播完自动暂停"，那就测不出"恢复播放"了
                const int64_t half = duration * 2 / 5;
                scrub.beginScrub(half);
                check(scrub.isScrubbing(), "按下进度条进入拖动状态");
                check(!scrub.isPlaying(), "拖动中暂停（时钟与声音都停）");
                check(abs64(scrub.scrubTargetMs() - half) <= frameMs, "把手停在按下位置");

                // 连拖几段：时钟要跟手停在把手位置，且**每一下只交一帧预览**——
                // 少了"只交一帧"，解码线程会把关键帧之后、还没到目标的帧一帧帧送出来
                // （它们的 PTS 都早于已重设到目标的时钟，全都算到点），画面看起来像在追赶
                int64_t worstPts = -1;
                int64_t moveTarget = half;
                for (int step = 1; step <= 4; ++step) {
                    moveTarget = half + static_cast<int64_t>(duration / 20) * step;
                    const int deliveriesBefore = deliveries;
                    scrub.updateScrub(moveTarget);
                    pumpScrub(250);
                    const int delivered = deliveries - deliveriesBefore;

                    const auto settleBegin = std::chrono::steady_clock::now();
                    while (abs64(scrub.positionMs() - moveTarget) > frameMs + 20 &&
                        elapsedMsSince(settleBegin) < 1000)
                        pumpScrub(20);

                    worstPts = (std::max)(worstPts, scrubDeliveredPts);
                    check(abs64(scrub.positionMs() - moveTarget) <= frameMs + 20,
                        std::format("拖动到 {}ms：时钟停在把手位置（{}ms）", moveTarget, scrub.positionMs()));
                    check(delivered <= 3,
                        std::format("拖动到 {}ms：一下只交 {} 帧预览（应 ≤ 3）", moveTarget, delivered));
                }
                check(worstPts >= 0 && worstPts <= scrub.scrubTargetMs() + frameMs,
                    std::format("拖动预览只出把手之前的关键帧（最大 {}ms ≤ {}ms）",
                        worstPts, scrub.scrubTargetMs() + frameMs));

                scrub.endScrub(moveTarget);
                check(!scrub.isScrubbing(), "松手后退出拖动状态");

                const auto landBegin = std::chrono::steady_clock::now();
                while ((scrubDeliveredPts < moveTarget - 2 * frameMs) && elapsedMsSince(landBegin) < 5000)
                    pumpScrub(20);
                check(scrubDeliveredPts >= moveTarget - 2 * frameMs,
                    std::format("松手后精确落位到把手位置（帧 {}ms / 目标 {}ms）", scrubDeliveredPts, moveTarget));

                const int64_t landedPos = scrub.positionMs();
                pumpScrub(500);
                check(scrub.isPlaying(), "落位后恢复播放（拖动前是在播的）");
                check(scrub.positionMs() > landedPos,
                    std::format("落位后时钟继续前进（{}ms → {}ms）", landedPos, scrub.positionMs()));
                scrub.close();
            }

            // 10) seek 之后音频必须从**目标附近**开始出声，不能把目标之前的音频也放出来。
            //     这条坏在"落点"上：av_seek_frame 用默认流（视频+音频的 MP4 就是视频流）时，
            //     音频实例跟着跳到**视频关键帧**上，于是 seek 之后先送出最多一个 GOP 的旧音频
            //     （听感：半秒到几秒的杂音 / 像在快放），之后才接上正确位置。音频流每个包都是
            //     关键帧，按自己的流 seek 就只差一帧。
            if (player->hasAudio()) {
                const int64_t audioTarget = duration * 3 / 5;
                if (auto audioOnly = MediaDecoder::open(mediaBytes, MediaDecoder::StreamFilter::AudioOnly)) {
                    MediaDecoder::Chunk chunk;
                    const bool seeked = audioOnly->seek(audioTarget);
                    int64_t firstAudioPts = -1;
                    for (int i = 0; i < 4000 && firstAudioPts < 0; ++i) {
                        if (!audioOnly->readNext(chunk))
                            break;
                        if (chunk.type == MediaDecoder::Chunk::Type::Audio && !chunk.audio.empty())
                            firstAudioPts = chunk.ptsMs;
                    }
                    check(seeked, "音频实例 seek 成功");
                    check(firstAudioPts >= 0 && firstAudioPts <= audioTarget + 50,
                        std::format("音频 seek 落点 {}ms 不晚于目标 {}ms（向后取关键帧）",
                            firstAudioPts, audioTarget));
                    check(firstAudioPts >= audioTarget - 100,
                        std::format("音频 seek 落点 {}ms 离目标 {}ms 在 100ms 内（不能落到视频关键帧上）",
                            firstAudioPts, audioTarget));
                }
                else {
                    check(false, "音频实例打不开（无法验证 seek 落点）");
                }

                // 落点还可能因为别的原因被拽到目标之前（容器索引粒度、同时解两路流的实例……），
                // 所以"早于目标的样本一律不出声"这条得独立于落点成立：拿一个**同时解音视频**的
                // 实例（seek 按视频流落到关键帧上，实测早 1.4s）来验——它交出的第一批音频
                // 也必须落在目标附近（靠 MediaDecoder::setSkipBeforeMs，与视频丢帧同一机制）
                if (auto both = MediaDecoder::open(mediaBytes)) {
                    both->setSkipBeforeMs(audioTarget);
                    MediaDecoder::Chunk mixed;
                    const bool seeked = both->seek(audioTarget);
                    int64_t firstPts = -1;
                    for (int i = 0; i < 8000 && firstPts < 0; ++i) {
                        if (!both->readNext(mixed))
                            break;
                        if (mixed.type == MediaDecoder::Chunk::Type::Audio && !mixed.audio.empty())
                            firstPts = mixed.ptsMs;
                    }
                    check(seeked && firstPts >= audioTarget - 100 && firstPts <= audioTarget + 50,
                        std::format("落点在目标之前时，交出音频的起点 {}ms 也被拉回目标 {}ms 附近",
                            firstPts, audioTarget));
                }
            }
        }

        report += std::format("---- video: {} ok, {} failed, {} skipped ----\n", passed, failed, skipped);
        return report;
    }

    // 缩放平滑插值自检（ImageResampler + CanvasRenderer 的配合）：
    // 重采样块是"已按 rotation 预旋转的名义空间位图 + 一块归一化区域"，采样端按块尺寸算采样密度。
    // 旋转方向、区域原点取整、源/位图尺寸换算这几处错了都不会崩，只是画面整体镜像/偏移，
    // 必须拿合成图跟"逐帧路径"逐像素对拍。
    std::string runResampleTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        // 有方向性的合成底图：R 随 x、G 随 y（镜像/转错方向立刻对不上），
        // B 铺 16 像素棋盘（错位超过半格也会对不上）
        const auto makeSource = [](int channels) {
            cv::Mat image(200, 320, CV_MAKETYPE(CV_8U, channels));
            for (int y = 0; y < image.rows; ++y) {
                for (int x = 0; x < image.cols; ++x) {
                    const uint8_t r = static_cast<uint8_t>(x * 255 / (image.cols - 1));
                    const uint8_t g = static_cast<uint8_t>(y * 255 / (image.rows - 1));
                    const uint8_t b = ((x / 16 + y / 16) & 1) ? 230 : 40;
                    if (channels == 1)
                        image.at<uint8_t>(y, x) = r;
                    else if (channels == 3)
                        image.at<cv::Vec3b>(y, x) = cv::Vec3b(b, g, r);
                    else
                        image.at<cv::Vec4b>(y, x) = cv::Vec4b(b, g, r, 255);
                }
            }
            return image;
        };

        const cv::Size canvasSize(420, 320);

        // 图像区域（非背景）的包围盒：两者一致才说明几何/朝向对上了。
        // 背景是打包的主题色（BWRA 一个 uint32），要按整像素比较——拿单通道去比打包值
        // 会永远不相等，包围盒会退化成整块画布，检查就成了摆设。
        const auto imageBounds = [&](const cv::Mat& canvas) {
            const uint32_t background = GlobalVar::currentTheme.BG;
            cv::Rect bounds{};
            for (int y = 0; y < canvas.rows; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(canvas.ptr(y));
                for (int x = 0; x < canvas.cols; ++x) {
                    if (row[x] != background) {
                        bounds = bounds.area() == 0 ? cv::Rect(x, y, 1, 1)
                            : bounds | cv::Rect(x, y, 1, 1);
                    }
                }
            }
            return bounds;
        };

        const auto meanDiff = [&](const cv::Mat& a, const cv::Mat& b, cv::Rect area) {
            double sum = 0.0;
            int64_t count = 0;
            for (int y = area.y; y < area.y + area.height; ++y) {
                for (int x = area.x; x < area.x + area.width; ++x) {
                    const cv::Vec4b& pa = a.at<cv::Vec4b>(y, x);
                    const cv::Vec4b& pb = b.at<cv::Vec4b>(y, x);
                    sum += std::abs(pa[0] - pb[0]) + std::abs(pa[1] - pb[1]) + std::abs(pa[2] - pb[2]);
                    count += 3;
                }
            }
            return count == 0 ? 1e9 : sum / static_cast<double>(count);
        };

        struct Case { int rotation; double scale; };
        for (const int channels : { 3, 4 }) {
            const cv::Mat source = makeSource(channels);
            for (const Case testCase : { Case{0, 2.0}, Case{0, 3.5}, Case{0, 0.5}, Case{1, 2.0},
                                          Case{2, 2.0}, Case{3, 2.0}, Case{1, 0.5} }) {
                jark::ViewState view;
                view.imageWidth = source.cols;
                view.imageHeight = source.rows;
                view.zoomBase = 1 << 16;
                view.zoom = static_cast<int64_t>(std::llround(view.zoomBase * testCase.scale));
                view.rotation = testCase.rotation;

                const std::string tag = std::format("{}ch rot={} {:.2f}x", channels, testCase.rotation,
                    testCase.scale);

                cv::Mat reference(canvasSize, CV_8UC4);
                jark::drawImageToCanvas(source, reference, view);

                const auto block = jark::resampleVisibleRegion(source, view, canvasSize);
                if (!block.valid) {
                    check(false, std::format("{}：重采样块生成", tag));
                    continue;
                }

                jark::ViewState blockView = view;
                blockView.sourcePreRotated = true;
                blockView.sourceLeft = block.left;
                blockView.sourceTop = block.top;
                blockView.sourceWidth = block.width;
                blockView.sourceHeight = block.height;

                cv::Mat rendered(canvasSize, CV_8UC4);
                jark::drawImageToCanvas(block.image, rendered, blockView);

                const cv::Rect referenceBounds = imageBounds(reference);
                const cv::Rect renderedBounds = imageBounds(rendered);
                const bool geometryOk = std::abs(referenceBounds.x - renderedBounds.x) <= 1 &&
                    std::abs(referenceBounds.y - renderedBounds.y) <= 1 &&
                    std::abs(referenceBounds.width - renderedBounds.width) <= 2 &&
                    std::abs(referenceBounds.height - renderedBounds.height) <= 2;
                check(geometryOk, std::format("{}：图像矩形一致（{} vs {}）", tag,
                    referenceBounds.width, renderedBounds.width));

                const cv::Rect overlap = referenceBounds & renderedBounds;
                const double diff = meanDiff(reference, rendered, overlap);
                // 平滑重采样与最近邻本就不同（这正是目的），但同一朝向/同一位置时平均差很小；
                // 镜像或转错方向会差到几十
                check(diff < 12.0, std::format("{}：与逐帧路径逐像素接近（平均差 {:.2f}）", tag, diff));
            }
        }

        // 缩小 0.5x：面积平均必须比逐帧路径的 2×2 近似更平（细密棋盘不再出现摩尔纹）
        {
            cv::Mat checker(200, 320, CV_8UC3);
            for (int y = 0; y < checker.rows; ++y)
                for (int x = 0; x < checker.cols; ++x)
                    checker.at<cv::Vec3b>(y, x) = cv::Vec3b(((x + y) & 1) ? 235 : 20,
                        ((x + y) & 1) ? 235 : 20, ((x + y) & 1) ? 235 : 20);

            jark::ViewState view;
            view.imageWidth = checker.cols;
            view.imageHeight = checker.rows;
            view.zoomBase = 1 << 16;
            view.zoom = view.zoomBase / 2;

            cv::Mat reference(canvasSize, CV_8UC4);
            jark::drawImageToCanvas(checker, reference, view);

            const auto block = jark::resampleVisibleRegion(checker, view, canvasSize);
            check(block.valid, "1×1 棋盘 0.5x：重采样块生成");

            if (block.valid) {
                jark::ViewState blockView = view;
                blockView.sourcePreRotated = true;
                blockView.sourceLeft = block.left;
                blockView.sourceTop = block.top;
                blockView.sourceWidth = block.width;
                blockView.sourceHeight = block.height;
                cv::Mat rendered(canvasSize, CV_8UC4);
                jark::drawImageToCanvas(block.image, rendered, blockView);

                const cv::Rect area = imageBounds(reference) & imageBounds(rendered);
                cv::Scalar mean, stddev;
                cv::meanStdDev(cv::Mat(reference, area), mean, stddev);
                cv::Scalar renderedMean, renderedStddev;
                cv::meanStdDev(cv::Mat(rendered, area), renderedMean, renderedStddev);
                check(renderedStddev[0] < stddev[0] * 0.6,
                    std::format("1×1 棋盘 0.5x：面积平均后更平（标准差 {:.1f} -> {:.1f}）",
                        stddev[0], renderedStddev[0]));
                check(std::abs(renderedMean[0] - mean[0]) < 25.0,
                    std::format("1×1 棋盘 0.5x：平均亮度不变（{:.1f} -> {:.1f}）",
                        mean[0], renderedMean[0]));
            }
        }

        // 1:1 时不做重采样（最近邻就是精确值）
        {
            jark::ViewState view;
            view.imageWidth = 320;
            view.imageHeight = 200;
            view.zoomBase = 1 << 16;
            view.zoom = view.zoomBase;
            check(!jark::shouldResample(view), "1:1 缩放不做重采样");
            view.zoom = view.zoomBase + 1;
            check(jark::shouldResample(view), "非 1:1 缩放做重采样");
        }

        // 余量：可视区域挪一点还能复用同一块，挪出去就得重算
        {
            jark::ViewState view;
            view.imageWidth = 320;
            view.imageHeight = 200;
            view.zoomBase = 1 << 16;
            view.zoom = view.zoomBase * 2;
            const cv::Mat source = makeSource(3);
            const auto block = jark::resampleVisibleRegion(source, view, canvasSize);
            check(block.valid, "余量用例：重采样块生成");

            if (block.valid) {
                const auto geometry = jark::imageGeometry(view, canvasSize, source.size());
                check(block.covers(geometry), "原视图落在块内");

                jark::ViewState moved = view;
                moved.slideX += static_cast<int>(geometry.visible.width * geometry.renderedSize.width * 0.1);
                check(block.covers(jark::imageGeometry(moved, canvasSize, source.size())),
                    "视图挪一小段仍在块内（余量生效）");

                jark::ViewState far_ = view;
                far_.slideX += static_cast<int>(geometry.renderedSize.width);
                check(!block.covers(jark::imageGeometry(far_, canvasSize, source.size())),
                    "视图挪出块外不复用");
            }
        }

        // 超宽位图（宽 > SHRT_MAX）：warpAffine 的非最近邻插值内部走 cv::remap，而 remap 断言
        // src/dst 两个方向都 < SHRT_MAX(32767)——把**整幅**位图喂进去，宽或高超过 32767 的图
        // （长卷轴、大扫描件）在放大到 100% 以上时就会抛 cv::Exception：界面线程没人接，
        // 进程直接退出（用户的报告就是"打开没问题、放大到一百多个百分点时闪退"）。
        // 这条用例钉住"先裁出用得到的源区再仿射"这个修法：把裁剪去掉，这里会 FAIL
        // （OpenCV 抛的异常被 resampleVisibleRegion 的兜底接住，退化成 valid=false）。
        {
            constexpr int WIDE_W = 33000, WIDE_H = 40; // 宽超过 SHRT_MAX(32767)
            cv::Mat wide(WIDE_H, WIDE_W, CV_8UC4);
            for (int y = 0; y < wide.rows; ++y) {
                for (int x = 0; x < wide.cols; ++x) {
                    // 4 像素竖条 + 横条：裁剪原点算错一个像素，相位立刻对不上
                    wide.at<cv::Vec4b>(y, x) = cv::Vec4b(((x / 4) & 1) ? 235 : 20,
                        ((y / 4) & 1) ? 235 : 20,
                        static_cast<uint8_t>(x * 255 / (wide.cols - 1)), 255);
                }
            }

            jark::ViewState view;
            view.imageWidth = wide.cols;
            view.imageHeight = wide.rows;
            view.zoomBase = 1 << 16;
            view.zoom = view.zoomBase * 2; // 放大分支（缩小走 resize，不受这条断言约束）
            view.rotation = 0;
            view.border = false; // 逐字节比像素，别把图像边框算进去

            cv::Mat reference(canvasSize, CV_8UC4);
            jark::drawImageToCanvas(wide, reference, view);

            const auto block = jark::resampleVisibleRegion(wide, view, canvasSize);
            check(block.valid, std::format("超宽位图 {}x{} 2.0x：放大分支照常出块", WIDE_W, WIDE_H));

            if (block.valid) {
                jark::ViewState blockView = view;
                blockView.border = false;
                blockView.sourcePreRotated = true;
                blockView.sourceLeft = block.left;
                blockView.sourceTop = block.top;
                blockView.sourceWidth = block.width;
                blockView.sourceHeight = block.height;
                cv::Mat rendered(canvasSize, CV_8UC4);
                jark::drawImageToCanvas(block.image, rendered, blockView);

                const cv::Rect referenceBounds = imageBounds(reference);
                const cv::Rect renderedBounds = imageBounds(rendered);
                const bool geometryOk = std::abs(referenceBounds.x - renderedBounds.x) <= 1 &&
                    std::abs(referenceBounds.y - renderedBounds.y) <= 1 &&
                    std::abs(referenceBounds.width - renderedBounds.width) <= 2 &&
                    std::abs(referenceBounds.height - renderedBounds.height) <= 2;
                check(geometryOk, std::format("超宽位图：图像矩形一致（{}x{} vs {}x{}）",
                    referenceBounds.width, referenceBounds.height,
                    renderedBounds.width, renderedBounds.height));

                // 内容没挪动：2.00x 时"离图像原点偶数个画布像素"的位置正好落在源像素中心
                // （Lanczos 核在整数处取 1、别处取 0），块与逐帧路径在这些位置都等于同一个源像素；
                // 只有两点都不在整像素上的位置才允许被插值糊开。裁剪原点错 1 个源像素，这些位置
                // 就落到半像素上，差值立刻变成一整条条纹的反差（215）。
                const auto geometry = jark::imageGeometry(view, canvasSize, wide.size());
                int phaseCount = 0;
                int phaseMaxDiff = 0;
                for (int y = 0; y < canvasSize.height; ++y) {
                    const int srcY = (y - geometry.origin.y) / 2;
                    if ((y - geometry.origin.y) % 2 != 0 || srcY < 0 || srcY >= wide.rows)
                        continue;
                    for (int x = 0; x < canvasSize.width; ++x) {
                        const int srcX = (x - geometry.origin.x) / 2;
                        if ((x - geometry.origin.x) % 2 != 0 || srcX < 0 || srcX >= wide.cols)
                            continue;
                        const cv::Vec4b& a = rendered.at<cv::Vec4b>(y, x);
                        const cv::Vec4b& b = reference.at<cv::Vec4b>(y, x);
                        for (int c = 0; c < 3; ++c)
                            phaseMaxDiff = (std::max)(phaseMaxDiff, std::abs(a[c] - b[c]));
                        ++phaseCount;
                    }
                }
                check(phaseCount > 100 && phaseMaxDiff <= 2,
                    std::format("超宽位图：裁剪没挪动内容（{} 个整像素点，最大差 {}）",
                        phaseCount, phaseMaxDiff));
            }
        }

        report = std::format("---- 缩放平滑插值自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // 图像缓存自检：容量是"条数 + 总字节"双重约束，字节那条是给大图兜底的——一张
    // 43890x38875 的扫描件解码后 6.36GB，按条数留 4 张就是 25GB（32GB 机器直接爆）。
    // 这里直接继承生产用的 ImageAssetCache，loader 造 1KB 的小真图（量的是同一套
    // ImageAsset::memoryBytes()），把预算淘汰、LRU 顺序、"外部持有的条目踢了也不省内存"、
    // 单条超预算、最少留 2 张这几种情况全钉住。用例里每条一样大，所以 unit 的整数倍
    // 就是预算，"能装几条"一眼能对上。
    std::string runCacheTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        const auto makeAsset = [] {
            ImageAsset asset;
            asset.primaryFrame = cv::Mat(1, 1024, CV_8UC1); // 1KB
            return asset;
        };
        const size_t unit = makeAsset().memoryBytes(); // 一条（含 sizeof 等固定开销）的字节数

        // 局部的测试缓存：loader 造 1KB 的小真图（局部类抓不到外面的 lambda，自己造一遍）
        class TestCache : public ImageAssetCache {
        public:
            static ImageAsset smallAsset() {
                ImageAsset asset;
                asset.primaryFrame = cv::Mat(1, 1024, CV_8UC1);
                return asset;
            }
            ImageAsset loader(const std::wstring&) override { return smallAsset(); }
        };
        const auto put = [](TestCache& cache, const wchar_t* key) {
            cache.put(key, TestCache::smallAsset());
        };

        // 1) 预算满了从最久未用的那头踢
        {
            TestCache cache;
            cache.setCapacity(64);
            cache.setMinEntries(1);
            cache.setByteBudget(3 * unit);
            put(cache, L"1");
            put(cache, L"2");
            put(cache, L"3");
            check(cache.size() == 3 && cache.bytes() == 3 * unit,
                std::format("预算内三条都在（{} 条 / {} 字节）", cache.size(), cache.bytes()));
            put(cache, L"4"); // 4*unit > 3*unit：踢最久未用的 1
            check(cache.size() == 3 && cache.bytes() == 3 * unit,
                std::format("超预算按最久未用淘汰（剩 {} 条 / {} 字节）", cache.size(), cache.bytes()));
            check(cache.tryGetPtr(L"1") == nullptr && cache.tryGetPtr(L"2") != nullptr &&
                cache.tryGetPtr(L"4") != nullptr,
                "被淘汰的取不到、留着的取得到");
        }

        // 2) 刚用过的不会被踢（LRU 顺序，不是"按插入时间"）
        {
            TestCache cache;
            cache.setCapacity(64);
            cache.setMinEntries(1);
            cache.setByteBudget(2 * unit);
            put(cache, L"1");
            put(cache, L"2");
            cache.tryGetPtr(L"1"); // 用一下 1：队列变成 [1, 2]，最久未用的成了 2
            put(cache, L"3");      // 3*unit > 2*unit：该踢的是 2，不是刚用过的 1
            check(cache.tryGetPtr(L"1") != nullptr && cache.tryGetPtr(L"3") != nullptr,
                "预算内留着的条目都在");
            check(cache.tryGetPtr(L"2") == nullptr && cache.bytes() == 2 * unit,
                std::format("踢掉的是最久未用的那条，不是刚用过的（{} 字节）", cache.bytes()));
        }

        // 3) 至少留 2 张（用户口径：最少缓存 2 张）——单张都超预算时也保住两条
        {
            TestCache cache;
            cache.setCapacity(64);
            cache.setByteBudget(unit / 2); // 比任何一条都小
            put(cache, L"1");
            put(cache, L"2");
            check(cache.size() == 2 && cache.bytes() == 2 * unit,
                std::format("预算装不下两张时仍保住 2 条（{} 条 / {} 字节）", cache.size(), cache.bytes()));
            put(cache, L"3");
            check(cache.size() == 2, std::format("第 3 条进来后仍然只有 2 条（{} 条）", cache.size()));
        }

        // 4) 外部还持有引用的条目：踢了也不释放内存（只是白丢一次命中），所以轮不到它
        {
            TestCache cache;
            cache.setCapacity(64);
            put(cache, L"1");
            auto held = cache.tryGetPtr(L"1"); // 模拟主窗口持着当前显示的那张图
            put(cache, L"2");
            cache.setByteBudget(2 * unit);
            put(cache, L"3"); // 3*unit > 2*unit：最久未用的是 1，但它被持有 → 踢 2
            check(held != nullptr && held->primaryFrame.cols == 1024,
                "被持有的条目没有被释放（use-after-free）");
            check(cache.tryGetPtr(L"1") != nullptr && cache.tryGetPtr(L"3") != nullptr,
                "被持有的条目留在缓存里（踢它不省内存）");
            check(cache.tryGetPtr(L"2") == nullptr && cache.bytes() == 2 * unit,
                std::format("踢掉的是没人持有的那条（{} 字节）", cache.bytes()));
        }

        // 5) 单条就超预算：进来先留着（没得挑），下一条进来时被踢出去，不空转
        {
            TestCache cache;
            cache.setCapacity(64);
            cache.setMinEntries(1);
            cache.setByteBudget(unit / 2);
            put(cache, L"1");
            check(cache.size() == 1 && cache.bytes() == unit, "单条超预算时不会反复空转（保留它）");
            put(cache, L"2");
            check(cache.tryGetPtr(L"1") == nullptr && cache.bytes() == unit,
                std::format("有别的可踢时立刻回收（{} 字节）", cache.bytes()));
        }

        // 6) 调小预算立刻回收
        {
            TestCache cache;
            cache.setCapacity(64);
            cache.setMinEntries(1);
            put(cache, L"1");
            put(cache, L"2");
            put(cache, L"3");
            check(cache.bytes() == 3 * unit, "不限字节时三条都留着");
            cache.setByteBudget(unit);
            check(cache.bytes() <= unit && cache.size() == 1,
                std::format("调小预算立即回收到 {} 字节", cache.bytes()));
        }

        // 7) 条数上限仍然兜底（不限字节时），clear 归零
        {
            TestCache cache;
            cache.setByteBudget(0); // 0 = 不限
            cache.setCapacity(3);
            for (const wchar_t* key : { L"1", L"2", L"3", L"4", L"5" })
                put(cache, key);
            check(cache.size() == 3 && cache.bytes() == 3 * unit,
                std::format("条数上限兜底（{} 条 / {} 字节）", cache.size(), cache.bytes()));
            check(cache.tryGetPtr(L"2") == nullptr, "尾部条目被条数上限淘汰");
            cache.clear();
            check(cache.size() == 0 && cache.bytes() == 0, "clear 后字节数归零");
        }

        // 8) 真缓存接上线：预算 = 物理内存的一半（下限 512MB）
        {
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            const bool haveMem = GlobalMemoryStatusEx(&status) != FALSE;
            const size_t budget = ImageDatabase::defaultCacheBudgetBytes();
            check(budget >= (size_t(512) << 20), std::format("预算不低于 512MB（{} MB）", budget >> 20));
            if (haveMem) {
                // 内存够大时就是内存的一半；内存特别小时落到 512MB 下限
                const size_t half = static_cast<size_t>(status.ullTotalPhys / 2);
                check(budget >= (std::min)(half, size_t(512) << 20) &&
                    budget <= (std::max)(half, size_t(512) << 20),
                    std::format("预算 = 物理内存的 50%（{} MB / {} MB）",
                        budget >> 20, status.ullTotalPhys >> 20));
            }

            ImageDatabase database;
            check(database.byteBudgetBytes() == budget && database.bytes() == 0,
                std::format("ImageDatabase 构造后预算已生效（{} MB）", database.byteBudgetBytes() >> 20));

            const auto blob = [](int cols) {
                ImageAsset asset;
                asset.primaryFrame = cv::Mat(1, cols, CV_8UC4);
                return asset;
            };
            const size_t single = blob(64).memoryBytes();
            check(single >= size_t(64) * 4 + sizeof(ImageAsset),
                std::format("ImageAsset 字节估算含像素（{} 字节）", single));
            // 共享同一份像素的多个成员只算一次（实况/动图的 primaryFrame 与 frames[0] 是浅拷贝）
            ImageAsset shared;
            shared.primaryFrame = cv::Mat(4, 64, CV_8UC4);
            shared.frames.push_back(shared.primaryFrame);
            shared.frames.push_back(shared.primaryFrame);
            check(shared.memoryBytes() < single * 3,
                std::format("共享像素只算一次（{} 字节）", shared.memoryBytes()));
        }

        report = std::format("---- 图像缓存自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // 色彩管理自检：源与目标同为 sRGB 时变换是恒等的（跳过可省几百毫秒），大图按行并行——
    // 这两类改动出错不会崩也不会报错，只是颜色悄悄变了，必须用像素断言钉住。
    std::string runColorTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        // gamma 1.8 的 RGB profile：与 sRGB 明显不同，保证变换非恒等
        const auto makeGammaProfile = []() -> std::vector<uint8_t> {
            cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.8);
            cmsCIExyY white{ 0.3127, 0.3290, 1.0 };
            cmsCIExyYTRIPLE primaries{
                { 0.6400, 0.3300, 1.0 },
                { 0.3000, 0.6000, 1.0 },
                { 0.1500, 0.0600, 1.0 } };
            cmsToneCurve* curves[3] = { curve, curve, curve };
            cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
            cmsFreeToneCurve(curve);

            std::vector<uint8_t> bytes;
            cmsUInt32Number size = 0;
            if (profile && cmsSaveProfileToMem(profile, nullptr, &size) && size > 0) {
                bytes.resize(size);
                cmsSaveProfileToMem(profile, bytes.data(), &size);
                bytes.resize(size);
            }
            if (profile)
                cmsCloseProfile(profile);
            return bytes;
        };

        cv::Mat source(300, 400, CV_8UC3);
        for (int y = 0; y < source.rows; ++y)
            for (int x = 0; x < source.cols; ++x)
                source.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uint8_t>(x * 255 / 399),
                    static_cast<uint8_t>(y * 255 / 299), static_cast<uint8_t>((x + y) * 255 / 698));

        // 1) 源与目标都没有 profile（都按 sRGB 处理）：恒等跳过、像素一个都不动
        {
            cv::Mat mat = source.clone();
            const bool transformed = ColorManager::applyToMat(mat, {}, {});
            check(!transformed && cv::norm(mat, source, cv::NORM_INF) == 0.0,
                "空 profile 双双按 sRGB：恒等跳过且像素不变");
        }

        // 2) 图文 profile 与显示器 profile 逐字节相同：恒等跳过
        const auto gammaProfile = makeGammaProfile();
        check(!gammaProfile.empty(), "自检 profile 构造成功");
        {
            cv::Mat mat = source.clone();
            const bool transformed = ColorManager::applyToMat(mat, gammaProfile, gammaProfile);
            check(!transformed && cv::norm(mat, source, cv::NORM_INF) == 0.0,
                "同一 profile：恒等跳过且像素不变");
        }

        // 3) sRGB → gamma1.8：变换必须真的执行并改变像素
        {
            cv::Mat mat = source.clone();
            const bool transformed = ColorManager::applyToMat(mat, {}, gammaProfile);
            check(transformed && cv::norm(mat, source, cv::NORM_INF) > 0.0,
                "sRGB 到 gamma1.8：变换执行且像素变化");
        }

        // 4) 四通道：alpha 不参与变换（cmsFLAGS_COPY_ALPHA）
        {
            cv::Mat mat(64, 64, CV_8UC4);
            for (int y = 0; y < mat.rows; ++y)
                for (int x = 0; x < mat.cols; ++x)
                    mat.at<cv::Vec4b>(y, x) = cv::Vec4b(static_cast<uint8_t>(x * 4),
                        static_cast<uint8_t>(y * 4), 90, static_cast<uint8_t>(x + y));
            const cv::Mat before = mat.clone();
            ColorManager::applyToMat(mat, {}, gammaProfile);
            bool alphaSame = true;
            for (int y = 0; y < mat.rows && alphaSame; ++y) {
                for (int x = 0; x < mat.cols; ++x) {
                    if (mat.at<cv::Vec4b>(y, x)[3] != before.at<cv::Vec4b>(y, x)[3]) {
                        alphaSame = false;
                        break;
                    }
                }
            }
            check(alphaSame, "四通道图像：alpha 通道不被变换");
        }

        // 5) 超过并行门槛（约 200 万像素）的大图：按行并行与单线程串行逐字节一致
        {
            cv::Mat big(1200, 2000, CV_8UC3);
            for (int y = 0; y < big.rows; ++y)
                for (int x = 0; x < big.cols; ++x)
                    big.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uint8_t>(x * 255 / 1999),
                        static_cast<uint8_t>(y * 255 / 1199), static_cast<uint8_t>((x * 7 + y * 3) & 0xFF));

            cv::Mat parallelMat = big.clone();
            ColorManager::applyToMat(parallelMat, {}, gammaProfile);

            cv::Mat serialMat = big.clone();
            {
                cmsHPROFILE sourceProfile = cmsCreate_sRGBProfile();
                cmsHPROFILE targetProfile = cmsOpenProfileFromMem(gammaProfile.data(),
                    static_cast<cmsUInt32Number>(gammaProfile.size()));
                cmsHTRANSFORM transform = cmsCreateTransform(sourceProfile, TYPE_BGR_8,
                    targetProfile, TYPE_BGR_8, INTENT_PERCEPTUAL, 0);
                for (int y = 0; y < serialMat.rows; ++y) {
                    cmsDoTransform(transform, serialMat.ptr(y), serialMat.ptr(y),
                        static_cast<cmsUInt32Number>(serialMat.cols));
                }
                cmsDeleteTransform(transform);
                cmsCloseProfile(sourceProfile);
                cmsCloseProfile(targetProfile);
            }
            check(cv::norm(parallelMat, serialMat, cv::NORM_INF) == 0.0,
                "大图按行并行与逐行串行结果逐字节一致");
        }

        // 6) Display P3 → sRGB：变换方向、通道顺序与色域剪裁。
        // 这一组钉的是"看起来像 bug 的正确行为"：P3 的两个超饱和红（255,0,0 与 242,0,0）
        // 转成 sRGB 后都是 (255,0,0)——两者都在 sRGB 色域之外，被剪裁到同一个边界值，
        // 图案因此消失。用户拿到的 P3 测试图（如 Webkit-logo-P3.jxl：整幅只有这两种红）
        // 在 sRGB 显示器上"只剩纯红"是**正确**渲染，不是色彩管理坏了；关掉色彩管理看到的
        // 是把 P3 数值直接当 sRGB 用的未管理画面，那个"隐约的图案"才是假的。
        // 数值可与 lcms2 参考实现（PIL/ImageCms 同参数）逐字节对上。
        {
            const auto makeP3Profile = []() -> std::vector<uint8_t> {
                // Display P3：与 sRGB 同一白点(D65)、同一 TRC（sRGB 曲线），只有原色不同
                const cmsFloat64Number srgbCurve[5] = { 2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045 };
                cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, srgbCurve);
                cmsCIExyY white{ 0.3127, 0.3290, 1.0 };
                cmsCIExyYTRIPLE primaries{
                    { 0.6800, 0.3200, 1.0 },
                    { 0.2650, 0.6900, 1.0 },
                    { 0.1500, 0.0600, 1.0 } };
                cmsToneCurve* curves[3] = { curve, curve, curve };
                cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
                cmsFreeToneCurve(curve);

                std::vector<uint8_t> bytes;
                cmsUInt32Number size = 0;
                if (profile && cmsSaveProfileToMem(profile, nullptr, &size) && size > 0) {
                    bytes.resize(size);
                    cmsSaveProfileToMem(profile, bytes.data(), &size);
                    bytes.resize(size);
                }
                if (profile)
                    cmsCloseProfile(profile);
                return bytes;
            };

            const auto p3Profile = makeP3Profile();
            check(!p3Profile.empty(), "Display P3 自检 profile 构造成功");

            // 一行 4 个色块（BGRA：Mat 是 8UC4，红在 [2]）：纯红 / 次级饱和红 / 中灰 / 纯绿
            cv::Mat patches(1, 4, CV_8UC4);
            patches.at<cv::Vec4b>(0, 0) = cv::Vec4b(0, 0, 255, 255);
            patches.at<cv::Vec4b>(0, 1) = cv::Vec4b(0, 0, 242, 255);
            patches.at<cv::Vec4b>(0, 2) = cv::Vec4b(128, 128, 128, 255);
            patches.at<cv::Vec4b>(0, 3) = cv::Vec4b(0, 255, 0, 255);

            const bool transformed = ColorManager::applyToMat(patches, p3Profile, {});
            check(transformed, "P3 → sRGB：变换执行");
            check(patches.at<cv::Vec4b>(0, 0) == cv::Vec4b(0, 0, 255, 255),
                "P3 纯红 → sRGB 仍是纯红（超色域原色剪裁回自身）");
            check(patches.at<cv::Vec4b>(0, 1) == cv::Vec4b(0, 0, 255, 255),
                "P3 (242,0,0) → sRGB (255,0,0)：次级饱和红也被剪裁到同一个红（图案消失的机制）");
            check(patches.at<cv::Vec4b>(0, 2) == cv::Vec4b(128, 128, 128, 255),
                "P3 中灰 → sRGB 中灰不变（同白点同 TRC）");
            check(patches.at<cv::Vec4b>(0, 3) == cv::Vec4b(0, 255, 0, 255),
                "P3 纯绿 → sRGB 仍是纯绿（通道顺序没反）");
        }

        report = std::format("---- 色彩管理自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // SVG 自检：验证 CSS 函数折叠（light-dark/var）与光栅化的反预乘
    std::string runVectorTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        // 半透明方块：fill-opacity 0.5 的纯红，四周透明
        const std::string translucentSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">)svg"
            R"svg(<rect x="25" y="25" width="50" height="50" fill="#ff0000" fill-opacity="0.5"/></svg>)svg";

        // CSS 5 特性：--accent 定义在 :root，light-dark 与 var（含回退值）分别用在不同图元上
        const std::string cssSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">)svg"
            R"svg(<style>:root { --accent: #00ff00; } .box { fill: light-dark(#ff0000, #121212); stroke: var(--accent); }</style>)svg"
            R"svg(<rect class="box" x="20" y="20" width="60" height="60"/>)svg"
            R"svg(<rect x="120" y="20" width="60" height="60" style="fill: var(--accent);"/>)svg"
            R"svg(<rect x="60" y="60" width="10" height="10" fill="var(--undefined, #0000ff)"/>)svg"
            R"svg(</svg>)svg";

        SVGPreprocessor preprocessor;
        const std::string folded = preprocessor.preprocessSVG(cssSvg.data(), cssSvg.size());
        check(!folded.empty(), "CSS 用例预处理成功");
        check(folded.find("light-dark(") == std::string::npos && folded.find("var(") == std::string::npos,
            "light-dark()/var() 全部折叠成字面量");
        check(folded.find("#ff0000") != std::string::npos, "light-dark 取亮色分支");
        check(folded.find("#00ff00") != std::string::npos, "var() 解析 :root 定义的自定义属性");
        check(folded.find("#0000ff") != std::string::npos, "var() 未定义时使用回退值");

        // 光栅化 + 反预乘：lunasvg 输出的是预乘 alpha，若不反预乘，纯红在 alpha=0.5 处会变成 (0,0,128,128)
        const auto rasterize = [](const std::string& svg, int width, int height) {
            const std::string processed = SVGPreprocessor().preprocessSVG(svg.data(), svg.size());
            const std::string& source = processed.empty() ? svg : processed;
            auto document = lunasvg::Document::loadFromData(source.data(), source.size());
            if (!document)
                return cv::Mat();
            jark::VectorImage vectorImage;
            vectorImage.document = std::move(document);
            vectorImage.intrinsicWidth = width;
            vectorImage.intrinsicHeight = height;
            return jark::renderVectorImage(vectorImage, width, height);
        };

        const cv::Mat translucent = rasterize(translucentSvg, 100, 100);
        check(!translucent.empty() && translucent.type() == CV_8UC4, "半透明 SVG 光栅化为 BGRA 位图");
        if (!translucent.empty()) {
            const cv::Vec4b center = translucent.at<cv::Vec4b>(50, 50);
            check(center[2] >= 250 && center[1] <= 5 && center[0] <= 5 && std::abs(center[3] - 128) <= 2,
                std::format("半透明纯红反预乘为直通 alpha：(B,G,R,A)=({},{},{},{}) 期望约 (0,0,255,128)",
                    center[0], center[1], center[2], center[3]));
            const cv::Vec4b corner = translucent.at<cv::Vec4b>(5, 5);
            check(corner[3] == 0, "SVG 空白区域保持全透明");
        }

        const cv::Mat css = rasterize(cssSvg, 200, 100);
        check(!css.empty(), "CSS 用例光栅化成功");
        if (!css.empty()) {
            const cv::Vec4b lightDark = css.at<cv::Vec4b>(50, 50);
            check(lightDark[2] >= 250 && lightDark[3] == 255, "light-dark 图元按亮色分支绘制");
            const cv::Vec4b inlineVar = css.at<cv::Vec4b>(50, 150);
            check(inlineVar[1] >= 250 && inlineVar[3] == 255, "内联 style 中的 var() 生效");
        }

        // 可视区域光栅化：四象限图（TL 红 / TR 绿 / BL 蓝 / BR 白，均不透明），
        // 逐块渲染后按颜色断言——同时钉死 lunasvg::Document::render(matrix) 的坐标语义
        const std::string quadrantSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">)svg"
            R"svg(<rect x="0" y="0" width="50" height="50" fill="#ff0000"/>)svg"
            R"svg(<rect x="50" y="0" width="50" height="50" fill="#00ff00"/>)svg"
            R"svg(<rect x="0" y="50" width="50" height="50" fill="#0000ff"/>)svg"
            R"svg(<rect x="50" y="50" width="50" height="50" fill="#ffffff"/></svg>)svg";

        const auto makeVectorImage = [](const std::string& svg, int width, int height) {
            const std::string processed = SVGPreprocessor().preprocessSVG(svg.data(), svg.size());
            const std::string& source = processed.empty() ? svg : processed;
            jark::VectorImage vectorImage;
            vectorImage.document = lunasvg::Document::loadFromData(source.data(), source.size());
            vectorImage.intrinsicWidth = width;
            vectorImage.intrinsicHeight = height;
            return vectorImage;
        };

        jark::VectorImage quadrants = makeVectorImage(quadrantSvg, 100, 100);
        check(quadrants.document != nullptr, "四象限用例加载成功");

        const auto regionCenter = [&](const cv::Rect2d& rect, int rotation) {
            const cv::Mat tile = jark::renderVectorImageRegion(quadrants, rect, rotation,
                static_cast<int>(rect.width), static_cast<int>(rect.height));
            if (tile.empty() || tile.type() != CV_8UC4)
                return cv::Vec4b(0, 0, 0, 0);
            return tile.at<cv::Vec4b>(tile.rows / 2, tile.cols / 2);
        };
        const auto nearColor = [](const cv::Vec4b& value, int b, int g, int r) {
            return std::abs(value[0] - b) <= 4 && std::abs(value[1] - g) <= 4 &&
                std::abs(value[2] - r) <= 4 && value[3] >= 250;
        };

        {
            const cv::Vec4b tl = regionCenter({ 0, 0, 50, 50 }, 0);
            check(nearColor(tl, 0, 0, 255), std::format("区域渲染（旋转 0）左上块为红：(B,G,R,A)=({},{},{},{})",
                tl[0], tl[1], tl[2], tl[3]));
            const cv::Vec4b tr = regionCenter({ 50, 0, 50, 50 }, 0);
            check(nearColor(tr, 0, 255, 0), "区域渲染（旋转 0）右上块为绿");
            const cv::Vec4b bl = regionCenter({ 0, 50, 50, 50 }, 0);
            check(nearColor(bl, 255, 0, 0), "区域渲染（旋转 0）左下块为蓝");
        }
        {
            // 旋转 1 时名义空间 = 文档逆时针转 90°：名义左上块对应文档右上块
            const cv::Vec4b tile = regionCenter({ 0, 0, 50, 50 }, 1);
            check(nearColor(tile, 0, 255, 0), "区域渲染（旋转 1）名义左上块对应文档右上块（绿）");
            const cv::Vec4b lower = regionCenter({ 0, 50, 50, 50 }, 1);
            check(nearColor(lower, 0, 0, 255), "区域渲染（旋转 1）名义左下块对应文档左上块（红）");
        }
        {
            // 半透明：区域渲染同样要反预乘（与整幅渲染口径一致）
            jark::VectorImage translucent = makeVectorImage(translucentSvg, 100, 100);
            const cv::Mat tile = jark::renderVectorImageRegion(translucent, { 25, 25, 50, 50 }, 0, 20, 20);
            check(!tile.empty(), "半透明用例区域渲染成功");
            if (!tile.empty()) {
                const cv::Vec4b center = tile.at<cv::Vec4b>(10, 10);
                check(nearColor(center, 0, 0, 255) == false && center[2] >= 250 && std::abs(center[3] - 128) <= 2,
                    std::format("区域渲染同样反预乘：(B,G,R,A)=({},{},{},{})", center[0], center[1], center[2], center[3]));
            }
        }

        // <switch> 与 foreignObject：draw.io 导出把 XHTML 文本放 foreignObject、等价的
        // <text> 作兜底；lunasvg 画不了 foreignObject，必须落到 <text> 上，否则文字全丢
        const std::string switchSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="60" viewBox="0 0 200 60">)svg"
            R"svg(<rect width="200" height="60" fill="#ffffff"/>)svg"
            R"svg(<switch>)svg"
            R"svg(<foreignObject requiredFeatures="http://www.w3.org/TR/SVG11/feature#Extensibility" x="20" y="10" width="160" height="40">)svg"
            R"svg(<div xmlns="http://www.w3.org/1999/xhtml">FOREIGN</div></foreignObject>)svg"
            R"svg(<text x="100" y="40" font-size="30" text-anchor="middle" fill="#000000">兜底文字</text>)svg"
            R"svg(</switch></svg>)svg";

        const std::string switchFolded = SVGPreprocessor().preprocessSVG(switchSvg.data(), switchSvg.size());
        check(!switchFolded.empty(), "<switch> 用例预处理成功");
        check(switchFolded.find("foreignObject") == std::string::npos,
            "<switch>：不支持 Extensibility 的 foreignObject 被丢弃");
        check(switchFolded.find("<text") != std::string::npos,
            "<switch>：落到等价的 <text> 兜底而不是把它一起删掉");
        {
            // <text> 需要先注册系统字体（lunasvg 无内置字体），与查看器走同一套
            const bool hasFont = jark::ensureVectorFonts();
            jark::VectorImage textImage = makeVectorImage(switchSvg, 200, 60);
            const cv::Mat raster = jark::renderVectorImage(textImage, 200, 60);
            int darkPixels = 0;
            for (int y = 0; y < raster.rows; ++y)
                for (int x = 0; x < raster.cols; ++x) {
                    const cv::Vec4b px = raster.at<cv::Vec4b>(y, x);
                    if (px[3] > 128 && px[0] < 96 && px[1] < 96 && px[2] < 96)
                        ++darkPixels;
                }
            check(!hasFont || darkPixels > 50,
                hasFont ? std::format("<switch>：兜底 <text> 真的画出了文字（暗像素 {}）", darkPixels)
                        : std::string("<switch>：本机没有可注册的系统字体，跳过文字像素断言"));
        }

        // lunasvg 的能力边界（换渲染库或升级时这几条会先报到）：
        // ① SVG filter（feGaussianBlur/feDropShadow…）不支持——**图元必须照常画出来**，
        //    只是没有滤镜效果；真要哪天整块图元被丢掉（比"没模糊"严重得多），这条会失败。
        {
            const std::string filterSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">)svg"
                R"svg(<defs><filter id="blur"><feGaussianBlur stdDeviation="6"/></filter></defs>)svg"
                R"svg(<rect x="20" y="20" width="60" height="60" fill="#ff0000" filter="url(#blur)"/></svg>)svg";
            const cv::Mat filtered = rasterize(filterSvg, 100, 100);
            check(!filtered.empty(), "filter 用例光栅化成功");
            if (!filtered.empty()) {
                const cv::Vec4b center = filtered.at<cv::Vec4b>(50, 50);
                check(center[3] >= 250 && center[2] >= 250 && center[1] <= 5 && center[0] <= 5,
                    std::format("filter 不支持但图元仍绘制（中心 (B,G,R,A)=({},{},{},{})，滤镜被忽略而非丢图元）",
                        center[0], center[1], center[2], center[3]));
            }
        }
        // ② textPath 不支持（整段不画）——记录下来，真补上支持时这条断言会失败，提醒改文档
        {
            const std::string textPathSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">)svg"
                R"svg(<rect width="200" height="100" fill="#ffffff"/>)svg"
                R"svg(<defs><path id="curve" d="M10,80 Q100,10 190,80"/></defs>)svg"
                R"svg(<text font-size="20" fill="#000000"><textPath href="#curve">JarkViewer</textPath></text></svg>)svg";
            jark::ensureVectorFonts();
            const cv::Mat alongPath = rasterize(textPathSvg, 200, 100);
            check(!alongPath.empty(), "textPath 用例光栅化成功");
            if (!alongPath.empty()) {
                int darkPixels = 0;
                for (int y = 0; y < alongPath.rows; ++y)
                    for (int x = 0; x < alongPath.cols; ++x) {
                        const cv::Vec4b px = alongPath.at<cv::Vec4b>(y, x);
                        if (px[3] > 128 && px[0] < 96 && px[1] < 96 && px[2] < 96)
                            ++darkPixels;
                    }
                check(darkPixels == 0,
                    std::format("textPath 已知缺失：不渲染任何文字（暗像素 {}；支持后此条会失败，改文档即可）", darkPixels));
            }
        }

        report = std::format("---- SVG 自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // Exif UserComment 编码自检：AI 生图工具把提示词塞在 UserComment 里，各自的编码五花八门
    // （UNICODE 前缀 + UTF-16 大端/小端、带不带 BOM、ASCII 前缀、无前缀 UTF-8/UTF-16、
    // 8 个 0 前缀）。只按固定端序解，ASCII 提示词会整段变成汉字（"hyperdetailed" -> "栀礀瀀攀爀"）。
    // 这里现造最小 JPEG（SOI + APP1 + EOI），逐个断言解出来的文本。
    std::string runExifTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name, std::string_view got) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}{}\n", ok ? "ok" : "FAIL", name,
                ok ? std::string() : std::format("（解出来是：{}）", got));
        };

        const std::string english = "hyperdetailed ultra-detailed realistic, dark shot, rtx";
        const std::string chinese = "雾中古塔，水墨风格，极致细节，No.0123456789";

        const auto utf16 = [](const std::string& utf8, bool bigEndian, bool withBom) {
            std::vector<uint8_t> out;
            if (withBom) {
                out.push_back(bigEndian ? 0xFE : 0xFF);
                out.push_back(bigEndian ? 0xFF : 0xFE);
            }
            // 输入是 ASCII / UTF-8：这里只做测试用，中文按 UTF-8 切码位再转 UTF-16
            for (size_t i = 0; i < utf8.size();) {
                uint32_t code = static_cast<uint8_t>(utf8[i]);
                size_t length = 1;
                if (code >= 0xF0) { code &= 0x07; length = 4; }
                else if (code >= 0xE0) { code &= 0x0F; length = 3; }
                else if (code >= 0xC0) { code &= 0x1F; length = 2; }
                for (size_t k = 1; k < length && i + k < utf8.size(); ++k)
                    code = (code << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3F);
                i += length;
                if (bigEndian) {
                    out.push_back(static_cast<uint8_t>(code >> 8));
                    out.push_back(static_cast<uint8_t>(code & 0xFF));
                }
                else {
                    out.push_back(static_cast<uint8_t>(code & 0xFF));
                    out.push_back(static_cast<uint8_t>(code >> 8));
                }
            }
            return out;
        };

        // 最小 JPEG：SOI + APP1(Exif) + EOI；UserComment 直接放 IFD0
        const auto makeJpeg = [](const std::vector<uint8_t>& body, bool bigEndianTiff) {
            std::vector<uint8_t> tiff;
            const auto u16 = [&](uint16_t v) {
                tiff.push_back(static_cast<uint8_t>(bigEndianTiff ? v >> 8 : v & 0xFF));
                tiff.push_back(static_cast<uint8_t>(bigEndianTiff ? v & 0xFF : v >> 8));
            };
            const auto u32 = [&](uint32_t v) {
                for (int shift : { bigEndianTiff ? 24 : 0, bigEndianTiff ? 16 : 8,
                                   bigEndianTiff ? 8 : 16, bigEndianTiff ? 0 : 24 })
                    tiff.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
            };
            tiff.push_back(bigEndianTiff ? 'M' : 'I');
            tiff.push_back(bigEndianTiff ? 'M' : 'I');
            u16(42);
            u32(8);              // IFD0 偏移
            u16(1);              // 条目数
            u16(0x9286);         // UserComment
            u16(7);              // UNDEFINED
            u32(static_cast<uint32_t>(body.size()));
            u32(8 + 2 + 12 + 4); // 正文偏移：目录(2) + 一项(12) + next IFD(4)
            u32(0);              // 没有下一个 IFD
            tiff.insert(tiff.end(), body.begin(), body.end());

            std::vector<uint8_t> app1{ 'E', 'x', 'i', 'f', 0, 0 };
            app1.insert(app1.end(), tiff.begin(), tiff.end());

            std::vector<uint8_t> jpeg{ 0xFF, 0xD8, 0xFF, 0xE1 };
            const uint16_t segmentLength = static_cast<uint16_t>(app1.size() + 2);
            jpeg.push_back(static_cast<uint8_t>(segmentLength >> 8));
            jpeg.push_back(static_cast<uint8_t>(segmentLength & 0xFF));
            jpeg.insert(jpeg.end(), app1.begin(), app1.end());
            jpeg.push_back(0xFF);
            jpeg.push_back(0xD9);
            return jpeg;
        };

        const auto prefix = [](const char* code) {
            std::vector<uint8_t> out(8, 0);
            std::memcpy(out.data(), code, std::strlen(code));
            return out;
        };
        const auto text = [&](const std::string& s) {
            std::vector<uint8_t> out(s.begin(), s.end());
            return out;
        };
        const auto concat = [](std::vector<uint8_t> head, const std::vector<uint8_t>& tail) {
            head.insert(head.end(), tail.begin(), tail.end());
            return head;
        };

        const auto decode = [&](const std::vector<uint8_t>& body, bool bigEndianTiff,
            const std::string& name, const std::string& expected) {
            const auto jpeg = makeJpeg(body, bigEndianTiff);
            const std::string decoded = ExifParse::getExif(L"uc-test.jpg", jpeg.data(), jpeg.size());
            check(decoded.find(expected) != std::string::npos, name,
                decoded.substr(0, 60));
        };

        decode(concat(prefix("UNICODE"), utf16(english, false, false)), false,
            "UNICODE 前缀 + UTF-16LE（无 BOM）", english);
        decode(concat(prefix("UNICODE"), utf16(english, true, false)), false,
            "UNICODE 前缀 + UTF-16BE（无 BOM）", english);
        decode(concat(prefix("UNICODE"), utf16(english, false, true)), false,
            "UNICODE 前缀 + UTF-16LE + BOM", english);
        decode(concat(prefix("UNICODE"), utf16(english, true, true)), true,
            "UNICODE 前缀 + UTF-16BE + BOM", english);
        decode(concat(prefix("UNICODE"), utf16(chinese, false, false)), false,
            "UNICODE 前缀 + 中文 UTF-16LE", chinese);
        decode(concat(prefix("UNICODE"), utf16(chinese, true, false)), true,
            "UNICODE 前缀 + 中文 UTF-16BE", chinese);
        decode(concat(prefix("ASCII"), text(english)), false,
            "ASCII 前缀 + 单字节正文", english);
        decode(text(chinese), false, "无前缀 + UTF-8 正文", chinese);
        decode(utf16(chinese, false, false), false, "无前缀 + 中文 UTF-16LE", chinese);
        decode(concat(std::vector<uint8_t>(8, 0), text(english)), false,
            "8 个 0 前缀（未指定字符集）+ 单字节正文", english);

        report = std::format("---- Exif 自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // 文件列表排序自检：造三个文件，使名称序、修改时间序、大小序互不相同，
    // 逐个排序方式断言顺序，并验证"当前图片"的下标能跟着重排走。
    std::string runSortTest() {
        namespace fs = std::filesystem;

        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        const fs::path dir = fs::temp_directory_path() / L"JarkViewer-sort-tests";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (ec) {
            report += std::format("无法创建测试目录 {}\n", jarkUtils::wstringToUtf8(dir.wstring()));
            return std::format("---- 排序自检：0 通过, 1 失败 ----\n{}", report);
        }

        // 名称自然序：img1 < img2 < img10；大小降序：img10 > img1 > img2；时间降序：img2 > img10 > img1
        const auto writeFile = [&](const wchar_t* name, size_t bytes, int minutesAgo) {
            const fs::path path = dir / name;
            std::ofstream(path, std::ios::binary).write(std::string(bytes, 'x').data(),
                static_cast<std::streamsize>(bytes));
            fs::last_write_time(path, fs::file_time_type::clock::now() -
                std::chrono::minutes(minutesAgo), ec);
            return path.wstring();
        };

        const std::wstring file1 = writeFile(L"img1.png", 200, 30);
        const std::wstring file2 = writeFile(L"img2.png", 100, 10);
        const std::wstring file10 = writeFile(L"img10.png", 300, 20);

        const auto byName = [](const std::wstring& path) {
            return fs::path(path).filename().wstring();
        };
        // files 顺序与目录枚举无关，这里按固定的初始顺序摆放
        const std::vector<std::wstring> seed{ file1, file2, file10 };

        const auto orderedBy = [&](uint32_t mode, int currentIndex) {
            std::vector<std::wstring> files = seed;
            int index = currentIndex;
            jarkUtils::sortImageFileList(files, mode, index);
            std::wstring order;
            for (const auto& path : files)
                order += byName(path) + L" ";
            return std::pair<std::wstring, int>{ order, index };
        };

        {
            const auto [order, index] = orderedBy(0, 1); // 当前图片 = img2（初始下标 1）
            check(order == L"img1.png img2.png img10.png ", "名称：数字感知自然序");
            check(index == 1, "名称：当前图片下标跟随重排");
        }
        {
            const auto [order, index] = orderedBy(1, 1);
            check(order == L"img2.png img10.png img1.png ", "修改时间：新的在前");
            check(index == 0, "修改时间：当前图片下标跟随重排");
        }
        {
            const auto [order, index] = orderedBy(2, 1);
            check(order == L"img10.png img1.png img2.png ", "文件大小：大的在前");
            check(index == 2, "文件大小：当前图片下标跟随重排");
        }

        fs::remove_all(dir, ec);
        report = std::format("---- 排序自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
        return report;
    }

    // 实时频谱自检（纯合成，不需要语料）：频段映射、电平标定、衰减、以及"按播放位置取帧"。
    // 这几条坏掉时界面上只表现为"条在跳"，看不出跳得对不对——尤其"取哪一帧"这一条：
    // 拿解码线程最新算出的那一帧也能跳，但会比听到的声音早一两秒。
    std::string runSpectrumTest() {
        std::string report;
        int passed = 0, failed = 0;
        const auto check = [&](bool ok, const std::string& name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };

        constexpr int kRate = 48000;
        constexpr int kBandCount = jark::AudioSpectrumAnalyzer::kDefaultBandCount;
        constexpr double kPi = 3.14159265358979323846;

        // 立体声交错样本：正弦（幅度按满幅的几分之一）与静音
        const auto makeSine = [&](double hz, int milliseconds, double amplitude) {
            const int frames = kRate * milliseconds / 1000;
            std::vector<int16_t> samples(static_cast<size_t>(frames) * 2);
            for (int i = 0; i < frames; ++i) {
                const auto value = static_cast<int16_t>(std::lround(std::sin(2.0 * kPi * hz * i / kRate) * amplitude * 32767.0));
                samples[static_cast<size_t>(i) * 2] = value;
                samples[static_cast<size_t>(i) * 2 + 1] = value;
            }
            return samples;
        };
        const auto makeSilence = [&](int milliseconds) {
            return std::vector<int16_t>(static_cast<size_t>(kRate) * milliseconds / 1000 * 2, 0);
        };
        // 标称频率范围含 hz 的那一段（映射对不对，就看最强段是不是它）
        const auto bandOf = [&](const jark::AudioSpectrumAnalyzer& analyzer, double hz) {
            for (int band = 0; band < analyzer.bandCount(); ++band) {
                double lowHz = 0.0;
                double highHz = 0.0;
                analyzer.bandRangeHz(band, lowHz, highHz);
                if (hz >= lowHz && hz < highHz)
                    return band;
            }
            return analyzer.bandCount() - 1; // 高过最上段：归到最后一段
        };
        const auto strongestBand = [](const std::vector<float>& levels) {
            return static_cast<int>(std::max_element(levels.begin(), levels.end()) - levels.begin());
        };
        const auto inRange = [](const std::vector<float>& levels) {
            return std::all_of(levels.begin(), levels.end(),
                [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; });
        };
        // 高频段（上 1/4）与最低三段（40~120Hz）的平均电平：纯单音在两侧都应该几乎是 0。
        // 最低那三段是"直流偏置有没有滤掉"的哨兵——它们挤在同一个 bin 上，最容易被点亮
        const auto highMean = [](const std::vector<float>& levels) {
            const size_t from = levels.size() * 3 / 4;
            double sum = 0.0;
            for (size_t i = from; i < levels.size(); ++i)
                sum += levels[i];
            return levels.size() > from ? sum / static_cast<double>(levels.size() - from) : 0.0;
        };
        const auto lowMean = [](const std::vector<float>& levels) {
            const size_t count = (std::min)(size_t{ 3 }, levels.size());
            double sum = 0.0;
            for (size_t i = 0; i < count; ++i)
                sum += levels[i];
            return count > 0 ? sum / static_cast<double>(count) : 0.0;
        };

        // 1) 442Hz 级别的单音落在哪一段、电平标定对不对（幅度 1/4 ≈ -12dBFS）
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            analyzer.push(makeSine(440.0, 400, 0.25), 0);

            std::vector<float> levels(kBandCount, 0.0f);
            check(analyzer.readAt(300, levels), "喂过样本之后能取到频谱帧");
            check(inRange(levels), "所有频段电平都在 [0,1] 且有限");

            const int expected = bandOf(analyzer, 440.0);
            check(strongestBand(levels) == expected,
                std::format("440Hz 单音的最强段是第 {} 段（含 440Hz），实际第 {} 段", expected, strongestBand(levels)));
            check(levels[expected] > 0.4,
                std::format("单音段电平 {:.2f} > 0.4（幅度 1/4 ≈ -12dBFS）", levels[expected]));
            check(highMean(levels) < 0.2,
                std::format("纯单音在高频段几乎没有能量（上 1/4 平均 {:.2f}）", highMean(levels)));
        }

        // 2) 换成高频单音：最强段要跟着挪（映射在整个频段上都对，不是只在低频段对）
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            analyzer.push(makeSine(8000.0, 400, 0.25), 0);

            std::vector<float> levels(kBandCount, 0.0f);
            analyzer.readAt(300, levels);
            const int expected = bandOf(analyzer, 8000.0);
            check(strongestBand(levels) == expected,
                std::format("8kHz 单音的最强段是第 {} 段（含 8kHz），实际第 {} 段", expected, strongestBand(levels)));
            check(levels[bandOf(analyzer, 440.0)] < 0.3,
                std::format("8kHz 单音在 440Hz 段上很弱（{:.2f}）", levels[bandOf(analyzer, 440.0)]));
            check(inRange(levels), "8kHz 单音的电平仍在 [0,1] 内");
            check(lowMean(levels) < 0.1,
                std::format("8kHz 单音在最低三段上几乎是 0（平均 {:.2f}）", lowMean(levels)));
        }

        // 2b) 带直流偏置的单音：直流必须被高通掉。素材里几十 LSB 的直流偏置会全落进
        //     最低那一两个 bin，而低频那几段挤在同一个 bin 上——不滤掉的话最低几根条
        //     会一直亮着（实测 440Hz 的测试音就带 -50dBFS 的偏置）
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            const auto samples = makeSine(8000.0, 400, 0.25);
            std::vector<int16_t> biased = samples;
            for (auto& sample : biased) {
                const int32_t value = sample + 2000; // ≈ -24dBFS 的直流
                sample = static_cast<int16_t>(std::clamp(value, -32768, 32767));
            }
            analyzer.push(biased, 0);

            std::vector<float> levels(kBandCount, 0.0f);
            analyzer.readAt(300, levels);
            check(lowMean(levels) < 0.1,
                std::format("直流偏置不会把最低几段点亮（平均 {:.2f}）", lowMean(levels)));
            check(strongestBand(levels) == bandOf(analyzer, 8000.0), "带直流偏置时最强段仍是 8kHz");
        }

        // 3) 时间戳取用：同一个分析器里"前半段有声、后半段静音"，按不同播放位置取到不同结果
        //    ——"频谱跟着播放位置走"就是这一条，也是不直接用最新一帧的原因
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            analyzer.push(makeSine(440.0, 400, 0.25), 0);
            analyzer.push(makeSilence(600), 400);

            std::vector<float> levels(kBandCount, 0.0f);
            const int expected = bandOf(analyzer, 440.0);
            analyzer.readAt(200, levels);
            const float duringTone = levels[expected];
            analyzer.readAt(950, levels);
            const float duringSilence = levels[expected];

            check(duringTone > 0.4, std::format("位置 200ms（有声段）取到电平 {:.2f} > 0.4", duringTone));
            check(duringSilence < 0.05,
                std::format("位置 950ms（静音段）取到电平 {:.2f} < 0.05（没有拿旧的那一帧顶替）", duringSilence));
        }

        // 4) 衰减：静音之后要看得见"落下去"，且单调不增（不许反弹）
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            analyzer.push(makeSine(440.0, 400, 0.25), 0);

            std::vector<float> levels(kBandCount, 0.0f);
            const int expected = bandOf(analyzer, 440.0);
            analyzer.readAt(300, levels);
            float previous = levels[expected];

            bool monotonic = true;
            int64_t pts = 400;
            for (int step = 0; step < 12; ++step) {
                analyzer.push(makeSilence(50), pts);
                pts += 50;
                analyzer.readAt(pts - 1, levels);
                if (levels[expected] > previous + 1e-4f)
                    monotonic = false;
                previous = levels[expected];
            }

            check(monotonic, "静音段里电平单调不增（不会反弹）");
            check(previous < 0.05, std::format("静音 600ms 后电平落到 {:.2f} < 0.05", previous));
        }

        // 5) reset：seek 会调它，清掉之后不该再取到旧位置的帧
        {
            jark::AudioSpectrumAnalyzer analyzer(kRate);
            analyzer.push(makeSine(440.0, 400, 0.25), 0);

            std::vector<float> levels(kBandCount, 0.0f);
            check(analyzer.readAt(300, levels), "reset 之前能取到帧");
            analyzer.reset();
            check(!analyzer.readAt(300, levels), "reset 之后取不到帧（旧位置的帧已作废）");
        }

        report = std::format("---- 频谱自检：{} 通过, {} 失败 ----\n", passed, failed) + report;
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

    installCrashDumpHandler();

    std::vector<std::wstring> targets;
    std::wstring reportPath = L"decode-probe.txt";
    bool fullExif = false;
    bool audioTest = false;
    bool playbackTest = false;
    bool videoTest = false;
    bool languageTest = false;
    bool batchTest = false;
    bool annotateTest = false;
    bool navigationTest = false;
    bool colorTest = false;
    bool resampleTest = false;
    bool cacheTest = false;
    bool spectrumTest = false;
    bool thumbnailTest = false;
    bool shellThumbnailTest = false;
    bool vectorTest = false;
    bool sortTest = false;
    bool exifTest = false;
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
        if (argv[i] == L"--playback-test") {
            playbackTest = true;
            continue;
        }
        if (argv[i] == L"--video-test") {
            videoTest = true;
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
        if (argv[i] == L"--color-test") {
            colorTest = true;
            continue;
        }
        if (argv[i] == L"--resample-test") {
            resampleTest = true;
            continue;
        }
        if (argv[i] == L"--cache-test") {
            cacheTest = true;
            continue;
        }
        if (argv[i] == L"--spectrum-test") {
            spectrumTest = true;
            continue;
        }
        if (argv[i] == L"--svg-test") {
            vectorTest = true;
            continue;
        }
        if (argv[i] == L"--sort-test") {
            sortTest = true;
            continue;
        }
        if (argv[i] == L"--exif-test") {
            exifTest = true;
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

    // 合成类自检不需要输入文件（用合成底图/纯逻辑断言），用真实图片只是额外输出可视化结果
    if (targets.empty() && !annotateTest && !languageTest && !navigationTest && !colorTest && !resampleTest && !spectrumTest && !thumbnailTest && !vectorTest && !sortTest && !exifTest && !cacheTest && thumbnailWriter < 0) {
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
    if (colorTest) {
        const auto text = runColorTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (resampleTest) {
        const auto text = runResampleTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (cacheTest) {
        const auto text = runCacheTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (spectrumTest) {
        const auto text = runSpectrumTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (vectorTest) {
        const auto text = runVectorTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (sortTest) {
        const auto text = runSortTest();
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }
    if (exifTest) {
        const auto text = runExifTest();
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

    if (playbackTest) {
        const auto text = runPlaybackTest(targets);
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
    }

    if (videoTest) {
        const auto text = runVideoTest(targets);
        emit(text);
        return text.find("FAIL") == std::string::npos ? 0 : 1;
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
