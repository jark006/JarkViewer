#include "FormatSniffer.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace JarkThumbnail {
namespace {
bool startsWith(std::span<const uint8_t> data, std::initializer_list<uint8_t> magic) noexcept {
    if (data.size() < magic.size()) {
        return false;
    }

    return std::equal(magic.begin(), magic.end(), data.begin());
}

bool isAsciiWhitespace(uint8_t value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool matchesFourCc(std::span<const uint8_t> data, size_t offset, std::array<char, 4> fourCc) noexcept {
    return data.size() >= offset + fourCc.size() &&
        std::memcmp(data.data() + offset, fourCc.data(), fourCc.size()) == 0;
}

uint32_t readBe32(std::span<const uint8_t> data, size_t offset) noexcept {
    if (data.size() < offset + 4) {
        return 0;
    }

    return (static_cast<uint32_t>(data[offset]) << 24) |
        (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) |
        static_cast<uint32_t>(data[offset + 3]);
}

bool brandMatches(std::span<const uint8_t> data, size_t offset, std::array<char, 4> brand) noexcept {
    return matchesFourCc(data, offset, brand);
}

bool bmffHasBrand(std::span<const uint8_t> data, std::initializer_list<std::array<char, 4>> brands) noexcept {
    constexpr size_t scanLimit = 96;
    const size_t limit = std::min(data.size(), scanLimit);

    for (size_t boxOffset = 0; boxOffset + 12 <= limit; ++boxOffset) {
        if (!matchesFourCc(data, boxOffset + 4, { 'f', 't', 'y', 'p' })) {
            continue;
        }

        uint32_t boxSize = readBe32(data, boxOffset);
        if (boxSize < 16 || boxOffset + boxSize > data.size()) {
            boxSize = static_cast<uint32_t>(std::min<size_t>(data.size() - boxOffset, scanLimit));
        }

        const size_t brandEnd = boxOffset + boxSize;
        for (size_t brandOffset = boxOffset + 8; brandOffset + 4 <= brandEnd; brandOffset += 4) {
            for (const auto& brand : brands) {
                if (brandMatches(data, brandOffset, brand)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool looksLikeSvg(std::span<const uint8_t> data) noexcept {
    const size_t limit = std::min<size_t>(data.size(), 256);
    size_t offset = 0;
    while (offset < limit && isAsciiWhitespace(data[offset])) {
        ++offset;
    }

    if (offset + 4 <= limit && std::memcmp(data.data() + offset, "<svg", 4) == 0) {
        return true;
    }

    if (offset + 5 <= limit && std::memcmp(data.data() + offset, "<?xml", 5) == 0) {
        for (size_t i = offset + 5; i + 4 <= limit; ++i) {
            if (std::memcmp(data.data() + i, "<svg", 4) == 0) {
                return true;
            }
        }
    }

    return false;
}
}

Format sniffFormat(std::span<const uint8_t> data) noexcept {
    if (data.size() < 2) {
        return Format::Unknown;
    }

    if (startsWith(data, { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' }) ||
        startsWith(data, { 'G', 'I', 'F', '8' }) ||
        startsWith(data, { 0xFF, 0xD8, 0xFF }) ||
        startsWith(data, { 'R', 'I', 'F', 'F' })) {
        return Format::Unknown;
    }

    if (startsWith(data, { 'P', 'K', 0x03, 0x04 }) ||
        startsWith(data, { 'P', 'K', 0x05, 0x06 }) ||
        startsWith(data, { 'P', 'K', 0x07, 0x08 })) {
        return Format::Livp;
    }

    if (startsWith(data, { 0xFF, 0x0A }) ||
        bmffHasBrand(data, { { 'j', 'x', 'l', ' ' } })) {
        return Format::Jxl;
    }

    if (bmffHasBrand(data, { { 'a', 'v', 'i', 'f' }, { 'a', 'v', 'i', 's' } })) {
        return Format::Avif;
    }

    if (bmffHasBrand(data, {
        { 'h', 'e', 'i', 'c' }, { 'h', 'e', 'i', 'x' }, { 'h', 'e', 'v', 'c' },
        { 'h', 'e', 'v', 'x' }, { 'h', 'e', 'i', 'm' }, { 'h', 'e', 'i', 's' },
        { 'h', 'e', 'v', 'm' }, { 'h', 'e', 'v', 's' }, { 'm', 'i', 'f', '1' },
        { 'm', 's', 'f', '1' }
    })) {
        return Format::Heif;
    }

    if (startsWith(data, { '8', 'B', 'P', 'S' })) {
        return Format::Psd;
    }

    if (startsWith(data, { 'D', 'D', 'S', ' ' })) {
        return Format::Dds;
    }

    if (startsWith(data, { 'q', 'o', 'i', 'f' })) {
        return Format::Qoi;
    }

    if (startsWith(data, { 'B', 'L', 'P', '1' }) || startsWith(data, { 'B', 'L', 'P', '2' })) {
        return Format::Blp;
    }

    if (startsWith(data, { 'P', 'F', '\n' }) || startsWith(data, { 'P', 'f', '\n' })) {
        return Format::Pfm;
    }

    if (startsWith(data, { 0x76, 0x2F, 0x31, 0x01 })) {
        return Format::Exr;
    }

    if (bmffHasBrand(data, { { 'j', 'p', '2', ' ' }, { 'j', 'p', 'x', ' ' } }) ||
        startsWith(data, { 0x00, 0x00, 0x00, 0x0C, 'j', 'P', ' ', ' ' })) {
        return Format::Jp2;
    }

    if ((startsWith(data, { 'I', 'I', 0x2A, 0x00 }) || startsWith(data, { 'M', 'M', 0x00, 0x2A }) ||
        startsWith(data, { 'I', 'I', 0x2B, 0x00 }) || startsWith(data, { 'M', 'M', 0x00, 0x2B }))) {
        return Format::RawTiff;
    }

    if (startsWith(data, { 0x0A }) && data.size() >= 128) {
        return Format::Pcx;
    }

    if (startsWith(data, { '#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E' }) ||
        startsWith(data, { '#', '?', 'R', 'G', 'B', 'E' })) {
        return Format::Hdr;
    }

    if (startsWith(data, { 'P', '1' }) || startsWith(data, { 'P', '2' }) ||
        startsWith(data, { 'P', '3' }) || startsWith(data, { 'P', '4' }) ||
        startsWith(data, { 'P', '5' }) || startsWith(data, { 'P', '6' }) ||
        startsWith(data, { 'P', '7' })) {
        return Format::Pnm;
    }

    if (looksLikeSvg(data)) {
        return Format::Svg;
    }

    return Format::Unknown;
}
}
