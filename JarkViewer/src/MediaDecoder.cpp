#include "MediaDecoder.h"

#include "Localization.h"
#include "jarkUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace jark {
namespace {

    constexpr int kIoBufferSize = 4096;
    constexpr size_t kMaxVideoFrameBytes = 1ull << 30;   // 单帧 1GiB 上限
    constexpr int kMaxAudioSamplesPerChunk = 1 << 18;    // 单批样本上限（约 5 秒 48kHz 立体声）

    void initFfmpegOnce() {
        static std::once_flag flag;
        std::call_once(flag, []() {
            avformat_network_init();
        });
    }

    std::string ffmpegError(int errorCode) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        return av_make_error_string(buffer, sizeof(buffer), errorCode);
    }

    struct AvMallocDeleter {
        void operator()(void* ptr) const { av_free(ptr); }
    };

    // avio_alloc_context 接收缓冲区所有权；释放时需先释放其内部缓冲区
    struct AvioContextDeleter {
        void operator()(AVIOContext* ctx) const {
            if (ctx) {
                av_freep(&ctx->buffer);
                avio_context_free(&ctx);
            }
        }
    };

    struct FormatContextDeleter {
        void operator()(AVFormatContext* ctx) const {
            if (ctx)
                avformat_close_input(&ctx);
        }
    };

    struct CodecContextDeleter {
        void operator()(AVCodecContext* ctx) const {
            if (ctx)
                avcodec_free_context(&ctx);
        }
    };

    struct FrameDeleter {
        void operator()(AVFrame* frame) const {
            if (frame)
                av_frame_free(&frame);
        }
    };

    struct PacketDeleter {
        void operator()(AVPacket* packet) const {
            if (packet)
                av_packet_free(&packet);
        }
    };

    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const {
            if (ctx)
                sws_freeContext(ctx);
        }
    };

    struct SwrContextDeleter {
        void operator()(SwrContext* ctx) const {
            if (ctx)
                swr_free(&ctx);
        }
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using AvioContextPtr = std::unique_ptr<AVIOContext, AvioContextDeleter>;

    // 自定义 AVIO：直接在内存缓冲区上读取
    struct BufferContext {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t offset = 0;
    };

    int ioReadPacket(void* opaque, uint8_t* buffer, int bufferSize) {
        auto* context = static_cast<BufferContext*>(opaque);
        if (!context || !buffer || bufferSize <= 0 || context->offset > context->size)
            return AVERROR(EINVAL);

        const size_t remaining = context->size - context->offset;
        const size_t toCopy = (std::min)(static_cast<size_t>(bufferSize), remaining);
        if (toCopy == 0)
            return AVERROR_EOF;

        std::memcpy(buffer, context->data + context->offset, toCopy);
        context->offset += toCopy;
        return static_cast<int>(toCopy);
    }

    int64_t ioSeek(void* opaque, int64_t offset, int whence) {
        auto* context = static_cast<BufferContext*>(opaque);
        if (!context)
            return AVERROR(EINVAL);

        const int64_t size = static_cast<int64_t>(context->size);
        const int64_t current = static_cast<int64_t>(context->offset);

        int64_t target = 0;
        if (whence == AVSEEK_SIZE)
            return size;

        if (whence == SEEK_SET)
            target = offset;
        else if (whence == SEEK_CUR)
            target = current + offset;
        else if (whence == SEEK_END)
            target = size + offset;
        else
            return AVERROR(EINVAL);

        if (target < 0 || target > size)
            return AVERROR(EINVAL);

        context->offset = static_cast<size_t>(target);
        return target;
    }

    int rotationFromStream(const AVStream* stream) {
        if (!stream || !stream->codecpar)
            return 0;

        for (int i = 0; i < stream->codecpar->nb_coded_side_data; ++i) {
            if (stream->codecpar->coded_side_data[i].type != AV_PKT_DATA_DISPLAYMATRIX)
                continue;

            const double angle = av_display_rotation_get(
                reinterpret_cast<const int32_t*>(stream->codecpar->coded_side_data[i].data));
            int rounded = static_cast<int>(std::lround(-angle)) % 360;
            if (rounded < 0)
                rounded += 360;
            return (rounded / 90) * 90;
        }
        return 0;
    }

} // namespace

