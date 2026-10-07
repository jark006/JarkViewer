#pragma once

// 图像编辑与标注窗口（ImGui 版）。
//
// 标注模型在 ImageAnnotator（纯逻辑，可用 --probe --annotate 验证），这里只做交互与显示：
//   - 画布：已提交的内容作为纹理贴出，正在拖动的图形用 ImDrawList 矢量绘制（拖动过程中零上传）；
//   - 工具：矩形/椭圆/箭头/直线/画笔/马赛克/文字/裁剪，颜色、线宽、字号、填充；
//   - 编辑：旋转、翻转、反相、裁剪（裁剪选区用框选 + 应用）；
//   - 输出：另存为、复制到剪贴板、覆盖原文件。

#include "ImageAnnotator.h"
#include "Localization.h"
#include "TextRenderer.h"
#include "UiHost.h"
#include "jarkUtils.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <commdlg.h>

class EditorWindow {
public:
    static EditorWindow& instance() {
        static EditorWindow window;
        return window;
    }

    void open(std::wstring path, const cv::Mat& image) {
        sourcePath_ = std::move(path);
        document_.reset(image);
        textInput_.clear();
        textPlaced_ = false;
        clearSelection();
        resetView();
        textureDirty_ = true;
        statusText_.clear();
        statusUntil_ = 0.0;

        visible_ = true;
        focusRequested_ = true;
    }

    void close() { visible_ = false; }
    bool visible() const { return visible_; }

    void draw() {
        if (!visible_)
            return;

        const float scale = jark::ui::UiHost::instance().scale();
        ImGui::SetNextWindowSize({ 1100.0f * scale, 700.0f * scale }, ImGuiCond_FirstUseEver);
        // 不能再缩小到藏住工具栏/侧栏
        ImGui::SetNextWindowSizeConstraints({ 840.0f * scale, 520.0f * scale }, { FLT_MAX, FLT_MAX });
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

        if (textureDirty_)
            rebuildTexture();

        drawToolbar(scale);

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float sidebarWidth = 250.0f * scale;
        const ImVec2 canvasSize(
            (std::max)(120.0f, available.x - sidebarWidth - ImGui::GetStyle().ItemSpacing.x),
            (std::max)(120.0f, available.y));

        ImGui::BeginGroup();
        drawCanvas(canvasSize);
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::BeginGroup();
        drawSidebar(scale);
        ImGui::EndGroup();

        handleShortcuts();

        ImGui::End();

        if (!open)
            close();
    }

private:
    EditorWindow() = default;

    static std::string title() {
        return std::string(getUIString(kStrTitle)) + "###editor";
    }

    // —— 画布 ——

