#pragma once

// 图像编辑与标注窗口：左侧画布（拖动绘制、滚轮缩放、中键平移），右侧工具栏。
// 标注模型与渲染在 ImageAnnotator（纯逻辑，可用 --probe --annotate 单独验证），
// 这里只负责交互、视图变换与文件输出。

#include "ImageAnnotator.h"
#include "MatWindow.h"
#include "TextRenderer.h"
#include "UiFramework.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <commdlg.h>

class EditorWindow : public MatWindow {
private:
    static constexpr int kLogicalWidth = 1400;
    static constexpr int kLogicalHeight = 880;
    static constexpr int kSidebarWidth = 320;
    static constexpr int kGap = 16;
    static constexpr int kRowHeight = 42;
    static constexpr int kSliderHeight = 36;

    // 界面文案：追加在字符串表末尾，顺序必须与 stringRes.cpp 中新增条目一一对应
    static constexpr uint32_t kStrBase = 91;
    enum : uint32_t {
        kStrTitle = kStrBase, // 图像编辑与标注
        kStrTool,             // 工具
        kStrRect,             // 矩形
        kStrEllipse,          // 椭圆
        kStrArrow,            // 箭头
        kStrLine,             // 直线
        kStrPen,              // 画笔
        kStrMosaic,           // 马赛克
        kStrText,             // 文字
        kStrCrop,             // 裁剪
        kStrColor,            // 颜色
        kStrWidth,            // 线宽
        kStrFontSize,         // 字号
        kStrFilled,           // 填充图形
        kStrUndo,             // 撤销
        kStrRedo,             // 重做
        kStrEdit,             // 编辑
        kStrInvert,           // 反相
        kStrSaveAs,           // 另存为
        kStrCopy,             // 复制
        kStrOverwrite,        // 覆盖原文件
        kStrCopied,           // 已复制到剪贴板
        kStrSaved,            // 已保存
        kStrSaveFailed,       // 保存失败
        kStrHint,             // 操作提示
        kStrTextPrompt,       // 文字内容（回车放置）
        kStrDone,             // 完成
        kStrDiscard,          // 放弃修改
        kStrNeedSelection,    // 未选中区域
        kStrApplyCrop,        // 应用裁剪
    };

    static inline const wchar_t* windowsClassName = L"JarkEditorWnd";

    // 工具顺序必须与 AnnoTool 的取值一致（OptionGrid 按下标写入）
    static constexpr jark::AnnoTool kTools[] = {
        jark::AnnoTool::Rect, jark::AnnoTool::Ellipse, jark::AnnoTool::Arrow, jark::AnnoTool::Line,
        jark::AnnoTool::Pen, jark::AnnoTool::Mosaic, jark::AnnoTool::Text, jark::AnnoTool::Crop,
    };

    static inline const uint32_t kColors[] = {
        0xFFFF3B30, 0xFFFFCC00, 0xFF34C759, 0xFF0A84FF,
        0xFFFFFFFF, 0xFF000000, 0xFFFF2D95, 0xFFAF52DE,
    };

    // 复用批量处理窗口的文案 ID（见 BatchWindow.h 的 kStrBase 枚举）
    static constexpr uint32_t kBatchStrRotate90 = 77;  // 顺时针90°
    static constexpr uint32_t kBatchStrRotate270 = 78; // 逆时针90°
    static constexpr uint32_t kBatchStrFlipHorizontal = 79;
    static constexpr uint32_t kBatchStrFlipVertical = 80;

    // 画布控件：命中与事件都交回编辑器
    class CanvasControl : public jark::ui::Control {
    public:
        explicit CanvasControl(EditorWindow& owner) : owner_(owner) {}

        void draw(jark::ui::UiCanvas& canvas) override { owner_.drawImageCanvas(canvas, bounds); }
        bool onMouseDown(int x, int y) override { return owner_.onCanvasDown(x, y); }
        bool onMouseMove(int x, int y) override { return owner_.onCanvasMove(x, y); }
        bool onMouseUp(int x, int y) override { return owner_.onCanvasUp(x, y); }
        bool onWheel(int x, int y, int delta) override { return owner_.onCanvasWheel(x, y, delta); }
        bool hitTest(int x, int y) const override { return visible && bounds.contains(x, y); }

    private:
        EditorWindow& owner_;
    };

    std::wstring sourcePath_;
    jark::AnnotatorDocument document_;

