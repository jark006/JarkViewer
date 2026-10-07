#include "FormatSniffer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace jark {
namespace {

bool asciiEquals(std::span<const uint8_t> data, size_t offset, std::string_view text) noexcept {
    if (data.size() < offset + text.size())
        return false;
    return std::memcmp(data.data() + offset, text.data(), text.size()) == 0;
}

bool startsWith(std::span<const uint8_t> data, std::string_view magic) noexcept {
    return asciiEquals(data, 0, magic);
}

bool startsWithBytes(std::span<const uint8_t> data, std::initializer_list<uint8_t> magic) noexcept {
    if (data.size() < magic.size())
        return false;
    return std::equal(magic.begin(), magic.end(), data.begin());
}

uint32_t readBe32(std::span<const uint8_t> data, size_t offset) noexcept {
    if (data.size() < offset + 4)
        return 0;
    return (static_cast<uint32_t>(data[offset]) << 24) |
        (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) |
        static_cast<uint32_t>(data[offset + 3]);
}

bool isAsciiWhitespace(uint8_t value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' || value == '\f';
}

// ISO BMFF（ftyp 盒子）：mp4/avif/heif/jxl/jp2 共用外壳，靠 brand 区分
bool bmffHasBrand(std::span<const uint8_t> data, std::initializer_list<std::string_view> brands) noexcept {
    constexpr size_t scanLimit = 96;
    const size_t limit = std::min(data.size(), scanLimit);

    for (size_t boxOffset = 0; boxOffset + 12 <= limit; ++boxOffset) {
        if (!asciiEquals(data, boxOffset + 4, "ftyp"))
            continue;

        uint32_t boxSize = readBe32(data, boxOffset);
        if (boxSize < 16 || boxOffset + boxSize > data.size())
            boxSize = static_cast<uint32_t>(std::min<size_t>(data.size() - boxOffset, scanLimit));

        const size_t brandEnd = boxOffset + boxSize;
        for (size_t brandOffset = boxOffset + 8; brandOffset + 4 <= brandEnd; brandOffset += 4) {
            for (const auto& brand : brands) {
                if (asciiEquals(data, brandOffset, brand))
                    return true;
            }
        }
    }

    return false;
}

// PNG 规范要求首个数据块必须是长度为 13 的 IHDR
bool hasReasonablePngHeader(std::span<const uint8_t> data) noexcept {
    if (data.size() < 24)
        return false;
    return readBe32(data, 8) == 13 && asciiEquals(data, 12, "IHDR");
}

// SVG 是文本格式：跳过空白与 BOM 后查找根元素
bool looksLikeSvg(std::span<const uint8_t> data) noexcept {
    const size_t limit = std::min<size_t>(data.size(), 512);
    size_t offset = 0;

    if (limit >= 3 && startsWithBytes(data, { 0xEF, 0xBB, 0xBF }))
        offset = 3;

    while (offset < limit && isAsciiWhitespace(data[offset]))
        ++offset;

    if (offset + 4 <= limit && std::memcmp(data.data() + offset, "<svg", 4) == 0)
        return true;

    if (offset + 5 <= limit && std::memcmp(data.data() + offset, "<?xml", 5) == 0) {
        for (size_t i = offset + 5; i + 4 <= limit; ++i) {
            if (std::memcmp(data.data() + i, "<svg", 4) == 0)
                return true;
        }
    }

    return false;
}

// PCX 只有 1 字节厂商标识，需要校验后续字段避免误判
bool looksLikePcx(std::span<const uint8_t> data) noexcept {
    if (data.size() < 128 || data[0] != 0x0A)
        return false;

    const uint8_t version = data[1];
    const uint8_t encoding = data[2];
    const uint8_t bitsPerPixel = data[3];
    const uint8_t planes = data[65];
    const uint16_t xMax = static_cast<uint16_t>(data[8] | (data[9] << 8));
    const uint16_t yMax = static_cast<uint16_t>(data[10] | (data[11] << 8));

    const bool versionOk = version == 0 || version == 2 || version == 3 || version == 4 || version == 5;
    const bool bppOk = bitsPerPixel == 1 || bitsPerPixel == 2 || bitsPerPixel == 4 || bitsPerPixel == 8;
    const bool planesOk = planes >= 1 && planes <= 4;

    return versionOk && encoding <= 1 && bppOk && planesOk && xMax > 0 && yMax > 0;
}

// TGA 2.0 尾部签名；TGA 1.0 无签名，只能靠扩展名
bool looksLikeTga(std::span<const uint8_t> data) noexcept {
    constexpr size_t footerSize = 26;
    if (data.size() < footerSize)
        return false;

    const size_t footer = data.size() - 18;
    return asciiEquals(data, footer, "TRUEVISION-XFILE");
}

// MPEG-TS：包长 188 字节，同步字节 0x47
bool looksLikeMpegTs(std::span<const uint8_t> data) noexcept {
    if (data.size() < 189 * 3)
        return false;

    return data[0] == 0x47 && data[188] == 0x47 && data[376] == 0x47;
}

} // namespace

