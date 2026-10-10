#pragma once

// 内存媒体文件（视频/音频）解码。
//
// 一次解复用、按出现顺序产出「一个视频帧」或「一批音频样本」：这样调用方
// 不需要在两个流之间做队列同步，也不会因为交错顺序而丢包。
// 解码器内部持有 AVFormatContext，必须由单一线程调用。

#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

    // —— 信息面板（播放器按 I / Tab）用的补充信息 ——
    // 名字都是 FFmpeg 的短名（"h264"/"aac"），语言无关，不占多语言文案
    std::string formatName;      // 容器（解复用器名，如 "mov,mp4,m4a,3gp,3g2,mj2"）
    std::string videoCodec;      // 视频编码
    std::string audioCodec;      // 音频编码
    int64_t bitRate = 0;         // 容器总码率（bit/s，0 = 未知）
    int64_t audioBitRate = 0;    // 音轨码率（bit/s，0 = 未知）

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

    // 跳到指定位置（毫秒）：解复用器退到不晚于目标的关键帧，并冲掉解码器内部缓冲。
    // 只负责"定位到关键帧"——**精确落点由调用方丢弃 PTS 早于目标的帧完成**
    // （见 MediaPlayer 的落点过滤：从关键帧向前解码到目标，画面才真落在松手的位置上）。
    // 必须由持有该解码器的那一个线程调用（解码器内部状态不是线程安全的）。
    bool seek(int64_t ms);

    // 精确落点用：丢弃时间戳早于该值的视频帧与音频样本（< 0 表示不过滤）。
    // 视频侧关键是**在缩放与拷贝之前**就丢掉：从关键帧向前解码到目标要解上百帧，
    // 每帧 sws_scale + clone 就是几毫秒，那是落点耗时的大头（实测长 GOP 1080p
    // 一次精确落点 1.5s，其中绝大部分花在把要丢掉的帧也转换了一遍）。
    // 音频侧相反，必须**在 resample 之后**才裁（swr 的历史要连续喂着走），
    // 并把裁过的批的 ptsMs 一起往前推——播放端用它判断"到目标了没有"。
    // 只能由持有该解码器的那一个线程调用。
    void setSkipBeforeMs(int64_t ms) noexcept;

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

// 播放器信息面板（按 I / Tab 打开）的文本：文件属性 + 容器/流信息，多行 "标签: 值"。
// 纯函数（只读 MediaInfo 与文件属性），所以 --probe 也能拿它做断言。
std::string mediaInfoText(const std::wstring& path, const MediaInfo& info);

} // namespace jark
