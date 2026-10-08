#include "CanvasRenderer.h"

#include "jarkUtils.h"

#include <ppl.h>

namespace jark {

CanvasGeometry imageGeometry(const ViewState& view, cv::Size canvasSize, cv::Size fallbackSize) {
    CanvasGeometry geometry;
    geometry.nominalSize = view.imageWidth > 0 && view.imageHeight > 0
        ? cv::Size(view.imageWidth, view.imageHeight) : fallbackSize;
    if (view.rotation & 1)
        std::swap(geometry.nominalSize.width, geometry.nominalSize.height);
    if (geometry.nominalSize.empty() || canvasSize.empty() || view.zoom <= 0 || view.zoomBase <= 0)
        return geometry;

    geometry.scale = static_cast<double>(view.zoom) / view.zoomBase;
    geometry.renderedSize = { geometry.nominalSize.width * geometry.scale,
        geometry.nominalSize.height * geometry.scale };
    geometry.origin = { view.slideX + static_cast<int>(std::round((canvasSize.width - geometry.renderedSize.width) / 2.0)),
        view.slideY + static_cast<int>(std::round((canvasSize.height - geometry.renderedSize.height) / 2.0)) };
    const double left = std::clamp(-geometry.origin.x / geometry.renderedSize.width, 0.0, 1.0);
    const double top = std::clamp(-geometry.origin.y / geometry.renderedSize.height, 0.0, 1.0);
    const double right = std::clamp((canvasSize.width - geometry.origin.x) / geometry.renderedSize.width, 0.0, 1.0);
    const double bottom = std::clamp((canvasSize.height - geometry.origin.y) / geometry.renderedSize.height, 0.0, 1.0);
    geometry.visible = { left, top, (std::max)(0.0, right - left), (std::max)(0.0, bottom - top) };
    return geometry;
}

cv::Point navigationSlide(const CanvasGeometry& geometry, cv::Size canvasSize, cv::Point2d center) {
    const auto slide = [](double rendered, int canvas, double position) {
        if (rendered <= canvas)
            return 0;
        const double left = std::clamp(canvas / 2.0 - std::clamp(position, 0.0, 1.0) * rendered,
            canvas - rendered, 0.0);
        return static_cast<int>(std::round(left) - std::round((canvas - rendered) / 2.0));
    };
    return { slide(geometry.renderedSize.width, canvasSize.width, center.x),
        slide(geometry.renderedSize.height, canvasSize.height, center.y) };
}

namespace {

    constexpr int BG_GRID_WIDTH = 16; // 透明区域棋盘格边长

    // 画布底色：全屏时可选纯黑（设置项，默认关闭），否则跟随当前主题
    uint32_t canvasBackgroundColor() {
        if (GlobalVar::settingParameter.blackFullscreenBackground && jarkUtils::IsFullScreen())
            return 0xFF000000;
        return GlobalVar::currentTheme.BG;
    }

    uint32_t getSrcPx1(const cv::Mat& srcImg, int srcX, int srcY, bool isLowZoom) {
        uchar srcPx = srcImg.at<uchar>(srcY, srcX);
        if (isLowZoom && srcY > 0 && srcX > 0) { // 简单临近像素平均
            const uchar px0 = srcImg.at<uchar>(srcY - 1, srcX - 1);
            const uchar px1 = srcImg.at<uchar>(srcY - 1, srcX);
            const uchar px2 = srcImg.at<uchar>(srcY, srcX - 1);
            srcPx = (px0 + px1 + px2 + srcPx) >> 2;
        }
        return srcPx | srcPx << 8 | srcPx << 16 | 255 << 24;
    }

    uint32_t getSrcPx3(const cv::Mat& srcImg, int srcX, int srcY, bool isLowZoom) {
        cv::Vec3b srcPx = srcImg.at<cv::Vec3b>(srcY, srcX);

        if (isLowZoom && srcY > 0 && srcX > 0) { // 简单临近像素平均
            const cv::Vec3b px1 = srcImg.at<cv::Vec3b>(srcY - 1, srcX - 1);
            const cv::Vec3b px2 = srcImg.at<cv::Vec3b>(srcY - 1, srcX);
            const cv::Vec3b px3 = srcImg.at<cv::Vec3b>(srcY, srcX - 1);
            for (int i = 0; i < 3; i++)
                srcPx[i] = (px1[i] + px2[i] + px3[i] + srcPx[i]) >> 2;
        }
        return *((uint32_t*)&srcPx) | (255 << 24);
    }