FileFormat sniffFileFormat(std::span<const uint8_t> data) noexcept {
    if (data.size() < 8)
        return FileFormat::Unknown;

    // —— 常见位图 ——
    if (startsWithBytes(data, { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' }))
        return hasReasonablePngHeader(data) ? FileFormat::Png : FileFormat::Unknown;

    if (startsWith(data, "GIF87a") || startsWith(data, "GIF89a"))
        return FileFormat::Gif;

    if (startsWithBytes(data, { 0xFF, 0xD8, 0xFF }))
        return FileFormat::Jpeg;

    if (startsWith(data, "BM"))
        return FileFormat::Bmp;

    if (startsWith(data, "RIFF")) {
        if (asciiEquals(data, 8, "WEBP"))
            return FileFormat::WebP;
        if (asciiEquals(data, 8, "AVI "))
            return FileFormat::Video;
        return FileFormat::Unknown; // WAVE 等非图像 RIFF
    }

    if (startsWithBytes(data, { 0x00, 0x00, 0x01, 0x00 }))
        return FileFormat::Ico;

    // —— zip 容器（iOS 实况照片）——
    if (startsWithBytes(data, { 'P', 'K', 0x03, 0x04 }) ||
        startsWithBytes(data, { 'P', 'K', 0x05, 0x06 }) ||
        startsWithBytes(data, { 'P', 'K', 0x07, 0x08 }))
        return FileFormat::Livp;

    // —— JPEG XL：裸码流或 BMFF 容器 ——
    if (startsWithBytes(data, { 0xFF, 0x0A }) || bmffHasBrand(data, { "jxl " }))
        return FileFormat::Jxl;

    // —— ISO BMFF 家族 ——
    if (bmffHasBrand(data, { "avif", "avis" }))
        return FileFormat::Avif;

    if (bmffHasBrand(data, { "heic", "heix", "hevc", "hevx", "heim", "heis",
                             "hevm", "hevs", "heif", "mif1", "msf1" }))
        return FileFormat::Heif;

    if (bmffHasBrand(data, { "jp2 ", "jpx ", "jpm " }) ||
        startsWithBytes(data, { 0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ' }))
        return FileFormat::Jp2;

    if (bmffHasBrand(data, { "isom", "iso2", "iso4", "iso5", "iso6", "mp41", "mp42", "mp71",
                             "avc1", "dash", "qt  ", "M4V ", "M4A ", "mmp4", "f4v ",
                             "3gp4", "3gp5", "3gp6", "3g2a", "3g2b", "3g2c" }) ||
        bmffHasBrand(data, { "3gp", "3g2" }))
        return FileFormat::Video;

    // —— 需要专门解码器的格式 ——
    if (startsWith(data, "8BPS"))
        return FileFormat::Psd;

    if (startsWith(data, "DDS "))
        return FileFormat::Dds;

    if (startsWith(data, "qoif"))
        return FileFormat::Qoi;

    if (startsWith(data, "BLP1") || startsWith(data, "BLP2"))
        return FileFormat::Blp;

    if (startsWithBytes(data, { 'P', 'F', '\n' }) || startsWithBytes(data, { 'P', 'f', '\n' }))
        return FileFormat::Pfm;

    if (startsWithBytes(data, { 0x76, 0x2F, 0x31, 0x01 }))
        return FileFormat::Exr;

    // —— TIFF 家族（含绝大多数相机 RAW，最终由扩展名区分）——
    if (startsWithBytes(data, { 'I', 'I', 0x2A, 0x00 }) || startsWithBytes(data, { 'M', 'M', 0x00, 0x2A }) ||
        startsWithBytes(data, { 'I', 'I', 0x2B, 0x00 }) || startsWithBytes(data, { 'M', 'M', 0x00, 0x2B }))
        return FileFormat::Tiff;

    if (startsWithBytes(data, { 0x49, 0x49, 0xBC, 0x01 }))
        return FileFormat::Jxr;

    if (startsWith(data, "#?RADIANCE") || startsWith(data, "#?RGBE") || startsWith(data, "#?RGB"))
        return FileFormat::Hdr;

    if (startsWithBytes(data, { 'P', '1' }) || startsWithBytes(data, { 'P', '2' }) ||
        startsWithBytes(data, { 'P', '3' }) || startsWithBytes(data, { 'P', '4' }) ||
        startsWithBytes(data, { 'P', '5' }) || startsWithBytes(data, { 'P', '6' })) {
        const uint8_t next = data[2];
        if (isAsciiWhitespace(next) || next == '#')
            return FileFormat::Pnm;
        return FileFormat::Unknown;
    }

    if (startsWithBytes(data, { 0x53, 0x80, 0xF6, 0x34 }))
        return FileFormat::Pic;

    if (startsWithBytes(data, { 0x59, 0xA6, 0x6A, 0x95 }))
        return FileFormat::Ras;

    if (looksLikeSvg(data))
        return FileFormat::Svg;

    if (looksLikePcx(data))
        return FileFormat::Pcx;

    // —— 视频容器 ——
    if (startsWithBytes(data, { 0x1A, 0x45, 0xDF, 0xA3 })) // Matroska / WebM
        return FileFormat::Video;

    if (startsWithBytes(data, { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11 })) // ASF / WMV
        return FileFormat::Video;

    if (startsWithBytes(data, { 'F', 'L', 'V', 0x01 })) // FLV
        return FileFormat::Video;

    if (startsWithBytes(data, { 0x00, 0x00, 0x01, 0xBA }) ||  // MPEG-PS / VOB
        startsWithBytes(data, { 0x00, 0x00, 0x01, 0xB3 }))    // MPEG-VS
        return FileFormat::Video;

    if (startsWithBytes(data, { 0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0D, 0x01, 0x02 })) // MXF
        return FileFormat::Video;

    if (startsWith(data, ".RMF")) // RealMedia
        return FileFormat::Video;

    if (looksLikeMpegTs(data))
        return FileFormat::Video;

    if (looksLikeTga(data))
        return FileFormat::Tga;

    return FileFormat::Unknown;
}

FileFormat fileFormatFromExtension(std::wstring_view ext) noexcept {
    struct ExtEntry {
        std::wstring_view ext;
        FileFormat format;
    };

    // 仅列出扩展名与格式不完全同名（或需要分支）的项，其余由下面的通用规则覆盖
    static constexpr ExtEntry table[] = {
        { L"apng", FileFormat::Png },
        { L"jfif", FileFormat::Jpeg },
        { L"jpe", FileFormat::Jpeg },
        { L"dib", FileFormat::Bmp },
        { L"icon", FileFormat::Ico },
        { L"tif", FileFormat::Tiff },
        { L"avifs", FileFormat::Avif },
        { L"psdt", FileFormat::Psd },
        { L"pbm", FileFormat::Pnm },
        { L"pgm", FileFormat::Pnm },
        { L"ppm", FileFormat::Pnm },
        { L"pxm", FileFormat::Pnm },
        { L"pnm", FileFormat::Pnm },
        { L"ras", FileFormat::Ras },
        { L"sr", FileFormat::Ras },
        { L"jpeg", FileFormat::Jpeg },
        { L"tiff", FileFormat::Tiff },
        { L"psd", FileFormat::Psd },
        { L"qoi", FileFormat::Qoi },
        { L"svg", FileFormat::Svg },
        { L"dds", FileFormat::Dds },
        { L"blp", FileFormat::Blp },
        { L"pcx", FileFormat::Pcx },
        { L"pfm", FileFormat::Pfm },
        { L"exr", FileFormat::Exr },
        { L"jp2", FileFormat::Jp2 },
        { L"jxr", FileFormat::Jxr },
        { L"hdr", FileFormat::Hdr },
        { L"pic", FileFormat::Pic },
        { L"tga", FileFormat::Tga },
        { L"jxl", FileFormat::Jxl },
        { L"avif", FileFormat::Avif },
        { L"heic", FileFormat::Heif },
        { L"heif", FileFormat::Heif },
        { L"wp2", FileFormat::Wp2 },
        { L"livp", FileFormat::Livp },
        { L"lep", FileFormat::Lep },
        { L"bmp", FileFormat::Bmp },
        { L"png", FileFormat::Png },
        { L"gif", FileFormat::Gif },
        { L"jpg", FileFormat::Jpeg },
        { L"ico", FileFormat::Ico },
        { L"webp", FileFormat::WebP },
        { L"webm", FileFormat::Video },
        { L"mp4", FileFormat::Video },
        { L"mov", FileFormat::Video },
        { L"mkv", FileFormat::Video },
        { L"avi", FileFormat::Video },
        { L"wmv", FileFormat::Video },
        { L"flv", FileFormat::Video },
        { L"m4v", FileFormat::Video },
        { L"3gp", FileFormat::Video },
        { L"mts", FileFormat::Video },
        { L"m2ts", FileFormat::Video },
        { L"vob", FileFormat::Video },
        { L"evo", FileFormat::Video },
        { L"ts", FileFormat::Video },
        { L"mxf", FileFormat::Video },
    };

    for (const auto& entry : table) {
        if (entry.ext == ext)
            return entry.format;
    }

    return FileFormat::Unknown;
}

bool isExtensionAuthoritative(FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Raw:   // TIFF 魔数但需要 LibRaw 而非 libtiff
    case FileFormat::Video: // 容器魔法不足以确定解码方式
    case FileFormat::Livp:  // zip 容器，需按 livp 结构解析
    case FileFormat::Lep:   // 无文件头特征
    case FileFormat::Wp2:   // 实验格式，统一按扩展名处理
    case FileFormat::Tga:   // TGA 1.0 无签名
        return true;
    default:
        return false;
    }
}

std::string_view fileFormatName(FileFormat format) noexcept {
    switch (format) {
    case FileFormat::Unknown: return "Unknown";
    case FileFormat::Png:     return "PNG";
    case FileFormat::Gif:     return "GIF";
    case FileFormat::Jpeg:    return "JPEG";
    case FileFormat::WebP:    return "WebP";
    case FileFormat::Bmp:     return "BMP";
    case FileFormat::Ico:     return "ICO";
    case FileFormat::Tiff:    return "TIFF";
    case FileFormat::Jxl:     return "JXL";
    case FileFormat::Avif:    return "AVIF";
    case FileFormat::Heif:    return "HEIF";
    case FileFormat::Wp2:     return "WebP2";
    case FileFormat::Psd:     return "PSD";
    case FileFormat::Svg:     return "SVG";
    case FileFormat::Dds:     return "DDS";
    case FileFormat::Qoi:     return "QOI";
    case FileFormat::Blp:     return "BLP";
    case FileFormat::Pcx:     return "PCX";
    case FileFormat::Pfm:     return "PFM";
    case FileFormat::Exr:     return "EXR";
    case FileFormat::Jp2:     return "JP2";
    case FileFormat::Jxr:     return "JXR";
    case FileFormat::Hdr:     return "HDR";
    case FileFormat::Pic:     return "PIC";
    case FileFormat::Pnm:     return "PNM";
    case FileFormat::Ras:     return "RAS";
    case FileFormat::Tga:     return "TGA";
    case FileFormat::Livp:    return "LIVP";
    case FileFormat::Video:   return "Video";
    case FileFormat::Lep:     return "LEP";
    case FileFormat::Raw:     return "RAW";
    default:                  return "Unknown";
    }
}

} // namespace jark
