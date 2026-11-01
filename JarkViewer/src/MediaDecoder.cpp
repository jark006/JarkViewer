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
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace jark {
namespace {

    constexpr int kIoBufferSize = 4096;
    constexpr size_t kMaxVideoFrameBytes = 1ull << 30;   // 单帧 1GiB 上限
    constexpr int kMaxAudioSamplesPerChunk = 1 << 18;    // 单批样本上限（约 5 秒 48kHz 立体声）

    // 视频硬件解码只用 D3D11VA：Win10/11 的标准接口，NVIDIA/AMD/Intel 通用，静态库里
    // h264/hevc/av1/vp9/mpeg2/vc1/wmv3 都有对应的硬解器；DXVA2/NVDEC/QSV/AMF 各有各的坑，
    // 且对本工程（解码后还要回读成 cv::Mat）没有额外收益，不做多后端优先级链。
    constexpr AVHWDeviceType kHwDeviceType = AV_HWDEVICE_TYPE_D3D11VA;

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

    struct HwDeviceDeleter {
        void operator()(AVBufferRef* ref) const {
            if (ref)
                av_buffer_unref(&ref);
        }
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using AvioContextPtr = std::unique_ptr<AVIOContext, AvioContextDeleter>;
    using HwDevicePtr = std::unique_ptr<AVBufferRef, HwDeviceDeleter>;

    // 进程内共享的 D3D11VA 设备：一个进程里同时可能有多个 MediaDecoder（播放器的视频/音频两路、
    // 预读线程、缩略图后台解码线程），每开一个就 D3D11CreateDevice 一次既慢又白占显存。
    // 设备本身带锁（AVD3D11VADeviceContext 的 lock/unlock 由 ffmpeg 内部设成互斥量，保护的正是
    // device_context 与它内部那张回读用的暂存纹理），多个解码线程共用一个设备是它支持的用法。
    AVBufferRef* acquireSharedHwDevice() {
        static std::mutex mutex;
        static AVBufferRef* shared = nullptr;
        static bool failureLogged = false;

        const std::lock_guard<std::mutex> lock(mutex);
        if (shared)
            return av_buffer_ref(shared);

        // 只缓存成功、不缓存失败：远程桌面重连、驱动复位之后还能自己恢复
        // （每开一个视频多试一次 D3D11CreateDevice，没有显卡的机器上是几毫秒的事）
        const int result = av_hwdevice_ctx_create(&shared, kHwDeviceType, nullptr, nullptr, 0);
        if (result < 0) {
            shared = nullptr;
            if (!failureLogged) {   // 日志只打一次，免得批量开视频时刷屏
                failureLogged = true;
                JARK_LOG("D3D11VA 设备不可用（{}），视频一律软件解码", ffmpegError(result));
            }
            return nullptr;
        }
        // 分发的是引用：调用方各持自己的一份、析构时各自 unref；
        // 共享器里那一份故意不释放，随进程退出一起回收（少一处静态析构顺序的坑）
        return av_buffer_ref(shared);
    }

    // 设置页的「视频硬件解码」是取反存储的：旧设置文件里这一字节是 0，默认就是优先硬解
    bool hardwareDecodeAllowed(const MediaDecoder::VideoDecodeMode mode) {
        if (mode == MediaDecoder::VideoDecodeMode::SoftwareOnly)
            return false;
        return !GlobalVar::settingParameter.disableVideoHardwareDecode;
    }

    // "J 格式"（yuvj*）的语义就是全范围（JPEG/PC range）：libswscale 自己也是这么认的
    // （swscale 内部的 handle_jpeg()）。硬解回读出来的帧是 NV12/P010，这个语义在回读时
    // 丢掉了（实测 cpuFrame 的 color_range 变成 UNSPECIFIED），必须手工补，
    // 否则全范围片源会被按有限范围解释、整幅发灰（iPhone 拍的 yuvj420p 实测平均差 10 个色阶）。
    bool isFullRangeFormat(AVPixelFormat format) {
        switch (format) {
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ440P:
        case AV_PIX_FMT_YUVJ444P:
        case AV_PIX_FMT_YUVJ411P:
            return true;
        default:
            return false;
        }
    }

    // 该解码器有没有 D3D11VA 的 HW_DEVICE_CTX 档。只认这一种挂法：HW_FRAMES_CTX 要自己建帧池，
    // 对"解完还得回读成 cv::Mat"的用法没有意义。没有的（vp8/mpeg4…）就该老老实实软解。
    const AVCodecHWConfig* findD3D11VAConfig(const AVCodec* codec) {
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
            if (!config)
                return nullptr;
            if (config->device_type == kHwDeviceType &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                return config;
        }
    }

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

    // —— 视频硬件解码（D3D11VA）状态，见 openStream() / switchVideoToSoftwareDecode() ——
    HwDevicePtr hwDevice;                            // 进程内共享设备的本实例引用
    AVPixelFormat hwPixelFormat = AV_PIX_FMT_NONE;   // 本次打开要挑的硬解像素格式（get_format 用）
    bool hardwareSupported = false;                  // 该编码器声明了 D3D11VA 的 HW_DEVICE_CTX 配置
    bool hardwareRequested = false;                  // 挂上设备并打开了，运行期回退只做一次就是靠它
    bool hardwareDeclined = false;                   // 挂上了设备，但解码器为这一档内容放弃了硬解
    bool hardwareDecoding = false;                   // 真的解出过硬解帧
    bool hardwareFallback = false;                   // 试过硬解但运行期回退到了软解
    int64_t decodedVideoFrames = 0;                  // 收到过的视频帧数（含被丢掉的，用于判断"一帧都没有"）
    bool deliveredVideoFrame = false;                // 已对外交出过视频帧：之后不许再回退重来

    FramePtr decodeFrame{ av_frame_alloc() };
    FramePtr cpuFrame{ av_frame_alloc() };           // 硬解帧回读到内存后的落脚处
    FramePtr rgbFrame{ av_frame_alloc() };
    PacketPtr packet{ av_packet_alloc() };

    std::unique_ptr<SwsContext, SwsContextDeleter> swsContext;
    int swsSrcWidth = 0;
    int swsSrcHeight = 0;
    int swsDstWidth = 0;
    int swsDstHeight = 0;
    int swsSrcFormat = -1;
    bool swsSrcFullRange = false;   // 源是全范围（J 格式）：要在 sws 上下文上手工声明

    std::unique_ptr<SwrContext, SwrContextDeleter> swrContext;
    int swrInputRate = 0;
    int swrInputChannels = 0;
    int swrInputFormat = -1;

    std::unique_ptr<uint8_t, AvMallocDeleter> videoBuffer;

    bool demuxEof = false;
    bool videoFlushSent = false;
    bool audioFlushSent = false;
    int64_t skipBeforeMs = -1;   // 精确落点：早于它的视频帧不回读、不转换、直接丢
    std::optional<int64_t> lastAudioPtsMs;

    // 打开一路解码器：视频那一路按需优先挂 D3D11VA 硬解，其余一律软解
    CodecContextPtr openStream(int streamIndex, bool enableHardware);

    // 运行期回退：硬解一帧都解不出来时，把视频这一路换成软件解码重开
    bool switchVideoToSoftwareDecode();

    // avcodec 的 get_format 回调（挑硬解像素格式），context->opaque 指回本对象
    static AVPixelFormat selectHardwareFormat(AVCodecContext* context, const enum AVPixelFormat* formats);
};

MediaDecoder::MediaDecoder() : impl_(std::make_unique<Impl>()) {}

MediaDecoder::~MediaDecoder() = default;

// avcodec 的 get_format 回调。挑硬解格式；挑不中就在这个上下文里继续软解
// （挑不中的两种情况：解码器判断这一档内容不支持硬解，比如 H.264 4:4:4；或者硬解格式
// 初始化失败后回调被重新调用）。
//
// 回调契约只允许返回 formats 里的某个格式或 AV_PIX_FMT_NONE，**别顺手返回 formats[0]**：
// 清单里硬件格式排在前面、软件格式排在最后（libavcodec 就是这么摆的——avctx->sw_pix_fmt
// 取的就是最后那个），返回第一个等于让它去初始化一个我们没配过的硬解档（DXVA2_VLD、
// VULKAN…），只会换来一串错误日志再被剔掉重试。照 ffmpeg 自己那份 get_format 的做法：
// 往后走，遇到第一个**没有 HWACCEL 标志**的格式就用它。
AVPixelFormat MediaDecoder::Impl::selectHardwareFormat(AVCodecContext* context, const enum AVPixelFormat* formats) {
    auto* self = static_cast<Impl*>(context->opaque);
    if (self) {
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == self->hwPixelFormat)
                return *format;
        }
        // 走到这儿是"编码器有硬解档、但这一档内容它不肯用硬解"。只有上下文里**真挂着设备**
        // 才算——没挂上设备（将来有人把挂设备那一步改坏了）不算"解码器回绝"，
        // 得让自检（--probe --hw-test）把它当异常报出来
        if (context->hw_device_ctx)
            self->hardwareDeclined = true;
    }

    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *format;
    }
    return AV_PIX_FMT_NONE;   // 一个软件格式都没有：只能失败（调用方据此回退软解重开）
}

