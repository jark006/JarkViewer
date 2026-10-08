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

    // lunasvg 的 Bitmap 是 ARGB32 预乘格式（内存布局 B,G,R,A，颜色分量各自已乘过 alpha）；
    // 而画布合成（CanvasRenderer 的 getSrcPx4）按直通 alpha 计算 bg*(255-a)+src*a，
    // 不反预乘的话半透明区域会被再乘一次 alpha（fill-opacity/opacity 区域整体偏暗）。
    // 这里手工反预乘：既能修好偏暗，又保持 B,G,R,A 字节序——直接调 bitmap.convertToRGBA()
    // 会把字节序换成 R,G,B,A，而画布把第一个字节当 B 读，会导致红蓝互换。
    void unpremultiplyInPlace(cv::Mat& bgra) {
        for (int y = 0; y < bgra.rows; ++y) {
            uint8_t* px = bgra.ptr<uint8_t>(y);
            for (int x = 0; x < bgra.cols; ++x, px += 4) {
                const int alpha = px[3];
                if (alpha == 0) {
                    px[0] = px[1] = px[2] = 0;
                    continue;
                }
                if (alpha == 255)
                    continue;
                for (int channel = 0; channel < 3; ++channel)
                    px[channel] = static_cast<uint8_t>((std::min)(255, (px[channel] * 255 + alpha / 2) / alpha));
            }
        }
    }

} // namespace

cv::Mat renderVectorImage(const VectorImage& vectorImage, int width, int height) {
    if (!vectorImage.document || width <= 0 || height <= 0)
        return {};

    auto bitmap = vectorImage.document->renderToBitmap(width, height);
    if (bitmap.isNull())
        return {};

    cv::Mat raster = cv::Mat(height, width, CV_8UC4, bitmap.data(), bitmap.stride()).clone();
    unpremultiplyInPlace(raster);
    return raster;
}

cv::Mat renderVectorImageAtEdge(const VectorImage& vectorImage, int longEdge) {
    int width = 0;
    int height = 0;
    rasterSizeFor(vectorImage, longEdge, width, height);
    return renderVectorImage(vectorImage, width, height);
}

cv::Mat renderVectorImageRegion(const VectorImage& vectorImage, const cv::Rect2d& nominalRect,
    int rotation, int width, int height) {
    if (!vectorImage.document || width <= 0 || height <= 0 ||
        nominalRect.width < 1.0 || nominalRect.height < 1.0)
        return {};

    // 名义空间就是文档空间按 rotation 旋转后的结果；把"文档坐标 -> 名义坐标"的仿射
    // 系数写进渲染矩阵，再叠加"名义坐标 -> 输出位图"的缩放与平移，
    // 这样一张位图就是可视区域的高清块，无需再整体旋转。
    const double docW = vectorImage.intrinsicWidth;
    const double docH = vectorImage.intrinsicHeight;
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
    switch (rotation & 3) {
    case 1: a = 0.0; b = -1.0; c = 1.0; d = 0.0; e = 0.0; f = docW; break;
    case 2: a = -1.0; b = 0.0; c = 0.0; d = -1.0; e = docW; f = docH; break;
    case 3: a = 0.0; b = 1.0; c = -1.0; d = 0.0; e = docH; f = 0.0; break;
    default: break;
    }

    const double scaleX = width / nominalRect.width;
    const double scaleY = height / nominalRect.height;

    lunasvg::Bitmap bitmap(width, height);
    if (bitmap.isNull())
        return {};

    vectorImage.document->render(bitmap, lunasvg::Matrix(
        static_cast<float>(scaleX * a), static_cast<float>(scaleY * b),
        static_cast<float>(scaleX * c), static_cast<float>(scaleY * d),
        static_cast<float>(scaleX * (e - nominalRect.x)), static_cast<float>(scaleY * (f - nominalRect.y))));

    cv::Mat raster = cv::Mat(height, width, CV_8UC4, bitmap.data(), bitmap.stride()).clone();
    unpremultiplyInPlace(raster);
    return raster;
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
