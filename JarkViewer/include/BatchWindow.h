#pragma once

// 批量处理窗口：转换/缩放/图像调整、重命名、旋转翻转、删除到回收站。
// 界面用 UiFramework 搭建；处理逻辑在 BatchProcessor（可脱离界面单独验证）。

#include "BatchProcessor.h"
#include "MatWindow.h"
#include "TextDrawer.h"
#include "UiFramework.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <set>
#include <thread>

#include <shlobj.h>
#include <shlwapi.h>

class BatchWindow : public MatWindow {
private:
    static constexpr int kLogicalWidth = 900;
    static constexpr int kLogicalHeight = 940;
    static constexpr int kListHeight = 200;
    static constexpr int kRowHeight = 40;

    // 界面文案：追加在字符串表末尾，顺序必须与 stringRes.cpp 中新增条目一一对应，
    // 因此这里用连续枚举自动编号，避免手写序号错位。
    static constexpr uint32_t kStrBase = 58;
    enum : uint32_t {
        kStrTitle = kStrBase,  // 批量处理
        kStrFileList,          // 选择要处理的文件
        kStrConvert,           // 转换/缩放
        kStrRename,            // 重命名
        kStrRotate,            // 旋转/翻转
        kStrDelete,            // 删除
        kStrOutputFormat,      // 输出格式
        kStrKeepSize,          // 不变
        kStrColor,             // 彩色
        kStrGray,              // 黑白
        kStrDocument,          // 黑白文档
        kStrDither,            // 黑白抖动
        kStrStart,             // 开始处理
        kStrCancel,            // 取消
        kStrOutputDir,         // 输出目录
        kStrChooseDir,         // 选择目录
        kStrSameDir,           // 与源文件同目录
        kStrPrefix,            // 重命名前缀
        kStrNoRotation,        // 不旋转
        kStrRotate90,          // 顺时针90°
        kStrRotate270,         // 逆时针90°
        kStrFlipH,             // 水平翻转
        kStrFlipV,             // 垂直翻转
        kStrOverwrite,         // 覆盖已有文件
        kStrSelectFirst,       // 请先选择要处理的文件
        kStrProcessing,        // 正在处理...
        kStrFinished,          // 完成：成功 {}，跳过 {}，失败 {}
        kStrUnusedFormat,      // 选择输出文件格式（暂未使用）
        kStrTask,              // 任务
        kStrApplyAdjust,       // 应用图像调整
        kStrColorMode,         // 颜色模式
        kStrBrightness,        // 亮度
        kStrContrast,          // 对比度
    };

    static inline const wchar_t* windowsClassName = L"JarkBatchWnd";

    std::vector<std::wstring> files_;
    std::vector<std::string> fileNames_;
    std::set<size_t> selected_;

    jark::BatchOptions options_;
    std::string renamePrefix_ = "image_";
    std::wstring outputDirectory_;

    // 任务状态（工作线程写，界面线程读）
    std::atomic<bool> running_{ false };
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<int> progressValue_{ 0 };
    std::atomic<int> progressMax_{ 1 };
    jark::BatchResult result_;
    bool finished_ = false;
    bool startFailed_ = false;

    TextDrawer textDrawer;
    cv::Mat canvasMat;
    std::unique_ptr<jark::ui::Panel> root;

    jark::ui::TextBox* prefixBox = nullptr;   // 均为控件树中的实例（容器持有所有权）

    jark::ui::Label* outputDirLabel = nullptr;
    std::vector<jark::ui::Control*> convertControls;
    std::vector<jark::ui::Control*> renameControls;
    std::vector<jark::ui::Control*> rotateControls;
    std::vector<jark::ui::Control*> deleteControls;

    // 界面上用下标绑定的选项，提交前同步到 options_
    uint32_t taskIndex_ = 0;
    uint32_t formatIndex_ = 0;
    uint32_t maxEdgeIndex_ = 0;
    uint32_t rotationIndex_ = 0;
    uint32_t colorModeIndex_ = 0;

    void Init() {
        std::sort(files_.begin(), files_.end(), [](const std::wstring& a, const std::wstring& b) {
            return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
            });

        for (const auto& file : files_)
            fileNames_.push_back(jarkUtils::wstringToUtf8(std::filesystem::path(file).filename().wstring()));

        for (size_t i = 0; i < files_.size(); ++i)
            selected_.insert(i);
    }