// 打开一路解码器：视频那一路 enableHardware 为真时**先试硬解**（D3D11VA），
// 设备不可用 / 编码器没声明 D3D11VA（vp8、mpeg4…）/ 挂上设备后打不开，都退回同一编码器的软解。
// 两条路解出来的是同一套 YUV 帧，后面的缩放与旋转完全一致（`--probe --hw-test` 盯着这件事）。
CodecContextPtr MediaDecoder::Impl::openStream(int streamIndex, bool enableHardware) {
    if (streamIndex < 0)
        return nullptr;

    AVStream* stream = formatContext->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        JARK_LOG("no decoder for codec id {}", static_cast<int>(stream->codecpar->codec_id));
        return nullptr;
    }

    const auto fillParameters = [&](AVCodecContext* context) {
        const int copied = avcodec_parameters_to_context(context, stream->codecpar);
        if (copied < 0)
            JARK_LOG("avcodec_parameters_to_context failed: {}", ffmpegError(copied));
        return copied >= 0;
    };

    // 1) 硬件优先。"这个编码器有没有 D3D11VA 档"与"这回要不要用"是两件事，一律先问清楚——
    //    自检要靠它区分"本来就没这个能力"与"有能力却没挂上"。音频那一路没有硬解档，不查。
    const bool isVideoStream = streamIndex == videoStreamIndex;
    const AVCodecHWConfig* hwConfig = isVideoStream ? findD3D11VAConfig(codec) : nullptr;
    const bool useHardware = enableHardware && hwConfig && hwDevice;
    if (isVideoStream) {
        hardwareSupported = hwConfig != nullptr;
        hardwareRequested = useHardware;
        hardwareDecoding = false;
    }

    if (useHardware) {
        CodecContextPtr context(avcodec_alloc_context3(codec));
        if (context && fillParameters(context.get())) {
            context->opaque = this;
            context->get_format = &Impl::selectHardwareFormat;
            context->hw_device_ctx = av_buffer_ref(hwDevice.get());
            hwPixelFormat = hwConfig->pix_fmt;
            // 硬解这一段必须单线程，**别**沿用下面软解的 8（更别留给"自动"，那会按 CPU 核数
            // 开出十几条帧线程）。FFmpeg 允许帧线程与 hwaccel 并存，但 d3d11va 的 hwaccel
            // 没有 HWACCEL_CAP_THREAD_SAFE，所有帧线程要抢同一把锁串行跑；而每条线程各有一份
            // 子上下文、各自建一个硬解帧池（池大小还随线程数线性上涨），4K 上能吃掉好几 GB 显存，
            // 还会引入"每线程多一帧"的解码延迟——和精确 seek 落点、拖动预览只交一帧的语义直接打架。
            // 硬解本来就是 GPU 内部流水，单线程喂它不会掉吞吐
            context->thread_count = 1;
            const int opened = avcodec_open2(context.get(), codec, nullptr);
            if (opened >= 0)
                return context;

            // 硬解这一趟没成：运行期回退也没意义了（没有硬解可退），直接落到下面那条路
            hardwareRequested = false;
            JARK_LOG("D3D11VA 硬解打不开，改用软件解码: {} {}", codec->name, ffmpegError(opened));
        }
        hwPixelFormat = AV_PIX_FMT_NONE;   // 别让后面那一趟还惦记着硬解格式
    }

    // 2) 软件解码（原本这条路，一行没改）
    CodecContextPtr context(avcodec_alloc_context3(codec));
    if (!context)
        return nullptr;

    if (!fillParameters(context.get()))
        return nullptr;

    context->thread_count = 8;
    if (const int opened = avcodec_open2(context.get(), codec, nullptr); opened < 0) {
        // 别在这里静默失败：帧线程上下文初始化出错时 avcodec_open2 会返回错误，
        // 少了这行日志就只能看到"没有可用的解码器"，看不出原因
        JARK_LOG("avcodec_open2({}) failed: {}", codec->name, ffmpegError(opened));
        return nullptr;
    }

    return context;
}

