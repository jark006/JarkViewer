#include "NavigationOverlay.h"

#include "ThumbnailService.h"
#include "jarkUtils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace jark::ui {

bool NavigationOverlay::contains(const cv::Rect2f& rect, cv::Point point) {
    return rect.width > 0 && rect.height > 0 && point.x >= rect.x && point.y >= rect.y &&
        point.x < rect.x + rect.width && point.y < rect.y + rect.height;
}

void NavigationOverlay::setDirectory(const std::vector<std::wstring>& files, int current) {
    cancel();
    files_ = files;
    current_ = current;
    first_ = current - (capacity_ - 1) / 2; // 当前图片居中（layout 会按新容量夹取）
    requested_.clear();
    overviewDirty_ = true;
    for (auto& texture : textures_)
        texture.version = 0;
    layout();
}

void NavigationOverlay::sync(const ViewState& view, cv::Size clientSize, float scale, bool enabled,
    bool blocked, uint64_t imageVersion, int current) {
    const uint64_t clearVersion = ThumbnailService::instance().stats().clearVersion;
    if (clearVersion_ != clearVersion) {
        clearVersion_ = clearVersion;
        releaseTextures();
    }
    if (imageVersion_ != imageVersion || view_.rotation != view.rotation ||
        view_.imageWidth != view.imageWidth || view_.imageHeight != view.imageHeight ||
        clientSize_ != clientSize || scale_ != scale || enabled_ != enabled) {
        dragging_ = false;
        overviewDirty_ = true;
    }
    imageVersion_ = imageVersion;
    view_ = view;
    clientSize_ = clientSize;
    scale_ = scale;
    enabled_ = enabled;
    blocked_ = blocked || clientSize.empty() || view.imageWidth <= 0 || view.imageHeight <= 0;
    geometry_ = imageGeometry(view_, clientSize_);
    if (blocked_)
        cancel();
    layout();
    if (current_ != current) {
        current_ = current;
        first_ = current - (capacity_ - 1) / 2; // 当前图片变化时始终回到控件正中
        layout();
        hovered_ = itemAt(mouse_); // 重新居中后同一屏幕位置对应的单元已变，刷新悬停项
    }
    updateRequests();
}

void NavigationOverlay::layout() {
    overviewPanel_ = overviewImage_ = viewFrame_ = {};
    overviewClose_ = {};
    strip_ = previous_ = next_ = {};
    if (blocked_)
        return;
    const float margin = 10.0f * scale_;
    // 与原来的两侧热区保持一致，打印/设置入口始终可以到达。
    const float edge = clientSize_.width >= 500.0f * scale_ ? 50.0f * scale_ : clientSize_.width / 4.0f;
    const float width = (std::max)(0.0f, clientSize_.width - 2.0f * edge - margin);
    const float left = (clientSize_.width - width) / 2.0f;
    const float arrow = 26.0f * scale_;
    const float height = 112.0f * scale_;
    if (!files_.empty() && width > arrow * 2 + 24.0f * scale_) {
        // strip_ 就是触发区：鼠标进入整块预览带区域（而不是最底部一条窄边）即展开
        strip_ = { left, clientSize_.height - height - margin / 2, width, height };
        previous_ = { left, strip_.y, arrow, height };
        next_ = { left + width - arrow, strip_.y, arrow, height };
        capacity_ = std::clamp(static_cast<int>((width - arrow * 2) / (100.0f * scale_)), 1, kMaxVisible);
        if (capacity_ > 1 && capacity_ % 2 == 0)
            capacity_--; // 取奇数，保证有正中间那一格（当前图片严格居中）
        // 胶片带式排布：允许两侧留空——first_ 可以为负，(0-(capacity-1)/2) 恰能把当前图片放到正中
        const int centerSpan = (capacity_ - 1) / 2;
        firstMin_ = -centerSpan;
        firstMax_ = (std::max)(firstMin_, static_cast<int>(files_.size()) - 1 - centerSpan);
        first_ = std::clamp(first_, firstMin_, firstMax_);
    }
    else {
        capacity_ = 0;
        stripVisible_ = false;
    }
    if (!enabled_ || geometry_.scale <= 0.0)
        return;
    const float panelWidth = (std::min)(200.0f * scale_, width);
    const float bottom = stripVisible_ ? strip_.y - margin : clientSize_.height - margin;
    const float panelHeight = (std::min)(144.0f * scale_, bottom - margin);
    if (panelWidth <= margin * 2 || panelHeight <= margin * 2)
        return;
    overviewPanel_ = { left + width - panelWidth, bottom - panelHeight, panelWidth, panelHeight };
    const float fit = (std::min)((panelWidth - margin * 2) / geometry_.nominalSize.width,
        (panelHeight - margin * 2) / geometry_.nominalSize.height);
    const float iw = geometry_.nominalSize.width * fit;
    const float ih = geometry_.nominalSize.height * fit;
    overviewImage_ = { overviewPanel_.x + (panelWidth - iw) / 2,
        overviewPanel_.y + (panelHeight - ih) / 2, iw, ih };
    const auto& visible = geometry_.visible;
    viewFrame_ = { overviewImage_.x + static_cast<float>(visible.x) * iw,
        overviewImage_.y + static_cast<float>(visible.y) * ih,
        static_cast<float>(visible.width) * iw, static_cast<float>(visible.height) * ih };
    // 面板右上角的收起按钮：点击后隐藏鸟瞰（等同取消设置里的「显示鸟瞰图」勾选）
    const float closeSize = 18.0f * scale_;
    overviewClose_ = { overviewPanel_.x + panelWidth - closeSize - 4.0f * scale_,
        overviewPanel_.y + 4.0f * scale_, closeSize, closeSize };
}

