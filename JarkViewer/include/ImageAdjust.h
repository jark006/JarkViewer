#pragma once

// 图像调整（打印/编辑页与批量处理共用）：
// 亮度、对比度、颜色模式（彩色/黑白/黑白文档/黑白抖动）、反相。
// 原先这些实现内嵌在 Printer.h 里，批量处理需要同一套效果，故独立成模块。

#include <cstdint>

#include <opencv2/opencv.hpp>

namespace jark {

enum class ColorMode : uint32_t {
    Color = 0,        // 彩色
    Gray = 1,         // 黑白
    Document = 2,     // 黑白文档（均衡亮度）
    Dither = 3,       // 黑白抖动（Floyd-Steinberg）
};

// 统一转为 BGR：灰度复制三通道、BGRA 与白底混合
cv::Mat toBgrImage(const cv::Mat& image);

// 亮度/对比度：0~200，100 为不变
void adjustBrightnessContrast(cv::Mat& image, int brightness, int contrast);

// 文档模式：减去大核模糊估计的背景并归一化，突出字迹
cv::Mat balancedImageBrightness(const cv::Mat& input);

// 误差扩散抖动（Floyd-Steinberg），输入输出均为 BGR
void floydSteinbergDithering(cv::Mat& image);

// 统一入口：按颜色模式与参数处理图像（输入需为 CV_8UC3）
void applyImageAdjustments(cv::Mat& image, int brightness, int contrast, uint32_t colorMode, bool invertColors);

} // namespace jark