// 运行期回退：硬解挂上了、却一帧都解不出来。驱动不支持这一档 profile/分辨率时就是这种表现——
// avcodec_open2 照样成功，之后每一帧的 DecoderBeginFrame 都失败。把视频这一路换成软解重开。
// 只允许在**还没对外交出过视频帧**时做：已经送出去的帧收不回来，半路重来会让播放端拿到重复的
// 时间戳（那比一次回退失败更糟）。音频不算在内：见下。
bool MediaDecoder::Impl::switchVideoToSoftwareDecode() {
    // （"只挡视频帧"是有意的：真正播给用户听的都是各自独立的 VideoOnly / AudioOnly 实例，
    //   视频那一路本来就不会产出音频；会同时拿到声画的是取实况首帧、预解帧、--probe 这类
    //   Both 实例，它们只要视频帧，回退时重复吐一小段音频没有任何影响。）
    if (!hardwareRequested || !hwDevice || videoStreamIndex < 0 || deliveredVideoFrame)
        return false;

    // 先落标记：软解也打不开时不再反复重试（一次回退不成就一路软解跑到底）
    hardwareRequested = false;
    hardwareFallback = true;
    hardwareDecoding = false;

    CodecContextPtr software = openStream(videoStreamIndex, false);
    if (!software) {
        JARK_LOG("硬解解不出画面，软件解码器也打不开，视频这一路放弃");
        return false;
    }

    videoCodec = std::move(software);
    // sws 缓存按像素格式判等（硬解回读是 nv12/p010、软解是 yuv420p），格式一变它自己会重建；
    // 这里只要把上一帧回读的落脚处清掉，别让它继续持有一块没人用的内存
    if (cpuFrame)
        av_frame_unref(cpuFrame.get());

    JARK_LOG("硬解解不出画面，视频解码已回退软件解码");
    return true;
}