    cv::Mat displayImage_;  // 底图+已提交标注，透明处铺了棋盘格（显示用，始终不透明）
    cv::Mat preview_;       // displayImage_ + 进行中的标注
    cv::Rect previewDirty_;
    bool previewActive_ = false;

    cv::Mat canvasMat;
    TextRenderer textDrawer;
    std::unique_ptr<jark::ui::Panel> root;
    CanvasControl* canvasControl = nullptr;
    jark::ui::OptionGrid* toolGrid = nullptr;
    jark::ui::ColorRow* colorRow = nullptr;
    jark::ui::TextBox* textBox = nullptr;
    jark::ui::Label* statusLabel = nullptr;
    std::vector<jark::ui::Control*> textOnlyControls; // 仅文字工具可见

    // 控件绑定的状态
    uint32_t toolIndex_ = 0;
    uint32_t colorIndex_ = 0;
    int lineWidth_ = 6;
    int fontSize_ = 40;
    bool filled_ = false;
    std::string textInput_;

    // 视图
    double zoomFactor_ = 1.0; // 相对「适应窗口」
    double panX_ = 0.0;
    double panY_ = 0.0;
    bool panning_ = false;

    // 交互中的状态
    bool drawingShape_ = false;
    bool textPlaced_ = false;
    cv::Point textAnchor_;
    std::string lastPreviewText_;
    bool selecting_ = false;
    cv::Point selectionStart_;
    cv::Rect cropSelection_;
    bool hasSelection_ = false;

    std::string statusText_;
    std::string hintText_;
    double statusUntil_ = 0.0;

    struct ViewTransform {
        double scale = 1.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        bool valid = false;
    };

    // —— 视图 ——

    ViewTransform viewTransform(const jark::ui::Rect& area) const {
        ViewTransform vt;
        const double imageWidth = document_.width();
        const double imageHeight = document_.height();
        if (imageWidth < 1 || imageHeight < 1 || area.width < 8 || area.height < 8)
            return vt;

        const double fit = (std::min)(area.width / imageWidth, area.height / imageHeight);
        vt.scale = fit * zoomFactor_;
        if (vt.scale <= 0.0001)
            return vt;

        vt.offsetX = area.x + (area.width - imageWidth * vt.scale) / 2.0 + panX_;
        vt.offsetY = area.y + (area.height - imageHeight * vt.scale) / 2.0 + panY_;
        vt.valid = true;
        return vt;
    }

    jark::ui::Rect canvasArea() const {
        return canvasControl ? canvasControl->bounds : jark::ui::Rect{};
    }

    cv::Point toImagePoint(const ViewTransform& vt, int x, int y) const {
        if (!vt.valid)
            return {};
        return {
            static_cast<int>(std::lround((x - vt.offsetX) / vt.scale)),
            static_cast<int>(std::lround((y - vt.offsetY) / vt.scale))
        };
    }

    void resetView() {
        zoomFactor_ = 1.0;
        panX_ = 0.0;
        panY_ = 0.0;
    }

    // 当前显示比例（相对图像原始像素）
    double displayScale() const {
        const ViewTransform vt = viewTransform(canvasArea());
        return vt.valid ? vt.scale : 1.0;
    }

    // 把缩放比例设为「适应窗口」的 times 倍，并居中
    void setZoom(double times) {
        zoomFactor_ = std::clamp(times, 0.05, 40.0);
        panX_ = 0.0;
        panY_ = 0.0;
        isNeedRefreshUI = true;
    }

    // 1:1 显示：一个图像像素对应一个逻辑像素（高 DPI 下即 uiScale() 个物理像素）
    void zoomToActualPixels() {
        const double fitScale = displayScale() / zoomFactor_;
        if (fitScale > 0.0001)
            setZoom(uiScale() / fitScale);
    }

    void showZoomStatus() {
        // 百分比按逻辑像素计（与「1:1」的定义一致）
        const double percent = displayScale() / uiScale() * 100.0;
        setStatusText(std::format("{}%", static_cast<int>(std::lround(percent))));
        statusUntil_ = nowSeconds() + 2.0; // 短暂显示后回到操作提示
    }

    // —— 图像显示 ——

