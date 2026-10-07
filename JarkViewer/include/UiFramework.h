#pragma once

// 轻量 UI 框架：画布 + 控件树 + 竖直堆叠布局。
//
// 背景：设置/打印窗口原先用硬编码像素矩形 + 手工命中判断 + 每个标签页一个绘制函数，
// 增加一个控件要同时改布局、绘制、命中三处，且坐标靠人肉对齐。
// 这里改为「控件自己负责绘制与命中」，容器按顺序自动排布，新增控件只需 new 一个对象。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/opencv.hpp>

#include "jarkUtils.h"

class TextRenderer;

namespace jark::ui {

    using Color = uint32_t; // 0xAARRGGBB，与主题色一致

    struct Rect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        int right() const { return x + width; }
        int bottom() const { return y + height; }
        bool contains(int px, int py) const {
            return px >= x && px < right() && py >= y && py < bottom();
        }
        cv::Rect toCv() const { return { x, y, width, height }; }
        Rect inset(int delta) const { return { x + delta, y + delta, width - 2 * delta, height - 2 * delta }; }
    };

    enum class Align { Left, Center, Right };

    // 控件绘制时面对的接口：不直接操作 cv::Mat，避免各处重复写主题色与文字绘制
    class UiCanvas {
    public:
        UiCanvas(cv::Mat& target, TextRenderer& drawer, const ThemeColor& theme, float scale = 1.0f)
            : target_(target), drawer_(drawer), theme_(theme), scale_(scale > 0.0f ? scale : 1.0f) {}

        // 逻辑像素 -> 物理像素：控件内部尺寸一律用 dp() 表示
        float scale() const { return scale_; }
        int dp(int logical) const { return static_cast<int>(std::lround(logical * scale_)); }

        int width() const { return target_.cols; }
        int height() const { return target_.rows; }
        const ThemeColor& theme() const { return theme_; }
        cv::Mat& raw() { return target_; }

        void fill(Rect rect, Color color);
        void stroke(Rect rect, Color color, int thickness = 2);
        void line(int x0, int y0, int x1, int y1, Color color);
        void text(Rect rect, std::string_view text, Color color, Align align = Align::Left);
        void image(const cv::Mat& source, Rect destination);

        // 一行文字的建议高度（布局用）
        int lineHeight() const;

        // 文本像素宽度（等宽字体模型，与绘制一致）
        int measureText(std::string_view text) const;

    private:
        cv::Mat& target_;
        TextRenderer& drawer_;
        const ThemeColor& theme_;
        float scale_ = 1.0f;
    };

    class Control {
    public:
        virtual ~Control() = default;

        Rect bounds;
        bool visible = true;
        bool enabled = true;

        virtual void draw(UiCanvas& canvas) = 0;

        // 返回 true 表示状态有变化、需要重绘
        virtual bool onClick(int x, int y) { (void)x; (void)y; return false; }

        // 拖动类控件：按下返回 true 表示开始捕获鼠标，之后的移动/抬起都发给它
        virtual bool onMouseDown(int x, int y) { (void)x; (void)y; return false; }
        virtual bool onMouseMove(int x, int y) { (void)x; (void)y; return false; }
        virtual bool onMouseUp(int x, int y) { (void)x; (void)y; return false; }

        virtual bool onWheel(int x, int y, int delta) { (void)x; (void)y; (void)delta; return false; }

        virtual int preferredHeight() const { return 50; }

        // 是否吃掉这次点击（用于阻止穿透到下层控件）
        virtual bool hitTest(int x, int y) const { return visible && enabled && bounds.contains(x, y); }
    };

    using ControlPtr = std::unique_ptr<Control>;

    // —— 具体控件 ——

    class Label : public Control {
    public:
        Label(std::string text, Color color = 0, Align align = Align::Left)
            : text_(std::move(text)), color_(color), align_(align) {}

        void setText(std::string text) { text_ = std::move(text); }
        void draw(UiCanvas& canvas) override;

    private:
        std::string text_;
        Color color_;
        Align align_;
    };

    class CheckBox : public Control {
    public:
        CheckBox(std::string text, bool* value) : text_(std::move(text)), value_(value) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        std::string text_;
        bool* value_ = nullptr;
    };

    // 一行「标签 + 若干选项」，选项由容器均分宽度（与原设置页样式一致）
    class RadioGroup : public Control {
    public:
        RadioGroup(std::string label, std::vector<std::string> options, uint32_t* value)
            : label_(std::move(label)), options_(std::move(options)), value_(value) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        int itemWidth() const;
        int labelWidth() const;

        std::string label_;
        std::vector<std::string> options_;
        uint32_t* value_ = nullptr;
    };

    class Button : public Control {
    public:
        Button(std::string text, std::function<void()> action, bool primary = false)
            : text_(std::move(text)), action_(std::move(action)), primary_(primary) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        std::string text_;
        std::function<void()> action_;
        bool primary_ = false;
    };

    class TabBar : public Control {
    public:
        TabBar(std::vector<std::string> tabs, int* index, std::function<void()> onChanged)
            : tabs_(std::move(tabs)), index_(index), onChanged_(std::move(onChanged)) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        std::vector<std::string> tabs_;
        int* index_ = nullptr;
        std::function<void()> onChanged_;
    };

    // 拖动条：轨道位置由 trackX/trackWidth 指定（与资源图对齐），支持点击、拖动与滚轮
    // suffix 为空时只显示数值（线宽/字号这类非百分比场景）
    class Slider : public Control {
    public:
        Slider(std::string label, int* value, int maxValue, int trackX, int trackWidth,
            std::string suffix = "%")
            : label_(std::move(label)), maxValue_(maxValue > 0 ? maxValue : 1),
            trackX_(trackX), trackWidth_(trackWidth > 0 ? trackWidth : 1), suffix_(std::move(suffix)),
            getValue_([value]() { return *value; }),
            setValue_([value](int newValue) { *value = newValue; }) {
        }

        // 设置项里的间隔/分辨率等是 uint32_t，单独给个重载免得各处手工转换
        Slider(std::string label, uint32_t* value, int maxValue, int trackX, int trackWidth,
            std::string suffix = "%")
            : label_(std::move(label)), maxValue_(maxValue > 0 ? maxValue : 1),
            trackX_(trackX), trackWidth_(trackWidth > 0 ? trackWidth : 1), suffix_(std::move(suffix)),
            getValue_([value]() { return static_cast<int>(*value); }),
            setValue_([value](int newValue) { *value = static_cast<uint32_t>(newValue); }) {
        }

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;
        bool onMouseDown(int x, int y) override;
        bool onMouseMove(int x, int y) override;
        bool onMouseUp(int x, int y) override;
        bool onWheel(int x, int y, int delta) override;

    private:
        bool setFromX(int x);

        std::string label_;
        std::string suffix_ = "%";
        std::function<int()> getValue_;
        std::function<void(int)> setValue_;
        int maxValue_ = 100;
        int trackX_ = 0;          // 逻辑像素：轨道起点（相对控件）
        int trackWidth_ = 100;    // 逻辑像素：轨道宽度
        int trackXPhysical_ = 0;  // 绘制时换算出的物理轨道几何，事件处理使用
        int trackWidthPhysical_ = 0;
        bool dragging_ = false;
    };

    // 图片单选组：选项外观来自资源图；drawSelectedOnly 时只画选中项（如打印窗口的模式按钮）
    class ImageRadioGroup : public Control {
    public:
        ImageRadioGroup(std::vector<cv::Mat> images, std::function<int()> getValue,
            std::function<void(int)> setValue, bool invertInDarkMode = false, bool drawSelectedOnly = false)
            : images_(std::move(images)), getValue_(std::move(getValue)), setValue_(std::move(setValue)),
            invertInDarkMode_(invertInDarkMode), drawSelectedOnly_(drawSelectedOnly) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        std::vector<cv::Mat> images_;
        std::function<int()> getValue_;
        std::function<void(int)> setValue_;
        bool invertInDarkMode_ = false;
        bool drawSelectedOnly_ = false;
    };

    // 进度条：setValue 后由界面重绘
    class ProgressBar : public Control {
    public:
        ProgressBar(int* value, int maxValue) : value_(value), maxValue_(maxValue > 0 ? maxValue : 1) {}

        void draw(UiCanvas& canvas) override;

    private:
        int* value_ = nullptr;
        int maxValue_ = 100;
    };

    // 单行文本框：点选聚焦，内容按 UTF-8 保存（可输入中文，前提是窗口收到对应 WM_CHAR/WM_IME_CHAR）
    class TextBox : public Control {
    public:
        TextBox(std::string* text, std::string placeholder = {})
            : text_(text), placeholder_(std::move(placeholder)) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;
        bool onKeyChar(wchar_t character);
        bool onKeyDown(int virtualKey);
        bool focused() const { return focused_; }
        void setFocused(bool focused) { focused_ = focused; }

    private:
        std::string* text_ = nullptr;
        std::string placeholder_;
        bool focused_ = false;
    };

    // 可滚动复选列表：单列显示长文件名，滚轮滚动
    class CheckList : public Control {
    public:
        using IsChecked = std::function<bool(const std::string&)>;
        using SetChecked = std::function<void(const std::string&, bool)>;

        CheckList(std::vector<std::string> items, IsChecked isChecked, SetChecked setChecked)
            : items_(std::move(items)), isChecked_(std::move(isChecked)), setChecked_(std::move(setChecked)) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;
        bool onWheel(int x, int y, int delta) override;

        size_t checkedCount() const;

    private:
        int rowHeightPixels = 0;
        int scrollOffset_ = 0;   // 首行索引
        int visibleRows_ = 0;

        std::vector<std::string> items_;
        IsChecked isChecked_;
        SetChecked setChecked_;
    };

    // 隐形热区：用于图片上已经画好按钮外观的场景（帮助/关于页的链接）
    class HotArea : public Control {
    public:
        explicit HotArea(std::function<void()> action) : action_(std::move(action)) {}
        void draw(UiCanvas&) override {}
        bool onClick(int x, int y) override;
        int preferredHeight() const override { return 0; }

    private:
        std::function<void()> action_;
    };

    // 整幅图像（帮助/关于页）
    class ImageView : public Control {
    public:
        explicit ImageView(const cv::Mat* image) : image_(image) {}
        void draw(UiCanvas& canvas) override;

    private:
        const cv::Mat* image_ = nullptr;
    };

    // 扩展名网格（文件关联页）：等宽多列复选
    class CheckGrid : public Control {
    public:
        using IsChecked = std::function<bool(const std::string&)>;
        using SetChecked = std::function<void(const std::string&, bool)>;

        CheckGrid(std::vector<std::string> items, int columns, IsChecked isChecked, SetChecked setChecked)
            : items_(std::move(items)), columns_(columns),
            isChecked_(std::move(isChecked)), setChecked_(std::move(setChecked)) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        Rect itemRect(int index) const;

        std::vector<std::string> items_;
        int columns_ = 1;
        IsChecked isChecked_;
        SetChecked setChecked_;
    };

    // 单选网格：多列等宽按钮，用于工具栏这类「多选一」（与 CheckGrid 同布局）
    class OptionGrid : public Control {
    public:
        OptionGrid(std::vector<std::string> items, int columns, uint32_t* value)
            : items_(std::move(items)), columns_(columns > 0 ? columns : 1), value_(value) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

    private:
        Rect itemRect(int index) const;

        std::vector<std::string> items_;
        int columns_ = 1;
        uint32_t* value_ = nullptr;
    };

    // 颜色选择行：等宽色块，选中项加描边（值存选中下标）
    class ColorRow : public Control {
    public:
        ColorRow(std::vector<Color> colors, uint32_t* value)
            : colors_(std::move(colors)), value_(value) {}

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;

        Color selectedColor() const;

    private:
        Rect itemRect(int index) const;

        std::vector<Color> colors_;
        uint32_t* value_ = nullptr;
    };

    // 水平排布容器：等分宽度（用于按钮行等），事件按命中转发给子控件
    class Row : public Control {
    public:
        Control* add(ControlPtr control, int weight = 1);
        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;
        bool onMouseDown(int x, int y) override;
        bool onMouseMove(int x, int y) override;
        bool onMouseUp(int x, int y) override;
        bool onWheel(int x, int y, int delta) override;

        Control* controlAt(int x, int y);

        // 拖动中的控件：按下时捕获，移动/抬起都发给它
        Control* capturedControl = nullptr;

    private:
        struct Entry {
            ControlPtr control;
            int weight = 1;
        };
        std::vector<Entry> entries_;
        int totalWeight = 0;
    };

    // 竖直堆叠容器：按加入顺序自上而下排布（高度/间距为逻辑像素，绘制时按缩放换算）
    class Panel : public Control {
    public:
        // 返回控件裸指针，便于调用方按任务/状态切换 visible 等属性（所有权仍在容器）
        Control* add(ControlPtr control, int height = 0, int gap = 0);

        // 绝对定位（坐标相对 Panel 左上角、逻辑像素），用于图片上叠加的热区/文字
        Control* overlay(ControlPtr control, Rect logicalBounds);

        void draw(UiCanvas& canvas) override;
        bool onClick(int x, int y) override;
        bool onMouseDown(int x, int y) override;
        bool onMouseMove(int x, int y) override;
        bool onMouseUp(int x, int y) override;
        bool onWheel(int x, int y, int delta) override;

        // 事件路由：命中坐标处最上层的控件
        Control* controlAt(int x, int y);
        Control* find(int x, int y);

        // 拖动中的控件：按下时捕获，移动/抬起都发给它
        Control* capturedControl = nullptr;

    private:
        struct Entry {
            ControlPtr control;
            int height = 0;
            int gap = 0;
            Rect logical; // 覆盖层用：始终保存逻辑坐标，避免重绘时被重复换算
        };
        std::vector<Entry> entries_;
        std::vector<Entry> overlays_;
        int contentHeight = 0;
    };

} // namespace jark::ui
