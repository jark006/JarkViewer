#include "PsdDecoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "lcms2.h"
#include "zlib.h"

namespace jark {
namespace {

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    bool take(uint64_t count, std::span<const uint8_t>& result) {
        if (count > bytes_.size())
            return false;
        result = bytes_.first(static_cast<size_t>(count));
        bytes_ = bytes_.subspan(static_cast<size_t>(count));
        return true;
    }

    bool number(size_t count, uint64_t& value) {
        std::span<const uint8_t> bytes;
        if (!take(count, bytes))
            return false;
        value = 0;
        for (const uint8_t byte : bytes)
            value = (value << 8) | byte;
        return true;
    }

    bool section(size_t lengthBytes, std::span<const uint8_t>& result) {
        uint64_t length = 0;
        return number(lengthBytes, length) && take(length, result);
    }

    std::span<const uint8_t> rest() const { return bytes_; }

private:
    std::span<const uint8_t> bytes_;
};

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

bool readResources(std::span<const uint8_t> resources, std::vector<uint8_t>& icc, bool& hasRealMergedData) {
    Reader reader(resources);
    while (!reader.rest().empty()) {
        std::span<const uint8_t> signature, name, payload;
        uint64_t id = 0, nameLength = 0, padding = 0;
        if (!reader.take(4, signature) || std::memcmp(signature.data(), "8BIM", 4) != 0 ||
            !reader.number(2, id) || !reader.number(1, nameLength) ||
            !reader.take(nameLength | 1ULL, name) || !reader.section(4, payload) ||
            ((payload.size() & 1) && !reader.number(1, padding)))
            return false;
        if (id == 1039) // ICC Profile
            icc.assign(payload.begin(), payload.end());
        if (id == 1057) { // Version Info：未开启“最大兼容性”时，文件尾部可能仅有白色占位图。
            if (payload.size() < 5)
                return false;
            hasRealMergedData = payload[4] != 0;
        }
    }
    return true;
}

bool wideLayerTag(std::string_view tag) {
    return tag == "LMsk" || tag == "Lr16" || tag == "Lr32" || tag == "Layr" ||
        tag == "Mt16" || tag == "Mt32" || tag == "Mtrn" || tag == "Alph" ||
        tag == "FMsk" || tag == "lnk2" || tag == "FEid" || tag == "FXid" || tag == "PxSD";
}

std::span<const uint8_t> layerInfo(std::span<const uint8_t> layers, bool large) {
    if (layers.empty())
        return {};
    Reader reader(layers);
    std::span<const uint8_t> info, mask;
    if (!reader.section(large ? 8 : 4, info))
        return {};
    if (info.size() >= 2)
        return info;
    // 16 位的图层信息可放在 Lr16 附加块，主图层区为空。额外 alpha/专色通道
    // 只有在图层计数为负时才是合成图透明度，不能仅凭“通道数多一个”判断。
    if (!reader.section(4, mask))
        return {};
    while (!reader.rest().empty()) {
        std::span<const uint8_t> signature, key, payload;
        if (!reader.take(4, signature) || !reader.take(4, key))
            return {};
        if (std::memcmp(signature.data(), "8BIM", 4) && std::memcmp(signature.data(), "8B64", 4))
            return {};
        const std::string_view tag(reinterpret_cast<const char*>(key.data()), 4);
        const bool wide = large && wideLayerTag(tag);
        if (!reader.section(wide ? 8 : 4, payload))
            return {};
        if ((tag == "Lr16" || tag == "Lr32" || tag == "Layr") && payload.size() >= 2)
            return payload;
        std::span<const uint8_t> padding;
        if (!reader.take((4 - payload.size() % 4) % 4, padding))
            return {};
    }
    return {};
}

bool unpackRow(std::span<const uint8_t> packed, std::span<uint8_t> row) {
    size_t written = 0;
    Reader reader(packed);
    uint64_t code = 0;
    while (reader.number(1, code)) {
        if (code == 128)
            continue;
        const size_t count = code < 128 ? static_cast<size_t>(code + 1) : static_cast<size_t>(257 - code);
        std::span<const uint8_t> bytes;
        if (count > row.size() - written || !reader.take(code < 128 ? count : 1, bytes))
            return false;
        if (code < 128)
            std::memcpy(row.data() + written, bytes.data(), count);
        else
            std::memset(row.data() + written, bytes[0], count);
        written += count;
    }
    return written == row.size();
}

// 流式解压允许 PSB 的输入/输出超过 zlib 单次调用的 32 位长度；每次只保留一行。
class InflateRows {
public:
    explicit InflateRows(std::span<const uint8_t> input) : input_(input) {
        ready_ = inflateInit(&stream_) == Z_OK;
    }
    ~InflateRows() { if (ready_) inflateEnd(&stream_); }