    void initCanvas() {
        textDrawer.setSize(dp(22));
        canvasMat = cv::Mat(dp(kLogicalHeight), dp(kLogicalWidth), CV_8UC4,
            jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG));
        buildControls();
    }

    static std::string ui(uint32_t id) { return std::string(getUIString(id)); }

    void buildControls() {
        root = std::make_unique<jark::ui::Panel>();

        root->add(std::make_unique<jark::ui::Label>(ui(kStrFileList)), 30, 0);
        root->add(std::make_unique<jark::ui::CheckList>(
            fileNames_,
            [this](const std::string& name) {
                const int index = indexOfName(name);
                return index >= 0 && selected_.contains(static_cast<size_t>(index));
            },
            [this](const std::string& name, bool checked) {
                const int index = indexOfName(name);
                if (index < 0)
                    return;
                if (checked)
                    selected_.insert(static_cast<size_t>(index));
                else
                    selected_.erase(static_cast<size_t>(index));
                isNeedRefreshUI = true;
            }), kListHeight, 4);

        auto selectRow = std::make_unique<jark::ui::Row>();
        selectRow->add(std::make_unique<jark::ui::Button>(ui(8), [this]() {
            for (size_t i = 0; i < files_.size(); ++i)
                selected_.insert(i);
            isNeedRefreshUI = true;
        }));
        selectRow->add(std::make_unique<jark::ui::Button>(ui(9), [this]() {
            selected_.clear();
            isNeedRefreshUI = true;
        }));
        root->add(std::move(selectRow), 40, 6);

        // 任务类型
        root->add(std::make_unique<jark::ui::RadioGroup>(
            ui(kStrTask),
            std::vector<std::string>{ ui(kStrConvert), ui(kStrRename), ui(kStrRotate), ui(kStrDelete) },
            &taskIndex_), kRowHeight, 8);

        // —— 转换 / 缩放 ——
        convertControls.push_back(root->add(std::make_unique<jark::ui::RadioGroup>(
            ui(kStrOutputFormat),
            std::vector<std::string>{ "png", "jpg", "webp", "bmp", "tif" }, &formatIndex_), kRowHeight, 2));

        convertControls.push_back(root->add(std::make_unique<jark::ui::RadioGroup>(
            ui(41), // 分辨率
            std::vector<std::string>{ ui(kStrKeepSize), "4096", "2048", "1600", "1280", "800" },
            &maxEdgeIndex_), kRowHeight, 2));

        // 图像调整（与打印页同一套参数）
        convertControls.push_back(root->add(
            std::make_unique<jark::ui::CheckBox>(ui(kStrApplyAdjust), &options_.applyAdjustments), 36, 2));

        convertControls.push_back(root->add(std::make_unique<jark::ui::RadioGroup>(
            ui(kStrColorMode),
            std::vector<std::string>{ ui(kStrColor), ui(kStrGray), ui(kStrDocument), ui(kStrDither) },
            &colorModeIndex_), kRowHeight, 2));

        convertControls.push_back(root->add(std::make_unique<jark::ui::Slider>(
            ui(kStrBrightness), &options_.brightness, 200, 250, 500), 32, 2));
        convertControls.push_back(root->add(std::make_unique<jark::ui::Slider>(
            ui(kStrContrast), &options_.contrast, 200, 250, 500), 32, 2));

        convertControls.push_back(root->add(
            std::make_unique<jark::ui::CheckBox>(ui(kStrOverwrite), &options_.overwrite), 36, 4));

        // —— 重命名 ——
        renameControls.push_back(root->add(std::make_unique<jark::ui::Label>(ui(kStrPrefix)), 34, 4));
        prefixBox = static_cast<jark::ui::TextBox*>(
            root->add(std::make_unique<jark::ui::TextBox>(&renamePrefix_, "image_"), 40, 2));
        renameControls.push_back(prefixBox);

        // —— 旋转 / 翻转 ——
        rotateControls.push_back(root->add(std::make_unique<jark::ui::RadioGroup>(
            ui(kStrRotate),
            std::vector<std::string>{ ui(kStrNoRotation), ui(kStrRotate90), "180°", ui(kStrRotate270) },
            &rotationIndex_), kRowHeight, 2));

        rotateControls.push_back(root->add(
            std::make_unique<jark::ui::CheckBox>(ui(kStrFlipH), &options_.flipHorizontal), 36, 2));
        rotateControls.push_back(root->add(
            std::make_unique<jark::ui::CheckBox>(ui(kStrFlipV), &options_.flipVertical), 36, 2));

        // —— 删除说明 ——
        deleteControls.push_back(root->add(std::make_unique<jark::ui::Label>(ui(11)), 60, 4));

        // —— 输出目录（转换时有效）——
        auto dirRow = std::make_unique<jark::ui::Row>();
        dirRow->add(std::make_unique<jark::ui::Label>(ui(kStrOutputDir)));
        dirRow->add(std::make_unique<jark::ui::Button>(ui(kStrChooseDir), [this]() { chooseOutputDirectory(); }));
        convertControls.push_back(root->add(std::move(dirRow), 46, 6));

        outputDirLabel = static_cast<jark::ui::Label*>(root->add(
            std::make_unique<jark::ui::Label>(ui(kStrSameDir)), 26, 0));
        convertControls.push_back(outputDirLabel);

        // —— 进度与操作 ——
        root->add(std::make_unique<jark::ui::ProgressBar>(&progressPercent_, 100), 32, 10);

        auto actionRow = std::make_unique<jark::ui::Row>();
        actionRow->add(std::make_unique<jark::ui::Button>(ui(kStrStart), [this]() { startBatch(); }, true));
        actionRow->add(std::make_unique<jark::ui::Button>(ui(kStrCancel), [this]() { cancelRequested_ = true; }));
        root->add(std::move(actionRow), 46, 6);

        statusLabel = static_cast<jark::ui::Label*>(root->add(
            std::make_unique<jark::ui::Label>(""), 26, 2));
    }

    jark::ui::Label* statusLabel = nullptr;
    int progressPercent_ = 0;

    int indexOfName(const std::string& name) const {
        for (size_t i = 0; i < fileNames_.size(); ++i) {
            if (fileNames_[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    void chooseOutputDirectory() {
        BROWSEINFOW browse{};
        browse.lpszTitle = getUIStringW(kStrOutputDir);
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        if (LPITEMIDLIST item = SHBrowseForFolderW(&browse)) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(item, path)) {
                outputDirectory_ = path;
                outputDirLabel->setText(jarkUtils::wstringToUtf8(outputDirectory_));
            }
            CoTaskMemFree(item);
        }
        isNeedRefreshUI = true;
    }

    void syncOptionsFromControls() {
        static const wchar_t* const formats[] = { L"png", L"jpg", L"webp", L"bmp", L"tif" };
        static const int edges[] = { 0, 4096, 2048, 1600, 1280, 800 };
        static const int rotations[] = { 0, 90, 180, 270 };

        static const jark::BatchTask tasks[] = {
            jark::BatchTask::Convert, jark::BatchTask::Rename,
            jark::BatchTask::Rotate, jark::BatchTask::Delete,
        };

        if (taskIndex_ < std::size(tasks))
            options_.task = tasks[taskIndex_];
        if (formatIndex_ < std::size(formats))
            options_.outputExtension = formats[formatIndex_];
        if (maxEdgeIndex_ < std::size(edges))
            options_.maxEdge = edges[maxEdgeIndex_];
        if (rotationIndex_ < std::size(rotations))
            options_.rotationDegrees = rotations[rotationIndex_];

        options_.colorMode = colorModeIndex_;
        options_.outputDirectory = outputDirectory_;
        options_.renamePrefix = jarkUtils::utf8ToWstring(renamePrefix_);

        // 亮度/对比度由 Slider 直接写入 options_；调整关闭时不需要
        if (!options_.applyAdjustments) {
            options_.brightness = 100;
            options_.contrast = 100;
        }
    }

    void updateControlVisibility() {
        const auto task = options_.task;
        for (auto* control : convertControls)
            control->visible = task == jark::BatchTask::Convert;
        for (auto* control : renameControls)
            control->visible = task == jark::BatchTask::Rename;
        for (auto* control : rotateControls)
            control->visible = task == jark::BatchTask::Rotate;
        for (auto* control : deleteControls)
            control->visible = task == jark::BatchTask::Delete;
    }

    std::vector<std::wstring> selectedFiles() const {
        std::vector<std::wstring> result;
        for (const auto index : selected_) {
            if (index < files_.size())
                result.push_back(files_[index]);
        }
        return result;
    }

    void startBatch() {
        if (running_ || files_.empty())
            return;

        syncOptionsFromControls();
        auto targets = selectedFiles();

        if (targets.empty()) {
            startFailed_ = true;
            isNeedRefreshUI = true;
            return;
        }
        startFailed_ = false;

        if (options_.task == jark::BatchTask::Delete) {
            // 文案来自字符串表（运行期），需用 vformat；make_wformat_args 需要左值
            const size_t fileCount = targets.size();
            auto message = std::vformat(getUIStringW(43), std::make_wformat_args(fileCount));
            if (MessageBoxW(m_hwnd, message.c_str(), getUIStringW(42), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
        }

        running_ = true;
        cancelRequested_ = false;
        finished_ = false;
        progressValue_ = 0;
        progressMax_ = static_cast<int>(targets.size());
        result_ = {};
        isNeedRefreshUI = true;

        const auto options = options_;
        std::thread worker([this, targets, options]() {
            result_ = jark::runBatch(targets, options,
                [this](size_t current, size_t total, const std::wstring&) {
                    progressValue_ = static_cast<int>(current);
                    progressMax_ = static_cast<int>(total ? total : 1);
                    return !cancelRequested_.load();
                });

            finished_ = true;
            running_ = false;
            isNeedRefreshUI = true;
            });

        // 让界面持续重绘以显示进度
        std::thread refresher([this]() {
            while (running_)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            isNeedRefreshUI = true;
            });

        worker.detach();
        refresher.detach();
    }

public:
    static inline volatile bool isWorking = false;
    static inline volatile HWND hwnd = nullptr;

    explicit BatchWindow(std::vector<std::wstring> files) : files_(std::move(files)) {
        requestExitFlag = false;
        isWorking = true;

        Init();
        runWindow();

        requestExitFlag = false;
        isWorking = false;
        hwnd = nullptr;
    }

    ~BatchWindow() = default;

    static void requestExit() {
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

protected:
    void onPaint(HDC hdc) override {
        if (!canvasMat.empty())
            blitMat(hdc, canvasMat);
    }

    void onLButtonDown() override {
        if (!root)
            return;

        if (root->onMouseDown(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onLButtonUp() override {
        if (!root)
            return;

        root->onMouseUp(m_x, m_y);
        isNeedRefreshUI = true;
    }

    void onMouseMove(WPARAM keyState) override {
        (void)keyState;
        if (root && root->onMouseMove(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onMouseWheel(int delta) override {
        if (root && root->onWheel(m_x, m_y, delta))
            isNeedRefreshUI = true;
    }

    void onKeyChar(wchar_t character) override {
        if (prefixBox && prefixBox->onKeyChar(character))
            isNeedRefreshUI = true;
    }

    void onKeyDown(WPARAM key) override {
        if (prefixBox && prefixBox->onKeyDown(static_cast<int>(key))) {
            isNeedRefreshUI = true;
            return;
        }

        if (key == VK_ESCAPE && !running_)
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }

    void drawingUI() override {
        jark::ui::UiCanvas canvas(canvasMat, textDrawer, GlobalVar::currentTheme, uiScale());
        canvas.fill({ 0, 0, canvas.width(), canvas.height() }, GlobalVar::currentTheme.BG);

        progressPercent_ = progressMax_ > 0 ? progressValue_ * 100 / progressMax_ : 0;
        updateControlVisibility();

        if (statusLabel) {
            if (startFailed_)
                statusLabel->setText(ui(kStrSelectFirst));
            else if (running_)
                statusLabel->setText(ui(kStrProcessing));
            else if (finished_) {
                const size_t succeeded = result_.succeeded;
                const size_t skipped = result_.skipped;
                const size_t failed = result_.failed;
                statusLabel->setText(std::vformat(getUIString(kStrFinished),
                    std::make_format_args(succeeded, skipped, failed)));
            }
            else
                statusLabel->setText("");
        }

        root->bounds = { canvas.dp(20), canvas.dp(20), canvas.width() - canvas.dp(40), canvas.height() - canvas.dp(40) };
        root->draw(canvas);
    }

    void runWindow() {
        if (!createWindow(kLogicalWidth, kLogicalHeight, windowsClassName, getUIStringW(kStrTitle))) {
            return;
        }

        hwnd = m_hwnd;
        initCanvas();
        isNeedRefreshUI = true;
        runMessageLoop();
    }
};