    // 透明像素铺棋盘格，得到始终不透明的显示图（保存/复制仍用带 alpha 的原图）
    static cv::Mat compositeOnCheckerboard(const cv::Mat& source) {
        if (source.empty())
            return {};

        cv::Mat result;
        if (source.type() != CV_8UC4) {
            cv::cvtColor(source, result, cv::COLOR_BGR2BGRA);
            return result;
        }

        cv::Mat alpha;
        cv::extractChannel(source, alpha, 3);
        double minValue = 0.0;
        double maxValue = 0.0;
        cv::minMaxLoc(alpha, &minValue, &maxValue);
        if (minValue >= 255.0)
            return source.clone();

        const int cell = 12;
        cv::Mat tile(2 * cell, 2 * cell, CV_8UC4, cv::Scalar(96, 96, 96, 255));
        tile(cv::Rect(0, 0, cell, cell)).setTo(cv::Scalar(128, 128, 128, 255));
        tile(cv::Rect(cell, cell, cell, cell)).setTo(cv::Scalar(128, 128, 128, 255));

        cv::resize(tile, result, source.size(), 0, 0, cv::INTER_NEAREST);

        for (int y = 0; y < source.rows; ++y) {
            const cv::Vec4b* srcRow = source.ptr<cv::Vec4b>(y);
            cv::Vec4b* dstRow = result.ptr<cv::Vec4b>(y);
            for (int x = 0; x < source.cols; ++x) {
                const int a = srcRow[x][3];
                if (a == 0)
                    continue;
                if (a == 255) {
                    dstRow[x] = srcRow[x];
                    continue;
                }

                const int inverse = 255 - a;
                dstRow[x] = cv::Vec4b(
                    static_cast<uint8_t>((srcRow[x][0] * a + dstRow[x][0] * inverse + 127) / 255),
                    static_cast<uint8_t>((srcRow[x][1] * a + dstRow[x][1] * inverse + 127) / 255),
                    static_cast<uint8_t>((srcRow[x][2] * a + dstRow[x][2] * inverse + 127) / 255),
                    255);
            }
        }
        return result;
    }

    void rebuildDisplay() {
        displayImage_ = compositeOnCheckerboard(document_.committedImage());
        previewActive_ = false;
        preview_ = cv::Mat();
        previewDirty_ = {};
    }

    const cv::Mat& previewImage() {
        if (previewActive_ && !preview_.empty())
            return preview_;
        return displayImage_;
    }

    void beginPreview() {
        preview_ = displayImage_.clone();
        previewActive_ = true;
        previewDirty_ = {};
    }

    // 还原上一帧的脏区后重画进行中的标注（避免整幅拷贝）
    void refreshPreview() {
        if (!previewActive_)
            return;

        const cv::Rect canvasRect(0, 0, preview_.cols, preview_.rows);
        if (previewDirty_.width > 0) {
            const cv::Rect restore = previewDirty_ & canvasRect;
            if (restore.width > 0 && restore.height > 0)
                displayImage_(restore).copyTo(preview_(restore));
        }

        const jark::Annotation active = document_.active();
        if (!active.empty())
            jark::drawAnnotation(preview_, active, textDrawer);

        previewDirty_ = jark::annotationBounds(active) & canvasRect;
    }

    void endPreview() {
        previewActive_ = false;
        preview_ = cv::Mat();
        previewDirty_ = {};
    }

    // —— 绘制 ——