    bool row(std::span<uint8_t> output) {
        if (!ready_ || ended_ || output.size() > (std::numeric_limits<uInt>::max)())
            return false;
        stream_.next_out = output.data();
        stream_.avail_out = static_cast<uInt>(output.size());
        while (stream_.avail_out) {
            if (!stream_.avail_in) {
                const size_t count = (std::min)(input_.size(), size_t((std::numeric_limits<uInt>::max)()));
                stream_.next_in = const_cast<Bytef*>(input_.data());
                stream_.avail_in = static_cast<uInt>(count);
                input_ = input_.subspan(count);
            }
            const auto beforeIn = stream_.avail_in;
            const auto beforeOut = stream_.avail_out;
            const int status = inflate(&stream_, Z_NO_FLUSH);
            ended_ = status == Z_STREAM_END;
            if (ended_)
                return stream_.avail_out == 0;
            if (status != Z_OK || (beforeIn == stream_.avail_in && beforeOut == stream_.avail_out))
                return false;
        }
        return true;
    }

    bool finish() {
        if (!ended_) {
            uint8_t extra = 0;
            // 消耗校验尾部；多出一个像素字节或缺失 ZIP 尾部都必须失败。
            if (row(std::span<uint8_t>(&extra, 1)) || !ended_)
                return false;
        }
        return stream_.avail_in == 0 && input_.empty();
    }

private:
    z_stream stream_{};
    std::span<const uint8_t> input_;
    bool ready_ = false;
    bool ended_ = false;
};

bool decodePlane(Reader& reader, Reader& rowLengths, InflateRows& inflater, uint64_t compression,
    size_t width, size_t height, uint64_t depth, bool large, std::span<uint16_t> samples) {
    const size_t rowBytes = width * (depth / 8);
    std::vector<uint8_t> row(rowBytes);
    for (size_t y = 0; y < height; ++y) {
        std::span<const uint8_t> bytes;
        uint64_t length = rowBytes;
        if (compression == 1 && (!rowLengths.number(large ? 4 : 2, length) ||
            !reader.take(length, bytes) || !unpackRow(bytes, row)))
            return false;
        if (compression == 0) {
            if (!reader.take(rowBytes, bytes))
                return false;
            std::memcpy(row.data(), bytes.data(), rowBytes);
        }
        if (compression >= 2 && !inflater.row(row))
            return false;
        uint16_t previous = 0;
        for (size_t x = 0; x < width; ++x) {
            uint16_t value = depth == 8 ? row[x] : be16(row.data() + x * 2);
            if (compression == 3) {
                value = depth == 8 ? static_cast<uint8_t>(value + previous) : static_cast<uint16_t>(value + previous);
                previous = value;
            }
            if (!samples.empty())
                samples[y * width + x] = depth == 8 ? value * 257 : value;
        }
    }
    return true;
}

struct LayerChannel {
    int16_t id = 0;
    uint64_t length = 0;
    std::span<const uint8_t> data;
};

struct RasterLayer {
    std::array<int32_t, 4> rect{}; // top, left, bottom, right，允许超出画布。
    std::vector<LayerChannel> channels;
    uint8_t opacity = 255;
    bool visible = true;
    bool supported = true;
};

bool readRasterLayer(Reader& reader, bool large, RasterLayer& layer) {
    uint64_t value = 0, channelCount = 0;
    for (auto& coordinate : layer.rect) {
        if (!reader.number(4, value))
            return false;
        coordinate = std::bit_cast<int32_t>(static_cast<uint32_t>(value));
    }
    if (!reader.number(2, channelCount) || channelCount > 56)
        return false;
    for (size_t c = 0; c < channelCount; ++c) {
        LayerChannel channel;
        if (!reader.number(2, value) || !reader.number(large ? 8 : 4, channel.length))
            return false;
        channel.id = std::bit_cast<int16_t>(static_cast<uint16_t>(value));
        layer.channels.push_back(channel);
    }
    std::span<const uint8_t> blend, attributes, extra;
    if (!reader.take(8, blend) || std::memcmp(blend.data(), "8BIM", 4) ||
        !reader.take(4, attributes) || !reader.section(4, extra))
        return false;
    layer.opacity = attributes[0];
    layer.visible = !(attributes[2] & 2);
    layer.supported = std::memcmp(blend.data() + 4, "norm", 4) == 0 && attributes[1] == 0;
    Reader details(extra);
    std::span<const uint8_t> mask, ranges, name;
    if (!details.section(4, mask) || !details.section(4, ranges) || !details.number(1, value) ||
        !details.take(((value + 4) & ~3ULL) - 1, name))
        return false;
    // 用户/矢量蒙版、混合范围、图层效果等需要另外的渲染语义，不能忽略后当作成功。
    if (!mask.empty() && (mask.size() < 18 || !(mask[17] & 2)))
        layer.supported = false;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i] != (i % 4 < 2 ? 0 : 255))
            layer.supported = false;
    }
    while (!details.rest().empty()) {
        if (details.rest().size() < 12) {
            for (const uint8_t byte : details.rest()) {
                if (byte != 0)
                    return false;
            }
            break;
        }
        std::span<const uint8_t> signature, key, payload, padding;
        if (!details.take(4, signature) || !details.take(4, key) ||
            (std::memcmp(signature.data(), "8BIM", 4) && std::memcmp(signature.data(), "8B64", 4)))
            return false;
        const std::string_view tag(reinterpret_cast<const char*>(key.data()), 4);
        if (!details.section(large && wideLayerTag(tag) ? 8 : 4, payload) ||
            !details.take(payload.size() % 2, padding))
            return false;
        if (tag == "iOpa" && !payload.empty())
            layer.opacity = static_cast<uint8_t>((uint32_t(layer.opacity) * payload[0] + 127) / 255);
        if (tag == "lrFX" || tag == "lfx2" || tag == "lmfx" || tag == "vmsk" || tag == "vsms" ||
            tag == "brit" || tag == "levl" || tag == "curv" || tag == "expA" || tag == "vibA" ||
            tag == "hue2" || tag == "hue " || tag == "blnc" || tag == "blwh" || tag == "phfl" ||
            tag == "mixr" || tag == "clrL" || tag == "nvrt" || tag == "post" || tag == "thrs" ||
            tag == "grdm" || tag == "selc" || tag == "SoCo" || tag == "GdFl" || tag == "PtFl")
            layer.supported = false;
        if (payload.size() >= 4 && (payload[0] || payload[1] || payload[2] || payload[3])) {
            // 隐藏组也影响子图层的可见性，不能像普通隐藏图层那样直接跳过。
            if (tag == "lsct" || tag == "lsdk")
                return false;
            if (tag == "knko")
                layer.supported = false;
        }
    }
    return true;
}