struct MediaDecoder::Impl {
    BufferContext bufferContext;
    AvioContextPtr avioContext;
    FormatContextPtr formatContext;
    CodecContextPtr videoCodec;
    CodecContextPtr audioCodec;
    int videoStreamIndex = -1;
    int audioStreamIndex = -1;

    FramePtr decodeFrame{ av_frame_alloc() };
    FramePtr rgbFrame{ av_frame_alloc() };
    PacketPtr packet{ av_packet_alloc() };

    std::unique_ptr<SwsContext, SwsContextDeleter> swsContext;
    int swsSrcWidth = 0;
    int swsSrcHeight = 0;
    int swsDstWidth = 0;
    int swsDstHeight = 0;
    int swsSrcFormat = -1;

    std::unique_ptr<SwrContext, SwrContextDeleter> swrContext;
    int swrInputRate = 0;
    int swrInputChannels = 0;
    int swrInputFormat = -1;

    std::unique_ptr<uint8_t, AvMallocDeleter> videoBuffer;

    bool demuxEof = false;
    bool videoFlushSent = false;
    bool audioFlushSent = false;
    int64_t skipBeforeMs = -1;   // 精确落点：早于它的视频帧不转换、直接丢
    std::optional<int64_t> lastAudioPtsMs;
};

MediaDecoder::MediaDecoder() : impl_(std::make_unique<Impl>()) {}

MediaDecoder::~MediaDecoder() = default;