    // 要求 srcImg 必须是内存紧凑布局，每行像素数据没有填充字节，且每行连接紧凑
    uint32_t getSrcPx4(const cv::Mat& srcImg, int srcX, int srcY, int mainX, int mainY, bool isLowZoom) {
        const intUnion* srcPtr = (intUnion*)srcImg.ptr();
        const int srcW = srcImg.cols;

        intUnion srcPx = srcPtr[srcW * srcY + srcX];

        if (isLowZoom && srcY > 0 && srcX > 0) { // 简单临近像素平均
            intUnion px1 = srcPtr[srcW * (srcY - 1) + srcX - 1];
            intUnion px2 = srcPtr[srcW * (srcY - 1) + srcX];
            intUnion px3 = srcPtr[srcW * srcY + srcX - 1];
            for (int i = 0; i < 4; i++)
                srcPx[i] = (px1[i] + px2[i] + px3[i] + srcPx[i]) >> 2;
        }

        if (srcPx[3] == 255) return srcPx.u32;

        intUnion bgPx = ((mainX / BG_GRID_WIDTH + mainY / BG_GRID_WIDTH) & 1) ? 
            GlobalVar::currentTheme.BLACK_GRID : GlobalVar::currentTheme.WHITE_GRID;
        if (srcPx[3] == 0) return bgPx.u32;

        const int alpha = srcPx[3];
        //intUnion ret = 255;
        //ret[0] = (bgPx[0] * (255 - alpha) + srcPx[0] * alpha + 255) >> 8;
        //ret[1] = (bgPx[1] * (255 - alpha) + srcPx[1] * alpha + 255) >> 8;
        //ret[2] = (bgPx[2] * (255 - alpha) + srcPx[2] * alpha + 255) >> 8;
        return 255 << 24 |
            (((bgPx[2] * (255 - alpha) + srcPx[2] * alpha + 255) & 0xff00) << 8) |
            ((bgPx[1] * (255 - alpha) + srcPx[1] * alpha + 255) & 0xff00) |
            ((bgPx[0] * (255 - alpha) + srcPx[0] * alpha + 255) >> 8);
    }

