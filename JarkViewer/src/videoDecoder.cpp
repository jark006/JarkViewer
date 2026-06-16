#include "videoDecoder.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
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
#include <libavutil/mem.h>
}

// OpenCV Headers
#include <opencv2/opencv.hpp>


// 自定义删除器，用于 RAII 管理 FFmpeg 资源
struct AvMallocDeleter {
    void operator()(void* ptr) const {
        av_free(ptr);
    }
};

struct AvioContextDeleter {
    void operator()(AVIOContext* ctx) const {
        if (ctx) {
            av_freep(&ctx->buffer);
            avio_context_free(&ctx);
        }
    }
};

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

constexpr int AVIO_BUFFER_SIZE = 4096;
constexpr size_t MAX_RGB_FRAME_BUFFER_BYTES = 1024ULL * 1024ULL * 1024ULL;

static bool checked_add_i64(int64_t lhs, int64_t rhs, int64_t& result) {
    if ((rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs)) {
        return false;
    }

    result = lhs + rhs;
    return true;
}

static void init_ffmpeg_network_once() {
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        avformat_network_init();
    });
}

// AVIO 读取回调
static int io_read_packet(void* opaque, uint8_t* buf, int buf_size) {
    BufferContext* ctx = static_cast<BufferContext*>(opaque);
    if (!ctx || !buf || buf_size <= 0 || ctx->offset > ctx->size) {
        return AVERROR(EINVAL);
    }

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
    if (!ctx || ctx->size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        return AVERROR(EINVAL);
    }

    int64_t new_offset = 0;
    const int64_t size = static_cast<int64_t>(ctx->size);
    const int64_t current = static_cast<int64_t>(ctx->offset);

    if (whence == AVSEEK_SIZE) {
        return size;
    }

    if (whence == SEEK_SET) {
        new_offset = offset;
    }
    else if (whence == SEEK_CUR) {
        if (!checked_add_i64(current, offset, new_offset)) {
            return AVERROR(EINVAL);
        }
    }
    else if (whence == SEEK_END) {
        if (!checked_add_i64(size, offset, new_offset)) {
            return AVERROR(EINVAL);
        }
    }
    else {
        return AVERROR(EINVAL);
    }

    if (new_offset < 0 || new_offset > size) {
        return AVERROR(EINVAL);
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

// 视频解码 若 maxFrames > 0 则限制解码帧数，适用于预览等场景
std::vector<cv::Mat> DecodeVideoFrames(const uint8_t* videoBuffer, size_t size, size_t maxFrames) {
    std::vector<cv::Mat> frames;

    if (!videoBuffer || size < MIN_VIDEO_BUFF_SIZE) {
        JARK_LOG("Invalid video buffer: 0x{:X} or size: {} bytes", reinterpret_cast<std::uintptr_t>(videoBuffer), size);
        return frames;
    }
    if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        JARK_LOG("Video buffer is too large: {} bytes", size);
        return frames;
    }

    // 1. 初始化 FFmpeg (通常应用程序启动时调用一次即可，这里确保安全性)
    init_ffmpeg_network_once();

    // 2. 准备自定义 IO 上下文
    BufferContext bufferCtx{ videoBuffer, size, 0 };
    std::unique_ptr<uint8_t, AvMallocDeleter> ioBuffer(static_cast<uint8_t*>(av_malloc(AVIO_BUFFER_SIZE)));
    if (!ioBuffer) {
        JARK_LOG("bad_alloc");
        return frames;
    }

    std::unique_ptr<AVIOContext, AvioContextDeleter> avioCtx(avio_alloc_context(
        ioBuffer.get(), AVIO_BUFFER_SIZE, 0, &bufferCtx, io_read_packet, nullptr, io_seek
    ));
    if (!avioCtx) {
        JARK_LOG("Failed to allocate AVIOContext");
        return frames;
    }
    // avio_alloc_context() takes ownership of the current buffer pointer.
    ioBuffer.release();

    // 3. 打开输入格式上下文
    std::unique_ptr<AVFormatContext, AvFormatContextDeleter> formatCtx(avformat_alloc_context());
    if (!formatCtx) {
        JARK_LOG("Failed to allocate AVFormatContext");
        return frames;
    }

    formatCtx->pb = avioCtx.get();
    // Keep custom IO ownership with avioCtx; formatCtx is destroyed first.
    formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // 文件名设为空，因为我们是流式输入
    auto rawFormatCtx = formatCtx.release();
    int result = avformat_open_input(&rawFormatCtx, "", nullptr, nullptr);
    if (result < 0) {
        char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        JARK_LOG("Failed to open input stream: {}", av_make_error_string(errStr, sizeof(errStr), result));
        return frames;
    }
    formatCtx.reset(rawFormatCtx);

    result = avformat_find_stream_info(formatCtx.get(), nullptr);
    if (result < 0) {
        char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        JARK_LOG("Failed to find stream info: {}", av_make_error_string(errStr, sizeof(errStr), result));
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
    codecCtx->thread_count = 8;

    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        JARK_LOG("Failed to open codec");
        return frames;
    }

    if (codecCtx->width <= 0 || codecCtx->height <= 0 ||
        av_image_check_size(static_cast<unsigned int>(codecCtx->width), static_cast<unsigned int>(codecCtx->height), 0, nullptr) < 0) {
        JARK_LOG("Invalid video frame size: {}x{}", codecCtx->width, codecCtx->height);
        return frames;
    }

    // 计算目标尺寸：如果宽或高大于 1920，缩放到最长边为 1920
    int srcWidth = codecCtx->width;
    int srcHeight = codecCtx->height;
    int dstWidth = srcWidth;
    int dstHeight = srcHeight;

    if (srcWidth > 1920 || srcHeight > 1920) {
        if (srcWidth >= srcHeight) {
            // 宽边更长或相等
            dstWidth = 1920;
            dstHeight = static_cast<int>(srcHeight * 1920.0 / srcWidth + 0.5); // 四舍五入
        } else {
            // 高边更长
            dstHeight = 1920;
            dstWidth = static_cast<int>(srcWidth * 1920.0 / srcHeight + 0.5); // 四舍五入
        }
        // 确保尺寸至少为 1
        if (dstWidth < 1) dstWidth = 1;
        if (dstHeight < 1) dstHeight = 1;
    }

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, dstWidth, dstHeight, 1);
    if (numBytes <= 0) {
        char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        JARK_LOG("Invalid RGB frame buffer size: {} ({})", numBytes, av_make_error_string(errStr, sizeof(errStr), numBytes));
        return frames;
    }
    if (static_cast<size_t>(numBytes) > MAX_RGB_FRAME_BUFFER_BYTES) {
        JARK_LOG("RGB frame buffer is too large: {} bytes", numBytes);
        return frames;
    }

    // 6. 准备颜色空间转换上下文 (YUV -> BGR)，并应用缩放
    std::unique_ptr<SwsContext, SwsContextDeleter> swsCtx(sws_getContext(
        srcWidth, srcHeight, codecCtx->pix_fmt,
        dstWidth, dstHeight, AV_PIX_FMT_BGR24,
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
    std::unique_ptr<uint8_t, AvMallocDeleter> rgbBuffer(static_cast<uint8_t*>(av_malloc(static_cast<size_t>(numBytes))));
    if (!rgbBuffer) {
        JARK_LOG("bad_alloc");
        return frames;
    }

    // 将 rgbBuffer 关联到 rgbFrame
    result = av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer.get(),
        AV_PIX_FMT_BGR24, dstWidth, dstHeight, 1);
    if (result < 0) {
        char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        JARK_LOG("Failed to fill RGB frame arrays: {}", av_make_error_string(errStr, sizeof(errStr), result));
        return frames;
    }

    // 获取旋转信息
    int rotation = get_rotation_angle(videoStream);

    // 8. 解码循环
    while (av_read_frame(formatCtx.get(), packet.get()) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            int sendResult = avcodec_send_packet(codecCtx.get(), packet.get());
            if (sendResult < 0) {
                char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                JARK_LOG("Error sending packet: {}", av_make_error_string(errStr, sizeof(errStr), sendResult));
                break;
            }

            while (true) {
                int receiveResult = avcodec_receive_frame(codecCtx.get(), frame.get());
                if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                    break;
                }
                else if (receiveResult < 0) {
                    char errStr[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    JARK_LOG("Error decoding frame: {}", av_make_error_string(errStr, sizeof(errStr), receiveResult));
                    break;
                }

                // 9. 颜色转换 (YUV -> BGR) 并缩放
                int scaledRows = sws_scale(swsCtx.get(), frame->data, frame->linesize, 0, srcHeight,
                    rgbFrame->data, rgbFrame->linesize);
                if (scaledRows <= 0) {
                    JARK_LOG("Failed to convert video frame: {} rows scaled", scaledRows);
                    continue;
                }

                // 10. 创建 cv::Mat (注意：OpenCV 使用 BGR)
                // 数据是连续的，直接封装
                cv::Mat decodedMat(dstHeight, dstWidth, CV_8UC3, rgbFrame->data[0]);

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

        if (maxFrames && frames.size() >= maxFrames) {
            break;
        }
    }

    return frames;
}
