// windows.h 的 min/max 宏会破坏 exiv2 等第三方头文件（ImageDatabase 依赖），
// 本编译单元先关掉它们，自身的 std::min/std::max 也因此不受宏影响。
#define NOMINMAX 1

#include "BatchProcessor.h"

#include "ImageAdjust.h"
#include "ImageDatabase.h"
#include "jarkUtils.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

#include <shellapi.h>

namespace jark {
namespace {

    // 批量解码复用一个解码器；线程局部，避免与界面线程的缓存/预读互相干扰
    ImageDatabase& batchDecoder() {
        static thread_local ImageDatabase database;
        return database;
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

    bool deleteToRecycleBin(const std::vector<std::wstring>& files, std::wstring& error) {
        if (files.empty())
            return true;

        // SHFileOperationW 需要双零结尾的多字符串
        std::wstring buffer;
        for (const auto& file : files) {
            buffer += file;
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');

        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = buffer.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

        const int result = SHFileOperationW(&operation);
        if (result != 0 || operation.fAnyOperationsAborted) {
            error = std::format(L"删除失败，错误码 {}",
                result != 0 ? result : ::GetLastError());
            return false;
        }
        return true;
    }

} // namespace

std::vector<std::wstring> batchOutputExtensions() {
    return { L"png", L"jpg", L"webp", L"bmp", L"tif" };
}

bool processImageFile(const std::wstring& sourcePath, const std::wstring& targetPath,
    const BatchOptions& options, std::wstring& errorMessage) {
    errorMessage.clear();

    cv::Mat image = loadImage(sourcePath, errorMessage);
    if (image.empty())
        return false;

    // 缩放（按长边）
    if (options.maxEdge > 0) {
        const int longEdge = (std::max)(image.cols, image.rows);
        if (longEdge > options.maxEdge) {
            const double scale = static_cast<double>(options.maxEdge) / longEdge;
            cv::Mat resized;
            cv::resize(image, resized, cv::Size(), scale, scale, cv::INTER_AREA);
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

    if (options.task == BatchTask::Delete)
        return [&]() {
            BatchResult deleteResult;
            std::wstring error;
            if (deleteToRecycleBin(files, error)) {
                deleteResult.succeeded = files.size();
            }
            else {
                deleteResult.failed = files.size();
                deleteResult.messages.push_back(error);
            }
            return deleteResult;
        }();

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

        // —— 转换 / 旋转 ——
        std::filesystem::path target;
        if (options.outputDirectory.empty()) {
            if (options.task == BatchTask::Rotate) {
                target = source; // 就地旋转（界面会提示）
            }
            else {
                const std::wstring extension = options.outputExtension.empty() ?
                    lowerExtension(source) : options.outputExtension;
                target = source.parent_path() / (source.stem().wstring() + L"." + extension);
            }
        }
        else {
            const std::wstring extension = options.outputExtension.empty() ?
                lowerExtension(source) : options.outputExtension;
            target = std::filesystem::path(options.outputDirectory) /
                (source.stem().wstring() + L"." + extension);
        }

        if (target == source && !options.overwrite && options.task != BatchTask::Rotate) {
            ++result.skipped;
            result.messages.push_back(std::format(L"输出与源文件同名，跳过: {}", sourcePath));
            continue;
        }
        if (options.task != BatchTask::Rotate && !options.overwrite &&
            std::filesystem::exists(target, errorCode)) {
            ++result.skipped;
            result.messages.push_back(std::format(L"目标已存在，跳过: {}", target.wstring()));
            continue;
        }

        std::wstring error;
        if (processImageFile(sourcePath, target.wstring(), options, error)) {
            ++result.succeeded;
        }
        else {
            ++result.failed;
            result.messages.push_back(std::format(L"{}: {}", source.filename().wstring(), error));
        }
    }

    return result;
}

} // namespace jark
