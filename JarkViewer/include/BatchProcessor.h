#pragma once

// 批量处理：转换格式、缩放、重命名、旋转翻转。
//
// 这里只做纯逻辑（读取用工程内解码器，写出用 OpenCV），不依赖窗口，
// 便于用命令行或单元测试单独验证；界面在 BatchWindow.h。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jark {

enum class BatchTask {
    Convert = 0, // 转换格式（可应用图像调整）
    Scale = 1,   // 缩放尺寸
    Rename = 2,  // 批量重命名
    Rotate = 3,  // 旋转 / 翻转
};

// 缩放方式
enum class ScaleMode {
    Percent = 0,  // 按百分比
    Width = 1,    // 指定宽度，高度按比例
    Height = 2,   // 指定高度，宽度按比例
    Stretch = 3,  // 自定义宽高（允许拉伸变形）
    LongEdge = 4, // 限制长边（仅缩小，不放大）
};

// 缩放算法，顺序与界面下拉框一致；Auto 为缩小用面积平均、放大用 Lanczos
enum class ScaleAlgorithm {
    Auto = 0,
    Nearest = 1,
    Linear = 2,
    Area = 3,
    Cubic = 4,
    Lanczos = 5,
};

struct ImageSize {
    int width = 0;
    int height = 0;
    bool operator==(const ImageSize&) const = default;
    bool empty() const noexcept { return width <= 0 || height <= 0; }
};

struct BatchOptions {
    BatchTask task = BatchTask::Convert;

    // —— 输出 ——
    std::wstring outputDirectory;             // 空 = 输出到原目录
    std::wstring outputExtension = L"png";    // 输出格式（不含点）；空 = 保持原格式（缩放用）
    int jpegQuality = 92;                     // 1~100
    bool overwrite = false;                   // 允许覆盖同名输出

    // 图像调整（与打印/编辑页共用同一组参数）
    bool applyAdjustments = false;
    int brightness = 100;
    int contrast = 100;
    uint32_t colorMode = 0;                   // 0彩色 1黑白 2黑白文档 3黑白抖动
    bool invertColors = false;

    // —— 缩放 ——
    ScaleMode scaleMode = ScaleMode::Width;
    ScaleAlgorithm scaleAlgorithm = ScaleAlgorithm::Auto;
    int scalePercent = 50;                    // 百分比（1~1000）
    int scaleWidth = 1920;                    // 目标宽（按宽度 / 自定义宽高）
    int scaleHeight = 1080;                   // 目标高（按高度 / 自定义宽高）
    int scaleMaxEdge = 2048;                  // 长边上限（限制长边用）

    // —— 重命名 ——
    std::wstring renamePrefix = L"image_";
    int renameStart = 1;
    int renameDigits = 3;

    // —— 旋转 ——
    int rotationDegrees = 0;                  // 0/90/180/270（顺时针）
    bool flipHorizontal = false;
    bool flipVertical = false;
};

struct BatchResult {
    size_t succeeded = 0;
    size_t skipped = 0;
    size_t failed = 0;
    std::vector<std::wstring> messages;       // 失败/跳过原因（含文件名）

    bool hasFailure() const noexcept { return failed > 0; }
};

// 进度回调：当前索引/总数/当前文件；返回 false 表示取消
using BatchProgress = std::function<bool(size_t current, size_t total, const std::wstring& file)>;

// 计算缩放后的目标尺寸（纯函数：批处理与界面预览共用同一套规则）
ImageSize scaledSize(ImageSize source, const BatchOptions& options);

// 执行批量处理（阻塞，请在工作线程调用）
BatchResult runBatch(const std::vector<std::wstring>& files, const BatchOptions& options,
    const BatchProgress& progress = {});

// 单张图像处理（供批处理内部与测试使用）：读取 → 缩放 → 调整 → 旋转 → 写出
bool processImageFile(const std::wstring& sourcePath, const std::wstring& targetPath,
    const BatchOptions& options, std::wstring& errorMessage);

// 读取图像尺寸（界面预览输出分辨率用；走工程内解码器，失败返回 false）
bool loadImageSize(const std::wstring& path, ImageSize& size);

} // namespace jark
