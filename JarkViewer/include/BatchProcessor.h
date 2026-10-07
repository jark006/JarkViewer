#pragma once

// 批量处理：转换格式/缩放、重命名、旋转翻转、删除到回收站。
//
// 这里只做纯逻辑（读取用工程内解码器，写出用 OpenCV），不依赖窗口，
// 便于用命令行或单元测试单独验证；界面在 BatchWindow.h。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jark {

enum class BatchTask {
    Convert = 0, // 转换格式 / 缩放 / 应用图像调整
    Rename = 1,  // 批量重命名
    Rotate = 2,  // 旋转 / 翻转
    Delete = 3,  // 移到回收站
};

struct BatchOptions {
    BatchTask task = BatchTask::Convert;

    // —— 转换 ——
    std::wstring outputDirectory;             // 空 = 输出到原目录
    std::wstring outputExtension = L"png";    // 输出格式（不含点）
    int maxEdge = 0;                          // 0 = 保持原尺寸
    int jpegQuality = 92;                     // 1~100

    // 图像调整（与打印/编辑页共用同一组参数）
    bool applyAdjustments = false;
    int brightness = 100;
    int contrast = 100;
    uint32_t colorMode = 0;                   // 0彩色 1黑白 2黑白文档 3黑白抖动
    bool invertColors = false;

    // —— 重命名 ——
    std::wstring renamePrefix = L"image_";
    int renameStart = 1;
    int renameDigits = 3;

    // —— 旋转 ——
    int rotationDegrees = 0;                  // 0/90/180/270（顺时针）
    bool flipHorizontal = false;
    bool flipVertical = false;

    bool overwrite = false;                   // 允许覆盖同名输出
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

// 支持的输出格式（供界面列出）
std::vector<std::wstring> batchOutputExtensions();

// 执行批量处理（阻塞，请在工作线程调用）
BatchResult runBatch(const std::vector<std::wstring>& files, const BatchOptions& options,
    const BatchProgress& progress = {});

// 单张图像处理（供批处理内部与测试使用）：读取 → 缩放 → 调整 → 旋转 → 写出
bool processImageFile(const std::wstring& sourcePath, const std::wstring& targetPath,
    const BatchOptions& options, std::wstring& errorMessage);

} // namespace jark
