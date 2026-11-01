#pragma once

// 打印窗口（ImGui 版）：颜色模式/正反色/亮度对比度 + 实时预览，确认后走 Windows 打印对话框。
// 图像调整与批量共用 ImageAdjust（applyImageAdjustments）。

#include "ImageAdjust.h"
#include "Localization.h"
#include "UiHost.h"
#include "jarkUtils.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include <windows.h>

class PrintWindow {
public:
    static PrintWindow& instance() {
        static PrintWindow window;
        return window;
    }

    // 主窗口调用：image 为已按显示旋转处理过的图像
    void open(const cv::Mat& image, int rotation) {
        cv::Mat rotated;
        switch (rotation) {
        case 1: cv::rotate(image, rotated, cv::ROTATE_90_COUNTERCLOCKWISE); break;
        case 2: cv::rotate(image, rotated, cv::ROTATE_180); break;
        case 3: cv::rotate(image, rotated, cv::ROTATE_90_CLOCKWISE); break;
        default: rotated = image; break;
        }

        // 必须深拷贝（toBgrImage 内部 clone）：下面的调整都是就地做的，
        // 浅拷贝会把主窗口正在显示的图一起改掉
        sourceImage_ = jark::toBgrImage(rotated);
        if (sourceImage_.empty())
            return;

        if (!jarkUtils::limitSizeTo16K(sourceImage_)) {
            MessageBoxW(nullptr, L"调整图像尺寸发生错误", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        // 载入上次使用的参数
        auto& parameter = GlobalVar::settingParameter;
        brightness_ = std::clamp<int>(static_cast<int>(parameter.printerBrightness), 0, 200);
        contrast_ = std::clamp<int>(static_cast<int>(parameter.printerContrast), 0, 200);
        colorMode_ = std::min<uint32_t>(parameter.printercolorMode, 3);
        invert_ = parameter.printerInvertColors;

        brightnessWheelRemainder_ = contrastWheelRemainder_ = 0.0f;
        previewDirty_ = true;
        visible_ = true;
        focusRequested_ = true;
    }

    void close() {
        // 关掉窗口就记住这次的调整参数（原来只在另存/打印时记，调完直接关闭会丢）
        rememberParameters();

        visible_ = false;
        previewTexture_ = 0;
    }

    bool visible() const { return visible_; }

    // 经实际 GDI 绘制后读回像素，覆盖上下方向、24bpp 行对齐与非连续源图。
    static std::string runBitmapTests() {
        std::string report;
        int passed = 0, failed = 0;
        HDC screenDc = GetDC(nullptr);
        for (int width = 1; width <= 8; ++width) {
            for (bool useRoi : { false, true }) {
                cv::Mat backing(7, useRoi ? width + 3 : width, CV_8UC3);
                cv::Mat image = useRoi ? backing(cv::Rect(1, 1, width, 5)) : backing;
                for (int y = 0; y < image.rows; ++y) {
                    for (int x = 0; x < image.cols; ++x)
                        image.at<cv::Vec3b>(y, x) = cv::Vec3b(20 + x * 20, 30 + y * 20, 200 - y * 20);
                }

                HBITMAP source = toBitmap(image);
                HDC sourceDc = CreateCompatibleDC(screenDc);
                HDC targetDc = CreateCompatibleDC(screenDc);
                HBITMAP target = CreateCompatibleBitmap(screenDc, width * 2, image.rows * 2);
                HGDIOBJ oldSource = source && sourceDc ? SelectObject(sourceDc, source) : nullptr;
                HGDIOBJ oldTarget = target && targetDc ? SelectObject(targetDc, target) : nullptr;
                bool ok = screenDc && oldSource && oldSource != HGDI_ERROR &&
                    oldTarget && oldTarget != HGDI_ERROR;
                if (ok) {
                    SetStretchBltMode(targetDc, COLORONCOLOR);
                    ok = StretchBlt(targetDc, 0, 0, width * 2, image.rows * 2,
                        sourceDc, 0, 0, width, image.rows, SRCCOPY) != FALSE;
                    for (int y = 0; y < image.rows && ok; ++y) {
                        for (int x = 0; x < image.cols && ok; ++x) {
                            const auto pixel = image.at<cv::Vec3b>(y, x);
                            ok = GetPixel(targetDc, x * 2, y * 2) == RGB(pixel[2], pixel[1], pixel[0]);
                        }
                    }
                }
                if (oldSource && oldSource != HGDI_ERROR)
                    SelectObject(sourceDc, oldSource);
                if (oldTarget && oldTarget != HGDI_ERROR)
                    SelectObject(targetDc, oldTarget);
                if (targetDc) DeleteDC(targetDc);
                if (sourceDc) DeleteDC(sourceDc);
                if (target) DeleteObject(target);
                if (source) DeleteObject(source);
                ok ? ++passed : ++failed;
                report += std::format("[{}] GDI 打印位图方向/颜色/行距：宽 {}，{}\n",
                    ok ? "ok" : "FAIL", width, useRoi ? "非连续 ROI" : "连续图像");
            }
        }
        if (screenDc) ReleaseDC(nullptr, screenDc);

        const auto check = [&](bool ok, std::string_view name) {
            ok ? ++passed : ++failed;
            report += std::format("[{}] {}\n", ok ? "ok" : "FAIL", name);
        };
        for (const auto source : { cv::Size(4000, 3000), cv::Size(3000, 4000),
            cv::Size(10000, 1), cv::Size(1, 10000) }) {
            for (float scale : { 1.0f, 1.25f, 1.5f, 2.0f }) {
                const ImVec2 origin{ 13.25f, 47.5f }, available{ 731.5f, 487.25f };
                const auto layout = ditherPreviewLayout(source, origin, available, { scale, scale });
                const bool aligned = std::abs(layout.position.x * scale - std::round(layout.position.x * scale)) < 0.001f &&
                    std::abs(layout.position.y * scale - std::round(layout.position.y * scale)) < 0.001f;
                const bool fits = layout.position.x >= origin.x && layout.position.y >= origin.y &&
                    layout.position.x + layout.size.x <= origin.x + available.x + 0.001f &&
                    layout.position.y + layout.size.y <= origin.y + available.y + 0.001f;
                check(!layout.pixels.empty() && aligned && fits &&
                    std::abs(layout.size.x * scale - layout.pixels.width) < 0.001f &&
                    std::abs(layout.size.y * scale - layout.pixels.height) < 0.001f,
                    std::format("抖动预览按物理像素对齐且不越界：{}x{}，缩放 {}", source.width, source.height, scale));
            }
        }
        check(ditherPreviewLayout({ 4000, 3000 }, {}, { 800, 600 }, { 1, 1 }).pixels == cv::Size(800, 600) &&
            ditherPreviewLayout({ 4000, 3000 }, {}, { 1000, 750 }, { 1, 1 }).pixels == cv::Size(1000, 750),
            "窗口尺寸变化后使用新的预览像素尺寸");
        check(ditherPreviewLayout({ 3000, 4000 }, {}, { 800, 600 }, { 1, 1 }).pixels == cv::Size(450, 600),
            "竖图预览保持宽高比");
        check(ditherPreviewLayout({ 10000, 1 }, {}, { 800, 600 }, { 1, 1 }).pixels == cv::Size(800, 1) &&
            ditherPreviewLayout({ 1, 10000 }, {}, { 800, 600 }, { 1, 1 }).pixels == cv::Size(1, 600),
            "极端长宽比预览短边至少为 1 像素");
        check(ditherPreviewLayout({ 100, 100 }, {}, { 0, 600 }, { 1, 1 }).pixels.empty(),
            "空预览区域不创建位图");

        PrintWindow preview;
        cv::Mat backing(67, 103, CV_8UC3);
        cv::randu(backing, 40, 220);
        preview.sourceImage_ = backing(cv::Rect(1, 1, 97, 61));
        const auto original = preview.sourceImage_.clone();
        preview.colorMode_ = static_cast<uint32_t>(jark::ColorMode::Dither);
        preview.brightness_ = 80;
        preview.contrast_ = 100;
        for (const auto size : { cv::Size(43, 27), cv::Size(97, 61), cv::Size(149, 94) }) {
            const auto image = preview.makePreviewImage(size);
            cv::Mat gray;
            cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            check(image.size() == size && cv::countNonZero((gray != 0) & (gray != 255)) == 0,
                std::format("先缩放再抖动，预览只含黑白像素：{}x{}", size.width, size.height));
            preview.invert_ = true;
            const auto inverted = preview.makePreviewImage(size);
            cv::Mat restored;
            cv::bitwise_not(inverted, restored);
            check(cv::norm(image, restored, cv::NORM_INF) == 0, "预览反相逐像素互补");
            preview.invert_ = false;
        }
        check(cv::norm(preview.sourceImage_, original, cv::NORM_INF) == 0, "预览不修改非连续源图像素");
        return std::format("---- 打印自检：{} 通过, {} 失败 ----\n{}", passed, failed, report);
    }

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        // 尺寸先收敛到主视口内：装不下的窗口会被多视口模式分离成独立 OS 窗口（角外露黑底、跑到主窗口外）
        ImGui::SetNextWindowSize(
            jark::ui::fitWindowSizeToMainViewport({ 760.0f * scale, 620.0f * scale }), ImGuiCond_FirstUseEver);
        // 最小宽度按控件行实际占用算（见 minWidth_）：窄了会把“打印”顶到窗口边缘甚至截断；
        // 上限同样钳到主视口，防拖拽放大后被分离
        ImGui::SetNextWindowSizeConstraints(
            jark::ui::fitWindowSizeToMainViewport({ 0.0f, 360.0f * scale }),
            jark::ui::fitWindowSizeToMainViewport({ FLT_MAX, FLT_MAX }),
            sizeConstraints, this);
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

        drawControls(scale);
        ImGui::Separator();
        drawPreview();

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape))
            close();

