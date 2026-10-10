#pragma once

// 文件格式嗅探：只依据文件头（魔数）判断真实格式，不依赖扩展名。
// 设计上与 JarkThumbnailProvider/include/FormatSniffer.h 同源，后续将合并为共用模块。
//
// 使用方式：读取文件头若干字节（建议 ≥1KiB，TGA 需要文件尾，故传入整个缓冲区最佳），
// 得到 FileFormat 后交给解码路由；嗅探失败（Unknown）再由扩展名兜底。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace jark {

enum class FileFormat : uint8_t {
    Unknown = 0,

    // —— 常见位图 ——
    Png,
    Gif,
    Jpeg,
    WebP,
    Bmp,
    Ico,

    // —— TIFF 家族（含大部分相机 RAW）——
    Tiff,

    // —— 需要专门解码器的格式 ——
    Jxl,
    Avif,
    Heif,
    Wp2,
    Psd,
    Svg,
    Dds,
    Qoi,
    Blp,
    Pcx,
    Pfm,
    Exr,
    Jp2,
    Jxr,
    Hdr,
    Pic,
    Pnm,
    Ras,
    Tga,

    // —— 复合容器 ——
    Livp,   // iOS 实况照片（zip 容器）
    Video,  // 视频容器：mp4/mov/mkv/webm/avi/...

    // —— 仅能由扩展名判定 ——
    Lep,    // lepton（无文件头特征）
    Raw,    // 相机 RAW：文件头多为 TIFF，只能靠扩展名区分
};

// 嗅探文件头。data 越大越准确（TGA 需要文件尾 18 字节）。
FileFormat sniffFileFormat(std::span<const uint8_t> data) noexcept;

// 由扩展名（不含点，大小写不敏感）推断格式；无法识别返回 Unknown。
FileFormat fileFormatFromExtension(std::wstring_view ext) noexcept;

// 该路径是不是视频文件（按扩展名判定，只看文件名部分）。
// **这就是"交给独立播放器还是走看图"的那一条判定**：命令行入参、拖放、Ctrl+O
// 三处都走它，避免同一个文件在不同入口下进了不同的模式。
bool isVideoFile(const std::wstring& path) noexcept;

// 该格式的扩展名是否需要优先于文件头嗅探结果（魔数无法表达的信息，如 RAW/视频）
bool isExtensionAuthoritative(FileFormat format) noexcept;

// 便于日志与调试
std::string_view fileFormatName(FileFormat format) noexcept;

} // namespace jark
