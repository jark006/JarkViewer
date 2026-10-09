#include "ImageResampler.h"

#include <algorithm>
#include <cmath>

namespace jark {
namespace {

    // 名义空间（旋转后）的归一化点上取源位图坐标：与 CanvasRenderer 的采样约定一致
    // （rot 1 = 逆时针 90°，2 = 180°，3 = 顺时针 90°）
    struct PointMapper {
        int rotation = 0;
        double srcCols = 0.0;
        double srcRows = 0.0;

        cv::Point2d operator()(double nx, double ny) const {
            double ux = nx, uy = ny;
            switch (rotation & 3) {
            case 1: ux = 1.0 - ny; uy = nx; break;
            case 2: ux = 1.0 - nx; uy = 1.0 - ny; break;
            case 3: ux = ny; uy = 1.0 - nx; break;
            default: break;
            }
            return { ux * srcCols, uy * srcRows };
        }
    };

    cv::Mat rotateToNominal(const cv::Mat& image, int rotation) {
        cv::Mat out;
        switch (rotation & 3) {
        case 1: cv::rotate(image, out, cv::ROTATE_90_COUNTERCLOCKWISE); break;
        case 2: cv::rotate(image, out, cv::ROTATE_180); break;
        case 3: cv::rotate(image, out, cv::ROTATE_90_CLOCKWISE); break;
        default: out = image; break;
        }
        return out;
    }

} // namespace

bool shouldResample(const ViewState& view) {
    if (view.zoom <= 0 || view.zoomBase <= 0 || view.imageWidth <= 0 || view.imageHeight <= 0)
        return false;
    return view.zoom != view.zoomBase; // 1:1 时最近邻采到的就是原像素，没有重采样的余地
}

ResampledView resampleVisibleRegion(const cv::Mat& source, const ViewState& view,
    cv::Size canvasSize, double margin) {
    ResampledView result;
    if (source.empty() || source.depth() != CV_8U || canvasSize.empty())
        return result;
    if (!shouldResample(view))
        return result;

    const CanvasGeometry geometry = imageGeometry(view, canvasSize, source.size());
    if (geometry.scale <= 0.0 || geometry.nominalSize.empty())
        return result;

    const double scale = geometry.scale; // 画布像素 / 名义像素
    const int nominalW = geometry.nominalSize.width;  // 旋转后的名义尺寸
    const int nominalH = geometry.nominalSize.height;

    const double visibleW = geometry.visible.width * nominalW;
    const double visibleH = geometry.visible.height * nominalH;
    if (visibleW < 1.0 || visibleH < 1.0)
        return result;

    // 可视区域外加一圈余量，视图只挪一点时不用重算
    double regionX = geometry.visible.x * nominalW - visibleW * margin;
    double regionY = geometry.visible.y * nominalH - visibleH * margin;
    double regionW = visibleW * (1.0 + 2.0 * margin);
    double regionH = visibleH * (1.0 + 2.0 * margin);
    if (regionX < 0.0) { regionW += regionX; regionX = 0.0; }
    if (regionY < 0.0) { regionH += regionY; regionY = 0.0; }
    regionW = (std::min)(regionW, nominalW - regionX);
    regionH = (std::min)(regionH, nominalH - regionY);
    if (regionW < 2.0 || regionH < 2.0)
        return result;

    // 块的原点取整一次并回填成归一化区域：采样端算出的画布原点必须与这里同一处
    // （sourceLeft * nominalW * scale 恰好等于 blockX）
    const int blockX = static_cast<int>(std::llround(regionX * scale));
    const int blockY = static_cast<int>(std::llround(regionY * scale));
    const int blockW = static_cast<int>(std::llround((regionX + regionW) * scale)) - blockX;
    const int blockH = static_cast<int>(std::llround((regionY + regionH) * scale)) - blockY;
    if (blockW < 2 || blockH < 2)
        return result;

    const int srcCols = source.cols;
    const int srcRows = source.rows;
    const int rotation = view.rotation & 3;
    const PointMapper toSource{ rotation, static_cast<double>(srcCols), static_cast<double>(srcRows) };
    const double invScale = 1.0 / scale;

    cv::Mat block;
    if (scale >= 1.0) {
        // 放大：仿射采样，每个输出像素精确映射回源（不累积缩放误差），插值用 Lanczos4
        const double ex = blockX * invScale / nominalW; // 块原点在名义空间里的归一化坐标
        const double ey = blockY * invScale / nominalH;
        const double dx = invScale / nominalW;
        const double dy = invScale / nominalH;

        const cv::Point2d origin = toSource(ex, ey);
        const cv::Point2d stepX = toSource(ex + dx, ey);
        const cv::Point2d stepY = toSource(ex, ey + dy);

        // 不用 cv::Mat_ 的逗号初始化：Mat/Mat_ 从 MatCommaInitializer_ 构造已被标记弃用（C4996），
        // Matx 一样能直接喂给 warpAffine（_InputArray 有 Matx 重载）
        const cv::Matx23d matrix(
            stepX.x - origin.x, stepY.x - origin.x, origin.x,
            stepX.y - origin.y, stepY.y - origin.y, origin.y);
        cv::warpAffine(source, block, matrix, cv::Size(blockW, blockH),
            cv::INTER_LANCZOS4 | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
    }
    else {
        // 缩小：warpAffine 不支持 INTER_AREA，改为裁出源里对应的矩形再做面积平均
        // （裁切按外包围盒取整，最多多带一个源像素，缩小后不足一个目标像素）
        const cv::Point2d corners[4] = {
            toSource(regionX / nominalW, regionY / nominalH),
            toSource((regionX + regionW) / nominalW, regionY / nominalH),
            toSource(regionX / nominalW, (regionY + regionH) / nominalH),
            toSource((regionX + regionW) / nominalW, (regionY + regionH) / nominalH),
        };

        double left = corners[0].x, right = corners[0].x;
        double top = corners[0].y, bottom = corners[0].y;
        for (const auto& corner : corners) {
            left = (std::min)(left, corner.x);
            right = (std::max)(right, corner.x);
            top = (std::min)(top, corner.y);
            bottom = (std::max)(bottom, corner.y);
        }

        const int cropX = std::clamp(static_cast<int>(std::floor(left)), 0, srcCols - 1);
        const int cropY = std::clamp(static_cast<int>(std::floor(top)), 0, srcRows - 1);
        const int cropRight = std::clamp(static_cast<int>(std::ceil(right)), cropX + 1, srcCols);
        const int cropBottom = std::clamp(static_cast<int>(std::ceil(bottom)), cropY + 1, srcRows);
        const cv::Mat crop = source(cv::Rect(cropX, cropY, cropRight - cropX, cropBottom - cropY));

        // 旋转会交换宽高，先缩到"旋转前"的目标尺寸再转
        const bool swapAxes = (rotation == 1 || rotation == 3);
        const cv::Size scaledSize = swapAxes ? cv::Size(blockH, blockW) : cv::Size(blockW, blockH);
        cv::Mat scaled;
        cv::resize(crop, scaled, scaledSize, 0.0, 0.0, cv::INTER_AREA);
        block = rotateToNominal(scaled, rotation);
        if (block.empty() || block.cols != blockW || block.rows != blockH)
            return result;
    }

    if (block.empty() || block.cols != blockW || block.rows != blockH)
        return result;

    result.image = std::move(block);
    result.left = static_cast<double>(blockX) / (nominalW * scale);
    result.top = static_cast<double>(blockY) / (nominalH * scale);
    result.width = static_cast<double>(result.image.cols) / (nominalW * scale);
    result.height = static_cast<double>(result.image.rows) / (nominalH * scale);
    result.valid = true;
    return result;
}

} // namespace jark
