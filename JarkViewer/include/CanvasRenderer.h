#pragma once

// 画布绘制：把图像按视图参数（缩放 / 平移 / 旋转）绘制到 BGRA 画布上。
// 从 main.cpp 的 JarkViewerApp 中独立出来，便于复用与单独验证。

#include <cstdint>

#include <opencv2/opencv.hpp>

namespace jark {

// 视图参数。imageWidth/Height 是“名义尺寸”（100% 缩放时屏幕上应有的尺寸），
// 与位图实际分辨率无关——矢量图的位图会随缩放变化。
struct ViewState {
    int imageWidth = 0;
    int imageHeight = 0;
    int64_t zoom = 0;
    int64_t zoomBase = 1 << 16;
    int slideX = 0;
    int slideY = 0;
    int rotation = 0; // 0/1/2/3 分别表示 0°、逆时针 90°、180°、顺时针 90°
    bool border = true; // 占位界面（主页/解码失败）不画图像边框

    // 源位图只覆盖名义图像的一块区域时的归一化区域（相对**旋转后**的名义图像）；
    // 默认 (0,0,1,1) 表示整幅。矢量图放大超过全幅光栅化上限后按可视区域出高清块，
    // 由它描述这块位图对应图像里的哪一部分。
    double sourceLeft = 0.0;
    double sourceTop = 0.0;
    double sourceWidth = 1.0;
    double sourceHeight = 1.0;
    // 位图已按 rotation 预旋转（区域光栅化时由渲染矩阵一次完成），采样不再套旋转
    bool sourcePreRotated = false;
};

// 旋转后的图像几何。visible 为归一化可见区域，位图采样分辨率不参与定位。
struct CanvasGeometry {
    cv::Size nominalSize;
    cv::Point origin;
    cv::Size2d renderedSize;
    cv::Rect2d visible;
    double scale = 0.0;
};

CanvasGeometry imageGeometry(const ViewState& view, cv::Size canvasSize, cv::Size fallbackSize = {});
// 鸟瞰定位：将归一化图像点移到客户区中心；完整可见的轴保持居中，不影响普通拖图的边界。
cv::Point navigationSlide(const CanvasGeometry& geometry, cv::Size canvasSize, cv::Point2d center);

// 把 srcImage 绘制到 canvas（canvas 会被填充为背景色并绘制图像边框）
void drawImageToCanvas(const cv::Mat& srcImage, cv::Mat& canvas, const ViewState& view);

} // namespace jark
