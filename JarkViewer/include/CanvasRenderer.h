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
};

// 把 srcImage 绘制到 canvas（canvas 会被填充为背景色并绘制图像边框）
void drawImageToCanvas(const cv::Mat& srcImage, cv::Mat& canvas, const ViewState& view);

} // namespace jark
