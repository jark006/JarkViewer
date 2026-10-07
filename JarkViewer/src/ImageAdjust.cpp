#include "ImageAdjust.h"

#include "jarkUtils.h"

#include <algorithm>
#include <cmath>

namespace jark {

cv::Mat toBgrImage(const cv::Mat& image) {
    if (image.empty())
        return {};

    switch (image.channels()) {
    case 1: {
        cv::Mat bgr;
        cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }
    case 3:
        return image.clone();
    case 4: {
        // Alpha 与白底混合（打印/转换场景下纸张是白的）
        cv::Mat bgr(image.rows, image.cols, CV_8UC3);
        for (int y = 0; y < image.rows; ++y) {
            const auto* source = image.ptr<uchar>(y);
            auto* target = bgr.ptr<uchar>(y);
            for (int x = 0; x < image.cols; ++x) {
                const int alpha = source[x * 4 + 3];
                for (int c = 0; c < 3; ++c)
                    target[x * 3 + c] = static_cast<uchar>((source[x * 4 + c] * alpha + 255 * (255 - alpha) + 255) >> 8);
            }
        }
        return bgr;
    }
    default:
        return {};
    }
}

void adjustBrightnessContrast(cv::Mat& src, int brightnessInt, int contrastInt) {
    if (src.empty() || src.type() != CV_8UC3)
        return;

    // 取值不能极端
    brightnessInt = std::clamp(brightnessInt, 1, 199);
    contrastInt = std::clamp(contrastInt, 0, 200);

    // brightness: 0 ~ 200 映射到 0 ~ 2.0；contrast 同理
    double brightness = brightnessInt / 100.0;
    double contrast = contrastInt / 100.0;

    brightness = std::pow(2.0 - brightness, 3.0); // 增大亮度比例
    contrast = std::pow(contrast, 3.0);           // 增大对比度比例

    for (int y = 0; y < src.rows; ++y) {
        auto* row = src.ptr<cv::Vec3b>(y);
        for (int x = 0; x < src.cols; ++x) {
            for (int c = 0; c < 3; ++c) {
                // 以 128 为中心调整对比度，再按亮度曲线拉伸
                double adjusted = (row[x][c] - 128.0) * contrast + 128.0;
                // 对比度>100 时低灰度会算出负值，而 pow(负底数, 非整数) 是 NaN（整片变黑），先夹住
                adjusted = std::clamp(adjusted, 0.0, 255.0);
                adjusted = std::pow(adjusted / 255.0, brightness) * 255.0;
                row[x][c] = cv::saturate_cast<uchar>(adjusted);
            }
        }
    }
}

cv::Mat balancedImageBrightness(const cv::Mat& inputImage) {
    if (inputImage.type() != CV_8UC3) {
        JARK_LOG("balancedImageBrightness 只接受 BGR/CV_8UC3 图像");
        return {};
    }

    int kernelSize = (std::max)(inputImage.cols, inputImage.rows) / 20;
    if (kernelSize < 3)
        kernelSize = 3;
    kernelSize |= 1; // 核大小必须为奇数

    cv::Mat gray;
    cv::cvtColor(inputImage, gray, cv::COLOR_BGR2GRAY);

    // 大核模糊估计背景，再相减以均衡亮度
    cv::Mat background;
    cv::GaussianBlur(gray, background, cv::Size(kernelSize, kernelSize), 0);

    cv::Mat corrected;
    cv::addWeighted(gray, 1.0, background, -1.0, 128, corrected);
    cv::normalize(corrected, corrected, 0, 255, cv::NORM_MINMAX);
    corrected.convertTo(corrected, CV_8U);
    cv::cvtColor(corrected, corrected, cv::COLOR_GRAY2BGR);
    return corrected;
}

void floydSteinbergDithering(cv::Mat& image) {
    cv::cvtColor(image, image, cv::COLOR_BGR2GRAY);

    const int height = image.rows;
    const int width = image.cols;

    for (int y = 0; y < height; ++y) {
        auto* row = image.ptr<uchar>(y);
        auto* nextRow = (y + 1 < height) ? image.ptr<uchar>(y + 1) : nullptr;

        for (int x = 0; x < width; ++x) {
            const uchar oldValue = row[x];
            const uchar newValue = oldValue < 128 ? 0 : 255;
            row[x] = newValue;

            const int error = oldValue - newValue;

            if (x + 1 < width)
                row[x + 1] = cv::saturate_cast<uchar>(row[x + 1] + error * 7 / 16);

            if (nextRow) {
                if (x > 0)
                    nextRow[x - 1] = cv::saturate_cast<uchar>(nextRow[x - 1] + error * 3 / 16);
                nextRow[x] = cv::saturate_cast<uchar>(nextRow[x] + error * 5 / 16);
                if (x + 1 < width)
                    nextRow[x + 1] = cv::saturate_cast<uchar>(nextRow[x + 1] + error * 1 / 16);
            }
        }
    }

    cv::cvtColor(image, image, cv::COLOR_GRAY2BGR);
}

void applyImageAdjustments(cv::Mat& image, int brightness, int contrast, uint32_t colorMode, bool invertColors) {
    if (image.empty())
        return;

    switch (static_cast<ColorMode>(colorMode)) {
    case ColorMode::Gray:
    case ColorMode::Dither: {
        cv::Mat gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(gray, image, cv::COLOR_GRAY2BGR);
    } break;
    case ColorMode::Document:
        if (auto balanced = balancedImageBrightness(image); !balanced.empty())
            image = std::move(balanced);
        break;
    default:
        break;
    }

    adjustBrightnessContrast(image, brightness, contrast);

    if (static_cast<ColorMode>(colorMode) == ColorMode::Dither)
        floydSteinbergDithering(image);

    if (invertColors)
        cv::bitwise_not(image, image);
}

} // namespace jark