// 硬解解不出来时换成软件解码、解复用器与两个解码器一起退回落点重来。
// 退到 **skipBeforeMs**（精确落点那条线）而不是 0：之前若有过精确 seek，播放端还在
// 等着落在那个位置，回到 0 一路丢上去也能对，但要白解一大段。skipBeforeMs 为 -1
// （没在等落点，就是普通开播/拖动预览）才退回文件开头。
// 必须由持有本解码器的那一个线程调用（与 seek 同一份约束）。
bool MediaDecoder::fallbackToSoftwareDecode() {
    if (!impl_->switchVideoToSoftwareDecode())
        return false;

    const int64_t rewindMs = impl_->skipBeforeMs >= 0 ? impl_->skipBeforeMs : 0;
    return seek(rewindMs);
}

std::unique_ptr<MediaDecoder> MediaDecoder::open(std::span<const uint8_t> data, StreamFilter filter,
    VideoDecodeMode videoDecode) {
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

    // 视频这一路按需取一份共享的硬解设备（音频没有 D3D11VA 这一档，永远软解；
    // 纯音频文件连设备都不该去要——那是几十 MB 驱动内存加一次初始化，白花的）
    if (impl.videoStreamIndex >= 0 && hardwareDecodeAllowed(videoDecode))
        impl.hwDevice.reset(acquireSharedHwDevice());

    impl.videoCodec = impl.openStream(impl.videoStreamIndex, impl.hwDevice != nullptr);
    impl.audioCodec = impl.openStream(impl.audioStreamIndex, false);

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

    // 日志里的解码方式以"设备真的挂上了"为准：真硬解还要等 readNext 出帧才算数
    // （挂上设备后 get_format 仍可能退回软件格式），这里给的是打开这一步的结果
    const char* const videoDecodeName = impl.videoCodec && impl.videoCodec->hw_device_ctx
        ? "硬解(d3d11va)" : "软件";

    JARK_LOG("media opened: video={} 编码 {}x{} 显示 {}x{} rot={} fps={:.2f} 解码={} | audio={} {}Hz {}ch | duration={}ms",
        info.hasVideo, info.width, info.height, info.displayWidth(), info.displayHeight(),
        info.rotationDegrees, info.frameRate, videoDecodeName,
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
                ++impl.decodedVideoFrames;
                const AVFrame* frame = impl.decodeFrame.get();

                // 丢帧的几条路都要把两个帧松掉：decodeFrame 是解码器交出来的，
                // cpuFrame 是硬解帧回读的落脚处（软解那一路它一直是空的）
                const auto dropFrame = [&impl] {
                    av_frame_unref(impl.decodeFrame.get());
                    av_frame_unref(impl.cpuFrame.get());
                };

                // 时间戳要在转换之前算：早于目标的帧连回读/缩放/拷贝都不做（见 setSkipBeforeMs）
                const AVRational videoTimeBase = impl.formatContext->streams[impl.videoStreamIndex]->time_base;
                const int64_t framePts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                    ? frame->best_effort_timestamp : frame->pts;
                const int64_t framePtsMs = framePts == AV_NOPTS_VALUE ? 0
                    : av_rescale_q(framePts, videoTimeBase, AVRational{ 1, 1000 });

                if (impl.skipBeforeMs >= 0 && framePtsMs < impl.skipBeforeMs) {
                    dropFrame();
                    continue;
                }

                // 这一路实际喂给 sws 的像素格式是不是全范围（J 格式）。硬解回读出来的是
                // NV12/P010，范围内涵在回读时丢了，只能问 avctx->sw_pix_fmt——那是"软解会用"
                // 的那个格式，也正是 sws 判断范围的依据，两条路的范围解释由此保持一致
                const bool sourceFullRange = isFullRangeFormat(frame->hw_frames_ctx
                    ? impl.videoCodec->sw_pix_fmt
                    : static_cast<AVPixelFormat>(frame->format));

                // 硬解帧先回读到内存（NV12/P010），之后的缩放、旋转、clone 与软解那条路完全一致。
                // 回读排在丢帧判定之后：精确 seek 要从关键帧向前解上百帧，那些帧不值得白花一次拷贝
                if (frame->hw_frames_ctx) {
                    av_frame_unref(impl.cpuFrame.get());
                    const int transferResult = av_hwframe_transfer_data(impl.cpuFrame.get(), frame, 0);
                    if (transferResult < 0) {
                        JARK_LOG("硬解帧回读到内存失败: {}", ffmpegError(transferResult));
                        dropFrame();
                        // "驱动解不了这个流"的另一种表现。还没交出过数据的话就换软解从起点
                        // 重来；已经交出过数据的话这次回退不会生效，这一帧只能丢掉
                        fallbackToSoftwareDecode();
                        continue;
                    }
                    impl.hardwareDecoding = true;
                    frame = impl.cpuFrame.get();
                }

                const int srcWidth = frame->width > 0 ? frame->width : info_.width;
                const int srcHeight = frame->height > 0 ? frame->height : info_.height;
                if (srcWidth <= 0 || srcHeight <= 0) {
                    dropFrame();
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
                    impl.swsSrcFormat != frame->format || impl.swsSrcFullRange != sourceFullRange;

                if (needNewContext) {
                    impl.swsContext.reset(sws_getContext(srcWidth, srcHeight,
                        static_cast<AVPixelFormat>(frame->format), dstWidth, dstHeight, AV_PIX_FMT_BGR24,
                        SWS_BILINEAR, nullptr, nullptr, nullptr));

                    // 全范围片源要手工声明：sws_getContext 是按"有限范围"起头的，
                    // 只有输入格式本身是 J 格式（软解那条路）它才自己认得出来。
                    // 这里只改 srcRange 一项，其余（系数表/目标范围/亮度对比度饱和度）原样保留
                    if (impl.swsContext && sourceFullRange) {
                        int* inverseTable = nullptr;
                        int* table = nullptr;
                        int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
                        if (sws_getColorspaceDetails(impl.swsContext.get(), &inverseTable, &srcRange,
                                &table, &dstRange, &brightness, &contrast, &saturation) >= 0) {
                            sws_setColorspaceDetails(impl.swsContext.get(), inverseTable, 1,
                                table, dstRange, brightness, contrast, saturation);
                        }
                    }

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
                    impl.swsSrcFullRange = sourceFullRange;
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

                        dropFrame();
                        impl.deliveredVideoFrame = true;
                        return true;
                    }
                }

                // 以前这里是无声 continue：硬解接上之后，"转换这一环失败导致一帧都不出"
                // 会和"这个文件本来就没有帧"在日志上完全分不开，得说一声
                JARK_LOG("视频帧转换失败（{}x{} 像素格式 {}）：{}", srcWidth, srcHeight, frame->format,
                    impl.swsContext ? "目标缓冲分配失败" : "sws 上下文建立失败");
                dropFrame();
                continue;
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
                JARK_LOG("video decode error: {}", ffmpegError(result));
                // 硬解解不出画面（驱动不支持这一档 profile/分辨率）时换成软解重来；
                // 只在还没交出过任何数据时生效，见 fallbackToSoftwareDecode
                fallbackToSoftwareDecode();
            }
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

        // 硬解一路连一帧都没解出来：有些驱动对不支持的档位既不报错也不出帧，到这儿才发现。
        // 换成软解从起点再来一遍，别让用户看到"打得开、画面却一直没有"
        if (impl.decodedVideoFrames == 0 && fallbackToSoftwareDecode())
            continue;

        chunk = Chunk{};
        chunk.type = Chunk::Type::End;
        return false;
    }
}

// —— 视频解码方式（日志与自检用）——

bool MediaDecoder::hardwareDecodeAvailable() {
    const HwDevicePtr device(acquireSharedHwDevice());
    return device != nullptr;
}

bool MediaDecoder::hardwareDecodeSupported() const noexcept {
    return impl_->hardwareSupported;
}

bool MediaDecoder::hardwareDecoding() const noexcept {
    return impl_->hardwareDecoding;
}

bool MediaDecoder::hardwareDecodeFallback() const noexcept {
    return impl_->hardwareFallback;
}

bool MediaDecoder::hardwareDecodeDeclined() const noexcept {
    return impl_->hardwareDeclined;
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
    // 重采样器也持有上一位置的样本与滤波历史；只冲 avcodec 会把旧位置的尾音带过来。
    impl.swrContext.reset();
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
