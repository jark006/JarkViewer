#pragma once

// 矢量图（SVG）按需光栅化。
//
// 旧实现把 SVG 预渲染成固定 4000px 位图：一枚 32x32 的图标也要占 64MB 内存，
// 而放大超过该分辨率后又依旧模糊。现在只保留矢量文档本身，按当前缩放真正
// 需要的像素尺寸重新光栅化——不放大就不浪费内存，放多大都清晰。

#include <cstdint>
#include <memory>

#include <lunasvg.h>
#include <opencv2/opencv.hpp>

struct ImageAsset; // 定义见 jarkUtils.h

namespace jark {

struct VectorImage {
    std::unique_ptr<lunasvg::Document> document;
    int intrinsicWidth = 0;   // 文档声明的尺寸，作为 100% 缩放的基准
    int intrinsicHeight = 0;
    int rasterWidth = 0;      // 当前位图的分辨率
    int rasterHeight = 0;
    int requestedEdge = 0;    // 上次请求过的长边像素，避免同一目标反复尝试
};

// 光栅化长边上限，兼顾清晰度、内存与耗时
inline constexpr int VECTOR_RASTER_MAX_EDGE = 4096;

// 首次光栅化的长边上限（首帧要快，随后空闲时会自动升到实际需要的分辨率）
inline constexpr int VECTOR_RASTER_INITIAL_EDGE = 1024;

// 重新光栅化的滞后阈值：目标与当前分辨率的比值超出 [1/r, r] 才重新渲染，
// 避免缩放动画过程中反复渲染
inline constexpr double VECTOR_RASTER_HYSTERESIS = 1.25;

// 按指定像素尺寸光栅化矢量文档；失败返回空 Mat
cv::Mat renderVectorImage(const VectorImage& vectorImage, int width, int height);

// 按长边像素光栅化（自动保持文档宽高比）；失败返回空 Mat
cv::Mat renderVectorImageAtEdge(const VectorImage& vectorImage, int longEdge);

// 当前缩放所需的光栅长边像素（按文档尺寸换算并夹取到 VECTOR_RASTER_MAX_EDGE）
int vectorTargetEdge(const ImageAsset& imageAsset, int64_t zoomCur, int64_t zoomBase);

// 需要时重新光栅化并替换 imageAsset.primaryFrame；返回 true 表示已更新
bool refreshVectorRaster(ImageAsset& imageAsset, int targetEdge);

// 首次光栅化的尺寸（长边不超过 VECTOR_RASTER_INITIAL_EDGE）
int initialVectorRasterEdge(const VectorImage& vectorImage);

} // namespace jark