bool decodeLayerChannel(std::span<const uint8_t> data, size_t width, size_t height,
    uint64_t depth, bool large, std::vector<uint16_t>& plane) {
    Reader reader(data);
    uint64_t compression = 0;
    std::span<const uint8_t> lengths;
    if (!reader.number(2, compression) || compression > 3 ||
        (compression == 0 && uint64_t(width) * height * (depth / 8) > reader.rest().size()) ||
        (compression == 1 && !reader.take(height * (large ? 4ULL : 2ULL), lengths)))
        return false;
    Reader rowLengths(lengths);
    InflateRows inflater(compression >= 2 ? reader.rest() : std::span<const uint8_t>{});
    plane.resize(width * height);
    return decodePlane(reader, rowLengths, inflater, compression, width, height, depth, large, plane) &&
        (compression < 2 || inflater.finish());
}

bool compositeRasterLayers(std::span<const uint8_t> info, bool large, uint64_t depth,
    size_t maxPixels, PsdBitmap& bitmap) {
    Reader reader(info);
    uint64_t rawCount = 0;
    if (!reader.number(2, rawCount))
        return false;
    const int count = std::bit_cast<int16_t>(static_cast<uint16_t>(rawCount));
    std::vector<RasterLayer> layers(static_cast<size_t>(count < 0 ? -count : count));
    if (layers.empty() || layers.size() > reader.rest().size() / 34)
        return false;
    for (auto& layer : layers) {
        if (!readRasterLayer(reader, large, layer))
            return false;
    }
    for (auto& layer : layers) {
        for (auto& channel : layer.channels) {
            if (!reader.take(channel.length, channel.data))
                return false;
        }
    }
    bitmap.bgra.assign(size_t(bitmap.width) * bitmap.height * 4, 0);
    // PSD 图层记录按从底到顶排列。保留透明画布，普通图层像素本身为直通 alpha，
    // 不能套用合成图的白底消光还原，否则黑色笔画边缘会被误改。
    for (const auto& layer : layers) {
        if (!layer.visible || layer.opacity == 0)
            continue;
        if (!layer.supported)
            return false;
        const int64_t height = int64_t(layer.rect[2]) - layer.rect[0];
        const int64_t width = int64_t(layer.rect[3]) - layer.rect[1];
        if (width < 0 || height < 0 || width > 300000 || height > 300000 ||
            uint64_t(width) * height > maxPixels)
            return false;
        if (!width || !height)
            continue;
        std::array<std::vector<uint16_t>, 4> planes;
        for (const auto& channel : layer.channels) {
            const int index = channel.id == -1 ? 3 : (channel.id >= 0 && channel.id < 3 ? channel.id : -1);
            if (index < 0 || index > 3)
                continue;
            if (!planes[index].empty() || !decodeLayerChannel(channel.data, static_cast<size_t>(width),
                static_cast<size_t>(height), depth, large, planes[index]))
                return false;
        }
        if (planes[0].empty() || planes[1].empty() || planes[2].empty())
            return false;
        const int64_t left = (std::max)(int64_t(0), int64_t(layer.rect[1]));
        const int64_t top = (std::max)(int64_t(0), int64_t(layer.rect[0]));
        const int64_t right = (std::min)(int64_t(bitmap.width), int64_t(layer.rect[3]));
        const int64_t bottom = (std::min)(int64_t(bitmap.height), int64_t(layer.rect[2]));
        for (int64_t y = top; y < bottom; ++y) {
            for (int64_t x = left; x < right; ++x) {
                const size_t i = static_cast<size_t>((y - layer.rect[0]) * width + x - layer.rect[1]);
                uint8_t* dst = bitmap.bgra.data() + (size_t(y) * bitmap.width + size_t(x)) * 4;
                const uint32_t alpha = ((planes[3].empty() ? 255 : (planes[3][i] + 128) / 257) * layer.opacity + 127) / 255;
                const uint32_t oldAlpha = dst[3];
                const uint32_t combined = alpha * 255 + oldAlpha * (255 - alpha);
                if (!combined)
                    continue;
                for (size_t c = 0; c < 3; ++c) {
                    const uint32_t value = (planes[2 - c][i] + 128) / 257;
                    dst[c] = static_cast<uint8_t>((value * alpha * 255 + uint32_t(dst[c]) * oldAlpha * (255 - alpha) + combined / 2) / combined);
                }
                dst[3] = static_cast<uint8_t>((combined + 127) / 255);
            }
        }
    }
    bitmap.hasAlpha = false;
    for (size_t i = 3; i < bitmap.bgra.size(); i += 4)
        bitmap.hasAlpha = bitmap.hasAlpha || bitmap.bgra[i] != 255;
    return true;
}

