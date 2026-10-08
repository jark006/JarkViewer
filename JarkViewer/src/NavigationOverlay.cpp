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
    first_ = (std::max)(0, current - capacity_ / 2);
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
        if (current < first_ || current >= first_ + capacity_)
            first_ = (std::max)(0, current - capacity_ / 2);
        layout();
    }
    updateRequests();
}

void NavigationOverlay::layout() {
    overviewPanel_ = overviewImage_ = viewFrame_ = {};
    strip_ = trigger_ = previous_ = next_ = {};
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
        trigger_ = { left, clientSize_.height - 24.0f * scale_, width, 24.0f * scale_ };
        strip_ = { left, clientSize_.height - height - margin / 2, width, height };
        previous_ = { left, strip_.y, arrow, height };
        next_ = { left + width - arrow, strip_.y, arrow, height };
        capacity_ = std::clamp(static_cast<int>((width - arrow * 2) / (100.0f * scale_)), 1, kMaxVisible);
        first_ = std::clamp(first_, 0, (std::max)(0, static_cast<int>(files_.size()) - capacity_));
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
    for (int n = 0; n < capacity_ && first_ + n < static_cast<int>(files_.size()); ++n)
        if (contains(cellRect(n), point))
            return first_ + n;
    return -1;
}

bool NavigationOverlay::hit(cv::Point point) const {
    return !blocked_ && (contains(overviewPanel_, point) ||
        (stripVisible_ && contains(strip_, point)) || contains(trigger_, point));
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
    const bool visible = contains(trigger_, point) || (stripVisible_ && contains(strip_, point));
    if (stripVisible_ != visible) {
        stripVisible_ = visible;
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
    mouse_ = { -1, -1 };
    if (ownsGesture())
        return false;
    const bool changed = stripVisible_ || hovered_ >= 0;
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
        for (int n = first_; n < static_cast<int>(files_.size()) && n < first_ + capacity_; ++n)
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
    }
    if (!stripVisible_)
        return;
    draw->AddRectFilled(topLeft(strip_), bottomRight(strip_), background, radius);
    draw->AddRect(topLeft(strip_), bottomRight(strip_), border, radius);
    const auto arrow = [&](const cv::Rect2f& rect, const char* glyph, bool available) {
        if (available && contains(rect, mouse_))
            draw->AddRectFilled(topLeft(rect), bottomRight(rect), ImGui::GetColorU32(ImGuiCol_ButtonHovered), radius);
        const ImVec2 size = ImGui::CalcTextSize(glyph);
        draw->AddText(pos(rect.x + (rect.width - size.x) / 2, rect.y + (rect.height - size.y) / 2),
            available ? ImGui::GetColorU32(ImGuiCol_Text) : text, glyph);
    };
    arrow(previous_, icon::kPrev, first_ > 0);
    arrow(next_, icon::kNext, first_ + capacity_ < static_cast<int>(files_.size()));
    for (int n = 0; n < capacity_ && first_ + n < static_cast<int>(files_.size()); ++n) {
        const int index = first_ + n;
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
        draw->AddRectFilled(topLeft(tooltip), bottomRight(tooltip), background, radius);
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