    void drawImageCanvas(jark::ui::UiCanvas& canvas, const jark::ui::Rect& area) {
        canvas.fill(area, GlobalVar::currentTheme.BG_DEEP);

        if (document_.width() < 1 || area.width < 8 || area.height < 8)
            return;

        const ViewTransform vt = viewTransform(area);
        if (!vt.valid)
            return;

        const cv::Mat& source = previewImage();
        if (source.empty())
            return;

        cv::Mat& target = canvas.raw();

        // 只取可见部分，避免整幅缩放
        const int x0 = std::clamp(static_cast<int>(std::floor((area.x - vt.offsetX) / vt.scale)), 0, source.cols);
        const int y0 = std::clamp(static_cast<int>(std::floor((area.y - vt.offsetY) / vt.scale)), 0, source.rows);
        const int x1 = std::clamp(static_cast<int>(std::ceil((area.right() - vt.offsetX) / vt.scale)), 0, source.cols);
        const int y1 = std::clamp(static_cast<int>(std::ceil((area.bottom() - vt.offsetY) / vt.scale)), 0, source.rows);
        if (x1 <= x0 || y1 <= y0)
            return;

        const cv::Rect sourceRect(x0, y0, x1 - x0, y1 - y0);
        const int dstX = static_cast<int>(std::lround(vt.offsetX + x0 * vt.scale));
        const int dstY = static_cast<int>(std::lround(vt.offsetY + y0 * vt.scale));
        const int dstWidth = (std::max)(1, static_cast<int>(std::lround(sourceRect.width * vt.scale)));
        const int dstHeight = (std::max)(1, static_cast<int>(std::lround(sourceRect.height * vt.scale)));

        const jark::ui::Rect viewRect{ dstX, dstY, dstWidth, dstHeight };
        const cv::Rect clipped = viewRect.toCv() & area.toCv() & cv::Rect(0, 0, target.cols, target.rows);
        if (clipped.width <= 0 || clipped.height <= 0)
            return;

        cv::Mat view;
        cv::resize(source(sourceRect), view, { dstWidth, dstHeight }, 0, 0,
            vt.scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

        const cv::Rect fromView(clipped.x - dstX, clipped.y - dstY, clipped.width, clipped.height);
        view(fromView).copyTo(target(clipped));

        // 图像边框，便于区分画布与图像
        canvas.stroke(viewRect, GlobalVar::currentTheme.BG_LIGHT, 1);

        // 裁剪选区：框外压暗
        if (cropSelection_.width > 0 && cropSelection_.height > 0) {
            const jark::ui::Rect selection{
                static_cast<int>(std::lround(vt.offsetX + cropSelection_.x * vt.scale)),
                static_cast<int>(std::lround(vt.offsetY + cropSelection_.y * vt.scale)),
                static_cast<int>(std::lround(cropSelection_.width * vt.scale)),
                static_cast<int>(std::lround(cropSelection_.height * vt.scale))
            };

            const cv::Rect visible = selection.toCv() & area.toCv() & cv::Rect(0, 0, target.cols, target.rows);
            if (visible.width > 0 && visible.height > 0) {
                const cv::Rect shade[4] = {
                    { area.x, area.y, area.width, visible.y - area.y },
                    { area.x, visible.y + visible.height, area.width, area.bottom() - (visible.y + visible.height) },
                    { area.x, visible.y, visible.x - area.x, visible.height },
                    { visible.x + visible.width, visible.y, area.right() - (visible.x + visible.width), visible.height },
                };

                // 框外压暗（画布不透明像素，直接乘系数即可）
                for (const auto& part : shade) {
                    const cv::Rect piece = part & area.toCv() & cv::Rect(0, 0, target.cols, target.rows);
                    if (piece.width <= 0 || piece.height <= 0)
                        continue;
                    target(piece) *= 0.55;
                }
            }

            canvas.stroke(selection, GlobalVar::currentTheme.FG, canvas.dp(2));
        }
    }

    // —— 交互 ——

    bool onCanvasDown(int x, int y) {
        const jark::ui::Rect area = canvasArea();
        const ViewTransform vt = viewTransform(area);
        if (!vt.valid)
            return false;

        if (currentTool() == jark::AnnoTool::Crop) {
            selecting_ = true;
            selectionStart_ = toImagePoint(vt, x, y);
            cropSelection_ = cv::Rect(selectionStart_, selectionStart_);
            hasSelection_ = false;
            return true;
        }

        const cv::Point point = toImagePoint(vt, x, y);
        document_.begin(currentTool(), currentStyle(), point);
        drawingShape_ = true;

        if (currentTool() == jark::AnnoTool::Text) {
            // 换一个锚点前，先把上一段文字落下去
            if (textPlaced_ && !textInput_.empty())
                commitTextAt(textInput_, textAnchor_);

            textAnchor_ = point;
            textPlaced_ = true;
            document_.begin(jark::AnnoTool::Text, currentStyle(), point);
            document_.setText(textInput_);
            lastPreviewText_ = textInput_;
            if (textBox)
                textBox->setFocused(true);
        }

        beginPreview();
        refreshPreview();
        return true;
    }

    bool onCanvasMove(int x, int y) {
        const jark::ui::Rect area = canvasArea();
        const ViewTransform vt = viewTransform(area);
        if (!vt.valid)
            return false;

        if (panning_) {
            panX_ += x - lastPanX_;
            panY_ += y - lastPanY_;
            lastPanX_ = x;
            lastPanY_ = y;
            return true;
        }

        if (selecting_) {
            const cv::Point point = toImagePoint(vt, x, y);
            cropSelection_ = cv::Rect(
                (std::min)(selectionStart_.x, point.x), (std::min)(selectionStart_.y, point.y),
                std::abs(point.x - selectionStart_.x), std::abs(point.y - selectionStart_.y));
            return true;
        }

        if (drawingShape_) {
            document_.update(toImagePoint(vt, x, y));
            refreshPreview();
            return true;
        }
        return false;
    }

    bool onCanvasUp(int x, int y) {
        if (panning_) {
            panning_ = false;
            return true;
        }

        if (selecting_) {
            selecting_ = false;
            cropSelection_ = cropSelection_ & cv::Rect(0, 0, document_.width(), document_.height());
            hasSelection_ = cropSelection_.width >= 4 && cropSelection_.height >= 4;
            setStatus(hasSelection_ ? kStrCrop : kStrNeedSelection);
            return true;
        }

        // 窗口类没有 CS_DBLCLKS，自己按系统双击时间与距离判断
        const auto now = std::chrono::steady_clock::now();
        const bool isDoubleClick = lastClickTime_ != std::chrono::steady_clock::time_point{} &&
            now - lastClickTime_ < std::chrono::milliseconds(::GetDoubleClickTime()) &&
            std::abs(x - lastClickX_) < 8 && std::abs(y - lastClickY_) < 8;
        lastClickTime_ = now;
        lastClickX_ = x;
        lastClickY_ = y;

        if (!drawingShape_) {
            // 没有正在绘制的图形（例如未按下就抬起）：双击切换缩放
            return isDoubleClick && handleDoubleClick();
        }

        drawingShape_ = false;

        // 文字工具：点击只定位锚点，内容在文本框里输入后按回车放置
        if (currentTool() == jark::AnnoTool::Text) {
            document_.setText(textInput_);
            return true;
        }

        document_.commit();
        rebuildDisplay();

        if (isDoubleClick)
            handleDoubleClick();
        return true;
    }

    bool onCanvasWheel(int x, int y, int delta) {
        const jark::ui::Rect area = canvasArea();
        const ViewTransform before = viewTransform(area);
        if (!before.valid)
            return false;

        const double factor = delta > 0 ? 1.15 : 1.0 / 1.15;
        const double next = std::clamp(zoomFactor_ * factor, 0.05, 40.0);
        if (std::abs(next - zoomFactor_) < 0.0001)
            return false;

        // 让光标下的图像点保持不动
        const double imageX = (x - before.offsetX) / before.scale;
        const double imageY = (y - before.offsetY) / before.scale;
        zoomFactor_ = next;

        const ViewTransform after = viewTransform(area);
        if (after.valid) {
            panX_ += x - (after.offsetX + imageX * after.scale);
            panY_ += y - (after.offsetY + imageY * after.scale);
        }

        showZoomStatus();
        return true;
    }

    // 双击画布：在「适应窗口」与 1:1 之间切换
    bool handleDoubleClick() {
        if (std::abs(displayScale() - uiScale()) < 0.01)
            setZoom(1.0); // 已经是 1:1，回到适应窗口
        else
            zoomToActualPixels();
        showZoomStatus();
        return true;
    }

    int lastPanX_ = 0;
    int lastPanY_ = 0;
    std::chrono::steady_clock::time_point lastClickTime_{};
    int lastClickX_ = 0;
    int lastClickY_ = 0;

    // —— 编辑操作 ——

    jark::AnnoTool currentTool() const {
        return toolIndex_ < std::size(kTools) ? kTools[toolIndex_] : jark::AnnoTool::Rect;
    }

    jark::AnnoStyle currentStyle() const {
        jark::AnnoStyle style;
        style.color = kColors[colorIndex_ < std::size(kColors) ? colorIndex_ : 0];
        style.width = (std::max)(1, lineWidth_);
        style.fontSize = (std::max)(8, fontSize_);
        style.filled = filled_;
        return style;
    }

    // 用当前画面替换底图（旋转/翻转/反相等一次性编辑），并清空标注历史
    template <typename Operation>
    void applyEdit(Operation operation) {
        if (document_.width() < 1)
            return;

        cv::Mat result;
        operation(document_.flatten(), result);
        if (result.empty())
            return;

        document_.applyEdit(result);
        rebuildDisplay();
        resetView();
        clearSelection();
        isNeedRefreshUI = true;
    }

    // 把一段文字落到画布上（锚点为左上角）
    void commitTextAt(const std::string& text, const cv::Point& anchor) {
        if (text.empty())
            return;

        document_.cancel();
        document_.begin(jark::AnnoTool::Text, currentStyle(), anchor);
        document_.setText(text);
        document_.commit();
        rebuildDisplay();
    }

    void clearSelection() {
        cropSelection_ = {};
        hasSelection_ = false;
        selecting_ = false;
    }

    void rotateLeft() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) { cv::rotate(source, target, cv::ROTATE_90_COUNTERCLOCKWISE); });
    }

    void rotateRight() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) { cv::rotate(source, target, cv::ROTATE_90_CLOCKWISE); });
    }

    void rotateHalf() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) { cv::rotate(source, target, cv::ROTATE_180); });
    }

    void flipHorizontal() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) { cv::flip(source, target, 1); });
    }

    void flipVertical() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) { cv::flip(source, target, 0); });
    }

    void invertColors() {
        applyEdit([](const cv::Mat& source, cv::Mat& target) {
            std::vector<cv::Mat> channels;
            cv::split(source, channels);
            for (size_t i = 0; i < 3 && i < channels.size(); ++i)
                cv::bitwise_not(channels[i], channels[i]);
            cv::merge(channels, target);
            });
    }

    void applyCrop() {
        if (!hasSelection_) {
            setStatus(kStrNeedSelection);
            return;
        }

        if (document_.cropTo(cropSelection_)) {
            rebuildDisplay();
            resetView();
            clearSelection();
            setStatus(kStrDone);
            isNeedRefreshUI = true;
        }
    }

    // —— 输出 ——

    static bool isEncodableExtension(const std::wstring& extension) {
        static const wchar_t* const supported[] = { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"tif", L"tiff" };
        for (const auto* item : supported) {
            if (extension == item)
                return true;
        }
        return false;
    }

    bool saveTo(const std::wstring& path, const std::wstring& extension) {
        std::vector<uint8_t> encoded;
        if (!jark::encodeAnnotatedImage(document_.flatten(), extension, encoded) || encoded.empty()) {
            setStatus(kStrSaveFailed);
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            setStatus(kStrSaveFailed);
            return false;
        }

        file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        file.close();
        setStatus(kStrSaved);
        return true;
    }

    void saveAs() {
        auto [path, isJpg] = jarkUtils::saveImageDialogW(uiW(kStrSaveAs));
        if (path.size() <= 2)
            return;

        saveTo(path, isJpg ? L"jpg" : L"png");
    }

    void overwriteSource() {
        if (sourcePath_.empty())
            return;

        std::wstring extension = std::filesystem::path(sourcePath_).extension().wstring();
        if (!extension.empty() && extension.front() == L'.')
            extension.erase(extension.begin());
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);

        if (!isEncodableExtension(extension)) {
            MessageBoxW(m_hwnd, uiW(kStrSaveFailed).c_str(), uiW(kStrTitle).c_str(), MB_OK | MB_ICONWARNING);
            return;
        }

        if (MessageBoxW(m_hwnd, getUIStringW(46), uiW(kStrTitle).c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;

        if (saveTo(sourcePath_, extension))
            GlobalVar::isNeedReloadImageCache = true; // 主窗口重新加载被覆盖的图片
    }

    void copyToClipboard() {
        cv::Mat flattened = document_.flatten();
        jarkUtils::copyImageToClipboard(flattened);
        setStatus(kStrCopied);
    }

    // —— 界面 ——

    void setStatus(uint32_t stringId) {
        setStatusText(std::string(getUIString(stringId)));
        statusUntil_ = nowSeconds() + 4.0;
    }

    void setStatusText(std::string text) {
        statusText_ = std::move(text);
        if (statusLabel)
            statusLabel->setText(statusText_);
        isNeedRefreshUI = true;
    }

    static double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static std::string ui(uint32_t id) { return std::string(getUIString(id)); }

    // Win32 API 需要宽字符：窄表条目齐全（宽表只有一部分历史文案），直接转换即可
    static std::wstring uiW(uint32_t id) { return jarkUtils::utf8ToWstring(getUIString(id)); }

    void initCanvas() {
        textDrawer.setSize(dp(20));
        canvasMat = cv::Mat(dp(kLogicalHeight), dp(kLogicalWidth), CV_8UC4,
            jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG));
        buildControls();
    }

    void buildControls() {
        root = std::make_unique<jark::ui::Panel>();

        const int canvasWidth = kLogicalWidth - 2 * kGap - kSidebarWidth - kGap;
        const int canvasHeight = kLogicalHeight - 2 * kGap;
        canvasControl = static_cast<CanvasControl*>(
            root->overlay(std::make_unique<CanvasControl>(*this), { kGap, kGap, canvasWidth, canvasHeight }));

        auto sidebar = std::make_unique<jark::ui::Panel>();

        sidebar->add(std::make_unique<jark::ui::Label>(ui(kStrTool)), 26, 0);
        toolGrid = static_cast<jark::ui::OptionGrid*>(sidebar->add(std::make_unique<jark::ui::OptionGrid>(
            std::vector<std::string>{ ui(kStrRect), ui(kStrEllipse), ui(kStrArrow), ui(kStrLine),
                ui(kStrPen), ui(kStrMosaic), ui(kStrText), ui(kStrCrop) },
            2, &toolIndex_), 156, 4));

        sidebar->add(std::make_unique<jark::ui::Label>(ui(kStrColor)), 26, 10);
        colorRow = static_cast<jark::ui::ColorRow*>(sidebar->add(std::make_unique<jark::ui::ColorRow>(
            std::vector<uint32_t>(std::begin(kColors), std::end(kColors)), &colorIndex_), 40, 2));

        sidebar->add(std::make_unique<jark::ui::Slider>(ui(kStrWidth), &lineWidth_, 40, 170, 130, ""),
            kSliderHeight, 2);
        sidebar->add(std::make_unique<jark::ui::CheckBox>(ui(kStrFilled), &filled_), 34, 2);

        textOnlyControls.push_back(sidebar->add(std::make_unique<jark::ui::Slider>(
            ui(kStrFontSize), &fontSize_, 160, 170, 130, ""), kSliderHeight, 2));
        textBox = static_cast<jark::ui::TextBox*>(sidebar->add(
            std::make_unique<jark::ui::TextBox>(&textInput_, ui(kStrTextPrompt)), 38, 2));
        textOnlyControls.push_back(textBox);

        auto historyRow = std::make_unique<jark::ui::Row>();
        historyRow->add(std::make_unique<jark::ui::Button>(ui(kStrUndo), [this]() {
            document_.undo();
            rebuildDisplay();
            isNeedRefreshUI = true;
            }));
        historyRow->add(std::make_unique<jark::ui::Button>(ui(kStrRedo), [this]() {
            document_.redo();
            rebuildDisplay();
            isNeedRefreshUI = true;
            }));
        sidebar->add(std::move(historyRow), 44, 8);

        sidebar->add(std::make_unique<jark::ui::Label>(ui(kStrEdit)), 26, 4);

        auto rotateRow = std::make_unique<jark::ui::Row>();
        rotateRow->add(std::make_unique<jark::ui::Button>(ui(kBatchStrRotate270), [this]() { rotateLeft(); }));
        rotateRow->add(std::make_unique<jark::ui::Button>(ui(kBatchStrRotate90), [this]() { rotateRight(); }));
        sidebar->add(std::move(rotateRow), 44, 2);

        auto rotateRow2 = std::make_unique<jark::ui::Row>();
        rotateRow2->add(std::make_unique<jark::ui::Button>("180°", [this]() { rotateHalf(); }));
        rotateRow2->add(std::make_unique<jark::ui::Button>(ui(kStrInvert), [this]() { invertColors(); }));
        sidebar->add(std::move(rotateRow2), 44, 2);

        auto flipRow = std::make_unique<jark::ui::Row>();
        flipRow->add(std::make_unique<jark::ui::Button>(ui(kBatchStrFlipHorizontal), [this]() { flipHorizontal(); }));
        flipRow->add(std::make_unique<jark::ui::Button>(ui(kBatchStrFlipVertical), [this]() { flipVertical(); }));
        sidebar->add(std::move(flipRow), 44, 2);

        sidebar->add(std::make_unique<jark::ui::Button>(ui(kStrApplyCrop), [this]() { applyCrop(); }, true), 46, 8);

        auto outputRow = std::make_unique<jark::ui::Row>();
        outputRow->add(std::make_unique<jark::ui::Button>(ui(kStrSaveAs), [this]() { saveAs(); }));
        outputRow->add(std::make_unique<jark::ui::Button>(ui(kStrCopy), [this]() { copyToClipboard(); }));
        sidebar->add(std::move(outputRow), 46, 6);

        sidebar->add(std::make_unique<jark::ui::Button>(ui(kStrOverwrite), [this]() { overwriteSource(); }), 46, 2);

        // 状态行：平时显示操作提示，操作后临时显示反馈
        hintText_ = ui(kStrHint);
        statusText_ = hintText_;
        statusLabel = static_cast<jark::ui::Label*>(sidebar->add(
            std::make_unique<jark::ui::Label>(statusText_), 44, 8));

        root->overlay(std::move(sidebar),
            { kLogicalWidth - kGap - kSidebarWidth, kGap, kSidebarWidth, kLogicalHeight - 2 * kGap });
    }

    void updateTextOnlyVisibility() {
        const bool isText = currentTool() == jark::AnnoTool::Text;
        for (auto* control : textOnlyControls)
            control->visible = isText;

        if (!isText) {
            if (textBox)
                textBox->setFocused(false);
            textPlaced_ = false;
        }
    }

    // 文字内容随输入实时预览
    void syncTextPreview() {
        if (currentTool() != jark::AnnoTool::Text || !textPlaced_)
            return;

        if (textInput_ == lastPreviewText_)
            return;
        lastPreviewText_ = textInput_;

        document_.cancel();
        document_.begin(jark::AnnoTool::Text, currentStyle(), textAnchor_);
        document_.setText(textInput_);
        beginPreview();
        refreshPreview();
        isNeedRefreshUI = true;
    }