        ImGui::End();

        if (!open)
            close();
    }

private:
    PrintWindow() = default;

    static std::string title() {
        return jarkUtils::wstringToUtf8(getUIStringW(40).c_str()) + "###print";
    }

    // 窗口最小尺寸：宽取控件行实际占用（minWidth_），高保证两个滑块 + 一点预览
    static void sizeConstraints(ImGuiSizeCallbackData* data) {
        const auto* self = static_cast<const PrintWindow*>(data->UserData);
        const float scale = jark::ui::UiHost::instance().scale();
        data->DesiredSize.x = (std::max)(data->DesiredSize.x, self->minWidth_);
        data->DesiredSize.y = (std::max)(data->DesiredSize.y, 360.0f * scale);
    }

    void drawAdjustmentSlider(const char* id, int& value, float& wheelRemainder) {
        if (ImGui::SliderInt(id, &value, 0, 200, "%d")) {
            previewDirty_ = true;
            wheelRemainder = 0.0f;
        }
        if (!ImGui::IsItemHovered() || ImGui::IsItemActive() ||
            !ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) {
            wheelRemainder = 0.0f;
            return;
        }

        // 每个滚轮刻度只调 1；高精度滚轮的小数在当前滑块内累积，移开后清掉。
        wheelRemainder += ImGui::GetIO().MouseWheel;
        const int steps = static_cast<int>(wheelRemainder);
        wheelRemainder -= steps;
        const int nextValue = std::clamp(value + steps, 0, 200);
        if (nextValue != value) {
            value = nextValue;
            previewDirty_ = true;
        }
        // 到达边界后丢弃向外的小数余量，反向滚动应立即响应。
        if ((value == 0 && wheelRemainder < 0) || (value == 200 && wheelRemainder > 0))
            wheelRemainder = 0.0f;
    }

    void drawControls(float scale) {
        const char* colorModes[] = { getUIString(kStrColor), getUIString(kStrGray), getUIString(kStrDocument), getUIString(kStrDither) };

        // 记录两行控件各自的右边界，取最宽的一行作为窗口最小宽度（下一帧生效，见 sizeConstraints）
        float rowWidth = 0.0f;
        auto noteRowWidth = [&] {
            rowWidth = (std::max)(rowWidth, ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x);
        };

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(kStrColorMode));
        ImGui::SameLine();

        for (int index = 0; index < 4; ++index) {
            if (index > 0)
                ImGui::SameLine();
            const std::string id = std::string(colorModes[index]) + "##colorMode";
            if (ImGui::RadioButton(id.c_str(), colorMode_ == static_cast<uint32_t>(index))) {
                colorMode_ = static_cast<uint32_t>(index);
                // 与旧版一致：切到文档/抖动模式时给出建议的亮度对比度
                if (index == 2) {
                    brightness_ = 160;
                    contrast_ = 180;
                }
                else if (index == 3) {
                    brightness_ = 80;
                    contrast_ = 100;
                }
                previewDirty_ = true;
            }
        }

        ImGui::SameLine();
        if (ImGui::Checkbox(getUIString(kStrInvert), &invert_))
            previewDirty_ = true;

        // 另存为/打印紧跟在“反相”右边（宽度取原来的一半，至少放得下文字）
        const float halfButton = 70.0f * scale;
        auto buttonWidth = [](const char* label, float minWidth) {
            return (std::max)(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f, minWidth);
        };

        ImGui::SameLine();
        if (ImGui::Button(getUIString(kStrSaveAs), { buttonWidth(getUIString(kStrSaveAs), halfButton), 0 })) {
            saveToFile();
        }
        ImGui::SameLine();
        if (ImGui::Button(getUIString(kStrPrint), { buttonWidth(getUIString(kStrPrint), halfButton), 0 })) {
            print();
        }
        noteRowWidth();

        // 标签放在控件左边
        const char* wheelHint = getUIString(kStrWheelFineTune);
        // 给右侧提示留空间；长语言文案缩短滑块，避免提示被窗口边缘裁切。
        const float labelsWidth = ImGui::CalcTextSize(getUIString(kStrBrightness)).x +
            ImGui::CalcTextSize(getUIString(kStrContrast)).x + ImGui::CalcTextSize(wheelHint).x;
        const auto* viewport = ImGui::GetMainViewport();
        const float availableWidth = (std::min)(ImGui::GetContentRegionAvail().x,
            viewport->Pos.x + viewport->Size.x - ImGui::GetCursorScreenPos().x - 8.0f * scale);
        const float sliderWidth = (std::min)(260.0f * scale, (std::max)(80.0f * scale,
            (availableWidth - labelsWidth - ImGui::GetStyle().ItemSpacing.x * 4.0f - 4.0f * scale) * 0.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(kStrBrightness));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        drawAdjustmentSlider("##brightness", brightness_, brightnessWheelRemainder_);

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(kStrContrast));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        drawAdjustmentSlider("##contrast", contrast_, contrastWheelRemainder_);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wheelHint);
        noteRowWidth();

        minWidth_ = rowWidth + ImGui::GetStyle().WindowPadding.x + 4.0f * scale;
    }

    void drawPreview() {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (sourceImage_.empty() || available.x <= 1.0f || available.y <= 1.0f)
            return;

        if (static_cast<jark::ColorMode>(colorMode_) == jark::ColorMode::Dither) {
            const auto layout = ditherPreviewLayout(sourceImage_.size(), ImGui::GetCursorScreenPos(),
                available, ImGui::GetIO().DisplayFramebufferScale);
            if (layout.pixels.empty())
                return;
            refreshPreviewIfNeeded(layout.pixels);
            if (!previewTexture_)
                return;

            // 点阵已按实际显示尺寸生成，不能再经过非整数缩放或线性过滤。
            ImGui::SetCursorScreenPos(layout.position);
            auto* drawList = ImGui::GetWindowDrawList();
            auto& platform = ImGui::GetPlatformIO();
            drawList->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
            ImGui::Image(previewTexture_, layout.size);
            drawList->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
            return;
        }

        refreshPreviewIfNeeded();
        if (!previewTexture_)
            return;

        // 等比缩放显示
        const float imageAspect = static_cast<float>(previewWidth_) / static_cast<float>(previewHeight_);
        float width = available.x;
        float height = width / imageAspect;
        if (height > available.y) {
            height = available.y;
            width = height * imageAspect;
        }

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - width) * 0.5f);
        ImGui::Image(previewTexture_, { width, height });
    }

    struct DitherPreviewLayout {
        ImVec2 position{};
        ImVec2 size{};
        cv::Size pixels;
    };

    static DitherPreviewLayout ditherPreviewLayout(cv::Size source, ImVec2 origin,
        ImVec2 available, ImVec2 framebufferScale) {
        if (source.empty() || framebufferScale.x <= 0 || framebufferScale.y <= 0)
            return {};
        // 内缩到完整物理像素，包含居中偏移；高 DPI 或半像素起点不能让点阵落在像素缝上。
        const int left = static_cast<int>(std::ceil(origin.x * framebufferScale.x));
        const int top = static_cast<int>(std::ceil(origin.y * framebufferScale.y));
        const int width = static_cast<int>(std::floor((origin.x + available.x) * framebufferScale.x)) - left;
        const int height = static_cast<int>(std::floor((origin.y + available.y) * framebufferScale.y)) - top;
        if (width <= 0 || height <= 0)
            return {};
        const double factor = (std::min)(static_cast<double>(width) / source.width,
            static_cast<double>(height) / source.height);
        const cv::Size pixels(std::clamp(static_cast<int>(std::round(source.width * factor)), 1, width),
            std::clamp(static_cast<int>(std::round(source.height * factor)), 1, height));
        return { { (left + (width - pixels.width) / 2) / framebufferScale.x, top / framebufferScale.y },
            { pixels.width / framebufferScale.x, pixels.height / framebufferScale.y }, pixels };
    }

    cv::Mat makePreviewImage(cv::Size size) const {
        cv::Mat resized;
        if (size == sourceImage_.size()) {
            // 调整会就地修改，小图也必须复制，不能让预览效果叠加到原图。
            resized = sourceImage_.clone();
        }
        else {
            cv::resize(sourceImage_, resized, size, 0, 0,
                size.area() < sourceImage_.size().area() ? cv::INTER_AREA : cv::INTER_LINEAR);
        }
        jark::applyImageAdjustments(resized, brightness_, contrast_, colorMode_, invert_);
        return resized;
    }

    void refreshPreviewIfNeeded(cv::Size size = {}) {
        if (sourceImage_.empty())
            return;

        if (size.empty()) {
            // 其它颜色模式保留固定上限，再由界面等比缩放显示。
            const int previewEdge = 1400;
            const int maxEdge = (std::max)(sourceImage_.cols, sourceImage_.rows);
            const double factor = (std::min)(1.0, static_cast<double>(previewEdge) / maxEdge);
            size = { (std::max)(1, static_cast<int>(std::round(sourceImage_.cols * factor))),
                (std::max)(1, static_cast<int>(std::round(sourceImage_.rows * factor))) };
        }
        if (!previewDirty_ && size == cv::Size(previewWidth_, previewHeight_))
            return;
        previewDirty_ = false;

        cv::Mat resized;
        try {
            resized = makePreviewImage(size);
        }
        catch (const cv::Exception& e) {
            JARK_LOG("print preview failed: {}", e.what());
            previewTexture_ = 0;
            return;
        }

        previewWidth_ = resized.cols;
        previewHeight_ = resized.rows;
        previewTexture_ = jark::ui::UiHost::instance().textureFromImage(resized, 1);
    }

    void saveToFile() {
        auto [filePath, isJpg] = jarkUtils::saveImageDialogW(getUIStringW(23).c_str());
        if (filePath.empty())
            return;

        cv::Mat output = sourceImage_.clone();
        jark::applyImageAdjustments(output, brightness_, contrast_, colorMode_, invert_);

        std::vector<uchar> buffer;
        if (!cv::imencode(isJpg ? ".jpg" : ".png", output, buffer))
            return;

        if (jarkUtils::writeFileAtomically(filePath, buffer)) {
            rememberParameters();
        }
    }

    void print() {
        cv::Mat output = sourceImage_.clone();
        jarkUtils::limitSizeTo16K(output);

        PRINTDLGW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;

        if (!PrintDlgW(&dialog))
            return;
        if (!dialog.hDC) {
            MessageBoxW(nullptr, L"无法获得打印机参数", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        DOCINFOW document{};
        document.cbSize = sizeof(document);
        document.lpszDocName = L"JarkViewer Printed Image";

        if (StartDocW(dialog.hDC, &document) <= 0) {
            DeleteDC(dialog.hDC);
            return;
        }

        if (StartPage(dialog.hDC) <= 0) {
            EndDoc(dialog.hDC);
            DeleteDC(dialog.hDC);
            return;
        }

        const int pageWidth = GetDeviceCaps(dialog.hDC, HORZRES);
        const int pageHeight = GetDeviceCaps(dialog.hDC, VERTRES);

        bool drew = false;
        try {
            // 留 5% 边距；短边夹到至少 1 像素（极端长宽比按比例算出 0 会让 resize 抛异常）
            const double factor = 0.9 * (std::min)(
                static_cast<double>(pageWidth) / output.cols,
                static_cast<double>(pageHeight) / output.rows);

            cv::Mat resized;
            cv::resize(output, resized, cv::Size(
                (std::max)(1, static_cast<int>(std::round(output.cols * factor))),
                (std::max)(1, static_cast<int>(std::round(output.rows * factor)))), 0, 0);

            jark::applyImageAdjustments(resized, brightness_, contrast_, colorMode_, invert_);

            cv::Mat page(pageHeight, pageWidth, CV_8UC3, cv::Scalar(255, 255, 255));
            const int offsetX = (pageWidth - resized.cols + 1) / 2;
            const int offsetY = (pageHeight - resized.rows + 1) / 2;
            resized.copyTo(page(cv::Rect(offsetX, offsetY, resized.cols, resized.rows)));

            HBITMAP bitmap = toBitmap(page);
            if (bitmap) {
                HDC memoryDc = CreateCompatibleDC(dialog.hDC);
                SelectObject(memoryDc, bitmap);

                SetStretchBltMode(dialog.hDC, COLORONCOLOR);
                SetBrushOrgEx(dialog.hDC, 0, 0, nullptr);
                StretchBlt(dialog.hDC, 0, 0, pageWidth, pageHeight,
                    memoryDc, 0, 0, pageWidth, pageHeight, SRCCOPY);

                DeleteDC(memoryDc);
                DeleteObject(bitmap);
                drew = true;
            }
        }
        catch (const cv::Exception& e) {
            // 未捕获的异常会让整个进程退出，兜住后照常结束打印作业
            JARK_LOG("print failed: {}", e.what());
        }

        if (drew)
            rememberParameters();

        EndPage(dialog.hDC);
        EndDoc(dialog.hDC);
        DeleteDC(dialog.hDC);
    }

    static HBITMAP toBitmap(const cv::Mat& bgr) {
        if (bgr.empty() || bgr.type() != CV_8UC3)
            return nullptr;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = bgr.cols;
        // 负高度是从上到下的 DIB，与 OpenCV 行序一致；复制时不能再倒序，否则打印会上下翻转。
        info.bmiHeader.biHeight = -bgr.rows;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits)
            return nullptr;

        const int rowBytes = bgr.cols * 3;
        // 24bpp DIB 的行距按 4 字节对齐，宽度不是 4 的倍数时不能按 cols*3 走行，否则逐行错位
        const int stride = (rowBytes + 3) & ~3;
        uint8_t* target = static_cast<uint8_t*>(bits);
        for (int y = 0; y < bgr.rows; ++y) {
            const uint8_t* source = bgr.ptr<uint8_t>(y);
            memcpy(target + static_cast<size_t>(y) * stride, source, rowBytes);
        }
        return bitmap;
    }

    void rememberParameters() {
        auto& parameter = GlobalVar::settingParameter;
        parameter.printerBrightness = static_cast<uint32_t>(brightness_);
        parameter.printerContrast = static_cast<uint32_t>(contrast_);
        parameter.printercolorMode = colorMode_;
        parameter.printerInvertColors = invert_;
    }

    // 字符串表 ID
    static constexpr uint32_t kStrColorMode = 88;   // 颜色模式
    static constexpr uint32_t kStrColor = 66;       // 彩色
    static constexpr uint32_t kStrGray = 67;        // 黑白
    static constexpr uint32_t kStrDocument = 68;    // 黑白文档
    static constexpr uint32_t kStrDither = 69;      // 黑白抖动
    static constexpr uint32_t kStrBrightness = 89;  // 亮度
    static constexpr uint32_t kStrContrast = 90;    // 对比度
    static constexpr uint32_t kStrInvert = 108;     // 反相
    static constexpr uint32_t kStrSaveAs = 109;     // 另存为
    static constexpr uint32_t kStrPrint = 126;      // 打印
    static constexpr uint32_t kStrWheelFineTune = 196; // 滚轮可微调

    bool visible_ = false;
    bool focusRequested_ = false;
    float minWidth_ = 0.0f; // 控件行实际占用宽度，drawControls 每帧更新

    cv::Mat sourceImage_;   // BGR
    cv::Mat previewImage_;
    bool previewDirty_ = true;
    ImTextureID previewTexture_ = 0;
    int previewWidth_ = 1;
    int previewHeight_ = 1;

    int brightness_ = 100;
    int contrast_ = 100;
    float brightnessWheelRemainder_ = 0.0f;
    float contrastWheelRemainder_ = 0.0f;
    uint32_t colorMode_ = 1;
    bool invert_ = false;
};