std::unique_ptr<MediaDecoder> MediaDecoder::open(std::span<const uint8_t> data, StreamFilter filter) {
    if (data.empty() || data.size() > static_cast<size_t>((std::numeric_limits<int64_t>::max)()))
        return nullptr;

    initFfmpegOnce();

    auto decoder = std::unique_ptr<MediaDecoder>(new MediaDecoder());
    auto& impl = *decoder->impl_;

    impl.bufferContext = BufferContext{ data.data(), data.size(), 0 };

    auto avioBuffer = static_cast<uint8_t*>(av_malloc(kIoBufferSize));
    if (!avioBuffer)
        return nullptr;

    AVIOContext* rawAvio = avio_alloc_context(
        avioBuffer, kIoBufferSize, 0, &impl.bufferContext, ioReadPacket, nullptr, ioSeek);
    if (!rawAvio) {
        av_free(avioBuffer);
        return nullptr;
    }
    impl.avioContext.reset(rawAvio);

    AVFormatContext* rawContext = avformat_alloc_context();
    if (!rawContext)
        return nullptr;

    rawContext->pb = impl.avioContext.get();
    rawContext->flags |= AVFMT_FLAG_CUSTOM_IO;

    const int openResult = avformat_open_input(&rawContext, "", nullptr, nullptr);
    if (openResult < 0) {
        JARK_LOG("avformat_open_input failed: {}", ffmpegError(openResult));
        avformat_free_context(rawContext);
        return nullptr;
    }
    impl.formatContext.reset(rawContext);

    if (avformat_find_stream_info(impl.formatContext.get(), nullptr) < 0) {
        JARK_LOG("avformat_find_stream_info failed");
        return nullptr;
    }

    impl.videoStreamIndex = av_find_best_stream(impl.formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    impl.audioStreamIndex = av_find_best_stream(impl.formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 按过滤条件把不要的那一路当作"没有"：不打开它的解码器，readNext 里也就不会
    // 把它的包送去解码（解复用仍要按文件顺序读过去，只是不花钱解码）
    if (filter == StreamFilter::VideoOnly)
        impl.audioStreamIndex = -1;
    else if (filter == StreamFilter::AudioOnly)
        impl.videoStreamIndex = -1;

    const auto openCodec = [&](int streamIndex) -> CodecContextPtr {
        if (streamIndex < 0)
            return nullptr;

        AVStream* stream = impl.formatContext->streams[streamIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            JARK_LOG("no decoder for codec id {}", static_cast<int>(stream->codecpar->codec_id));
            return nullptr;
        }

        CodecContextPtr context(avcodec_alloc_context3(codec));
        if (!context)
            return nullptr;

        if (const int copied = avcodec_parameters_to_context(context.get(), stream->codecpar); copied < 0) {
            JARK_LOG("avcodec_parameters_to_context failed: {}", ffmpegError(copied));
            return nullptr;
        }

        context->thread_count = 8;
        if (const int opened = avcodec_open2(context.get(), codec, nullptr); opened < 0) {
            // 别在这里静默失败：帧线程上下文初始化出错时 avcodec_open2 会返回错误，
            // 少了这行日志就只能看到"没有可用的解码器"，看不出原因
            JARK_LOG("avcodec_open2({}) failed: {}", codec->name, ffmpegError(opened));
            return nullptr;
        }

        return context;
    };

    impl.videoCodec = openCodec(impl.videoStreamIndex);
    impl.audioCodec = openCodec(impl.audioStreamIndex);

    if (impl.videoStreamIndex >= 0 && !impl.videoCodec) {
        JARK_LOG("no usable video decoder, ignoring video stream");
        impl.videoStreamIndex = -1;
    }
    if (impl.audioStreamIndex >= 0 && !impl.audioCodec) {
        JARK_LOG("no usable audio decoder, ignoring audio stream");
        impl.audioStreamIndex = -1;
    }
    if (impl.videoStreamIndex < 0 && impl.audioStreamIndex < 0)
        return nullptr;

    auto& info = decoder->info_;
    if (impl.videoStreamIndex >= 0) {
        AVStream* stream = impl.formatContext->streams[impl.videoStreamIndex];
        AVCodecContext* codec = impl.videoCodec.get();

        info.hasVideo = codec->width > 0 && codec->height > 0;
        info.width = codec->width;
        info.height = codec->height;
        info.rotationDegrees = rotationFromStream(stream);
        info.frameRate = av_q2d(stream->avg_frame_rate);
        if (info.frameRate <= 0.0)
            info.frameRate = av_q2d(stream->r_frame_rate);
        if (const char* name = avcodec_get_name(stream->codecpar->codec_id))
            info.videoCodec = name;
    }

    if (impl.audioStreamIndex >= 0) {
        AVStream* stream = impl.formatContext->streams[impl.audioStreamIndex];
        AVCodecContext* codec = impl.audioCodec.get();
        info.hasAudio = true;
        info.audioSampleRate = codec->sample_rate;
        info.audioChannels = codec->ch_layout.nb_channels;
        if (const char* name = avcodec_get_name(stream->codecpar->codec_id))
            info.audioCodec = name;
        info.audioBitRate = (std::max)(int64_t{ 0 },
            stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate : codec->bit_rate);
    }

    // 容器名与总码率（信息面板用；解复用器名形如 "mov,mp4,m4a,3gp,3g2,mj2"，够一眼看出封装）
    if (impl.formatContext->iformat && impl.formatContext->iformat->name)
        info.formatName = impl.formatContext->iformat->name;
    info.bitRate = (std::max)(int64_t{ 0 }, impl.formatContext->bit_rate);

    if (impl.formatContext->duration != AV_NOPTS_VALUE)
        info.durationMs = impl.formatContext->duration * 1000 / AV_TIME_BASE;

    JARK_LOG("media opened: video={} 编码 {}x{} 显示 {}x{} rot={} fps={:.2f} | audio={} {}Hz {}ch | duration={}ms",
        info.hasVideo, info.width, info.height, info.displayWidth(), info.displayHeight(),
        info.rotationDegrees, info.frameRate,
        info.hasAudio, info.audioSampleRate, info.audioChannels, info.durationMs);

    return decoder;
}

bool MediaDecoder::readNext(Chunk& chunk) {
    auto& impl = *impl_;

    while (true) {
        // 1) 先取出解码器内部已缓存的视频帧
        if (impl.videoCodec) {
            const int result = avcodec_receive_frame(impl.videoCodec.get(), impl.decodeFrame.get());
            if (result == 0) {
                const AVFrame* frame = impl.decodeFrame.get();

                const int srcWidth = frame->width > 0 ? frame->width : info_.width;
                const int srcHeight = frame->height > 0 ? frame->height : info_.height;
                if (srcWidth <= 0 || srcHeight <= 0) {
                    av_frame_unref(impl.decodeFrame.get());
                    continue;
                }

                // 时间戳要在转换之前算：早于目标的帧连缩放/拷贝都不做（见 setSkipBeforeMs）
                const AVRational videoTimeBase = impl.formatContext->streams[impl.videoStreamIndex]->time_base;
                const int64_t framePts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp : frame->pts;
                const int64_t framePtsMs = framePts == AV_NOPTS_VALUE ? 0
                    : av_rescale_q(framePts, videoTimeBase, AVRational{ 1, 1000 });

                if (impl.skipBeforeMs >= 0 && framePtsMs < impl.skipBeforeMs) {
                    av_frame_unref(impl.decodeFrame.get());
                    continue;
                }

                int dstWidth = srcWidth;
                int dstHeight = srcHeight;
                const int longEdge = (std::max)(srcWidth, srcHeight);
                if (longEdge > kMaxVideoEdge) {
                    const double scale = static_cast<double>(kMaxVideoEdge) / longEdge;
                    dstWidth = (std::max)(1, static_cast<int>(std::lround(srcWidth * scale)));
                    dstHeight = (std::max)(1, static_cast<int>(std::lround(srcHeight * scale)));
                }

                const bool needNewContext = !impl.swsContext ||
                    impl.swsSrcWidth != srcWidth || impl.swsSrcHeight != srcHeight ||
                    impl.swsDstWidth != dstWidth || impl.swsDstHeight != dstHeight ||
                    impl.swsSrcFormat != frame->format;

                if (needNewContext) {
                    impl.swsContext.reset(sws_getContext(srcWidth, srcHeight,
                        static_cast<AVPixelFormat>(frame->format), dstWidth, dstHeight, AV_PIX_FMT_BGR24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr));

                    const int bytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dstWidth, dstHeight, 1);
                    if (bytes > 0 && static_cast<size_t>(bytes) <= kMaxVideoFrameBytes)
                        impl.videoBuffer.reset(static_cast<uint8_t*>(av_malloc(static_cast<size_t>(bytes))));
                    else
                        impl.videoBuffer.reset();

                    impl.swsSrcWidth = srcWidth;
                    impl.swsSrcHeight = srcHeight;
                    impl.swsDstWidth = dstWidth;
                    impl.swsDstHeight = dstHeight;
                    impl.swsSrcFormat = frame->format;
                }

                if (impl.swsContext && impl.videoBuffer &&
                    av_image_fill_arrays(impl.rgbFrame->data, impl.rgbFrame->linesize, impl.videoBuffer.get(),
                        AV_PIX_FMT_BGR24, dstWidth, dstHeight, 1) >= 0) {
                    const int scaledRows = sws_scale(impl.swsContext.get(), frame->data, frame->linesize, 0, srcHeight,
                        impl.rgbFrame->data, impl.rgbFrame->linesize);
                    if (scaledRows > 0) {
                        chunk = Chunk{};
                        chunk.type = Chunk::Type::Video;
                        chunk.video = cv::Mat(dstHeight, dstWidth, CV_8UC3, impl.rgbFrame->data[0],
                            impl.rgbFrame->linesize[0]).clone();
                        chunk.ptsMs = framePtsMs;

                        if (info_.rotationDegrees == 90)
                            cv::rotate(chunk.video, chunk.video, cv::ROTATE_90_CLOCKWISE);
                        else if (info_.rotationDegrees == 180)
                            cv::rotate(chunk.video, chunk.video, cv::ROTATE_180);
                        else if (info_.rotationDegrees == 270)
                            cv::rotate(chunk.video, chunk.video, cv::ROTATE_90_COUNTERCLOCKWISE);

                        av_frame_unref(impl.decodeFrame.get());
                        return true;
                    }
                }

                av_frame_unref(impl.decodeFrame.get());
                continue;
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
                JARK_LOG("video decode error: {}", ffmpegError(result));
        }

        // 2) 再取出音频帧
        if (impl.audioCodec) {
            const int result = avcodec_receive_frame(impl.audioCodec.get(), impl.decodeFrame.get());
            if (result == 0) {
                const AVFrame* frame = impl.decodeFrame.get();

                const bool needNewContext = !impl.swrContext ||
                    impl.swrInputRate != frame->sample_rate ||
                    impl.swrInputChannels != frame->ch_layout.nb_channels ||
                    impl.swrInputFormat != frame->format;

                if (needNewContext) {
                    SwrContext* rawSwr = nullptr;
                    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
                    const int initResult = swr_alloc_set_opts2(&rawSwr,
                        &outputLayout, AV_SAMPLE_FMT_S16, kOutputSampleRate,
                        &frame->ch_layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
                        0, nullptr);

                    impl.swrContext.reset(rawSwr);
                    if (initResult < 0 || !impl.swrContext || swr_init(impl.swrContext.get()) < 0) {
                        JARK_LOG("swr init failed: {}", ffmpegError(initResult));
                        impl.swrContext.reset();
                    }

                    impl.swrInputRate = frame->sample_rate;
                    impl.swrInputChannels = frame->ch_layout.nb_channels;
                    impl.swrInputFormat = frame->format;
                }

                if (impl.swrContext) {
                    int64_t outSamples = av_rescale_rnd(
                        swr_get_delay(impl.swrContext.get(), frame->sample_rate) + frame->nb_samples,
                        kOutputSampleRate, frame->sample_rate, AV_ROUND_UP);
                    outSamples = (std::min)(outSamples, static_cast<int64_t>(kMaxAudioSamplesPerChunk));

                    if (outSamples > 0) {
                        std::vector<int16_t> samples(static_cast<size_t>(outSamples) * kOutputChannels);
                        uint8_t* outputPlanes[1] = { reinterpret_cast<uint8_t*>(samples.data()) };

                        const int converted = swr_convert(impl.swrContext.get(), outputPlanes,
                            static_cast<int>(outSamples),
                            const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);

                        if (converted > 0) {
                            chunk = Chunk{};
                            chunk.type = Chunk::Type::Audio;
                            samples.resize(static_cast<size_t>(converted) * kOutputChannels);
                            chunk.audio = std::move(samples);

                            const AVRational timeBase = impl.formatContext->streams[impl.audioStreamIndex]->time_base;
                            const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? frame->best_effort_timestamp : frame->pts;

                            if (pts != AV_NOPTS_VALUE) {
                                chunk.ptsMs = av_rescale_q(pts, timeBase, AVRational{ 1, 1000 });
                            }
                            else {
                                const int64_t durationMs = static_cast<int64_t>(converted) * 1000 / kOutputSampleRate;
                                chunk.ptsMs = impl.lastAudioPtsMs ? *impl.lastAudioPtsMs + durationMs : 0;
                            }

                            // 精确落点：早于目标的音频一律不出声（与视频的 skipBeforeMs 同一语义，
                            // 但**先 resample 再裁**——swr 的内部历史要连续喂着走，直接跳过输入帧
                            // 会在接缝处留下一声爆音）。整批都在目标之前就整批丢掉，下一批接着裁。
                            if (impl.skipBeforeMs >= 0 && chunk.ptsMs < impl.skipBeforeMs) {
                                const int64_t skipFrames = (impl.skipBeforeMs - chunk.ptsMs) *
                                    kOutputSampleRate / 1000;
                                const int64_t totalFrames = static_cast<int64_t>(chunk.audio.size()) /
                                    kOutputChannels;
                                if (skipFrames >= totalFrames) {
                                    av_frame_unref(impl.decodeFrame.get());
                                    continue;
                                }
                                chunk.audio.erase(chunk.audio.begin(),
                                    chunk.audio.begin() + static_cast<size_t>(skipFrames) * kOutputChannels);
                                chunk.ptsMs += skipFrames * 1000 / kOutputSampleRate;
                            }

                            impl.lastAudioPtsMs = chunk.ptsMs;
                            av_frame_unref(impl.decodeFrame.get());
                            return true;
                        }
                    }
                }

                av_frame_unref(impl.decodeFrame.get());
                continue;
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
                JARK_LOG("audio decode error: {}", ffmpegError(result));
        }

        // 3) 需要新数据包
        if (!impl.demuxEof) {
            const int readResult = av_read_frame(impl.formatContext.get(), impl.packet.get());
            if (readResult < 0) {
                impl.demuxEof = true;
            }
            else {
                const int streamIndex = impl.packet->stream_index;
                if (streamIndex == impl.videoStreamIndex && impl.videoCodec) {
                    if (avcodec_send_packet(impl.videoCodec.get(), impl.packet.get()) < 0)
                        JARK_LOG("video: send packet failed");
                }
                else if (streamIndex == impl.audioStreamIndex && impl.audioCodec) {
                    if (avcodec_send_packet(impl.audioCodec.get(), impl.packet.get()) < 0)
                        JARK_LOG("audio: send packet failed");
                }
                av_packet_unref(impl.packet.get());
                continue;
            }
        }

        // 4) 输入结束后冲空解码器
        if (impl.videoCodec && !impl.videoFlushSent) {
            impl.videoFlushSent = true;
            avcodec_send_packet(impl.videoCodec.get(), nullptr);
            continue;
        }
        if (impl.audioCodec && !impl.audioFlushSent) {
            impl.audioFlushSent = true;
            avcodec_send_packet(impl.audioCodec.get(), nullptr);
            continue;
        }

        chunk = Chunk{};
        chunk.type = Chunk::Type::End;
        return false;
    }
}

void MediaDecoder::setSkipBeforeMs(int64_t ms) noexcept {
    impl_->skipBeforeMs = ms;
}

bool MediaDecoder::seek(int64_t ms) {
    auto& impl = *impl_;
    if (!impl.formatContext)
        return false;

    // 按**本实例负责的那条流** seek，不用 stream_index = -1（默认流）："视频+音频"的
    // MP4 默认流是视频流，音频实例跟着跳到**视频关键帧**上，于是 seek 之后会先送出最多
    // 一个 GOP 的旧音频（听感就是半秒到几秒的杂音/像在快放），之后才接上正确位置。
    // 音频流每个包都是关键帧，自己 seek 只差一帧；视频流仍按 AVSEEK_FLAG_BACKWARD 落到
    // 关键帧，再由 setSkipBeforeMs 丢掉目标之前的帧补齐到目标。
    int streamIndex = -1;
    if (impl.videoCodec && impl.videoStreamIndex >= 0)
        streamIndex = impl.videoStreamIndex;
    else if (impl.audioCodec && impl.audioStreamIndex >= 0)
        streamIndex = impl.audioStreamIndex;

    // 给了 stream_index 就得用**该流的时间基**（只有不给时才是 AV_TIME_BASE 微秒）
    // AVSEEK_FLAG_BACKWARD 保证落到"不晚于目标"的关键帧
    int64_t timestamp = ms > 0 ? ms * 1000 : 0;
    if (streamIndex >= 0) {
        const AVRational timeBase = impl.formatContext->streams[streamIndex]->time_base;
        timestamp = av_rescale_q(ms > 0 ? ms : 0, AVRational{ 1, 1000 }, timeBase);
    }

    const int result = av_seek_frame(impl.formatContext.get(), streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        JARK_LOG("av_seek_frame({}ms) failed: {}", ms, ffmpegError(result));
        return false;
    }

    // 解码器内部还留着旧位置的帧与解析状态（B 帧还要靠它们），必须冲掉；
    // 解复用器侧的 EOF / "已冲空解码器"标记同理，否则 seek 之后读不出新数据
    if (impl.videoCodec) avcodec_flush_buffers(impl.videoCodec.get());
    if (impl.audioCodec) avcodec_flush_buffers(impl.audioCodec.get());
    av_packet_unref(impl.packet.get());
    if (impl.decodeFrame) av_frame_unref(impl.decodeFrame.get());
    impl.demuxEof = false;
    impl.videoFlushSent = false;
    impl.audioFlushSent = false;
    impl.lastAudioPtsMs.reset();
    return true;
}

namespace {

    // 信息面板的时长：带上毫秒（短的实况视频/音频只在毫秒上分得出差别）
    std::string formatDurationMs(int64_t ms) {
        if (ms < 0)
            ms = 0;

        const int64_t hours = ms / 3600000;
        const int64_t minutes = (ms / 60000) % 60;
        const int64_t seconds = (ms / 1000) % 60;
        const int64_t millis = ms % 1000;

        return hours > 0
            ? std::format("{}:{:02d}:{:02d}.{:03d}", hours, minutes, seconds, millis)
            : std::format("{:02d}:{:02d}.{:03d}", minutes, seconds, millis);
    }

    std::string formatBitRate(int64_t bitsPerSecond) {
        return std::format("{} kb/s", (bitsPerSecond + 500) / 1000);
    }

} // namespace

std::string mediaInfoText(const std::wstring& path, const MediaInfo& info) {
    std::error_code errorCode;
    const auto fileSize = std::filesystem::file_size(path, errorCode);

    // 与看图那条 EXIF 面板同一个排版：每行 "标签: 值"。标签取现有文案
    // （39 路径 / 40 大小 / 164 视频 / 190 音频），流参数用 FFmpeg 的短名与单位，
    // 语言无关，不占多语言表
    std::string text = std::format("{}: {}\n{}: {}\n{}: {}\n",
        getUIString(39), jarkUtils::wstringToUtf8(path),
        getUIString(40), errorCode ? std::string("-") : jarkUtils::size2Str(fileSize),
        getUIString(191), formatDurationMs(info.durationMs));

    if (!info.formatName.empty())
        text += std::format("{}: {}\n", getUIString(192), info.formatName);
    if (info.bitRate > 0)
        text += std::format("{}: {}\n", getUIString(193), formatBitRate(info.bitRate));

    if (info.hasVideo) {
        text += std::format("{}: {} {}x{}", getUIString(164),
            info.videoCodec.empty() ? "?" : info.videoCodec, info.width, info.height);
        if (info.displayWidth() != info.width || info.displayHeight() != info.height)
            text += std::format(" → {}x{}", info.displayWidth(), info.displayHeight());
        if (info.rotationDegrees != 0)
            text += std::format(" rot={}", info.rotationDegrees);
        if (info.frameRate > 0.0)
            text += std::format(" {:.2f}fps", info.frameRate);
        text += '\n';
    }

    if (info.hasAudio) {
        text += std::format("{}: {} {}Hz {}ch", getUIString(190),
            info.audioCodec.empty() ? "?" : info.audioCodec, info.audioSampleRate, info.audioChannels);
        if (info.audioBitRate > 0)
            text += std::format(" {}", formatBitRate(info.audioBitRate));
        text += '\n';
    }

    return text;
}

} // namespace jark
