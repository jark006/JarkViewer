#include "UiFramework.h"

#include "TextRenderer.h"

namespace jark::ui {
namespace {

    constexpr int kCheckBoxSize = 34;
    constexpr int kCheckBoxInset = 8;

    // 追加一个 UTF-8 码点（文本框用）
    void appendUtf8(std::string& text, uint32_t codePoint) {
        if (codePoint < 0x80) {
            text.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800) {
            text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint < 0x10000) {
            text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else {
            text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

} // namespace

// —— UiCanvas ——

void UiCanvas::fill(Rect rect, Color color) {
    const cv::Rect clipped = rect.toCv() & cv::Rect(0, 0, target_.cols, target_.rows);
    if (clipped.width > 0 && clipped.height > 0)
        cv::rectangle(target_, clipped, jarkUtils::to_cv_scalar(color), -1);
}

void UiCanvas::stroke(Rect rect, Color color, int thickness) {
    if (rect.width > 0 && rect.height > 0)
        cv::rectangle(target_, rect.toCv(), jarkUtils::to_cv_scalar(color), thickness);
}

void UiCanvas::line(int x0, int y0, int x1, int y1, Color color) {
    cv::line(target_, { x0, y0 }, { x1, y1 }, jarkUtils::to_cv_scalar(color), 1);
}

void UiCanvas::text(Rect rect, std::string_view text, Color color, Align align) {
    if (text.empty() || rect.width <= 0 || rect.height <= 0)
        return;

    const std::string owned(text);
    const intUnion textColor(color);
    switch (align) {
    case Align::Center: drawer_.putAlignCenter(target_, rect.toCv(), owned.c_str(), textColor); break;
    case Align::Right:  drawer_.putAlignRight(target_, rect.toCv(), owned.c_str(), textColor); break;
    default:            drawer_.putAlignLeft(target_, rect.toCv(), owned.c_str(), textColor); break;
    }
}

void UiCanvas::image(const cv::Mat& source, Rect destination) {
    if (source.empty() || destination.width <= 0 || destination.height <= 0)
        return;

    // 资源图按目标矩形缩放（高 DPI 下窗口是物理像素，直接贴图会显示成一半大小）
    cv::Mat scaled;
    const cv::Mat* drawSource = &source;
    if (source.cols != destination.width || source.rows != destination.height) {
        cv::resize(source, scaled, { destination.width, destination.height }, 0, 0, cv::INTER_AREA);
        drawSource = &scaled;
    }

    jarkUtils::overlayImg(target_, *drawSource, destination.x, destination.y);
}

int UiCanvas::lineHeight() const {
    return 30;
}

int UiCanvas::measureText(std::string_view text) const {
    const std::string owned(text);
    return drawer_.measureText(owned.c_str());
}

// —— Label ——

void Label::draw(UiCanvas& canvas) {
    const Color color = color_ != 0 ? color_ : canvas.theme().FG;
    canvas.text(bounds, text_, color, align_);
}

// —— CheckBox ——

void CheckBox::draw(UiCanvas& canvas) {
    const int boxSize = canvas.dp(kCheckBoxSize);
    const int inset = canvas.dp(kCheckBoxInset);
    const Rect box{ bounds.x + inset, bounds.y + inset, boxSize, boxSize };
    canvas.stroke(box, canvas.theme().FG_DEEP, canvas.dp(4));

    if (value_ && *value_)
        canvas.fill(box.inset(inset), canvas.theme().CHECK);

    const Rect textRect{ bounds.x + bounds.height, bounds.y, bounds.width - bounds.height, bounds.height };
    canvas.text(textRect, text_, canvas.theme().FG);
}

bool CheckBox::onClick(int x, int y) {
    if (!value_ || !bounds.contains(x, y))
        return false;

    *value_ = !*value_;
    return true;
}

// —— RadioGroup ——

int RadioGroup::itemWidth() const {
    const int count = static_cast<int>(options_.size()) + 1; // 首列是标签
    return count > 0 ? bounds.width / count : bounds.width;
}

void RadioGroup::draw(UiCanvas& canvas) {
    if (options_.empty())
        return;

    const int width = itemWidth();
    size_t index = value_ ? *value_ : 0;
    if (index >= options_.size())
        index = 0;

    const int vInset = canvas.dp(4);
    const Rect selected{ bounds.x + width * static_cast<int>(1 + index), bounds.y + vInset, width, bounds.height - vInset * 2 };
    canvas.fill(selected, canvas.theme().CHECK);

    const Rect frame{ bounds.x + width, bounds.y + vInset, bounds.width - width, bounds.height - vInset * 2 };
    canvas.stroke(frame, canvas.theme().FG_DEEP, canvas.dp(2));

    canvas.text({ bounds.x, bounds.y, width, bounds.height }, label_, canvas.theme().FG, Align::Center);

    for (size_t i = 0; i < options_.size(); ++i) {
        const Rect item{ bounds.x + width * static_cast<int>(1 + i), bounds.y, width, bounds.height };
        canvas.text(item, options_[i], canvas.theme().FG, Align::Center);
    }
}

bool RadioGroup::onClick(int x, int y) {
    if (!value_ || options_.empty() || !bounds.contains(x, y))
        return false;

    const int width = itemWidth();
    const int index = (x - bounds.x) / (width > 0 ? width : 1) - 1; // 首列是标签
    if (index < 0 || index >= static_cast<int>(options_.size()) || *value_ == static_cast<uint32_t>(index))
        return false;

    *value_ = static_cast<uint32_t>(index);
    return true;
}

// —— Button ——

void Button::draw(UiCanvas& canvas) {
    // 主按钮用高亮色，普通按钮用按钮底色（FG_DEEP 是“最深的文字色”，深色主题下是浅色，不能当底色）
    canvas.fill(bounds, primary_ ? canvas.theme().CHECK : canvas.theme().BG_BTN);
    canvas.text(bounds, text_, canvas.theme().FG, Align::Center);
}

bool Button::onClick(int x, int y) {
    if (!bounds.contains(x, y) || !action_)
        return false;

    action_();
    return true;
}

// —— TabBar ——

void TabBar::draw(UiCanvas& canvas) {
    if (tabs_.empty())
        return;

    const int width = bounds.width / static_cast<int>(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const Rect item{ bounds.x + width * static_cast<int>(i), bounds.y, width, bounds.height };
        if (index_ && *index_ == static_cast<int>(i))
            canvas.fill(item, canvas.theme().FG_DEEP);

        canvas.text(item, tabs_[i], canvas.theme().FG, Align::Center);
    }
}

bool TabBar::onClick(int x, int y) {
    if (!index_ || tabs_.empty() || !bounds.contains(x, y))
        return false;

    const int width = bounds.width / static_cast<int>(tabs_.size());
    const int tab = (x - bounds.x) / (width > 0 ? width : 1);
    if (tab < 0 || tab >= static_cast<int>(tabs_.size()) || *index_ == tab)
        return false;

    *index_ = tab;
    if (onChanged_)
        onChanged_();
    return true;
}

// —— Slider ——

bool Slider::setFromX(int x) {
    if (!getValue_ || !setValue_)
        return false;

    // 轨道几何在绘制时换算为物理像素；事件坐标也是物理像素
    const int trackWidth = trackWidthPhysical_ > 0 ? trackWidthPhysical_ : 1;
    const int relative = x - (bounds.x + trackXPhysical_);
    int newValue = relative <= 0 ? 0
        : (relative >= trackWidth ? maxValue_ : relative * maxValue_ / trackWidth);

    newValue = std::clamp(newValue, 0, maxValue_);
    if (getValue_() == newValue)
        return false;

    setValue_(newValue);
    return true;
}

void Slider::draw(UiCanvas& canvas) {
    if (!getValue_)
        return;

    const int value = std::clamp(getValue_(), 0, maxValue_);
    const int trackHeight = canvas.dp(30);
    const int trackY = bounds.y + (bounds.height - trackHeight) / 2;
    trackXPhysical_ = canvas.dp(trackX_);
    trackWidthPhysical_ = canvas.dp(trackWidth_);
    const Rect track{ bounds.x + trackXPhysical_, trackY, trackWidthPhysical_, trackHeight };

    canvas.stroke(track, 0xFF0000FF, canvas.dp(2));

    const int filled = track.width * value / maxValue_;
    if (filled > 0) {
        Rect fill = track;
        fill.width = filled;
        canvas.fill(fill, 0xFF3F48CC);
    }

    // 标签在轨道左侧，数值紧随其后（与背景资源图上的标题错开）
    if (!label_.empty())
        canvas.text({ bounds.x, bounds.y, canvas.dp(trackX_ - 20), bounds.height },
            label_, canvas.theme().FG, Align::Left);

    // 数值右对齐贴在轨道左侧，避免与标签重叠
    canvas.text({ bounds.x + canvas.dp(trackX_ - 130), bounds.y, canvas.dp(120), bounds.height },
        suffix_.empty() ? std::format("{}", value) : std::format("{} {}", value, suffix_),
        canvas.theme().FG, Align::Right);
}

bool Slider::onClick(int x, int y) {
    return bounds.contains(x, y) && setFromX(x);
}

bool Slider::onMouseDown(int x, int y) {
    if (!bounds.contains(x, y))
        return false;

    dragging_ = true;
    setFromX(x);
    return true;
}

bool Slider::onMouseMove(int x, int y) {
    if (!dragging_)
        return false;

    (void)y;
    setFromX(x);
    return true;
}

bool Slider::onMouseUp(int x, int y) {
    (void)x; (void)y;
    const bool wasDragging = dragging_;
    dragging_ = false;
    return wasDragging;
}

bool Slider::onWheel(int x, int y, int delta) {
    if (!getValue_ || !setValue_ || !bounds.contains(x, y))
        return false;

    const int step = delta > 0 ? 1 : -1;
    const int newValue = std::clamp(getValue_() + step, 0, maxValue_);
    if (newValue == getValue_())
        return false;

    setValue_(newValue);
    return true;
}

// —— ImageRadioGroup ——

void ImageRadioGroup::draw(UiCanvas& canvas) {
    if (images_.empty() || !getValue_)
        return;

    int index = getValue_();
    if (index < 0 || index >= static_cast<int>(images_.size()))
        index = 0;

    cv::Mat inverted;
    const auto prepare = [&](const cv::Mat& source) -> const cv::Mat& {
        if (!invertInDarkMode_ || source.channels() != 4)
            return source;

        std::vector<cv::Mat> channels(4);
        cv::split(source, channels);
        for (int i = 0; i < 3; ++i)
            channels[i] = 255 - channels[i];
        cv::merge(channels, inverted);
        return inverted;
    };

    if (drawSelectedOnly_) {
        canvas.image(prepare(images_[index]), bounds);
        return;
    }

    const int itemWidth = bounds.width / static_cast<int>(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        const Rect item{ bounds.x + itemWidth * static_cast<int>(i), bounds.y, itemWidth, bounds.height };
        canvas.image(prepare(images_[i]), item);
    }
}

bool ImageRadioGroup::onClick(int x, int y) {
    if (!bounds.contains(x, y) || images_.empty() || !setValue_)
        return false;

    const int itemWidth = bounds.width / static_cast<int>(images_.size());
    const int index = (x - bounds.x) / (itemWidth > 0 ? itemWidth : 1);
    if (index < 0 || index >= static_cast<int>(images_.size()))
        return false;
    if (getValue_ && getValue_() == index)
        return false;

    setValue_(index);
    return true;
}

// —— ProgressBar ——

void ProgressBar::draw(UiCanvas& canvas) {
    if (!value_)
        return;

    canvas.stroke(bounds, canvas.theme().FG_DEEP, canvas.dp(2));

    const int value = std::clamp(*value_, 0, maxValue_);
    const int filled = bounds.width * value / maxValue_;
    if (filled > 0) {
        Rect fill = bounds.inset(canvas.dp(2));
        fill.width = filled - canvas.dp(4);
        if (fill.width > 0)
            canvas.fill(fill, canvas.theme().CHECK);
    }

    canvas.text(bounds, std::format("{} / {}", value, maxValue_), canvas.theme().FG, Align::Center);
}

// —— TextBox ——

void TextBox::draw(UiCanvas& canvas) {
    canvas.stroke(bounds, focused() ? canvas.theme().CHECK : canvas.theme().FG_DEEP, canvas.dp(2));

    const Rect textRect = bounds.inset(canvas.dp(10));
    if (text_ && !text_->empty()) {
        canvas.text(textRect, *text_, canvas.theme().FG);
    }
    else {
        canvas.text(textRect, placeholder_, canvas.theme().FG_DEEP);
    }

    if (focused_) {
        // 光标画在文本末尾
        const int width = text_ ? canvas.measureText(*text_) : 0;
        const int cursorX = (std::min)(textRect.right() - canvas.dp(2), textRect.x + width + canvas.dp(2));
        canvas.fill({ cursorX, textRect.y + canvas.dp(4), canvas.dp(2), textRect.height - canvas.dp(8) },
            canvas.theme().FG);
    }
}

bool TextBox::onClick(int x, int y) {
    if (!bounds.contains(x, y)) {
        focused_ = false;
        return false;
    }

    focused_ = true;
    return true;
}

bool TextBox::onKeyChar(wchar_t character) {
    if (!focused_ || !text_)
        return false;

    if (character == L'\b') {
        if (text_->empty())
            return false;

        // 删除最后一个 UTF-8 码点（连同它的续字节）
        while (!text_->empty()) {
            const char removed = text_->back();
            text_->pop_back();
            if ((static_cast<uint8_t>(removed) & 0xC0) != 0x80)
                break;
        }
        return true;
    }

    // 控制字符不进入文本；其余（含中文）按 UTF-8 追加
    if (character < 32 || character == 127)
        return false;

    appendUtf8(*text_, static_cast<uint32_t>(character));
    return true;
}

bool TextBox::onKeyDown(int virtualKey) {
    if (!focused_)
        return false;

    if (virtualKey == VK_ESCAPE) {
        focused_ = false;
        return true;
    }
    return false;
}

// —— CheckList ——

size_t CheckList::checkedCount() const {
    if (!isChecked_)
        return 0;

    size_t count = 0;
    for (const auto& item : items_) {
        if (isChecked_(item))
            ++count;
    }
    return count;
}

void CheckList::draw(UiCanvas& canvas) {
    const int rowHeight = canvas.dp(40);
    rowHeightPixels = rowHeight;
    visibleRows_ = rowHeight > 0 ? bounds.height / rowHeight : 0;
    scrollOffset_ = std::clamp(scrollOffset_, 0, (std::max)(0, (int)items_.size() - visibleRows_));

    canvas.fill(bounds, canvas.theme().BG_DEEP);

    for (int row = 0; row < visibleRows_; ++row) {
        const int index = scrollOffset_ + row;
        if (index >= (int)items_.size())
            break;

        const Rect item{ bounds.x, bounds.y + row * rowHeight, bounds.width, rowHeight };
        const int boxSize = (std::min)(rowHeight - canvas.dp(12), canvas.dp(26));
        const Rect box{ item.x + canvas.dp(8), item.y + (rowHeight - boxSize) / 2, boxSize, boxSize };

        canvas.stroke(box, canvas.theme().FG_DEEP, canvas.dp(2));
        if (isChecked_ && isChecked_(items_[index]))
            canvas.fill(box.inset(canvas.dp(4)), canvas.theme().CHECK);

        canvas.text({ box.right() + canvas.dp(8), item.y, item.width - boxSize - canvas.dp(20), rowHeight },
            items_[index], canvas.theme().FG);
    }
}

bool CheckList::onClick(int x, int y) {
    if (!bounds.contains(x, y) || !setChecked_ || rowHeightPixels <= 0)
        return false;

    const int row = (y - bounds.y) / rowHeightPixels;
    const int index = scrollOffset_ + row;
    if (index < 0 || index >= (int)items_.size())
        return false;

    setChecked_(items_[index], !(isChecked_ && isChecked_(items_[index])));
    return true;
}

bool CheckList::onWheel(int x, int y, int delta) {
    if (!bounds.contains(x, y) || rowHeightPixels <= 0)
        return false;

    const int before = scrollOffset_;
    scrollOffset_ += delta > 0 ? -3 : 3;
    scrollOffset_ = std::clamp(scrollOffset_, 0, (std::max)(0, (int)items_.size() - visibleRows_));
    return scrollOffset_ != before;
}

// —— OptionGrid ——

Rect OptionGrid::itemRect(int index) const {
    if (columns_ <= 0 || items_.empty())
        return {};

    const int rows = (static_cast<int>(items_.size()) + columns_ - 1) / columns_;
    const int itemWidth = bounds.width / columns_;
    const int itemHeight = rows > 0 ? bounds.height / rows : 0;
    return { bounds.x + itemWidth * (index % columns_),
             bounds.y + itemHeight * (index / columns_),
             itemWidth, itemHeight };
}

void OptionGrid::draw(UiCanvas& canvas) {
    const int gap = canvas.dp(4);
    const size_t selected = value_ ? *value_ : 0;

    for (size_t i = 0; i < items_.size(); ++i) {
        const Rect item = itemRect(static_cast<int>(i));
        if (item.height <= gap * 2)
            continue;

        const Rect cell = item.inset(gap);
        canvas.fill(cell, i == selected ? canvas.theme().CHECK : canvas.theme().BG_BTN);
        canvas.text(cell, items_[i], canvas.theme().FG, Align::Center);
    }
}

bool OptionGrid::onClick(int x, int y) {
    if (!value_ || !bounds.contains(x, y))
        return false;

    for (size_t i = 0; i < items_.size(); ++i) {
        if (itemRect(static_cast<int>(i)).contains(x, y)) {
            if (*value_ == static_cast<uint32_t>(i))
                return false;

            *value_ = static_cast<uint32_t>(i);
            return true;
        }
    }
    return false;
}

// —— ColorRow ——

Rect ColorRow::itemRect(int index) const {
    const int count = static_cast<int>(colors_.size());
    if (count <= 0)
        return {};

    const int itemWidth = bounds.width / count;
    return { bounds.x + itemWidth * index, bounds.y, itemWidth, bounds.height };
}

Color ColorRow::selectedColor() const {
    if (!value_ || colors_.empty())
        return 0xFFFF3B30;

    const size_t index = (std::min)(static_cast<size_t>(*value_), colors_.size() - 1);
    return colors_[index];
}

void ColorRow::draw(UiCanvas& canvas) {
    const size_t selected = value_ ? *value_ : 0;

    for (size_t i = 0; i < colors_.size(); ++i) {
        const Rect item = itemRect(static_cast<int>(i));
        const int gap = canvas.dp(4);
        const Rect cell = item.inset(gap);

        canvas.fill(cell, colors_[i]);
        if (i == selected)
            canvas.stroke(item, canvas.theme().FG, canvas.dp(3));
    }
}

bool ColorRow::onClick(int x, int y) {
    if (!value_ || !bounds.contains(x, y))
        return false;

    for (size_t i = 0; i < colors_.size(); ++i) {
        if (itemRect(static_cast<int>(i)).contains(x, y)) {
            if (*value_ == static_cast<uint32_t>(i))
                return false;

            *value_ = static_cast<uint32_t>(i);
            return true;
        }
    }
    return false;
}

// —— HotArea ——

bool HotArea::onClick(int x, int y) {
    if (!bounds.contains(x, y) || !action_)
        return false;

    action_();
    return true;
}

// —— ImageView ——

void ImageView::draw(UiCanvas& canvas) {
    if (image_ && !image_->empty())
        canvas.image(*image_, bounds);
}

// —— CheckGrid ——

Rect CheckGrid::itemRect(int index) const {
    if (columns_ <= 0 || items_.empty())
        return {};

    const int rows = (static_cast<int>(items_.size()) + columns_ - 1) / columns_;
    const int itemWidth = bounds.width / columns_;
    const int itemHeight = rows > 0 ? bounds.height / rows : 0;
    return { bounds.x + itemWidth * (index % columns_),
             bounds.y + itemHeight * (index / columns_),
             itemWidth, itemHeight };
}

void CheckGrid::draw(UiCanvas& canvas) {
    for (size_t i = 0; i < items_.size(); ++i) {
        const Rect item = itemRect(static_cast<int>(i));
        if (item.height <= 0 || item.y + item.height > bounds.bottom())
            break;

        const int margin = canvas.dp(8);
        const int boxSize = (std::min)(item.height - margin, canvas.dp(28));
        if (boxSize <= 0)
            continue;

        const Rect box{ item.x + canvas.dp(4), item.y + (item.height - boxSize) / 2, boxSize, boxSize };
        canvas.stroke(box, canvas.theme().FG_DEEP, canvas.dp(3));

        if (isChecked_ && isChecked_(items_[i]))
            canvas.fill(box.inset(canvas.dp(5)), canvas.theme().CHECK);

        canvas.text({ box.right() + canvas.dp(6), item.y, item.width - boxSize - canvas.dp(12), item.height },
            items_[i], canvas.theme().FG);
    }
}

bool CheckGrid::onClick(int x, int y) {
    if (!bounds.contains(x, y) || !setChecked_)
        return false;

    for (size_t i = 0; i < items_.size(); ++i) {
        const Rect item = itemRect(static_cast<int>(i));
        if (item.height <= 0 || item.y + item.height > bounds.bottom())
            break;

        if (item.contains(x, y)) {
            setChecked_(items_[i], !(isChecked_ && isChecked_(items_[i])));
            return true;
        }
    }
    return false;
}

// —— Row ——

Control* Row::add(ControlPtr control, int weight) {
    if (!control)
        return nullptr;

    Control* raw = control.get();
    totalWeight += weight > 0 ? weight : 1;
    entries_.push_back(Entry{ std::move(control), weight > 0 ? weight : 1 });
    return raw;
}

void Row::draw(UiCanvas& canvas) {
    if (entries_.empty() || totalWeight <= 0)
        return;

    int x = bounds.x;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const int width = (i + 1 == entries_.size())
            ? bounds.right() - x
            : bounds.width * entries_[i].weight / totalWeight;
        entries_[i].control->bounds = { x, bounds.y, width, bounds.height };
        x += width;

        if (entries_[i].control->visible)
            entries_[i].control->draw(canvas);
    }
}

bool Row::onClick(int x, int y) {
    for (auto& entry : entries_) {
        Control* control = entry.control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onClick(x, y))
            return true;
    }
    return false;
}

Control* Row::controlAt(int x, int y) {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Control* control = it->control.get();
        if (control->visible && control->enabled && control->bounds.contains(x, y))
            return control;
    }
    return nullptr;
}

bool Row::onMouseDown(int x, int y) {
    Control* control = controlAt(x, y);
    if (!control)
        return false;

    if (control->onMouseDown(x, y)) {
        capturedControl = control;
        return true;
    }
    return control->bounds.contains(x, y);
}

bool Row::onMouseMove(int x, int y) {
    if (capturedControl) {
        capturedControl->onMouseMove(x, y);
        return true;
    }

    for (auto& entry : entries_) {
        Control* control = entry.control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onMouseMove(x, y))
            return true;
    }
    return false;
}

