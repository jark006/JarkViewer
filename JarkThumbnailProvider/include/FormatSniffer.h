#pragma once

#include <cstdint>
#include <span>

namespace JarkThumbnail {
enum class Format {
    Unknown = 0,
    Jxl,
    Avif,
    Heif,
    Wp2,
    Psd,
    Svg,
    Dds,
    RawTiff,
    Tga,
    Hdr,
    Pic,
    Jp2,
    Exr,
    Pcx,
    Pnm,
    Ras,
    Sr,
    Qoi,
    Pfm,
    Lep,
    Blp,
    Jxr,
    Livp,
};

Format sniffFormat(std::span<const uint8_t> data) noexcept;
}