    void drawCanvasImpl(const cv::Mat& srcImg, cv::Mat& canvas, const ViewState& view) {
        // 预旋转过的源位图（矢量图的区域高清块）采样时不再套旋转
        const int rotation = view.sourcePreRotated ? 0 : view.rotation;

        int srcH, srcW;
        if (rotation == 0 || rotation == 2) {
            srcH = srcImg.rows;
            srcW = srcImg.cols;
        }
        else {
            srcH = srcImg.cols;
            srcW = srcImg.rows;
        }

        const int canvasH = canvas.rows;
        const int canvasW = canvas.cols;

        if (srcH <= 0 || srcW <= 0)
            return;

        // 主画布与鸟瞰共用几何；位图尺寸仍只决定采样密度。
        const auto geometry = imageGeometry(view, canvas.size(), srcImg.size());
        if (geometry.scale <= 0.0)
            return;
        const int nominalW = geometry.nominalSize.width;
        const int nominalH = geometry.nominalSize.height;
        // 位图可能只覆盖名义图像的一块（sourceWidth/Height 为那块区域的归一化尺寸），
        // 采样密度按"位图像素 / 该区域的名义像素"算
        const double sourceWidth = view.sourceWidth > 0.0 ? view.sourceWidth : 1.0;
        const double sourceHeight = view.sourceHeight > 0.0 ? view.sourceHeight : 1.0;
        const float srcScaleX = (float)srcW / ((float)nominalW * (float)sourceWidth);
        const float srcScaleY = (float)srcH / ((float)nominalH * (float)sourceHeight);
        const double renderedW = geometry.renderedSize.width;
        const double renderedH = geometry.renderedSize.height;
        const int deltaW = geometry.origin.x;
        const int deltaH = geometry.origin.y;
        // 采样原点：位图左上角在画布上的位置（整幅时与图像原点重合）
        const int sampleDeltaW = deltaW +
            (int)std::llround(view.sourceLeft * nominalW * geometry.scale);
        const int sampleDeltaH = deltaH +
            (int)std::llround(view.sourceTop * nominalH * geometry.scale);

        int xStart = deltaW < 0 ? 0 : deltaW;
        int yStart = deltaH < 0 ? 0 : deltaH;
        int xEnd = (int)std::round(renderedW) + deltaW;
        int yEnd = (int)std::round(renderedH) + deltaH;
        if (xEnd > canvasW) xEnd = canvasW;
        if (yEnd > canvasH) yEnd = canvasH;

        JARK_LOG("drawCanvas: src={}x{} type={} nominal={}x{} zoom={} slide=({},{}) rect=[{},{}-{},{}] canvas={}x{} srcScale=({}, {})",
            srcW, srcH, srcImg.type(), nominalW, nominalH, view.zoom,
            view.slideX, view.slideY,
            xStart, yStart, xEnd, yEnd, canvasW, canvasH, srcScaleX, srcScaleY);

        uint32_t* ptrStart = (uint32_t*)canvas.ptr();
        uint32_t* ptrEnd = ptrStart + canvasH * canvasW;
        std::fill(ptrStart, ptrEnd, canvasBackgroundColor());

        if (view.border) { // 普通图像  画边框（主页/解码失败的界面画面不画）
            const uint32_t lineColor = 0xFF808080;
            if (0 < xStart and xStart < canvasW) {
                const int yMax = (std::min)(yEnd + 1, canvasH);
                for (int y = (std::max)(yStart - 1, 0); y < yMax; y++) {
                    ((uint32_t*)canvas.ptr())[y * canvasW + xStart - 1] = lineColor;
                }
            }
            if (0 < xEnd and xEnd < canvasW) {
                const int yMax = (std::min)(yEnd + 1, canvasH);
                for (int y = (std::max)(yStart - 1, 0); y < yMax; y++) {
                    ((uint32_t*)canvas.ptr())[y * canvasW + xEnd] = lineColor;
                }
            }

            if (0 < yStart and yStart < canvasH) {
                for (int x = xStart; x < xEnd; x++) {
                    ((uint32_t*)canvas.ptr())[(yStart - 1) * canvasW + x] = lineColor;
                }
            }
            if (0 < yEnd and yEnd < canvasH) {
                for (int x = xStart; x < xEnd; x++) {
                    ((uint32_t*)canvas.ptr())[yEnd * canvasW + x] = lineColor;
                }
            }
        }

        const float zoomInvert = (float)view.zoomBase / view.zoom;
        // 采样步长：画布像素 -> 名义像素 -> 位图像素
        const float zoomInvertX = zoomInvert * srcScaleX;
        const float zoomInvertY = zoomInvert * srcScaleY;
        const bool isLowZoom = view.zoom < view.zoomBase;

        switch (srcImg.type()) {
        case CV_8UC4: {
            concurrency::parallel_for(yStart, yEnd, [&](int y) {
                auto ptr = ((uint32_t*)canvas.ptr()) + y * canvasW;
                //int srcY = (int)((int64_t)(y - deltaH) * view.zoomBase / view.zoom); // 2K屏 50%缩放一帧34ms 100%缩放一帧14ms
                int srcY = (int)((y - sampleDeltaH) * zoomInvertY); // 快一点  2K屏 50%缩放一帧28ms 100%缩放一帧14ms

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcX, srcY, x, y, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcH - 1 - srcY, srcX, x, y, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, x, y, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcY, srcW - 1 - srcX, x, y, isLowZoom);
                    }
                    break;
                }
            });
        }break;

        case CV_8UC3: {
            concurrency::parallel_for(yStart, yEnd, [&](int y) {
                auto ptr = ((uint32_t*)canvas.ptr()) + y * canvasW;
                //int srcY = (int)((int64_t)(y - deltaH) * view.zoomBase / view.zoom);
                int srcY = (int)((y - sampleDeltaH) * zoomInvertY);

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcX, srcY, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcH - 1 - srcY, srcX, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcY, srcW - 1 - srcX, isLowZoom);
                    }
                    break;
                }
            });
        }break;

        case CV_8UC1: {
            concurrency::parallel_for(yStart, yEnd, [&](int y) {
                auto ptr = ((uint32_t*)canvas.ptr()) + y * canvasW;
                //int srcY = (int)((int64_t)(y - deltaH) * view.zoomBase / view.zoom);
                int srcY = (int)((y - sampleDeltaH) * zoomInvertY);

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcX, srcY, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcH - 1 - srcY, srcX, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - sampleDeltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcY, srcW - 1 - srcX, isLowZoom);
                    }
                    break;
                }
            });
        }break;
        }
    }

} // namespace

void drawImageToCanvas(const cv::Mat& srcImage, cv::Mat& canvas, const ViewState& view) {
    drawCanvasImpl(srcImage, canvas, view);
}

} // namespace jark