    void drawCanvas(const ImVec2& canvasSize) {
        ImGui::BeginChild("editorCanvas", canvasSize, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 region = ImGui::GetContentRegionAvail();
        canvasRect_ = ImVec4(origin.x, origin.y, region.x, region.y);

        // 视图变换：图像像素 -> 屏幕像素。编辑/标注按适应窗口固定显示，不提供缩放：
        // 标注坐标与显示一一对应，避免缩放带来的误标与坐标换算问题。
        fitScale_ = (std::min)(region.x / document_.width(), region.y / document_.height());
        if (fitScale_ <= 0.0f)
            fitScale_ = 1.0f;

        const float viewScale = fitScale_;
        const ImVec2 imageSize(document_.width() * viewScale, document_.height() * viewScale);
        const ImVec2 imagePos(
            origin.x + (region.x - imageSize.x) * 0.5f,
            origin.y + (region.y - imageSize.y) * 0.5f);
        imageRect_ = ImVec4(imagePos.x, imagePos.y, imageSize.x, imageSize.y);
        viewScale_ = viewScale;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled({ origin.x, origin.y }, { origin.x + region.x, origin.y + region.y },
            ImGui::GetColorU32(ImGuiCol_FrameBg));

        const ImVec2 imageMax(imagePos.x + imageSize.x, imagePos.y + imageSize.y);
        if (canvasTexture_)
            drawList->AddImage(canvasTexture_, imagePos, imageMax);
        // 图像边框：透明图也能看出可编辑范围
        drawList->AddRect(imagePos, imageMax, ImGui::GetColorU32(ImGuiCol_Border));

        drawSelectionOverlay(drawList);
        drawActiveShape(drawList);

        // 交互：整块画布区域都可响应
        ImGui::InvisibleButton("canvasInput", region, ImGuiButtonFlags_MouseButtonLeft);

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        handleCanvasDrawing(mouse);

        ImGui::EndChild();
    }

    ImVec2 toScreen(const cv::Point& point) const {
        return { imageRect_.x + point.x * viewScale_, imageRect_.y + point.y * viewScale_ };
    }

    cv::Point toImage(const ImVec2& screen) const {
        const int width = document_.width();
        const int height = document_.height();
        const int x = static_cast<int>(std::lround((screen.x - imageRect_.x) / viewScale_));
        const int y = static_cast<int>(std::lround((screen.y - imageRect_.y) / viewScale_));
        // 拖到画面外时夹回图内：标注/裁剪坐标必须落在图像上
        return { std::clamp(x, 0, (std::max)(width - 1, 0)), std::clamp(y, 0, (std::max)(height - 1, 0)) };
    }

    // 鼠标是否落在图像本身上（画布空白处不算）
    bool isOnImage(const ImVec2& screen) const {
        return screen.x >= imageRect_.x && screen.x < imageRect_.x + imageRect_.z &&
            screen.y >= imageRect_.y && screen.y < imageRect_.y + imageRect_.w;
    }

    void handleCanvasDrawing(const ImVec2& mouse) {
        const bool isText = currentTool() == jark::AnnoTool::Text;

        // 开始：必须点在图像上，点画布空白处不产生标注
        if (ImGui::IsItemHovered() && isOnImage(mouse) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (currentTool() == jark::AnnoTool::Crop) {
                selecting_ = true;
                cropSelection_ = cv::Rect(toImage(mouse), cv::Size(0, 0));
                cropStart_ = toImage(mouse);
                hasSelection_ = false;
                return;
            }

            if (isText) {
                if (textPlaced_ && !textInput_.empty())
                    commitTextAt(textInput_, textAnchor_);

                textAnchor_ = toImage(mouse);
                textPlaced_ = true;
                document_.begin(jark::AnnoTool::Text, currentStyle(), textAnchor_);
                document_.setText(textInput_);
                return;
            }

            document_.begin(currentTool(), currentStyle(), toImage(mouse));
            drawingShape_ = true;
            return;
        }

        // 拖动
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (selecting_) {
                const cv::Point current = toImage(mouse);
                cropSelection_ = cv::Rect(
                    (std::min)(cropStart_.x, current.x), (std::min)(cropStart_.y, current.y),
                    std::abs(current.x - cropStart_.x), std::abs(current.y - cropStart_.y));
            }
            else if (drawingShape_ && !isText) {
                document_.update(toImage(mouse));
            }
        }

        // 结束
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (selecting_) {
                selecting_ = false;
                cropSelection_ = cropSelection_ & cv::Rect(0, 0, document_.width(), document_.height());
                hasSelection_ = cropSelection_.width >= 4 && cropSelection_.height >= 4;
                setStatus(hasSelection_ ? kStrCrop : kStrNeedSelection);
                return;
            }

            if (drawingShape_) {
                drawingShape_ = false;
                document_.commit();
                textureDirty_ = true;
            }
        }
    }

    // 正在拖动的图形：矢量预览（拖动期间不重新上传纹理）
    void drawActiveShape(ImDrawList* drawList) {
        if (!drawingShape_)
            return;

        const jark::Annotation& active = document_.active();
        if (active.points.empty())
            return;

        const ImU32 color = imColor(active.style.color);
        const float thickness = (std::max)(1.0f, active.style.width * viewScale_);
        const ImVec2 from = toScreen(active.points.front());
        const ImVec2 to = toScreen(active.points.back());

        switch (active.tool) {
        case jark::AnnoTool::Rect:
            drawList->AddRect(from, to, color, 0.0f, 0, thickness);
            break;

        case jark::AnnoTool::Ellipse: {
            const ImVec2 center((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
            drawList->AddEllipse(center, { std::abs(to.x - from.x) * 0.5f, std::abs(to.y - from.y) * 0.5f },
                color, 0.0f, 0, thickness);
        } break;

        case jark::AnnoTool::Line:
            drawList->AddLine(from, to, color, thickness);
            break;

        case jark::AnnoTool::Arrow: {
            drawList->AddLine(from, to, color, thickness);
            const ImVec2 direction(to.x - from.x, to.y - from.y);
            const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            if (length > 1.0f) {
                const float head = (std::clamp)(length * 0.25f, thickness * 2.5f, thickness * 8.0f);
                const ImVec2 unit(direction.x / length, direction.y / length);
                const ImVec2 base(to.x - unit.x * head, to.y - unit.y * head);
                const ImVec2 perpendicular(-unit.y * head * 0.35f, unit.x * head * 0.35f);
                drawList->AddTriangleFilled(to,
                    { base.x + perpendicular.x, base.y + perpendicular.y },
                    { base.x - perpendicular.x, base.y - perpendicular.y }, color);
            }
        } break;

        case jark::AnnoTool::Pen: {
            std::vector<ImVec2> points;
            points.reserve(active.points.size());
            for (const auto& point : active.points)
                points.push_back(toScreen(point));
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 0, thickness);
        } break;

        case jark::AnnoTool::Mosaic: {
            // 马赛克在松开时才真正生效，这里先给个虚线示意
            const ImVec2 min((std::min)(from.x, to.x), (std::min)(from.y, to.y));
            const ImVec2 max((std::max)(from.x, to.x), (std::max)(from.y, to.y));
            drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 60));
            drawList->AddRect(min, max, color, 0.0f, 0, thickness);
        } break;

        case jark::AnnoTool::Text: {
            if (!textInput_.empty()) {
                const float fontSize = (std::max)(8.0f, active.style.fontSize * viewScale_);
                drawList->AddText(ImGui::GetFont(), fontSize, from, color, textInput_.c_str());
            }
        } break;

        default:
            break;
        }
    }

    void drawSelectionOverlay(ImDrawList* drawList) {
        if (cropSelection_.width <= 0 || cropSelection_.height <= 0)
            return;

        const ImVec2 min = toScreen({ cropSelection_.x, cropSelection_.y });
        const ImVec2 max = toScreen({ cropSelection_.x + cropSelection_.width, cropSelection_.y + cropSelection_.height });

        // 框外压暗
        const ImU32 shade = IM_COL32(0, 0, 0, 130);
        const ImVec2 canvasMin(canvasRect_.x, canvasRect_.y);
        const ImVec2 canvasMax(canvasRect_.x + canvasRect_.z, canvasRect_.y + canvasRect_.w);

        drawList->AddRectFilled(canvasMin, { canvasMax.x, min.y }, shade);
        drawList->AddRectFilled({ canvasMin.x, max.y }, canvasMax, shade);
        drawList->AddRectFilled({ canvasMin.x, min.y }, { min.x, max.y }, shade);
        drawList->AddRectFilled({ max.x, min.y }, { canvasMax.x, max.y }, shade);

        drawList->AddRect(min, max, IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);
    }

    // —— 工具栏 ——

    void drawToolbar(float scale) {
        static const struct {
            const char* icon;
            uint32_t stringId;
            jark::AnnoTool tool;
        } tools[] = {
            // 图标暂用文字占位，后续可换成自绘图标
            { "", kStrRect, jark::AnnoTool::Rect },
            { "", kStrEllipse, jark::AnnoTool::Ellipse },
            { jark::ui::icon::kArrow, kStrArrow, jark::AnnoTool::Arrow },
            { jark::ui::icon::kLine, kStrLine, jark::AnnoTool::Line },
            { "", kStrPen, jark::AnnoTool::Pen },
            { jark::ui::icon::kMosaic, kStrMosaic, jark::AnnoTool::Mosaic },
            { "T", kStrText, jark::AnnoTool::Text },
            { "", kStrCrop, jark::AnnoTool::Crop },
        };

        for (size_t index = 0; index < std::size(tools); ++index) {
            if (index > 0)
                ImGui::SameLine();

            const bool selected = toolIndex_ == static_cast<uint32_t>(index);
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

            const std::string icon = tools[index].icon;
            const std::string label = (icon.empty() ? std::string() : icon + " ") +
                getUIString(tools[index].stringId) + "##tool" + std::to_string(index);
            if (ImGui::Button(label.c_str(), { 96.0f * scale, 0 }))
                toolIndex_ = static_cast<uint32_t>(index);

            if (selected)
                ImGui::PopStyleColor();
        }

        if (ImGui::Button(jark::ui::icon::kUndo, { 44.0f * scale, 0 }))
            undo();
        ImGui::SameLine();
        if (ImGui::Button(jark::ui::icon::kRedo, { 44.0f * scale, 0 }))
            redo();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ui(kStrDone));
    }

    // —— 侧栏 ——

    void drawSidebar(float scale) {
        ImGui::BeginChild("editorSidebar", { 0, 0 }, ImGuiChildFlags_None);

        // 颜色
        ImGui::TextUnformatted(ui(kStrColor));
        for (size_t index = 0; index < std::size(kColors); ++index) {
            if (index > 0)
                ImGui::SameLine();

            ImGui::PushID(static_cast<int>(index));
            const ImVec4 color = toImVec4(kColors[index]);
            if (index == colorIndex_)
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * scale);

            if (ImGui::ColorButton("##swatch", color, ImGuiColorEditFlags_NoTooltip, { 26.0f * scale, 26.0f * scale }))
                colorIndex_ = static_cast<uint32_t>(index);

            if (index == colorIndex_)
                ImGui::PopStyleVar();
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt(ui(kStrWidth), &lineWidth_, 1, 40, "%d");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt(ui(kStrFontSize), &fontSize_, 8, 200, "%d");
        ImGui::Checkbox(ui(kStrFilled), &filled_);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 文字输入
        ImGui::TextUnformatted(ui(kStrTextPrompt));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextMultiline("##editorText", &textInput_, { -1.0f, 60.0f * scale })) {
            if (currentTool() == jark::AnnoTool::Text && textPlaced_) {
                document_.cancel();
                document_.begin(jark::AnnoTool::Text, currentStyle(), textAnchor_);
                document_.setText(textInput_);
            }
        }

        if (ImGui::Button(ui(kStrDone), { -1.0f, 0 })) {
            if (textPlaced_ && !textInput_.empty()) {
                commitTextAt(textInput_, textAnchor_);
                textPlaced_ = false;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 编辑
        ImGui::TextUnformatted(ui(kStrEdit));
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (ImGui::Button(ui(kStrRotate270), { half, 0 }))
            applyTransform(+1);
        ImGui::SameLine();
        if (ImGui::Button(ui(kStrRotate90), { half, 0 }))
            applyTransform(-1);

        if (ImGui::Button("180°", { half, 0 }))
            applyTransform(2);
        ImGui::SameLine();
        if (ImGui::Button(ui(kStrInvert), { half, 0 }))
            invertColors();

        if (ImGui::Button(ui(kStrFlipH), { half, 0 }))
            applyTransform(3);
        ImGui::SameLine();
        if (ImGui::Button(ui(kStrFlipV), { half, 0 }))
            applyTransform(4);

        if (ImGui::Button(ui(kStrApplyCrop), { -1.0f, 0 }))
            applyCrop();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 输出
        if (ImGui::Button(ui(kStrSaveAs), { -1.0f, 0 }))
            saveAs();
        if (ImGui::Button(ui(kStrCopy), { -1.0f, 0 }))
            copyToClipboard();
        if (ImGui::Button(ui(kStrOverwrite), { -1.0f, 0 }))
            overwriteSource();

        ImGui::Spacing();
        if (!statusText_.empty() && nowSeconds() < statusUntil_)
            ImGui::TextWrapped("%s", statusText_.c_str());
        else
            ImGui::TextDisabled("%s", ui(kStrHint));

        ImGui::EndChild();
    }

    void handleShortcuts() {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            return;

        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) saveAs();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) copyToClipboard();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
    }

    // —— 行为 ——

    jark::AnnoTool currentTool() const {
        const jark::AnnoTool tools[] = {
            jark::AnnoTool::Rect, jark::AnnoTool::Ellipse, jark::AnnoTool::Arrow, jark::AnnoTool::Line,
            jark::AnnoTool::Pen, jark::AnnoTool::Mosaic, jark::AnnoTool::Text, jark::AnnoTool::Crop,
        };
        return toolIndex_ < std::size(tools) ? tools[toolIndex_] : jark::AnnoTool::Rect;
    }

    jark::AnnoStyle currentStyle() const {
        jark::AnnoStyle style;
        style.color = kColors[colorIndex_ < std::size(kColors) ? colorIndex_ : 0];
        style.width = (std::max)(1, lineWidth_);
        style.fontSize = (std::max)(8, fontSize_);
        style.filled = filled_;
        return style;
    }

    void rebuildTexture() {
        textureDirty_ = false;

        const cv::Mat& image = document_.committedImage();
        if (image.empty()) {
            canvasTexture_ = 0;
            return;
        }

        // 带透明通道的图先合成到棋盘格上，否则透明区域和画布底色一样，看不出可编辑范围
        if (image.channels() == 4) {
            // 棋盘格约 16 屏幕像素一格（画布固定适应窗口，尺度基本稳定）
            const int gridWidth = std::clamp(static_cast<int>(16.0f / (viewScale_ > 0.0f ? viewScale_ : 1.0f)), 4, 64);
            const uint32_t lightGrid = GlobalVar::currentTheme.WHITE_GRID;
            const uint32_t darkGrid = GlobalVar::currentTheme.BLACK_GRID;

            cv::Mat composed(image.rows, image.cols, CV_8UC3);
            for (int y = 0; y < image.rows; ++y) {
                const uint8_t* source = image.ptr<uint8_t>(y);
                uint8_t* target = composed.ptr<uint8_t>(y);
                for (int x = 0; x < image.cols; ++x) {
                    const uint32_t grid = ((x / gridWidth + y / gridWidth) & 1) ? darkGrid : lightGrid;
                    const int alpha = source[x * 4 + 3];
                    for (int c = 0; c < 3; ++c) {
                        const int gridChannel = (grid >> (c * 8)) & 0xFF; // 0xAARRGGBB -> BGR
                        target[x * 3 + c] = static_cast<uint8_t>(
                            (source[x * 4 + c] * alpha + gridChannel * (255 - alpha) + 255) >> 8);
                    }
                }
            }

            canvasTexture_ = jark::ui::UiHost::instance().textureFromImage(composed, 2);
            return;
        }

        canvasTexture_ = jark::ui::UiHost::instance().textureFromImage(image, 2);
    }

    void undo() {
        document_.undo();
        textureDirty_ = true;
    }

    void redo() {
        document_.redo();
        textureDirty_ = true;
    }

    void resetView() {
        // 画布固定按适应窗口显示，没有视图状态需要重置（保留调用点以便以后加）
    }

    void clearSelection() {
        cropSelection_ = {};
        hasSelection_ = false;
        selecting_ = false;
    }

    void commitTextAt(const std::string& text, const cv::Point& anchor) {
        if (text.empty())
            return;

        document_.cancel();
        document_.begin(jark::AnnoTool::Text, currentStyle(), anchor);
        document_.setText(text);
        document_.commit();
        textureDirty_ = true;
    }

    // 1=逆时针 90  -1=顺时针 90  2=180  3=水平翻转  4=垂直翻转
    void applyTransform(int operation) {
        cv::Mat result;
        const cv::Mat flat = document_.flatten();

        if (operation == 1)
            cv::rotate(flat, result, cv::ROTATE_90_COUNTERCLOCKWISE);
        else if (operation == -1)
            cv::rotate(flat, result, cv::ROTATE_90_CLOCKWISE);
        else if (operation == 2)
            cv::rotate(flat, result, cv::ROTATE_180);
        else
            cv::flip(flat, result, operation == 3 ? 1 : 0);

        document_.applyEdit(result);
        resetView();
        clearSelection();
        textureDirty_ = true;
    }

    void invertColors() {
        const cv::Mat flat = document_.flatten();
        std::vector<cv::Mat> channels;
        cv::split(flat, channels);
        for (size_t i = 0; i < 3 && i < channels.size(); ++i)
            cv::bitwise_not(channels[i], channels[i]);

        cv::Mat result;
        cv::merge(channels, result);
        document_.applyEdit(result);
        textureDirty_ = true;
    }

    void applyCrop() {
        if (!hasSelection_) {
            setStatus(kStrNeedSelection);
            return;
        }

        if (document_.cropTo(cropSelection_)) {
            clearSelection();
            resetView();
            textureDirty_ = true;
            setStatus(kStrDone);
        }
    }

    void saveAs() {
        auto [path, isJpg] = jarkUtils::saveImageDialogW(getUIStringW(kStrSaveAs));
        if (path.size() <= 2)
            return;

        saveTo(path, isJpg ? L"jpg" : L"png");
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
        setStatus(kStrSaved);
        return true;
    }

    void overwriteSource() {
        if (sourcePath_.empty())
            return;

        std::wstring extension = std::filesystem::path(sourcePath_).extension().wstring();
        if (!extension.empty() && extension.front() == L'.')
            extension.erase(extension.begin());
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);

        static const wchar_t* const supported[] = { L"png", L"jpg", L"jpeg", L"webp", L"bmp", L"tif", L"tiff" };
        const bool encodable = std::any_of(std::begin(supported), std::end(supported),
            [&extension](const wchar_t* item) { return extension == item; });

        if (!encodable) {
            setStatus(kStrSaveFailed);
            return;
        }

        if (MessageBoxW(nullptr, getUIStringW(46), getUIStringW(kStrTitle), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;

        if (saveTo(sourcePath_, extension))
            GlobalVar::isNeedReloadImageCache = true;
    }

    void copyToClipboard() {
        jarkUtils::copyImageToClipboard(document_.flatten());
        setStatus(kStrCopied);
    }

    void setStatus(uint32_t stringId) {
        setStatusText(ui(stringId));
        statusUntil_ = nowSeconds() + 3.0;
    }

    void setStatusText(std::string text) {
        statusText_ = std::move(text);
    }

    static double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static const char* ui(uint32_t id) { return getUIString(id); }

    static ImU32 imColor(uint32_t argb) {
        return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
    }

    static ImVec4 toImVec4(uint32_t argb) {
        return { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
            (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
    }

    // 文案 ID（见 stringRes.cpp 图像编辑与标注段）
    static constexpr uint32_t kStrTitle = 91;
    static constexpr uint32_t kStrRect = 93;
    static constexpr uint32_t kStrEllipse = 94;
    static constexpr uint32_t kStrArrow = 95;
    static constexpr uint32_t kStrLine = 96;
    static constexpr uint32_t kStrPen = 97;
    static constexpr uint32_t kStrMosaic = 98;
    static constexpr uint32_t kStrText = 99;
    static constexpr uint32_t kStrCrop = 100;
    static constexpr uint32_t kStrColor = 101;
    static constexpr uint32_t kStrWidth = 102;
    static constexpr uint32_t kStrFontSize = 103;
    static constexpr uint32_t kStrFilled = 104;
    static constexpr uint32_t kStrUndo = 105;
    static constexpr uint32_t kStrRedo = 106;
    static constexpr uint32_t kStrEdit = 107;
    static constexpr uint32_t kStrInvert = 108;
    static constexpr uint32_t kStrSaveAs = 109;
    static constexpr uint32_t kStrCopy = 110;
    static constexpr uint32_t kStrOverwrite = 111;
    static constexpr uint32_t kStrCopied = 112;
    static constexpr uint32_t kStrSaved = 113;
    static constexpr uint32_t kStrSaveFailed = 114;
    static constexpr uint32_t kStrHint = 115;
    static constexpr uint32_t kStrTextPrompt = 116;
    static constexpr uint32_t kStrDone = 117;
    static constexpr uint32_t kStrNeedSelection = 119;
    static constexpr uint32_t kStrApplyCrop = 120;
    static constexpr uint32_t kStrRotate90 = 77;   // 顺时针90°
    static constexpr uint32_t kStrRotate270 = 78;  // 逆时针90°
    static constexpr uint32_t kStrFlipH = 79;
    static constexpr uint32_t kStrFlipV = 80;

    static inline const uint32_t kColors[] = {
        0xFFFF3B30, 0xFFFFCC00, 0xFF34C759, 0xFF0A84FF,
        0xFFFFFFFF, 0xFF000000, 0xFFFF2D95, 0xFFAF52DE,
    };

    bool visible_ = false;
    bool focusRequested_ = false;

    std::wstring sourcePath_;
    jark::AnnotatorDocument document_;
    TextRenderer textRenderer_;

    ImTextureID canvasTexture_ = 0;
    bool textureDirty_ = true;

    // 视图（固定适应窗口，不缩放）
    float fitScale_ = 1.0f;
    float viewScale_ = 1.0f;
    ImVec4 canvasRect_{ 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, w, h
    ImVec4 imageRect_{ 0.0f, 0.0f, 0.0f, 0.0f };  // x, y, w, h

    // 交互
    bool drawingShape_ = false;
    bool selecting_ = false;
    cv::Point cropStart_{};
    cv::Rect cropSelection_{};
    bool hasSelection_ = false;

    bool textPlaced_ = false;
    cv::Point textAnchor_{};
    std::string textInput_;

    uint32_t toolIndex_ = 0;
    uint32_t colorIndex_ = 0;
    int lineWidth_ = 6;
    int fontSize_ = 40;
    bool filled_ = false;

    std::string statusText_;
    double statusUntil_ = 0.0;
};
