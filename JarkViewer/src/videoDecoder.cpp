#include "videoDecoder.h"

#include "MediaDecoder.h"

// 视频解码 若 maxFrames > 0 则限制解码帧数，适用于预览等场景
std::vector<cv::Mat> DecodeVideoFrames(const uint8_t* videoBuffer, size_t size, size_t maxFrames) {
    std::vector<cv::Mat> frames;

    if (!videoBuffer || size < MIN_VIDEO_BUFF_SIZE) {
        JARK_LOG("Invalid video buffer: 0x{:X} or size: {} bytes",
            reinterpret_cast<std::uintptr_t>(videoBuffer), size);
        return frames;
    }

    // 实际的解码、缩放与旋转都在 MediaDecoder 中完成（实时播放复用同一实现）
    auto decoder = jark::MediaDecoder::open(std::span<const uint8_t>(videoBuffer, size));
    if (!decoder) {
        JARK_LOG("Cannot open video buffer ({} bytes)", size);
        return frames;
    }

    jark::MediaDecoder::Chunk chunk;
    while ((maxFrames == 0 || frames.size() < maxFrames) && decoder->readNext(chunk)) {
        if (chunk.type == jark::MediaDecoder::Chunk::Type::Video && !chunk.video.empty())
            frames.push_back(std::move(chunk.video));
    }

    return frames;
}
