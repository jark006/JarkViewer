#pragma once

#include <cstdint>
#include <vector>

namespace JarkThumbnail {
struct ThumbBitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasAlpha = false;
    std::vector<uint8_t> bgra;

    [[nodiscard]] bool empty() const noexcept {
        return width == 0 || height == 0 || bgra.empty();
    }
};
}
