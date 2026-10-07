#include "VectorImage.h"

#include "jarkUtils.h"

#include <algorithm>
#include <cmath>

namespace jark {
namespace {

    // 按长边像素换算位图宽高，保持文档宽高比
    void rasterSizeFor(const VectorImage& vectorImage, int longEdge, int& width, int& height) {
        const int intrinsicLongEdge = (std::max)(vectorImage.intrinsicWidth, vectorImage.intrinsicHeight);
        if (intrinsicLongEdge <= 0) {
            width = height = 0;
            return;
        }

        if (vectorImage.intrinsicWidth >= vectorImage.intrinsicHeight) {
            width = longEdge;
            height = static_cast<int>(std::llround(
                static_cast<double>(longEdge) * vectorImage.intrinsicHeight / vectorImage.intrinsicWidth));
        }
        else {
            height = longEdge;
            width = static_cast<int>(std::llround(
                static_cast<double>(longEdge) * vectorImage.intrinsicWidth / vectorImage.intrinsicHeight));
        }

        width = (std::max)(1, width);
        height = (std::max)(1, height);
    }

} // namespace

cv::Mat renderVectorImage(const VectorImage& vectorImage, int width, int height) {
    if (!vectorImage.document || width <= 0 || height <= 0)
        return {};

    auto bitmap = vectorImage.document->renderToBitmap(width, height);
    if (bitmap.isNull())
        return {};

    return cv::Mat(height, width, CV_8UC4, bitmap.data(), bitmap.stride()).clone();
}

cv::Mat renderVectorImageAtEdge(const VectorImage& vectorImage, int longEdge) {
    int width = 0;
    int height = 0;
    rasterSizeFor(vectorImage, longEdge, width, height);
    return renderVectorImage(vectorImage, width, height);
}

int initialVectorRasterEdge(const VectorImage& vectorImage) {
    const int intrinsicLongEdge = (std::max)(vectorImage.intrinsicWidth, vectorImage.intrinsicHeight);
    if (intrinsicLongEdge <= 0)
        return 0;

    return (std::min)(intrinsicLongEdge, VECTOR_RASTER_INITIAL_EDGE);
}

int vectorTargetEdge(const ImageAsset& imageAsset, int64_t zoomCur, int64_t zoomBase) {
    const auto& vectorImage = imageAsset.vectorSource;
    if (!vectorImage || zoomBase <= 0)
        return 0;

    const int intrinsicLongEdge = (std::max)(vectorImage->intrinsicWidth, vectorImage->intrinsicHeight);
    if (intrinsicLongEdge <= 0)
        return 0;

    const double scale = static_cast<double>(zoomCur) / static_cast<double>(zoomBase);
    const double pixels = static_cast<double>(intrinsicLongEdge) * scale;
    if (!std::isfinite(pixels) || pixels <= 0.0)
        return 0;

    return std::clamp(static_cast<int>(std::llround(pixels)), 1, VECTOR_RASTER_MAX_EDGE);
}

bool refreshVectorRaster(ImageAsset& imageAsset, int targetEdge) {
    const auto& vectorImage = imageAsset.vectorSource;
    if (!vectorImage || targetEdge <= 0)
        return false;

    // 同一目标只尝试一次：渲染失败也不要在空闲循环里空转
    if (vectorImage->requestedEdge == targetEdge)
        return false;

    const int currentEdge = (std::max)(vectorImage->rasterWidth, vectorImage->rasterHeight);
    if (currentEdge > 0) {
        const double ratio = static_cast<double>(targetEdge) / static_cast<double>(currentEdge);
        if (ratio <= VECTOR_RASTER_HYSTERESIS && ratio >= 1.0 / VECTOR_RASTER_HYSTERESIS)
            return false; // 当前分辨率已经够用
    }

    vectorImage->requestedEdge = targetEdge;

    auto raster = renderVectorImageAtEdge(*vectorImage, targetEdge);
    if (raster.empty())
        return false;

    vectorImage->rasterWidth = raster.cols;
    vectorImage->rasterHeight = raster.rows;

    // 位图分辨率变化不影响宽高比，绘制端按自身缩放比例缩放即可
    imageAsset.primaryFrame = std::move(raster);
    return true;
}

} // namespace jark