bool Row::onMouseUp(int x, int y) {
    if (capturedControl) {
        capturedControl->onMouseUp(x, y);
        capturedControl = nullptr;
        return true;
    }

    // 未捕获拖动的控件（按钮等）在抬起时视为一次点击
    return onClick(x, y);
}

bool Row::onWheel(int x, int y, int delta) {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Control* control = it->control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onWheel(x, y, delta))
            return true;
    }
    return false;
}

// —— Panel ——

Control* Panel::add(ControlPtr control, int height, int gap) {
    if (!control)
        return nullptr;

    Control* raw = control.get();
    contentHeight += gap + (height > 0 ? height : control->preferredHeight());
    entries_.push_back(Entry{ std::move(control), height, gap, {} });
    return raw;
}

Control* Panel::overlay(ControlPtr control, Rect logicalBounds) {
    if (!control)
        return nullptr;

    Control* raw = control.get();
    Entry entry{ std::move(control), 0, 0, logicalBounds };
    entry.control->bounds = logicalBounds;
    overlays_.push_back(std::move(entry));
    return raw;
}

void Panel::draw(UiCanvas& canvas) {
    int y = bounds.y;
    for (auto& entry : entries_) {
        // 不可见控件不占位：按任务类型切换参数区时不会留下空洞
        if (!entry.control->visible)
            continue;

        y += canvas.dp(entry.gap);
        const int height = canvas.dp(entry.height > 0 ? entry.height : entry.control->preferredHeight());
        entry.control->bounds = { bounds.x, y, bounds.width, height };
        y += height;
        entry.control->draw(canvas);
    }

    for (auto& entry : overlays_) {
        // 始终用保存的逻辑坐标换算，重复绘制不会累积缩放
        entry.control->bounds = { bounds.x + canvas.dp(entry.logical.x),
                                  bounds.y + canvas.dp(entry.logical.y),
                                  canvas.dp(entry.logical.width),
                                  canvas.dp(entry.logical.height) };
        if (entry.control->visible)
            entry.control->draw(canvas);
    }
}

