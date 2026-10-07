// windows.h 的 min/max 宏会破坏 exiv2 等第三方头文件（ImageDatabase 依赖），
// 本编译单元先关掉它们，自身的 std::min/std::max 也因此不受宏影响。
#define NOMINMAX 1

#include "BatchProcessor.h"

#include "ImageAdjust.h"
#include "ImageDatabase.h"
#include "jarkUtils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>

namespace jark {
namespace {

    // 批量解码复用一个解码器；线程局部，避免与界面线程的缓存/预读互相干扰
    ImageDatabase& batchDecoder() {
        static thread_local ImageDatabase database;
        return database;
    }

    // Windows 路径不区分大小写，比较/去重前统一转小写
    std::wstring pathKey(const std::filesystem::path& path) {
        std::wstring key = path.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        return key;
    }

    std::wstring lowerExtension(const std::filesystem::path& path) {
        std::wstring extension = path.extension().wstring();
        if (!extension.empty() && extension.front() == L'.')
            extension.erase(extension.begin());
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        return extension;
    }

    bool writeEncoded(const cv::Mat& image, const std::wstring& targetPath,
        const std::wstring& extension, int quality, std::wstring& error) {
        if (image.empty()) {
            error = L"图像为空";
            return false;
        }

        std::vector<int> encodeParams;
        if (extension == L"jpg" || extension == L"jpeg") {
            encodeParams = { cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100) };
        }
        else if (extension == L"webp") {
            encodeParams = { cv::IMWRITE_WEBP_QUALITY, std::clamp(quality, 1, 100) };
        }

        const std::string encodeExtension = "." + jarkUtils::wstringToUtf8(extension);
        std::vector<uchar> encoded;
        try {
            if (!cv::imencode(encodeExtension, image, encoded, encodeParams)) {
                error = std::format(L"编码为 {} 失败", extension);
                return false;
            }
        }
        catch (const cv::Exception& exception) {
            error = std::format(L"编码异常: {}", jarkUtils::utf8ToWstring(exception.what()));
            return false;
        }

        std::ofstream file(targetPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            error = L"无法写入文件";
            return false;
        }

        file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!file.good()) {
            error = L"写入文件失败";
            return false;
        }

        return true;
    }

    // 读取图像：走工程内解码器，因此 HEIC / AVIF / JXL / RAW 等也能参与批量处理
    cv::Mat loadImage(const std::wstring& path, std::wstring& error) {
        auto imageAsset = batchDecoder().myLoader(path);
        const cv::Mat* source = nullptr;
        if (!imageAsset.primaryFrame.empty())
            source = &imageAsset.primaryFrame;
        else if (!imageAsset.frames.empty())
            source = &imageAsset.frames.front(); // 动图取首帧

        if (!source || source->empty()) {
            error = L"无法解码";
            return {};
        }

        cv::Mat bgr = toBgrImage(*source);
        if (bgr.empty())
            error = L"不支持的颜色通道";

        return bgr;
    }

    // 插值方式 → OpenCV 标志；Auto 按缩放方向选：缩小时用面积平均（抗锯齿最干净），
    // 放大时用 Lanczos（比双三次锐利，且不会像 INTER_AREA 放大那样退化成最近邻）
    int resolveInterpolation(ImageSize target, ImageSize source, ScaleAlgorithm algorithm) {
        switch (algorithm) {
        case ScaleAlgorithm::Nearest: return cv::INTER_NEAREST;
        case ScaleAlgorithm::Linear: return cv::INTER_LINEAR;
        case ScaleAlgorithm::Area: return cv::INTER_AREA;
        case ScaleAlgorithm::Cubic: return cv::INTER_CUBIC;
        case ScaleAlgorithm::Lanczos: return cv::INTER_LANCZOS4;
        default:
            return (static_cast<int64_t>(target.width) * target.height <
                static_cast<int64_t>(source.width) * source.height) ? cv::INTER_AREA : cv::INTER_LANCZOS4;
        }
    }

} // namespace

ImageSize scaledSize(ImageSize source, const BatchOptions& options) {
    if (source.empty())
        return source;

    const auto scaled = [](double value) {
        return (std::max)(1, static_cast<int>(std::lround(value)));
        };

    switch (options.scaleMode) {
    case ScaleMode::Percent: {
        const double factor = std::clamp(options.scalePercent, 1, 1000) / 100.0;
        return { scaled(source.width * factor), scaled(source.height * factor) };
    }
    case ScaleMode::Width: {
        const int width = (std::max)(1, options.scaleWidth);
        return { width, scaled(static_cast<double>(source.height) * width / source.width) };
    }
    case ScaleMode::Height: {
        const int height = (std::max)(1, options.scaleHeight);
        return { scaled(static_cast<double>(source.width) * height / source.height), height };
    }
    case ScaleMode::Stretch:
        return { (std::max)(1, options.scaleWidth), (std::max)(1, options.scaleHeight) };
    case ScaleMode::LongEdge: {
        const int limit = (std::max)(1, options.scaleMaxEdge);
        const int longEdge = (std::max)(source.width, source.height);
        if (longEdge <= limit)
            return source; // 只缩不放
        const double factor = static_cast<double>(limit) / longEdge;
        return { scaled(source.width * factor), scaled(source.height * factor) };
    }
    }

    return source;
}