public:
    static inline volatile bool isWorking = false;
    static inline HWND hwnd = nullptr;

    EditorWindow(std::wstring path, cv::Mat image)
        : sourcePath_(std::move(path)) {
        requestExitFlag = false;
        isWorking = true;

        document_.reset(image);
        rebuildDisplay();
        runWindow();

        requestExitFlag = false;
        isWorking = false;
        hwnd = nullptr;
    }

    ~EditorWindow() = default;

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
        if (root && root->onMouseDown(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onLButtonUp() override {
        if (!root)
            return;

        root->onMouseUp(m_x, m_y);
        isNeedRefreshUI = true;
    }

    void onMButtonDown() override {
        panning_ = true;
        lastPanX_ = m_x;
        lastPanY_ = m_y;
    }

    void onMButtonUp() override {
        panning_ = false;
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
        if (textBox && textBox->onKeyChar(character)) {
            syncTextPreview();
            isNeedRefreshUI = true;
        }
    }

    void onKeyDown(WPARAM key) override {
        const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl && (key == 'Z' || key == 'Y')) {
            key == 'Z' ? document_.undo() : document_.redo();
            rebuildDisplay();
            isNeedRefreshUI = true;
            return;
        }

        if (ctrl && key == 'S') {
            saveAs();
            return;
        }

        if (ctrl && key == 'C') {
            copyToClipboard();
            return;
        }

        if (ctrl && (key == '0' || key == VK_NUMPAD0)) {
            setZoom(1.0); // 适应窗口
            showZoomStatus();
            return;
        }

        if (ctrl && (key == '1' || key == VK_NUMPAD1)) {
            zoomToActualPixels(); // 1:1
            showZoomStatus();
            return;
        }

        if (key == VK_RETURN && textBox && textBox->focused() && textPlaced_) {
            // 回车放置文字
            commitTextAt(textInput_, textAnchor_);
            textPlaced_ = false;
            lastPreviewText_.clear();
            isNeedRefreshUI = true;
            return;
        }

        if (key == VK_ESCAPE) {
            if (drawingShape_ || selecting_) {
                document_.cancel();
                drawingShape_ = false;
                selecting_ = false;
                textPlaced_ = false;
                clearSelection();
                endPreview();
                isNeedRefreshUI = true;
            }
            else {
                PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            }
        }
    }

    void drawingUI() override {
        jark::ui::UiCanvas canvas(canvasMat, textDrawer, GlobalVar::currentTheme, uiScale());
        canvas.fill({ 0, 0, canvas.width(), canvas.height() }, GlobalVar::currentTheme.BG);

        // 提示信息到期后回到操作提示
        if (nowSeconds() > statusUntil_ && statusText_ != hintText_)
            setStatusText(hintText_);

        updateTextOnlyVisibility();
        syncTextPreview();
        updateTextDrawerScale();

        root->bounds = { 0, 0, canvas.width(), canvas.height() };
        root->draw(canvas);
    }

private:
    void updateTextDrawerScale() {
        // 画布文字（若将来在图层面板显示文件名等）跟随 DPI
        const int size = dp(20);
        if (lastTextDrawerSize_ != size) {
            lastTextDrawerSize_ = size;
            textDrawer.setSize(size);
        }
    }

    int lastTextDrawerSize_ = 0;

    void runWindow() {
        if (!createWindow(kLogicalWidth, kLogicalHeight, windowsClassName, uiW(kStrTitle).c_str()))
            return;

        hwnd = m_hwnd;
        initCanvas();
        isNeedRefreshUI = true;
        runMessageLoop();
    }
};
