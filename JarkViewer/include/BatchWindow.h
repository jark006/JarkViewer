#pragma once

// 批量处理窗口（ImGui 版）：转换格式、缩放、重命名、旋转翻转。
// 处理逻辑仍在 BatchProcessor（可脱离界面验证），界面由主窗口的 DrawUi() 每帧驱动。

#include "BatchProcessor.h"
#include "Localization.h"
#include "UiHost.h"
#include "jarkUtils.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <shlobj.h>
#include <shlwapi.h>

class BatchWindow {
public:
    static BatchWindow& instance() {
        static BatchWindow window;
        return window;
    }

    void open(std::vector<std::wstring> files) {
        files_ = std::move(files);
        std::sort(files_.begin(), files_.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
            });

        fileNames_.clear();
        for (const auto& file : files_)
            fileNames_.push_back(jarkUtils::wstringToUtf8(std::filesystem::path(file).filename().wstring()));

        selected_.clear();
        for (size_t i = 0; i < files_.size(); ++i)
            selected_.insert(i);

        finished_ = false;
        startFailed_ = false;
        result_ = {};
        progressValue_ = 0;
        progressMax_ = (std::max)(size_t{ 1 }, files_.size());

        {   // 新的文件列表要重新取缩放预览的样张尺寸
            std::lock_guard lock(previewMutex_);
            previewPath_.clear();
            previewRequest_.clear();
            previewSource_ = {};
            previewLoading_ = false;
        }

        visible_ = true;
        focusRequested_ = true;
    }

    void close() { visible_ = false; }
    bool visible() const { return visible_; }

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::SetNextWindowSize({ 700.0f * scale, 580.0f * scale }, ImGuiCond_FirstUseEver);
        // 不能再缩小到藏住任务/参数/进度与底部按钮
        ImGui::SetNextWindowSizeConstraints({ 640.0f * scale, 460.0f * scale }, { FLT_MAX, FLT_MAX });
        if (focusRequested_) {
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, { 0.5f, 0.5f });
            focusRequested_ = false;
        }

        bool open = true;
        if (!ImGui::Begin(title().c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
            ImGui::End();
            if (!open)
                close();
            return;
        }

        drawFileList(scale);
        ImGui::Separator();
        drawTaskPanel(scale);
        ImGui::Separator();
        drawActions(scale);

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape) && !running_)
            close();

        ImGui::End();

        if (!open)
            close();
    }