cv::Rect2f NavigationOverlay::cellRect(int visibleIndex) const {
    if (capacity_ <= 0)
        return {};
    const float width = (next_.x - previous_.x - previous_.width) / capacity_;
    return { previous_.x + previous_.width + visibleIndex * width, strip_.y + 6.0f * scale_,
        width, strip_.height - 12.0f * scale_ };
}

int NavigationOverlay::itemAt(cv::Point point) const {
    if (!stripVisible_)
        return -1;
    for (int n = 0; n < capacity_; ++n) {
        const int index = first_ + n;
        if (index < 0 || index >= static_cast<int>(files_.size()))
            continue; // 留空的位置不可点
        if (contains(cellRect(n), point))
            return index;
    }
    return -1;
}

bool NavigationOverlay::hit(cv::Point point) const {
    return !blocked_ && (contains(overviewPanel_, point) || contains(strip_, point));
}

cv::Point NavigationOverlay::slideAt(cv::Point point) const {
    if (overviewImage_.width <= 0 || overviewImage_.height <= 0)
        return {};
    const cv::Point2d center((point.x - overviewImage_.x) / overviewImage_.width - grabOffset_.x,
        (point.y - overviewImage_.y) / overviewImage_.height - grabOffset_.y);
    return navigationSlide(geometry_, clientSize_, center);
}

NavigationOverlay::Event NavigationOverlay::mouseMove(cv::Point point, bool canvasDragging) {
    Event event;
    const bool moved = mouse_ != point;
    mouse_ = point;
    if (blocked_ || canvasDragging)
        return event;
    if (dragging_) {
        event.handled = event.redraw = true;
        event.slide = slideAt(point);
        return event;
    }
    // 鼠标在鸟瞰面板上时不切换预览带（否则面板会被顶上去，难以操作）；面板区域优先级更高
    const bool overPanel = contains(overviewPanel_, point);
    const bool visible = overPanel ? stripVisible_ : contains(strip_, point);
    if (stripVisible_ != visible) {
        stripVisible_ = visible;
        // 展开时把当前图片放到控件水平正中（两侧图片不足就留空；手动滚动后不会被抢回去）
        if (visible)
            first_ = current_ - (capacity_ - 1) / 2;
        layout();
        updateRequests();
        event.redraw = true;
    }
    const int hovered = itemAt(point);
    event.handled = hit(point) || ownsGesture();
    event.redraw |= hovered_ != hovered || (event.handled && moved);
    hovered_ = hovered;
    return event;
}