bool processImageFile(const std::wstring& sourcePath, const std::wstring& targetPath,
    const BatchOptions& options, std::wstring& errorMessage) {
    errorMessage.clear();

    cv::Mat image = loadImage(sourcePath, errorMessage);
    if (image.empty())
        return false;

    // 缩放
    if (options.task == BatchTask::Scale) {
        const ImageSize source{ image.cols, image.rows };
        const ImageSize target = scaledSize(source, options);
        if (target != source) {
            cv::Mat resized;
            cv::resize(image, resized, cv::Size(target.width, target.height), 0.0, 0.0,
                resolveInterpolation(target, source, options.scaleAlgorithm));
            image = std::move(resized);
        }
    }

    // 图像调整（与打印/编辑页同一套参数与实现）
    if (options.applyAdjustments) {
        applyImageAdjustments(image, options.brightness, options.contrast,
            options.colorMode, options.invertColors);
    }

    // 旋转与翻转
    if (options.rotationDegrees == 90)
        cv::rotate(image, image, cv::ROTATE_90_CLOCKWISE);
    else if (options.rotationDegrees == 180)
        cv::rotate(image, image, cv::ROTATE_180);
    else if (options.rotationDegrees == 270)
        cv::rotate(image, image, cv::ROTATE_90_COUNTERCLOCKWISE);

    if (options.flipHorizontal)
        cv::flip(image, image, 1);
    if (options.flipVertical)
        cv::flip(image, image, 0);

    // 输出目录与扩展名
    const std::filesystem::path target(targetPath);
    if (target.has_parent_path()) {
        std::error_code errorCode;
        std::filesystem::create_directories(target.parent_path(), errorCode);
        if (errorCode) {
            errorMessage = L"无法创建输出目录";
            return false;
        }
    }

    std::wstring extension = lowerExtension(target);
    if (extension.empty())
        extension = options.outputExtension;

    return writeEncoded(image, targetPath, extension, options.jpegQuality, errorMessage);
}

BatchResult runBatch(const std::vector<std::wstring>& files, const BatchOptions& options,
    const BatchProgress& progress) {
    BatchResult result;

    if (files.empty())
        return result;

    // 本次运行已产出的输出路径（小写），用于避免同名互相覆盖
    std::set<std::wstring> producedTargets;

    size_t index = 0;
    for (const auto& sourcePath : files) {
        ++index;
        if (progress && !progress(index, files.size(), sourcePath))
            break; // 用户取消

        std::error_code errorCode;
        const std::filesystem::path source(sourcePath);

        if (!std::filesystem::exists(source, errorCode)) {
            ++result.skipped;
            result.messages.push_back(std::format(L"文件不存在: {}", sourcePath));
            continue;
        }

        // —— 重命名 ——
        if (options.task == BatchTask::Rename) {
            const std::wstring number = std::format(L"{:0{}}", options.renameStart + (int)index - 1,
                (std::max)(1, options.renameDigits));
            const std::filesystem::path target = source.parent_path() /
                (options.renamePrefix + number + source.extension().wstring());

            if (target == source) {
                ++result.skipped;
                continue;
            }
            if (!options.overwrite && std::filesystem::exists(target, errorCode)) {
                ++result.skipped;
                result.messages.push_back(std::format(L"目标已存在，跳过: {}", target.wstring()));
                continue;
            }

            std::filesystem::rename(source, target, errorCode);
            if (errorCode) {
                ++result.failed;
                result.messages.push_back(std::format(L"重命名失败: {}", sourcePath));
            }
            else {
                ++result.succeeded;
            }
            continue;
        }

        // —— 转换 / 缩放 / 旋转 ——
        const bool inPlace = options.task == BatchTask::Rotate && options.outputDirectory.empty();

        std::filesystem::path target;
        if (inPlace) {
            target = source; // 就地旋转（界面会提示）
        }
        else {
            const std::wstring extension = options.outputExtension.empty() ?
                lowerExtension(source) : options.outputExtension;
            const auto directory = options.outputDirectory.empty() ?
                source.parent_path() : std::filesystem::path(options.outputDirectory);
            target = directory / (source.stem().wstring() + L"." + extension);

            // 本次运行里已经有别的源文件产出同名结果（例如 a.png 与 a.jpg 都转成 a.png）：
            // 自动加序号，避免后者覆盖前者
            const std::wstring stem = target.stem().wstring();
            const std::wstring targetExtension = target.extension().wstring();
            for (int suffix = 1; producedTargets.contains(pathKey(target)); ++suffix)
                target = directory / (stem + L"_" + std::to_wstring(suffix) + targetExtension);
        }

        if (!inPlace && target != source && !options.overwrite &&
            std::filesystem::exists(target, errorCode)) {
            ++result.skipped;
            result.messages.push_back(std::format(L"目标已存在，跳过: {}", target.wstring()));
            continue;
        }

        std::wstring error;
        if (processImageFile(sourcePath, target.wstring(), options, error)) {
            ++result.succeeded;
            if (!inPlace)
                producedTargets.insert(pathKey(target));
        }
        else {
            ++result.failed;
            result.messages.push_back(std::format(L"{}: {}", source.filename().wstring(), error));
        }
    }

    return result;
}

bool loadImageSize(const std::wstring& path, ImageSize& size) {
    std::wstring error;
    const cv::Mat image = loadImage(path, error);
    if (image.empty())
        return false;

    size = { image.cols, image.rows };
    return true;
}

} // namespace jark