private:
    BatchWindow() = default;

    static std::string title() {
        return jarkUtils::wstringToUtf8(getUIStringW(42).c_str()) + "###batch";
    }

    static const char* ui(uint32_t id) { return getUIString(id); }

    // —— 文件列表 ——

    void drawFileList(float scale) {
        ImGui::TextUnformatted(ui(kStrFileList));

        const float listHeight = 180.0f * scale;
        if (ImGui::BeginChild("batchFiles", { 0, listHeight }, ImGuiChildFlags_Borders)) {
            for (size_t index = 0; index < fileNames_.size(); ++index) {
                bool checked = selected_.contains(index);
                if (ImGui::Checkbox((fileNames_[index] + "##" + std::to_string(index)).c_str(), &checked)) {
                    if (checked)
                        selected_.insert(index);
                    else
                        selected_.erase(index);
                }
            }
        }
        ImGui::EndChild();

        if (ImGui::Button(ui(8), { 120.0f * scale, 0 })) {
            for (size_t i = 0; i < files_.size(); ++i)
                selected_.insert(i);
        }
        ImGui::SameLine();
        if (ImGui::Button(ui(9), { 120.0f * scale, 0 }))
            selected_.clear();

        ImGui::SameLine();
        ImGui::TextDisabled("%zu / %zu", selected_.size(), files_.size());
    }

    // —— 任务参数 ——

    void drawTaskPanel(float scale) {
        const char* taskNames[] = { ui(kStrConvert), ui(kStrScale), ui(kStrRename), ui(kStrRotate) };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui(kStrTask));
        ImGui::SameLine();

        for (int index = 0; index < 4; ++index) {
            if (index > 0)
                ImGui::SameLine();
            if (ImGui::RadioButton((std::string(taskNames[index]) + "##task").c_str(),
                taskIndex_ == static_cast<uint32_t>(index)))
                taskIndex_ = static_cast<uint32_t>(index);
        }

        ImGui::Spacing();

        switch (taskIndex_) {
        case 1:
            options_.task = jark::BatchTask::Scale;
            options_.outputExtension.clear(); // 缩放保持原格式，不转格式
            drawScalePanel(scale);
            break;
        case 2: options_.task = jark::BatchTask::Rename; drawRenamePanel(scale); break;
        case 3: options_.task = jark::BatchTask::Rotate; drawRotatePanel(scale); break;
        default:
            options_.task = jark::BatchTask::Convert;
            options_.outputExtension = jarkUtils::utf8ToWstring(kOutputFormats[formatIndex_]);
            drawConvertPanel(scale);
            break;
        }
    }

    void drawConvertPanel(float scale) {
        int format = static_cast<int>(formatIndex_);
        ImGui::SetNextItemWidth(140.0f * scale);
        if (ImGui::Combo(ui(kStrOutputFormat), &format, kOutputFormats, IM_ARRAYSIZE(kOutputFormats)))
            formatIndex_ = static_cast<uint32_t>(format);

        ImGui::Checkbox(ui(kStrApplyAdjust), &options_.applyAdjustments);
        if (options_.applyAdjustments) {
            ImGui::Indent(20.0f * scale);

            const char* colorModes[] = { ui(kStrColor), ui(kStrGray), ui(kStrDocument), ui(kStrDither) };
            int colorMode = static_cast<int>(colorModeIndex_);
            ImGui::SetNextItemWidth(200.0f * scale);
            if (ImGui::Combo(ui(kStrColorMode), &colorMode, colorModes, IM_ARRAYSIZE(colorModes))) {
                colorModeIndex_ = static_cast<uint32_t>(colorMode);
                options_.colorMode = colorModeIndex_;
            }

            ImGui::SetNextItemWidth(220.0f * scale);
            ImGui::SliderInt(ui(kStrBrightness), &options_.brightness, 0, 200, "%d");
            ImGui::SetNextItemWidth(220.0f * scale);
            ImGui::SliderInt(ui(kStrContrast), &options_.contrast, 0, 200, "%d");

            ImGui::Unindent(20.0f * scale);
        }

        drawOutputSection(scale);
    }

    // —— 缩放 ——

    void drawScalePanel(float scale) {
        const char* modeNames[] = { ui(kStrByPercent), ui(kStrByWidth), ui(kStrByHeight),
                                    ui(kStrCustomSize), ui(kStrMaxLongEdge) };
        const char* algorithmNames[] = { ui(kStrAuto), ui(kStrNearest), ui(kStrBilinear),
                                         ui(kStrArea), ui(kStrCubic), "Lanczos" };

        int mode = static_cast<int>(scaleModeIndex_);
        ImGui::SetNextItemWidth(140.0f * scale);
        if (ImGui::Combo(ui(kStrScaleMode), &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
            scaleModeIndex_ = static_cast<uint32_t>(mode);
            options_.scaleMode = static_cast<jark::ScaleMode>(scaleModeIndex_);
        }

        ImGui::SameLine(0.0f, 24.0f * scale);
        int algorithm = static_cast<int>(scaleAlgorithmIndex_);
        ImGui::SetNextItemWidth(140.0f * scale);
        if (ImGui::Combo(ui(kStrResample), &algorithm, algorithmNames, IM_ARRAYSIZE(algorithmNames))) {
            scaleAlgorithmIndex_ = static_cast<uint32_t>(algorithm);
            options_.scaleAlgorithm = static_cast<jark::ScaleAlgorithm>(scaleAlgorithmIndex_);
        }

        const auto inputSize = [&](const char* label, int* value, int maximum) {
            ImGui::SetNextItemWidth(120.0f * scale);
            if (ImGui::InputInt(label, value))
                *value = std::clamp(*value, 1, maximum);
            };

        switch (options_.scaleMode) {
        case jark::ScaleMode::Percent:
            inputSize(ui(kStrPercent), &options_.scalePercent, 1000);
            ImGui::SameLine();
            ImGui::TextDisabled("1 - 1000%%");
            break;
        case jark::ScaleMode::Width:
            inputSize(ui(kStrWidth), &options_.scaleWidth, 30000);
            break;
        case jark::ScaleMode::Height:
            inputSize(ui(kStrHeight), &options_.scaleHeight, 30000);
            break;
        case jark::ScaleMode::Stretch:
            inputSize(ui(kStrWidth), &options_.scaleWidth, 30000);
            ImGui::SameLine(0.0f, 16.0f * scale);
            inputSize(ui(kStrHeight), &options_.scaleHeight, 30000);
            break;
        default:
            inputSize(ui(kStrLongEdge), &options_.scaleMaxEdge, 30000);
            break;
        }

        drawScalePreview(scale);

        drawOutputSection(scale);
    }

    // 输出分辨率预览：样张取第一个选中文件（没有选中就用列表第一张）。
    // 取尺寸要整图解码（几十毫秒到几百毫秒），放到常驻工作线程，界面只读缓存。
    void drawScalePreview(float scale) {
        const std::wstring sample = previewSampleFile();
        if (sample.empty())
            return;

        bool wakePreviewThread = false;
        bool loading = false;
        jark::ImageSize source;
        {
            std::lock_guard lock(previewMutex_);
            if (previewPath_ != sample) {
                previewPath_ = sample;
                previewSource_ = {};
                previewLoading_ = true;
                previewRequest_ = sample;
                wakePreviewThread = true;
            }
            source = previewSource_;
            loading = previewLoading_;
        }
        if (wakePreviewThread) {
            ensurePreviewThread();
            previewCv_.notify_one();
        }

        const std::string name = jarkUtils::wstringToUtf8(
            std::filesystem::path(sample).filename().wstring());

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui(kStrPreview));
        ImGui::SameLine();
        if (source.empty()) {
            ImGui::TextDisabled("%s  %s", name.c_str(),
                loading ? ui(kStrLoading) : ui(kStrUnreadableImage));
            return;
        }

        const jark::ImageSize target = jark::scaledSize(source, options_);
        ImGui::TextDisabled("%s  %d × %d → %d × %d", name.c_str(),
            source.width, source.height, target.width, target.height);
    }

    std::wstring previewSampleFile() const {
        if (!selected_.empty()) {
            const size_t index = *selected_.begin(); // std::set 有序：文件列表中排最前的那个
            if (index < files_.size())
                return files_[index];
        }
        return files_.empty() ? std::wstring() : files_.front();
    }

    void ensurePreviewThread() {
        if (previewThreadStarted_)
            return;
        previewThreadStarted_ = true;

        std::thread([this] {
            for (;;) {
                std::wstring path;
                {
                    std::unique_lock lock(previewMutex_);
                    previewCv_.wait(lock, [this] { return !previewRequest_.empty(); });
                    path = std::move(previewRequest_);
                    previewRequest_.clear();
                }

                jark::ImageSize size;
                const bool ok = jark::loadImageSize(path, size);

                std::lock_guard lock(previewMutex_);
                if (previewPath_ == path) { // 期间换了样张就丢掉这次结果
                    previewSource_ = ok ? size : jark::ImageSize{};
                    previewLoading_ = false;
                }
            }
            }).detach();
    }

    void drawOutputSection(float scale) {
        ImGui::Checkbox(ui(kStrOverwrite), &options_.overwrite);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui(kStrOutputDir));
        ImGui::SameLine();
        if (ImGui::Button(ui(kStrChooseDir), { 140.0f * scale, 0 }))
            chooseOutputDirectory();
        ImGui::SameLine();
        if (ImGui::Button(ui(kStrSameDir), { 160.0f * scale, 0 }))
            outputDirectory_.clear();

        if (!outputDirectory_.empty())
            ImGui::TextDisabled("%s", jarkUtils::wstringToUtf8(outputDirectory_).c_str());
    }

    void drawRenamePanel(float scale) {
        ImGui::SetNextItemWidth(220.0f * scale);
        ImGui::InputText(ui(kStrPrefix), &renamePrefix_);

        ImGui::SetNextItemWidth(120.0f * scale);
        int start = options_.renameStart;
        if (ImGui::InputInt(ui(kStrStart), &start))
            options_.renameStart = (std::max)(0, start);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * scale);
        int digits = options_.renameDigits;
        if (ImGui::InputInt(ui(kStrDigits), &digits))
            options_.renameDigits = std::clamp(digits, 1, 8);

        const std::wstring extension = files_.empty() ? std::wstring()
            : std::filesystem::path(files_.front()).extension().wstring();
        ImGui::TextDisabled("%s", (std::string(renamePrefix_) +
            std::format("{:0{}}", options_.renameStart, options_.renameDigits) +
            jarkUtils::wstringToUtf8(extension)).c_str());
    }

    void drawRotatePanel(float scale) {
        const char* rotations[] = { ui(kStrNoRotation), ui(kStrRotate90), "180°", ui(kStrRotate270) };
        int rotation = static_cast<int>(rotationIndex_);
        ImGui::SetNextItemWidth(200.0f * scale);
        if (ImGui::Combo(ui(kStrRotate), &rotation, rotations, IM_ARRAYSIZE(rotations))) {
            rotationIndex_ = static_cast<uint32_t>(rotation);
            options_.rotationDegrees = static_cast<int>(rotationIndex_) * 90;
        }

        ImGui::Checkbox(ui(kStrFlipH), &options_.flipHorizontal);
        ImGui::SameLine();
        ImGui::Checkbox(ui(kStrFlipV), &options_.flipVertical);
    }

    // —— 进度与操作 ——

    void drawActions(float scale) {
        const float buttonWidth = 150.0f * scale;

        ImGui::BeginDisabled(running_ || selected_.empty());
        if (ImGui::Button(ui(kStrStart), { buttonWidth, 0 }))
            startBatch();
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!running_);
        if (ImGui::Button(ui(kStrCancel), { buttonWidth, 0 }))
            cancelRequested_ = true;
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(running_);
        if (ImGui::Button(ui(kStrClose), { buttonWidth, 0 }))
            close();
        ImGui::EndDisabled();

        const float progress = progressMax_ > 0
            ? static_cast<float>(progressValue_) / static_cast<float>(progressMax_)
            : 0.0f;
        ImGui::ProgressBar(progress, { ImGui::GetContentRegionAvail().x, 0 });

        if (startFailed_)
            ImGui::TextColored({ 1.0f, 0.45f, 0.35f, 1.0f }, "%s", ui(kStrSelectFirst));
        else if (running_)
            ImGui::TextUnformatted(ui(kStrProcessing));
        else if (finished_) {
            ImGui::TextUnformatted(std::vformat(getUIString(kStrFinished),
                std::make_format_args(result_.succeeded, result_.skipped, result_.failed)).c_str());
            for (const auto& message : result_.messages)
                ImGui::TextDisabled("%s", jarkUtils::wstringToUtf8(message).c_str());
        }
    }

    void chooseOutputDirectory() {
        // 用 owning 的局部变量：指针要活到对话框结束，不能指向临时对象
        const std::wstring browseTitle = getUIStringW(kStrOutputDir).str();

        BROWSEINFOW browse{};
        browse.lpszTitle = browseTitle.c_str();
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        if (LPITEMIDLIST item = SHBrowseForFolderW(&browse)) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(item, path))
                outputDirectory_ = path;
            CoTaskMemFree(item);
        }
    }

    void startBatch() {
        if (running_)
            return;

        std::vector<std::wstring> targets;
        for (const auto index : selected_) {
            if (index < files_.size())
                targets.push_back(files_[index]);
        }

        if (targets.empty()) {
            startFailed_ = true;
            return;
        }
        startFailed_ = false;

        options_.outputDirectory = outputDirectory_;
        options_.renamePrefix = jarkUtils::utf8ToWstring(renamePrefix_);

        running_ = true;
        cancelRequested_ = false;
        finished_ = false;
        progressValue_ = 0;
        progressMax_ = targets.size();
        result_ = {};

        const auto options = options_;
        std::thread worker([this, targets, options]() {
            result_ = jark::runBatch(targets, options,
                [this](size_t current, size_t total, const std::wstring&) {
                    progressValue_ = current;
                    progressMax_ = total ? total : 1;
                    return !cancelRequested_.load();
                });
            finished_ = true;
            running_ = false;
            {
                // 处理可能改写了源文件（就地缩放等），预览要重新取尺寸
                std::lock_guard lock(previewMutex_);
                previewPath_.clear();
            }
            });
        worker.detach();
    }

    // 输出格式（与 BatchProcessor 的 writeEncoded 支持范围一致）
    static constexpr const char* const kOutputFormats[] = { "png", "jpg", "webp", "bmp", "tif" };

    // 字符串表 ID（与 stringRes.cpp 中批量处理段落对应）
    static constexpr uint32_t kStrFileList = 59;
    static constexpr uint32_t kStrConvert = 60;
    static constexpr uint32_t kStrRename = 61;
    static constexpr uint32_t kStrRotate = 62;
    static constexpr uint32_t kStrOutputFormat = 64;
    static constexpr uint32_t kStrColor = 66;
    static constexpr uint32_t kStrGray = 67;
    static constexpr uint32_t kStrDocument = 68;
    static constexpr uint32_t kStrDither = 69;
    static constexpr uint32_t kStrStart = 70;
    static constexpr uint32_t kStrCancel = 71;
    static constexpr uint32_t kStrOutputDir = 72;
    static constexpr uint32_t kStrChooseDir = 73;
    static constexpr uint32_t kStrSameDir = 74;
    static constexpr uint32_t kStrPrefix = 75;
    static constexpr uint32_t kStrNoRotation = 76;
    static constexpr uint32_t kStrRotate90 = 77;
    static constexpr uint32_t kStrRotate270 = 78;
    static constexpr uint32_t kStrFlipH = 79;
    static constexpr uint32_t kStrFlipV = 80;
    static constexpr uint32_t kStrOverwrite = 81;
    static constexpr uint32_t kStrSelectFirst = 82;
    static constexpr uint32_t kStrProcessing = 83;
    static constexpr uint32_t kStrFinished = 84;
    static constexpr uint32_t kStrTask = 86;
    static constexpr uint32_t kStrApplyAdjust = 87;
    static constexpr uint32_t kStrColorMode = 88;
    static constexpr uint32_t kStrBrightness = 89;
    static constexpr uint32_t kStrContrast = 90;
    static constexpr uint32_t kStrClose = 124;  // 关闭
    static constexpr uint32_t kStrDigits = 125; // 序号位数

    // 缩放任务（窄表新增条目，追加在表尾）
    static constexpr uint32_t kStrScale = 129;        // 缩放
    static constexpr uint32_t kStrScaleMode = 130;    // 缩放方式
    static constexpr uint32_t kStrByPercent = 131;    // 按百分比
    static constexpr uint32_t kStrByWidth = 132;      // 按宽度
    static constexpr uint32_t kStrByHeight = 133;     // 按高度
    static constexpr uint32_t kStrCustomSize = 134;   // 自定义宽高
    static constexpr uint32_t kStrMaxLongEdge = 135;  // 限制长边
    static constexpr uint32_t kStrPercent = 136;      // 百分比
    static constexpr uint32_t kStrWidth = 137;        // 宽度
    static constexpr uint32_t kStrHeight = 138;       // 高度
    static constexpr uint32_t kStrLongEdge = 139;     // 长边
    static constexpr uint32_t kStrResample = 140;     // 缩放算法
    static constexpr uint32_t kStrAuto = 141;         // 自动
    static constexpr uint32_t kStrNearest = 142;      // 最近邻
    static constexpr uint32_t kStrBilinear = 143;     // 双线性
    static constexpr uint32_t kStrArea = 144;         // 面积平均
    static constexpr uint32_t kStrCubic = 145;        // 三次插值
    static constexpr uint32_t kStrPreview = 146;      // 预览
    static constexpr uint32_t kStrLoading = 147;      // 读取中...
    static constexpr uint32_t kStrUnreadableImage = 148; // 无法读取该图像

    bool visible_ = false;
    bool focusRequested_ = false;

    std::vector<std::wstring> files_;
    std::vector<std::string> fileNames_;
    std::set<size_t> selected_;

    jark::BatchOptions options_;
    std::string renamePrefix_ = "image_";
    std::wstring outputDirectory_;

    uint32_t taskIndex_ = 0;
    uint32_t formatIndex_ = 0;
    uint32_t scaleModeIndex_ = 1;       // 默认「按宽度」
    uint32_t scaleAlgorithmIndex_ = 0;  // 默认「自动」
    uint32_t rotationIndex_ = 0;
    uint32_t colorModeIndex_ = 0;

    std::atomic<bool> running_{ false };
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<size_t> progressValue_{ 0 };
    std::atomic<size_t> progressMax_{ 1 };
    jark::BatchResult result_;
    bool finished_ = false;
    bool startFailed_ = false;

    // 输出分辨率预览：工作线程解码样张取尺寸，界面线程只读写这几个字段
    std::mutex previewMutex_;
    std::condition_variable previewCv_;
    std::wstring previewPath_;
    std::wstring previewRequest_;
    jark::ImageSize previewSource_;
    bool previewLoading_ = false;
    bool previewThreadStarted_ = false;
};