NavigationOverlay::Event NavigationOverlay::mouseDown(cv::Point point, unsigned button) {
    auto event = mouseMove(point, false);
    swallowedButtons_ &= ~button;
    if (!hit(point))
        return event;
    event.handled = event.redraw = true;
    ownedButtons_ |= button;
    if (button != 1)
        return event;
    if (contains(overviewClose_, point)) {
        event.closeNavigator = true; // 按下即收起（不进入拖动；抬起仍由浮层收尾）
        return event;
    }
    if (stripVisible_ && contains(strip_, point)) {
        if (contains(previous_, point))
            first_ -= (std::max)(1, capacity_ - 1);
        else if (contains(next_, point))
            first_ += (std::max)(1, capacity_ - 1);
        else
            event.selected = itemAt(point);
        layout();
        updateRequests();
    }
    else if (contains(overviewImage_, point)) {
        grabOffset_ = {};
        if (contains(viewFrame_, point)) {
            grabOffset_.x = (point.x - overviewImage_.x) / overviewImage_.width -
                (geometry_.visible.x + geometry_.visible.width / 2);
            grabOffset_.y = (point.y - overviewImage_.y) / overviewImage_.height -
                (geometry_.visible.y + geometry_.visible.height / 2);
        }
        dragging_ = true;
        event.slide = slideAt(point);
    }
    return event;
}

NavigationOverlay::Event NavigationOverlay::mouseUp(unsigned button) {
    Event event;
    event.handled = ((ownedButtons_ | swallowedButtons_) & button) != 0;
    event.redraw = event.handled;
    ownedButtons_ &= ~button;
    swallowedButtons_ &= ~button;
    if (button == 1)
        dragging_ = false;
    return event;
}

NavigationOverlay::Event NavigationOverlay::mouseWheel(cv::Point point, int delta) {
    auto event = mouseMove(point, false);
    if (!event.handled)
        return event;
    if (stripVisible_ && contains(strip_, point)) {
        wheelRemainder_ += delta;
        first_ -= wheelRemainder_ / WHEEL_DELTA * 3;
        wheelRemainder_ %= WHEEL_DELTA;
        layout();
        hovered_ = itemAt(point);
        updateRequests();
        event.redraw = true;
    }
    return event;
}

bool NavigationOverlay::mouseLeave() {
    // 鼠标移出客户区后 ✕ 的悬停底色要擦掉：mouse_ 马上会被清掉，先把状态记下来
    const bool wasOverClose = contains(overviewClose_, mouse_);
    mouse_ = { -1, -1 };
    if (ownsGesture())
        return false;
    const bool changed = stripVisible_ || hovered_ >= 0 || wasOverClose;
    stripVisible_ = false;
    hovered_ = -1;
    layout();
    updateRequests();
    return changed;
}

bool NavigationOverlay::cancel() {
    const bool changed = stripVisible_ || dragging_ || ownedButtons_ != 0;
    swallowedButtons_ |= ownedButtons_;
    ownedButtons_ = 0;
    dragging_ = false;
    stripVisible_ = false;
    hovered_ = -1;
    updateRequests();
    return changed;
}

void NavigationOverlay::updateRequests() {
    std::vector<std::wstring> visible;
    std::vector<std::wstring> prefetch;
    if (!blocked_ && stripVisible_) {
        // first_ 可以为负（两侧留空），只请求真正存在的项
        for (int n = (std::max)(0, first_); n < static_cast<int>(files_.size()) && n < first_ + capacity_; ++n)
            visible.push_back(files_[n]);
        for (int n = (std::max)(0, first_ - 2); n < (std::min)(static_cast<int>(files_.size()), first_ + capacity_ + 2); ++n)
            if (n < first_ || n >= first_ + capacity_)
                prefetch.push_back(files_[n]);
    }
    if (visible != requested_) {
        requested_ = visible;
        ThumbnailService::instance().updateRequests(visible, prefetch);
    }
}

void NavigationOverlay::rebuildOverview(const cv::Mat& source) {
    if (source.empty() || overviewImage_.empty())
        return;
    // 只采样已解码图像；先缩小后旋转，避免为了鸟瞰复制整幅大图。
    const int width = (std::max)(1, static_cast<int>(std::round(overviewImage_.width)));
    const int height = (std::max)(1, static_cast<int>(std::round(overviewImage_.height)));
    cv::Mat preview;
    cv::resize(source, preview, view_.rotation & 1 ? cv::Size(height, width) : cv::Size(width, height),
        0, 0, cv::INTER_LINEAR);
    if (view_.rotation == 1)
        cv::rotate(preview, preview, cv::ROTATE_90_COUNTERCLOCKWISE);
    else if (view_.rotation == 2)
        cv::rotate(preview, preview, cv::ROTATE_180);
    else if (view_.rotation == 3)
        cv::rotate(preview, preview, cv::ROTATE_90_CLOCKWISE);
    overview_ = UiHost::instance().textureFromImage(preview, kOverviewSlot);
    overviewDirty_ = overview_ == 0;
}

