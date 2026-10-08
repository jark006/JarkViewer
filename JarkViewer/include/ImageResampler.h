#pragma once

// 静止视图的平滑重采样：把当前可视区域按显示分辨率重采样成一块位图。
// 逐帧采样路径（CanvasRenderer）为了速度只用最近邻（缩小时用 2×2 近似），
// 放大看文字必然有锯齿、缩小看细密纹理会有摩尔纹；画面停下来之后把可视区域
// 重采样一次（放大 Lanczos4、缩小面积平均）就能既保住交互流畅、又看得清细节。
// 交互过程中（缩放/平移动画、动图播放）不使用，避免每帧做重采样。

#include "CanvasRenderer.h"

#include <opencv2/opencv.hpp>

namespace jark {

// 重采样结果：一块**已按 rotation 预旋转**的名义空间位图 + 它覆盖的归一化区域。
// 交给 CanvasRenderer 时配 sourcePreRotated=true 与这组 left/top/width/height：
// 采样密度会自动算成 1:1（位图像素与画布像素一一对应）。
struct ResampledView {
    cv::Mat image;
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool valid = false;

    // 当前可视区域是否完全落在块内（含容差）。块是在某个视图状态下算出来的，
    // 视图只动了不到一个像素时可以直接复用。
    bool covers(const CanvasGeometry& geometry) const {
        if (!valid)
            return false;
        constexpr double tolerance = 1e-6;
        return geometry.visible.x >= left - tolerance &&
            geometry.visible.y >= top - tolerance &&
            geometry.visible.x + geometry.visible.width <= left + width + tolerance &&
            geometry.visible.y + geometry.visible.height <= top + height + tolerance;
    }
};

// 该视图状态下重采样是否有意义：缩放不是 1:1（1:1 时最近邻就是精确值）。
bool shouldResample(const ViewState& view);

// source：**未旋转**的整幅位图（8 位，CV_8UC1/3/4）；view：视图参数；
// margin：可视区域外多留的余量比例（便于视图微动时复用）。
// 失败（尺寸不合法、位图类型不支持等）时返回 valid=false。
ResampledView resampleVisibleRegion(const cv::Mat& source, const ViewState& view,
    cv::Size canvasSize, double margin = 0.25);

} // namespace jark
