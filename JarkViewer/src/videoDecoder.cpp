#include "videoDecoder.h"

#include <vector>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <iostream>

// FFmpeg Headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
}

// OpenCV Headers
#include <opencv2/opencv.hpp>


// 自定义删除器，用于 RAII 管理 FFmpeg 资源
struct AvFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) avformat_close_input(&ctx);
    }
};

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx) avcodec_free_context(&ctx);
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};

struct AvPacketDeleter {
    void operator()(AVPacket* pkt) const {
        if (pkt) av_packet_free(&pkt);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* ctx) const {
        if (ctx) sws_freeContext(ctx);
    }
};

// 用于自定义 AVIO 上下文的私有数据结构
struct BufferContext {
    const uint8_t* buffer;
    size_t size;
    size_t offset;
};

// AVIO 读取回调
static int io_read_packet(void* opaque, uint8_t* buf, int buf_size) {
    BufferContext* ctx = static_cast<BufferContext*>(opaque);
    size_t remaining = ctx->size - ctx->offset;
    size_t to_copy = std::min(static_cast<size_t>(buf_size), remaining);

    if (to_copy == 0) return AVERROR_EOF;

    std::memcpy(buf, ctx->buffer + ctx->offset, to_copy);
    ctx->offset += to_copy;
    return static_cast<int>(to_copy);
}

// AVIO 寻址回调
static int64_t io_seek(void* opaque, int64_t offset, int whence) {
    BufferContext* ctx = static_cast<BufferContext*>(opaque);
    int64_t new_offset;

    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(ctx->size);
    }

    if (whence == SEEK_SET) {
        new_offset = offset;
    }
    else if (whence == SEEK_CUR) {
        new_offset = static_cast<int64_t>(ctx->offset) + offset;
    }
    else if (whence == SEEK_END) {
        new_offset = static_cast<int64_t>(ctx->size) + offset;
    }
    else {
        return -1;
    }

    if (new_offset < 0 || new_offset > static_cast<int64_t>(ctx->size)) {
        return -1;
    }

    ctx->offset = static_cast<size_t>(new_offset);
    return new_offset;
}

// 获取旋转角度 (度)
static int get_rotation_angle(AVStream* stream) {
    int32_t rotate = 0;
    if (!stream || !stream->codecpar) {
        return 0;
    }

    for (int i = 0; i < stream->codecpar->nb_coded_side_data; i++) {
        if (stream->codecpar->coded_side_data[i].type == AV_PKT_DATA_DISPLAYMATRIX) {
            rotate = (int)av_display_rotation_get(reinterpret_cast<const int32_t*>(stream->codecpar->coded_side_data[i].data));
            break;
        }
    }
    // FFmpeg 中正值表示逆时针，OpenCV 旋转枚举通常基于顺时针逻辑，这里统一转换为标准角度
    return -rotate;
}

