#pragma once

#include "CanvasRenderer.h"
#include "UiHost.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace jark::ui {

// 主界面的局部浮层：只画图和提交导航意图，不负责读取原图或切换 ImageDatabase。
class NavigationOverlay {
public:
    struct Event {
        bool handled = false;
        bool redraw = false;
        int selected = -1;
        std::optional<cv::Point> slide;
    };

    void setDirectory(const std::vector<std::wstring>& files, int current);
    void sync(const ViewState& view, cv::Size clientSize, float scale, bool enabled,
        bool blocked, uint64_t imageVersion, int current);
    void draw(const cv::Mat& source, ImVec2 screenOrigin);
    Event mouseMove(cv::Point point, bool canvasDragging);
    Event mouseDown(cv::Point point, unsigned button);
    Event mouseUp(unsigned button);
    Event mouseWheel(cv::Point point, int delta);
    bool mouseLeave();
    bool cancel();
    bool ownsGesture() const { return ownedButtons_ != 0; }
    void releaseTextures();

private:
    void layout();
    void updateRequests();
    cv::Rect2f cellRect(int visibleIndex) const;
    int itemAt(cv::Point point) const;
    bool hit(cv::Point point) const;
    cv::Point slideAt(cv::Point point) const;
    void rebuildOverview(const cv::Mat& source);
    static bool contains(const cv::Rect2f& rect, cv::Point point);

    static constexpr int kOverviewSlot = 5;
    static constexpr int kThumbSlot = 16;
    static constexpr int kMaxVisible = 64;
    struct Texture {
        std::wstring path;
        uint64_t version = 0;
        ImTextureID id = 0;
    };
    std::array<Texture, kMaxVisible> textures_;
    ImTextureID overview_ = 0;
    bool overviewDirty_ = true;
    bool dark_ = false;
    uint64_t imageVersion_ = 0;
    uint64_t clearVersion_ = 0;
    ViewState view_;
    CanvasGeometry geometry_;
    cv::Size clientSize_;
    float scale_ = 1.0f;
    bool enabled_ = true;
    bool blocked_ = true;
    bool stripVisible_ = false;
    bool dragging_ = false;
    unsigned ownedButtons_ = 0;
    unsigned swallowedButtons_ = 0;
    cv::Point mouse_{ -1, -1 };
    cv::Point2d grabOffset_;
    cv::Rect2f overviewPanel_, overviewImage_, viewFrame_;
    cv::Rect2f strip_, previous_, next_; // strip_ 同时是展开触发区（鼠标进入即展开）
    int capacity_ = 0;
    int first_ = 0;
    int current_ = -1;
    int hovered_ = -1;
    int wheelRemainder_ = 0;
    std::vector<std::wstring> files_;
    std::vector<std::wstring> requested_;
};

} // namespace jark::ui