Control* Panel::controlAt(int x, int y) {
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        Control* control = it->control.get();
        if (control->visible && control->enabled && control->bounds.contains(x, y))
            return control;
    }

    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Control* control = it->control.get();
        if (control->visible && control->enabled && control->bounds.contains(x, y))
            return control;
    }
    return nullptr;
}

Control* Panel::find(int x, int y) {
    for (auto& entry : entries_) {
        if (entry.control->visible && entry.control->enabled && entry.control->bounds.contains(x, y))
            return entry.control.get();
    }
    return nullptr;
}

bool Panel::onClick(int x, int y) {
    // 覆盖层优先（后加入者在上）；只有真正处理了点击的控件才消费事件，
    // 纯装饰控件（标签、图片）不阻塞其下方的按钮
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
        Control* control = it->control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onClick(x, y))
            return true;
    }

    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Control* control = it->control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onClick(x, y))
            return true;
    }
    return false;
}

bool Panel::onMouseDown(int x, int y) {
    Control* control = controlAt(x, y);
    if (!control)
        return false;

    if (control->onMouseDown(x, y)) {
        capturedControl = control;
        return true;
    }
    return control->bounds.contains(x, y);
}

bool Panel::onMouseMove(int x, int y) {
    if (capturedControl) {
        capturedControl->onMouseMove(x, y);
        return true;
    }
    return false;
}

bool Panel::onMouseUp(int x, int y) {
    if (capturedControl) {
        // 拖动结束：值已在按下/移动时更新
        capturedControl->onMouseUp(x, y);
        capturedControl = nullptr;
        return true;
    }

    // 未捕获拖动的控件（按钮、复选、单选等）在抬起时视为一次点击
    return onClick(x, y);
}

bool Panel::onWheel(int x, int y, int delta) {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        Control* control = it->control.get();
        if (!control->visible || !control->enabled || !control->bounds.contains(x, y))
            continue;

        if (control->onWheel(x, y, delta))
            return true;
    }
    return false;
}

} // namespace jark::ui
