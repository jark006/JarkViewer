#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace jark {

struct PsdBitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasAlpha = false;
    std::vector<uint8_t> bgra;
    // 描述输出像素：CMYK/灰度 profile 转换后为 sRGB，不能再附回原始标签。
    std::vector<uint8_t> iccProfile;
};

// 优先读取“最大兼容性”合成图；Version Info 明确标记未保存合成图时，合成普通
// RGB 图层。支持 8/16 位及 Raw / PackBits / ZIP / ZIP prediction。
bool decodePsdComposite(std::span<const uint8_t> data, PsdBitmap& out,
    size_t maxPixels = (std::numeric_limits<size_t>::max)() / 8);

}
