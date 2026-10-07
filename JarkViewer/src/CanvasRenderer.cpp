#include "CanvasRenderer.h"

#include "jarkUtils.h"

#include <ppl.h>

namespace jark {
namespace {

    constexpr int BG_GRID_WIDTH = 16; // 透明区域棋盘格边长

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
        int srcH, srcW;
        if (view.rotation == 0 || view.rotation == 2) {
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

        // 名义尺寸（100% 缩放时屏幕上应有的尺寸）。矢量图（SVG）的位图分辨率会随缩放
        // 变化，因此几何尺寸必须按名义尺寸计算，位图分辨率只影响采样密度。
        int nominalW, nominalH;
        if (view.rotation == 0 || view.rotation == 2) {
            nominalW = view.imageWidth;
            nominalH = view.imageHeight;
        }
        else {
            nominalW = view.imageHeight;
            nominalH = view.imageWidth;
        }
        if (nominalW <= 0 || nominalH <= 0) {
            nominalW = srcW;
            nominalH = srcH;
        }

        const float srcScaleX = (float)srcW / (float)nominalW; // 位图分辨率 / 名义尺寸
        const float srcScaleY = (float)srcH / (float)nominalH;

        // 源图和画板canvas均100%缩放且居中重合，此时随机取一个点，先只考虑水平方向
        // 该点与画板中心的距离，等于该点与源图中心的距离
        // 即 canvasW / 2 - x = srcW / 2 - srcX
        // 再考虑偏移量：canvasW / 2 - x = srcW / 2 - srcX - slide * srcW
        // 再考虑源图缩放：canvasW / 2 - x = (srcW / 2 - srcX - slide * srcW) * zoom
        // 即为源图和画板在特定位移和缩放的坐标变换公式
        // x = canvasW / 2.0 - (srcW / 2.0 - srcX - slide * srcW) * zoom
        // srcX = srcW / 2.0 - ((canvasW / 2.0 - x) / zoom + slide * srcW)

        const double renderedW = (double)nominalW * view.zoom / view.zoomBase;
        const double renderedH = (double)nominalH * view.zoom / view.zoomBase;
        const int deltaW = view.slideX + (int)std::round((canvasW - renderedW) / 2.0);
        const int deltaH = view.slideY + (int)std::round((canvasH - renderedH) / 2.0);

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
        std::fill(ptrStart, ptrEnd, GlobalVar::currentTheme.BG);

        if (((srcH == 600 and srcW == 800) or (srcH == 800 and srcW == 600)) and 
            (*((uint32_t*)srcImg.ptr()) == deepTheme.BG) or (*((uint32_t*)srcImg.ptr()) == lightTheme.BG)) {
            // 内置的用于提示的图像
        }
        else { // 普通图像  画边框
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
                int srcY = (int)((y - deltaH) * zoomInvertY); // 快一点  2K屏 50%缩放一帧28ms 100%缩放一帧14ms

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (view.rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcX, srcY, x, y, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcH - 1 - srcY, srcX, x, y, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx4(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, x, y, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
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
                int srcY = (int)((y - deltaH) * zoomInvertY);

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (view.rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcX, srcY, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcH - 1 - srcY, srcX, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx3(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
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
                int srcY = (int)((y - deltaH) * zoomInvertY);

                srcY = std::clamp(srcY, 0, srcH - 1);

                switch (view.rotation) {
                case 0:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcX, srcY, isLowZoom);
                    }
                    break;
                case 1:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcH - 1 - srcY, srcX, isLowZoom);
                    }
                    break;
                case 2:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
                        srcX = std::clamp(srcX, 0, srcW - 1);
                        ptr[x] = getSrcPx1(srcImg, srcW - 1 - srcX, srcH - 1 - srcY, isLowZoom);
                    }
                    break;
                default:
                    for (int x = xStart; x < xEnd; x++) {
                        int srcX = (int)((x - deltaW) * zoomInvertX);
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
