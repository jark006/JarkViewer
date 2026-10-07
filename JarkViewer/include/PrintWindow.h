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

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::SetNextWindowSize({ 760.0f * scale, 620.0f * scale }, ImGuiCond_FirstUseEver);
        // 最小宽度按控件行实际占用算（见 minWidth_）：窄了会把“打印”顶到窗口边缘甚至截断
        ImGui::SetNextWindowSizeConstraints({ 0.0f, 360.0f * scale }, { FLT_MAX, FLT_MAX },
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
        const float sliderWidth = 260.0f * scale;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(kStrBrightness));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderInt("##brightness", &brightness_, 0, 200, "%d"))
            previewDirty_ = true;

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(getUIString(kStrContrast));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderInt("##contrast", &contrast_, 0, 200, "%d"))
            previewDirty_ = true;
        noteRowWidth();

        minWidth_ = rowWidth + ImGui::GetStyle().WindowPadding.x + 4.0f * scale;
    }

    void drawPreview() {
        refreshPreviewIfNeeded();
        if (!previewTexture_)
            return;

        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.x <= 1.0f || available.y <= 1.0f)
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

    void refreshPreviewIfNeeded() {
        if (!previewDirty_ || sourceImage_.empty())
            return;

        previewDirty_ = false;

        // 预览按窗口宽度缩放，避免大图每帧全尺寸处理
        const int previewEdge = 1400;
        cv::Mat resized;
        const int maxEdge = (std::max)(sourceImage_.cols, sourceImage_.rows);
        if (maxEdge > previewEdge) {
            const double factor = static_cast<double>(previewEdge) / maxEdge;
            cv::resize(sourceImage_, resized, {}, factor, factor, cv::INTER_AREA);
        }
        else {
            // 小图直接用原图缩放的副本（调整是就地做的，不能改到 sourceImage_，
            // 否则预览会叠加前一次的效果，另存/打印也跟着错）
            resized = sourceImage_.clone();
        }

        jark::applyImageAdjustments(resized, brightness_, contrast_, colorMode_, invert_);
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

        std::ofstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            file.close();
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

        // 留 5% 边距
        const double factor = 0.9 * (std::min)(
            static_cast<double>(pageWidth) / output.cols,
            static_cast<double>(pageHeight) / output.rows);

        cv::Mat resized;
        cv::resize(output, resized, cv::Size(
            static_cast<int>(std::round(output.cols * factor)),
            static_cast<int>(std::round(output.rows * factor))), 0, 0);

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
            rememberParameters();
        }

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
        info.bmiHeader.biHeight = -bgr.rows;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits)
            return nullptr;

        const int rowBytes = bgr.cols * 3;
        uint8_t* target = static_cast<uint8_t*>(bits);
        for (int y = 0; y < bgr.rows; ++y) {
            const uint8_t* source = bgr.ptr<uint8_t>(bgr.rows - 1 - y);
            memcpy(target + static_cast<size_t>(y) * rowBytes, source, rowBytes);
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
    uint32_t colorMode_ = 1;
    bool invert_ = false;
};
