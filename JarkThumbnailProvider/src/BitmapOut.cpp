#include "BitmapOut.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace JarkThumbnail {
namespace {
bool checkedPixelBytes(uint32_t width, uint32_t height, size_t* bytes) noexcept {
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t byteCount = pixelCount * 4ULL;
    if (byteCount > std::numeric_limits<size_t>::max()) {
        return false;
    }

    *bytes = static_cast<size_t>(byteCount);
    return true;
}

void calculateTargetSize(uint32_t width, uint32_t height, uint32_t maxEdge, uint32_t& targetWidth, uint32_t& targetHeight) noexcept {
    maxEdge = std::max<uint32_t>(1, maxEdge);
    const uint32_t longest = std::max(width, height);
    if (longest <= maxEdge) {
        targetWidth = width;
        targetHeight = height;
        return;
    }

    targetWidth = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(width) * maxEdge) / longest));
    targetHeight = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(height) * maxEdge) / longest));
}

std::vector<uint8_t> resizeNearest(const ThumbBitmap& source, uint32_t targetWidth, uint32_t targetHeight) {
    std::vector<uint8_t> resized(static_cast<size_t>(targetWidth) * targetHeight * 4ULL);

    for (uint32_t y = 0; y < targetHeight; ++y) {
        const uint32_t srcY = static_cast<uint32_t>((static_cast<uint64_t>(y) * source.height) / targetHeight);
        for (uint32_t x = 0; x < targetWidth; ++x) {
            const uint32_t srcX = static_cast<uint32_t>((static_cast<uint64_t>(x) * source.width) / targetWidth);
            const size_t srcOffset = (static_cast<size_t>(srcY) * source.width + srcX) * 4ULL;
            const size_t dstOffset = (static_cast<size_t>(y) * targetWidth + x) * 4ULL;
            resized[dstOffset + 0] = source.bgra[srcOffset + 0];
            resized[dstOffset + 1] = source.bgra[srcOffset + 1];
            resized[dstOffset + 2] = source.bgra[srcOffset + 2];
            resized[dstOffset + 3] = source.hasAlpha ? source.bgra[srcOffset + 3] : 255;
        }
    }

    return resized;
}

void normalizeAlpha(std::vector<uint8_t>& bgra, bool hasAlpha) noexcept {
    for (size_t offset = 0; offset + 3 < bgra.size(); offset += 4) {
        if (!hasAlpha) {
            bgra[offset + 3] = 255;
            continue;
        }

        const uint32_t alpha = bgra[offset + 3];
        bgra[offset + 0] = static_cast<uint8_t>((static_cast<uint32_t>(bgra[offset + 0]) * alpha + 127) / 255);
        bgra[offset + 1] = static_cast<uint8_t>((static_cast<uint32_t>(bgra[offset + 1]) * alpha + 127) / 255);
        bgra[offset + 2] = static_cast<uint8_t>((static_cast<uint32_t>(bgra[offset + 2]) * alpha + 127) / 255);
    }
}
}

HRESULT createThumbnailBitmap(const ThumbBitmap& source, UINT maxEdge, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType) noexcept {
    if (!bitmap || !alphaType) {
        return E_POINTER;
    }

    *bitmap = nullptr;
    *alphaType = WTSAT_UNKNOWN;

    size_t expectedBytes = 0;
    if (source.empty() || !checkedPixelBytes(source.width, source.height, &expectedBytes) ||
        source.bgra.size() < expectedBytes) {
        return E_INVALIDARG;
    }

    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    calculateTargetSize(source.width, source.height, maxEdge, targetWidth, targetHeight);

    std::vector<uint8_t> pixels = resizeNearest(source, targetWidth, targetHeight);
    normalizeAlpha(pixels, source.hasAlpha);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(targetWidth);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(targetHeight);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib || !dibBits) {
        if (dib) {
            DeleteObject(dib);
        }
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::copy(pixels.begin(), pixels.end(), static_cast<uint8_t*>(dibBits));
    *bitmap = dib;
    *alphaType = source.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
    return S_OK;
}
}