// 主解码函数
std::vector<cv::Mat> DecodeVideoFrames(const uint8_t* videoBuffer, size_t size) {
    std::vector<cv::Mat> frames;

    if (!videoBuffer || size < MIN_VIDEO_BUFF_SIZE) {
        JARK_LOG("Invalid video buffer: 0x{:X} or size: {} bytes", reinterpret_cast<const size_t>(videoBuffer), size);
        return frames;
    }

    // 1. 初始化 FFmpeg (通常应用程序启动时调用一次即可，这里确保安全性)
    avformat_network_init();

    // 2. 准备自定义 IO 上下文
    BufferContext bufferCtx{ videoBuffer, size, 0 };
    unsigned char* ioBuffer = static_cast<unsigned char*>(av_malloc(4096));
    if (!ioBuffer) {
        JARK_LOG("bad_alloc");
        return frames;
    }

    AVIOContext* avioCtx = avio_alloc_context(ioBuffer, 4096, 0, &bufferCtx, io_read_packet, nullptr, io_seek);
    if (!avioCtx) {
        av_free(ioBuffer);
        JARK_LOG("Failed to allocate AVIOContext");
        return frames;
    }

    // 3. 打开输入格式上下文
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> formatCtx(avformat_alloc_context());
    if (!formatCtx) {
        avio_context_free(&avioCtx);
        JARK_LOG("Failed to allocate AVFormatContext");
        return frames;
    }

    formatCtx->pb = avioCtx;
    // 文件名设为空，因为我们是流式输入
    auto ptr = formatCtx.get();
    if (ptr == nullptr || avformat_open_input(&ptr, "", nullptr, nullptr) < 0) {
        JARK_LOG("Failed to open input stream");
        return frames;
    }

    if (avformat_find_stream_info(formatCtx.get(), nullptr) < 0) {
        JARK_LOG("Failed to find stream info");
        return frames;
    }

    // 4. 查找视频流
    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1) {
        JARK_LOG("No video stream found");
        return frames;
    }

    AVStream* videoStream = formatCtx->streams[videoStreamIndex];

    // 5. 获取解码器并打开
    const AVCodec* codec = avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
        JARK_LOG("Codec not found");
        return frames;
    }

    std::unique_ptr<AVCodecContext, AvCodecContextDeleter> codecCtx(avcodec_alloc_context3(codec));
    if (!codecCtx) {
        JARK_LOG("Failed to allocate codec context");
        return frames;
    }

    if (avcodec_parameters_to_context(codecCtx.get(), videoStream->codecpar) < 0) {
        JARK_LOG("Failed to copy codec parameters");
        return frames;
    }

    // 设置多线程解码 (可选，小视频单线程也够)
    codecCtx->thread_count = 4;

    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        JARK_LOG("Failed to open codec");
        return frames;
    }

    // 6. 准备颜色空间转换上下文 (YUV -> BGR)
    std::unique_ptr<SwsContext, SwsContextDeleter> swsCtx(sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    ));

    if (!swsCtx) {
        JARK_LOG("Failed to create SwsContext");
        return frames;
    }

    // 7. 准备帧和包
    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, AvFrameDeleter> rgbFrame(av_frame_alloc());
    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());

    if (!frame || !rgbFrame || !packet) {
        JARK_LOG("Failed to allocate frames/packets");
        return frames;
    }

    // 为 rgbFrame 分配缓冲区
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, codecCtx->width, codecCtx->height, 1);
    uint8_t* rgbBuffer = static_cast<uint8_t*>(av_malloc(numBytes * sizeof(uint8_t)));
    if (!rgbBuffer) {
        JARK_LOG("bad_alloc");
        return frames;
    }

    // 将 rgbBuffer 关联到 rgbFrame
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer,
        AV_PIX_FMT_BGR24, codecCtx->width, codecCtx->height, 1);

    // 获取旋转信息
    int rotation = get_rotation_angle(videoStream);

    // 8. 解码循环
    while (av_read_frame(formatCtx.get(), packet.get()) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            int ret = avcodec_send_packet(codecCtx.get(), packet.get());
            if (ret < 0) {
                char errStr[64] = { 0 };
                JARK_LOG("Error sending packet: {}", av_make_error_string(errStr, sizeof(errStr), ret));
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codecCtx.get(), frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                else if (ret < 0) {
                    char errStr[64] = { 0 };
                    JARK_LOG("Error decoding frame: {}", av_make_error_string(errStr, sizeof(errStr), ret));
                    break;
                }

                // 9. 颜色转换 (YUV -> BGR)
                sws_scale(swsCtx.get(), frame->data, frame->linesize, 0, codecCtx->height,
                    rgbFrame->data, rgbFrame->linesize);

                // 10. 创建 cv::Mat (注意：OpenCV 使用 BGR)
                // 数据是连续的，直接封装
                cv::Mat decodedMat(codecCtx->height, codecCtx->width, CV_8UC3, rgbFrame->data[0]);

                // 深拷贝数据，因为 rgbBuffer 会在下一帧被覆盖
                cv::Mat finalMat = decodedMat.clone();

                // 11. 处理旋转
                if (rotation != 0) {
                    int cvRotateCode = -1;
                    int normRot = (rotation % 360 + 360) % 360;

                    if (normRot == 90) {
                        cvRotateCode = cv::ROTATE_90_CLOCKWISE;
                    }
                    else if (normRot == 180) {
                        cvRotateCode = cv::ROTATE_180;
                    }
                    else if (normRot == 270) {
                        cvRotateCode = cv::ROTATE_90_COUNTERCLOCKWISE;
                    }

                    if (cvRotateCode >= 0) {
                        cv::rotate(finalMat, finalMat, cvRotateCode);
                    }
                }

                frames.push_back(std::move(finalMat));
            }
        }
        av_packet_unref(packet.get());
    }

    // av_free 会自动在 formatCtx 关闭时处理 ioBuffer，但为了清晰，这里主要依赖 unique_ptr 清理
    // 注意：avformat_close_input 会释放 avioCtx 和 ioBuffer

    return frames;
}