using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using Transform = std::unique_ptr<void, decltype(&cmsDeleteTransform)>;

bool convertPlanarColor(PsdBitmap& bitmap, const std::vector<std::vector<uint16_t>>& planes, bool cmyk) {
    Profile source(bitmap.iccProfile.empty() ? nullptr : cmsOpenProfileFromMem(
        bitmap.iccProfile.data(), static_cast<cmsUInt32Number>(bitmap.iccProfile.size())), cmsCloseProfile);
    Profile target(cmsCreate_sRGBProfile(), cmsCloseProfile);
    Transform transform(source && target && cmsGetColorSpace(source.get()) == (cmyk ? cmsSigCmykData : cmsSigGrayData) ?
        cmsCreateTransform(source.get(), cmyk ? TYPE_CMYK_16 : TYPE_GRAY_16, target.get(), TYPE_BGRA_8,
            INTENT_PERCEPTUAL, 0) : nullptr, cmsDeleteTransform);
    const size_t colors = cmyk ? 4 : 1;
    std::vector<uint16_t> row(size_t(bitmap.width) * colors);
    for (uint32_t y = 0; y < bitmap.height; ++y) {
        uint8_t* dst = bitmap.bgra.data() + size_t(y) * bitmap.width * 4;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            const size_t i = size_t(y) * bitmap.width + x;
            // Photoshop 的 CMYK 合成通道存储反相墨量；ICC 接口需要正常墨量。
            for (size_t c = 0; c < colors; ++c)
                row[size_t(x) * colors + c] = cmyk ? 65535 - planes[c][i] : planes[c][i];
            if (!cmyk && bitmap.hasAlpha) {
                const uint32_t alpha = planes[1][i];
                const uint32_t value = planes[0][i];
                row[x] = alpha ? static_cast<uint16_t>((std::min)(65535ULL,
                    uint64_t(value > 65535U - alpha ? value - (65535U - alpha) : 0) * 65535 / alpha)) : 0;
            }
            if (!transform && cmyk) {
                const uint32_t k = planes[3][i];
                constexpr uint64_t scale = uint64_t(65535) * 65535;
                for (size_t c = 0; c < 3; ++c)
                    dst[size_t(x) * 4 + c] = static_cast<uint8_t>((uint64_t(planes[2 - c][i]) * k * 255 + scale / 2) / scale);
            }
            else if (!transform) {
                std::fill_n(dst + size_t(x) * 4, 3, static_cast<uint8_t>((row[x] + 128) / 257));
            }
        }
        if (transform)
            cmsDoTransform(transform.get(), row.data(), dst, bitmap.width);
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            const size_t i = size_t(y) * bitmap.width + x;
            dst[size_t(x) * 4 + 3] = bitmap.hasAlpha ? static_cast<uint8_t>((planes[colors][i] + 128) / 257) : 255;
        }
    }
    // 即使原 profile 无效而用了算术兜底，输出也已是 RGB，后续不能重新读入 CMYK 标签。
    cmsUInt32Number size = 0;
    if (!target || !cmsSaveProfileToMem(target.get(), nullptr, &size))
        return false;
    bitmap.iccProfile.resize(size);
    return cmsSaveProfileToMem(target.get(), bitmap.iccProfile.data(), &size) != 0;
}

}