void NavigationOverlay::draw(const cv::Mat& source, ImVec2 screenOrigin) {
    if (blocked_)
        return;
    auto* draw = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    const auto pos = [&](float x, float y) { return ImVec2(screenOrigin.x + x, screenOrigin.y + y); };
    const auto topLeft = [&](const cv::Rect2f& rect) { return pos(rect.x, rect.y); };
    const auto bottomRight = [&](const cv::Rect2f& rect) { return pos(rect.x + rect.width, rect.y + rect.height); };
    const ImU32 background = ImGui::GetColorU32(ImGuiCol_PopupBg);
    // 底部预览带（及其文件名浮签）改用半透明底：悬停时能透出后面的图像，
    // 与「加载中」「实况」浮标同一档透明度；鸟瞰面板保持不透明
    const ImU32 stripBackground = ImGui::GetColorU32(ImGuiCol_PopupBg, 0.82f);
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float radius = 5.0f * scale_;
    if (dark_ != GlobalVar::isCurrentUIDarkMode) {
        dark_ = GlobalVar::isCurrentUIDarkMode;
        overviewDirty_ = true;
    }
    if (!overviewImage_.empty()) {
        if (overviewDirty_)
            rebuildOverview(source);
        draw->AddRectFilled(topLeft(overviewPanel_), bottomRight(overviewPanel_), background, radius);
        draw->AddRect(topLeft(overviewPanel_), bottomRight(overviewPanel_), border, radius);
        // 小图透明区域沿用主图棋盘格，视口框深浅双色确保可见。
        const float grid = 8.0f * scale_;
        draw->PushClipRect(topLeft(overviewImage_), bottomRight(overviewImage_), true);
        for (int y = 0; y * grid < overviewImage_.height; ++y)
            for (int x = 0; x * grid < overviewImage_.width; ++x) {
                const uint32_t color = ((x + y) & 1) ? GlobalVar::currentTheme.BLACK_GRID : GlobalVar::currentTheme.WHITE_GRID;
                const ImU32 rgba = IM_COL32((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
                draw->AddRectFilled(pos(overviewImage_.x + x * grid, overviewImage_.y + y * grid),
                    pos(overviewImage_.x + (x + 1) * grid, overviewImage_.y + (y + 1) * grid), rgba);
            }
        if (overview_)
            draw->AddImage(overview_, topLeft(overviewImage_), bottomRight(overviewImage_));
        if (!viewFrame_.empty()) {
            draw->AddRect(topLeft(viewFrame_), bottomRight(viewFrame_), IM_COL32(0, 0, 0, 200), 0, 0, 4.0f * scale_);
            draw->AddRect(topLeft(viewFrame_), bottomRight(viewFrame_), IM_COL32(255, 255, 255, 255), 0, 0, 1.5f * scale_);
        }
        draw->PopClipRect();
        // 右上角收起按钮：常态只有一枚灰 ✕（尽量不抢画面），悬停时垫按钮底色并加亮
        if (!overviewClose_.empty()) {
            const bool closeHovered = contains(overviewClose_, mouse_);
            if (closeHovered)
                draw->AddRectFilled(topLeft(overviewClose_), bottomRight(overviewClose_),
                    ImGui::GetColorU32(ImGuiCol_ButtonHovered), radius);
            const float inset = 5.0f * scale_;
            const ImU32 closeColor = closeHovered ? ImGui::GetColorU32(ImGuiCol_Text) : text;
            draw->AddLine(pos(overviewClose_.x + inset, overviewClose_.y + inset),
                pos(overviewClose_.x + overviewClose_.width - inset,
                    overviewClose_.y + overviewClose_.height - inset),
                closeColor, 1.5f * scale_);
            draw->AddLine(pos(overviewClose_.x + overviewClose_.width - inset, overviewClose_.y + inset),
                pos(overviewClose_.x + inset, overviewClose_.y + overviewClose_.height - inset),
                closeColor, 1.5f * scale_);
        }
    }
    if (!stripVisible_)
        return;
    draw->AddRectFilled(topLeft(strip_), bottomRight(strip_), stripBackground, radius);
    draw->AddRect(topLeft(strip_), bottomRight(strip_), border, radius);
    const auto arrow = [&](const cv::Rect2f& rect, const char* glyph, bool available) {
        if (available && contains(rect, mouse_))
            draw->AddRectFilled(topLeft(rect), bottomRight(rect), ImGui::GetColorU32(ImGuiCol_ButtonHovered), radius);
        const ImVec2 size = ImGui::CalcTextSize(glyph);
        draw->AddText(pos(rect.x + (rect.width - size.x) / 2, rect.y + (rect.height - size.y) / 2),
            available ? ImGui::GetColorU32(ImGuiCol_Text) : text, glyph);
    };
    arrow(previous_, icon::kPrev, first_ > firstMin_);
    arrow(next_, icon::kNext, first_ < firstMax_);
    for (int n = 0; n < capacity_; ++n) {
        const int index = first_ + n;
        if (index < 0 || index >= static_cast<int>(files_.size())) {
            // 该位置没有图片：留空（顺带释放可能残留的槽纹理）
            if (textures_[n].id) {
                UiHost::instance().releaseTexture(kThumbSlot + n);
                textures_[n] = {};
            }
            continue;
        }
        auto rect = cellRect(n);
        rect.x += 3.0f * scale_;
        rect.width -= 6.0f * scale_;
        if (index == hovered_)
            draw->AddRectFilled(topLeft(rect), bottomRight(rect), ImGui::GetColorU32(ImGuiCol_ButtonHovered), radius);
        auto& texture = textures_[n];
        const auto thumbnail = ThumbnailService::instance().get(files_[index]);
        if (!thumbnail.image.empty()) {
            if (texture.path != files_[index] || texture.version != thumbnail.version || !texture.id) {
                texture.id = UiHost::instance().textureFromImage(thumbnail.image, kThumbSlot + n);
                texture.path = files_[index];
                texture.version = thumbnail.version;
            }
            const float fit = (std::min)((rect.width - 8 * scale_) / thumbnail.image.cols,
                (rect.height - 8 * scale_) / thumbnail.image.rows);
            const float iw = thumbnail.image.cols * fit;
            const float ih = thumbnail.image.rows * fit;
            const ImVec2 p0 = pos(rect.x + (rect.width - iw) / 2, rect.y + (rect.height - ih) / 2);
            if (texture.id)
                draw->AddImage(texture.id, p0, { p0.x + iw, p0.y + ih });
        }
        else {
            const char* glyph = thumbnail.failed ? icon::kPhoto.c_str() : "...";
            const ImVec2 size = ImGui::CalcTextSize(glyph);
            draw->AddText(pos(rect.x + (rect.width - size.x) / 2, rect.y + (rect.height - size.y) / 2), text, glyph);
        }
        if (index == current_)
            draw->AddRect(topLeft(rect), bottomRight(rect), accent, radius, 0, 2.0f * scale_);
    }
    // 释放本帧不再引用的尾部槽，清理缓存时也不会残留整排旧图。
    for (int n = capacity_; n < kMaxVisible; ++n) {
        if (textures_[n].id) {
            UiHost::instance().releaseTexture(kThumbSlot + n);
            textures_[n] = {};
        }
    }
    if (hovered_ >= 0 && hovered_ < static_cast<int>(files_.size())) {
        const std::string name = jarkUtils::wstringToUtf8(std::filesystem::path(files_[hovered_]).filename().wstring());
        const ImVec2 size = ImGui::CalcTextSize(name.c_str());
        const float width = (std::min)(size.x + 16 * scale_, strip_.width);
        const float x = std::clamp(static_cast<float>(mouse_.x) - width / 2, strip_.x, strip_.x + strip_.width - width);
        const cv::Rect2f tooltip(x, strip_.y - size.y - 12 * scale_, width, size.y + 8 * scale_);
        draw->AddRectFilled(topLeft(tooltip), bottomRight(tooltip), stripBackground, radius);
        draw->PushClipRect(topLeft(tooltip), bottomRight(tooltip), true);
        draw->AddText(pos(x + 8 * scale_, tooltip.y + 4 * scale_), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        draw->PopClipRect();
    }
}

void NavigationOverlay::releaseTextures() {
    UiHost::instance().releaseTexture(kOverviewSlot);
    overview_ = 0;
    overviewDirty_ = true;
    for (int n = 0; n < kMaxVisible; ++n) {
        UiHost::instance().releaseTexture(kThumbSlot + n);
        textures_[n] = {};
    }
}

} // namespace jark::ui
