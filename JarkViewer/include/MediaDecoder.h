#pragma once

// 内存媒体文件（视频/音频）解码。
//
// 一次解复用、按出现顺序产出「一个视频帧」或「一批音频样本」：这样调用方
// 不需要在两个流之间做队列同步，也不会因为交错顺序而丢包。
// 解码器内部持有 AVFormatContext，必须由单一线程调用。

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <opencv2/opencv.hpp>

namespace jark {

struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;             // 编码尺寸（未旋转）
    int height = 0;
    int rotationDegrees = 0;   // 元数据里的显示旋转角（0/90/180/270）
    double frameRate = 0.0;
    int64_t durationMs = 0;
    int audioSampleRate = 0;
    int audioChannels = 0;

    // 交给播放端的帧是**按 rotationDegrees 旋转过**的（见 MediaDecoder 里对 Chunk::video
    // 的 cv::rotate），所以"播放端看到的尺寸"要跟着换：手机竖拍视频的编码尺寸是横的，
    // 拿 width/height 当显示尺寸就会把竖帧塞进横框里拉伸（实况照片播放时尤其明显）。
    int displayWidth() const { return rotationDegrees % 180 != 0 ? height : width; }
    int displayHeight() const { return rotationDegrees % 180 != 0 ? width : height; }
};

class MediaDecoder {
public:
    ~MediaDecoder();

    // 只解某一路流：视频与音频各自需要独立的解复用进度时才开两个实例
    // （两个实例各读一遍同一段内存，被排除那一路的包只解复用不送去解码）。
    // 见 MediaPlayer 为什么要拆成两个线程：视频要按播放进度背压，音频必须永远跑在前面。
    enum class StreamFilter { Both, VideoOnly, AudioOnly };

    // 打开内存中的媒体数据；失败返回 nullptr
    static std::unique_ptr<MediaDecoder> open(std::span<const uint8_t> data,
        StreamFilter filter = StreamFilter::Both);

    const MediaInfo& info() const noexcept { return info_; }

    struct Chunk {
        enum class Type { Video, Audio, End };

        Type type = Type::End;
        cv::Mat video;                // BGR，已按元数据旋转
        std::vector<int16_t> audio;   // 交错立体声（见 kOutputChannels），采样率 kOutputSampleRate
        int64_t ptsMs = 0;            // 该帧/该批样本的显示时间
    };

    // 取下一个解码结果；返回 false 表示已结束（chunk.type == End）
    bool readNext(Chunk& chunk);

    // 音频统一转换到的输出格式
    static constexpr int kOutputSampleRate = 48000;
    static constexpr int kOutputChannels = 2;

    // 超过该边长的高分辨率视频会等比缩小（预览用途，控制内存与耗时）
    static constexpr int kMaxVideoEdge = 1920;

private:
    MediaDecoder();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    MediaInfo info_;
};

} // namespace jark