bool decodePsdComposite(std::span<const uint8_t> data, PsdBitmap& out, size_t maxPixels) {
    out = {};
    Reader reader(data);
    std::span<const uint8_t> signature, reserved, color, resources, layers;
    uint64_t version = 0, channels = 0, height = 0, width = 0, depth = 0, mode = 0, compression = 0;
    if (!reader.take(4, signature) || std::memcmp(signature.data(), "8BPS", 4) ||
        !reader.number(2, version) || (version != 1 && version != 2) || !reader.take(6, reserved) ||
        !reader.number(2, channels) || !reader.number(4, height) || !reader.number(4, width) ||
        !reader.number(2, depth) || !reader.number(2, mode))
        return false;
    const size_t baseChannels = mode == 1 ? 1 : mode == 3 ? 3 : mode == 4 ? 4 : 0;
    const uint64_t maxDimension = version == 1 ? 30000 : 300000;
    if (!baseChannels || channels < baseChannels || channels > 56 || (depth != 8 && depth != 16) ||
        !width || !height || width > maxDimension || height > maxDimension ||
        width * height > maxPixels || width * height > (std::numeric_limits<size_t>::max)() / 8 ||
        !reader.section(4, color) || !reader.section(4, resources) ||
        !reader.section(version == 2 ? 8 : 4, layers))
        return false;

    PsdBitmap bitmap;
    bitmap.width = static_cast<uint32_t>(width);
    bitmap.height = static_cast<uint32_t>(height);
    const auto info = layerInfo(layers, version == 2);
    bitmap.hasAlpha = channels > baseChannels && info.size() >= 2 && (be16(info.data()) & 0x8000) != 0;
    bool hasRealMergedData = true;
    if (!readResources(resources, bitmap.iccProfile, hasRealMergedData))
        return false;
    if (!hasRealMergedData) {
        if (mode != 3 || !compositeRasterLayers(info, version == 2, depth, maxPixels, bitmap))
            return false;
        out = std::move(bitmap);
        return true;
    }
    if (!reader.number(2, compression) || compression > 3)
        return false;
    const size_t pixelCount = static_cast<size_t>(width * height);
    const size_t rowBytes = static_cast<size_t>(width * (depth / 8));
    const size_t needed = baseChannels + (bitmap.hasAlpha ? 1 : 0);
    std::span<const uint8_t> lengths;
    if (compression == 0 && uint64_t(rowBytes) * height * channels > reader.rest().size())
        return false;
    if (compression == 1 && !reader.take(height * channels * (version == 2 ? 4 : 2), lengths))
        return false;
    Reader rowLengths(lengths);
    InflateRows inflater(compression >= 2 ? reader.rest() : std::span<const uint8_t>{});
    std::vector<std::vector<uint16_t>> planes(needed);
    for (auto& plane : planes)
        plane.resize(pixelCount);
    for (size_t c = 0; c < channels; ++c) {
        const auto samples = c < needed ? std::span<uint16_t>(planes[c]) : std::span<uint16_t>{};
        if (!decodePlane(reader, rowLengths, inflater, compression, static_cast<size_t>(width),
            static_cast<size_t>(height), depth, version == 2, samples))
            return false;
    }
    if (compression >= 2 && !inflater.finish())
        return false;
    bitmap.bgra.resize(pixelCount * 4);
    if (mode == 4 || (mode == 1 && !bitmap.iccProfile.empty())) {
        if (!convertPlanarColor(bitmap, planes, mode == 4))
            return false;
    }
    else {
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint16_t alpha = bitmap.hasAlpha ? planes[baseChannels][i] : 65535;
            for (size_t c = 0; c < 3; ++c) {
                uint32_t value = planes[mode == 1 ? 0 : 2 - c][i];
                // Photoshop 的 RGB 合成图带白底消光；显示链路需要直通 alpha。
                if (bitmap.hasAlpha)
                    value = alpha ? static_cast<uint32_t>((std::min)(65535ULL,
                        uint64_t(value > 65535U - alpha ? value - (65535U - alpha) : 0) * 65535 / alpha)) : 0;
                bitmap.bgra[i * 4 + c] = static_cast<uint8_t>((value + 128) / 257);
            }
            bitmap.bgra[i * 4 + 3] = static_cast<uint8_t>((alpha + 128) / 257);
        }
    }
    out = std::move(bitmap);
    return true;
}

}
