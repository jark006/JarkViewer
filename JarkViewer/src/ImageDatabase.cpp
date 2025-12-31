#include "ImageDatabase.h"

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

#ifndef QOI_IMPLEMENTATION
#define QOI_IMPLEMENTATION
#include "qoi.h"
#endif

static std::string jxlStatusCode2String(JxlDecoderStatus status) {
    switch (status) {
    case JXL_DEC_SUCCESS:
        return "Success: Function call finished successfully or decoding is finished.";
    case JXL_DEC_ERROR:
        return "Error: An error occurred, e.g. invalid input file or out of memory.";
    case JXL_DEC_NEED_MORE_INPUT:
        return "Need more input: The decoder needs more input bytes to continue.";
    case JXL_DEC_NEED_PREVIEW_OUT_BUFFER:
        return "Need preview output buffer: The decoder requests setting a preview output buffer.";
    case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
        return "Need image output buffer: The decoder requests an output buffer for the full resolution image.";
    case JXL_DEC_JPEG_NEED_MORE_OUTPUT:
        return "JPEG needs more output: The JPEG reconstruction buffer is too small.";
    case JXL_DEC_BOX_NEED_MORE_OUTPUT:
        return "Box needs more output: The box contents output buffer is too small.";
    case JXL_DEC_BASIC_INFO:
        return "Basic info: Basic information such as image dimensions and extra channels is available.";
    case JXL_DEC_COLOR_ENCODING:
        return "Color encoding: Color encoding or ICC profile from the codestream header is available.";
    case JXL_DEC_PREVIEW_IMAGE:
        return "Preview image: A small preview frame has been decoded.";
    case JXL_DEC_FRAME:
        return "Frame: Beginning of a frame, frame header information is available.";
    case JXL_DEC_FULL_IMAGE:
        return "Full image: A full frame or layer has been decoded.";
    case JXL_DEC_JPEG_RECONSTRUCTION:
        return "JPEG reconstruction: JPEG reconstruction data is decoded.";
    case JXL_DEC_BOX:
        return "Box: The header of a box in the container format (BMFF) is decoded.";
    case JXL_DEC_FRAME_PROGRESSION:
        return "Frame progression: A progressive step in decoding the frame is reached.";
    default:
        return "Unknown status code.";
    }
}


ImageAsset ImageDatabase::loadJXL(wstring_view path, const vector<uint8_t>& buf) {
    ImageAsset imageAsset;

    // Multi-threaded parallel runner.
    auto runner = JxlResizableParallelRunnerMake(nullptr);

    auto decoder = JxlDecoderMake(nullptr);
    JxlDecoderStatus status = JxlDecoderSubscribeEvents(decoder.get(),
        JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    if (JXL_DEC_SUCCESS != status) {
        JARK_LOG("JxlDecoderSubscribeEvents failed\n{}\n{}",
            jarkUtils::wstringToUtf8(path),
            jxlStatusCode2String(status));
        return imageAsset;
    }

    status = JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get());
    if (JXL_DEC_SUCCESS != status) {
        JARK_LOG("JxlDecoderSetParallelRunner failed\n{}\n{}",
            jarkUtils::wstringToUtf8(path),
            jxlStatusCode2String(status));
        return imageAsset;
    }

    JxlBasicInfo basic_info{};
    JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

    JxlDecoderSetInput(decoder.get(), buf.data(), buf.size());
    JxlDecoderCloseInput(decoder.get());

    cv::Mat image;
    int duration_ms = 0;
    bool got_basic_info = false;

    while(true) {
        status = JxlDecoderProcessInput(decoder.get());

        if (status == JXL_DEC_ERROR) {
            JARK_LOG("Decoder error\n{}\n{}",
                jarkUtils::wstringToUtf8(path),
                jxlStatusCode2String(status));
            break;
        }
        else if (status == JXL_DEC_NEED_MORE_INPUT) {
            JARK_LOG("Error, already provided all input\n{}\n{}",
                jarkUtils::wstringToUtf8(path),
                jxlStatusCode2String(status));
            break;
        }
        else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &basic_info)) {
                JARK_LOG("JxlDecoderGetBasicInfo failed\n{}\n{}",
                    jarkUtils::wstringToUtf8(path),
                    jxlStatusCode2String(status));
                break;
            }
            got_basic_info = true;

            JxlResizableParallelRunnerSetThreads(
                runner.get(),
                JxlResizableParallelRunnerSuggestThreads(basic_info.xsize, basic_info.ysize));
        }
        else if (status == JXL_DEC_FRAME) {
            JxlFrameHeader frame_header{};
            if (JxlDecoderGetFrameHeader(decoder.get(), &frame_header) == JXL_DEC_SUCCESS) {
                if (basic_info.have_animation && got_basic_info) {
                    uint32_t& duration_ticks = frame_header.duration;
                    uint32_t& tps_num = basic_info.animation.tps_numerator;
                    uint32_t& tps_den = basic_info.animation.tps_denominator;

                    duration_ms = tps_num > 0 ? ((duration_ticks * 1000 * tps_den) / tps_num) : 33;
                }
            }
            else {
                duration_ms = 33;
            }
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t buffer_size;

            status = JxlDecoderImageOutBufferSize(decoder.get(), &format, &buffer_size);
            if (JXL_DEC_SUCCESS != status) {
                JARK_LOG("JxlDecoderImageOutBufferSize failed\n{}\n{}",
                    jarkUtils::wstringToUtf8(path),
                    jxlStatusCode2String(status));
                break;
            }
            auto byteSizeRequire = 4ULL * basic_info.xsize * basic_info.ysize;
            if (buffer_size != byteSizeRequire) {
                JARK_LOG("Invalid out buffer size {} {}\n{}\n{}",
                    buffer_size, byteSizeRequire,
                    jarkUtils::wstringToUtf8(path),
                    jxlStatusCode2String(status));
                break;
            }
            image = cv::Mat(basic_info.ysize, basic_info.xsize, CV_8UC4);
            status = JxlDecoderSetImageOutBuffer(decoder.get(), &format, image.ptr(), byteSizeRequire);
            if (JXL_DEC_SUCCESS != status) {
                JARK_LOG("JxlDecoderSetImageOutBuffer failed\n{}\n{}",
                    jarkUtils::wstringToUtf8(path),
                    jxlStatusCode2String(status));
                break;
            }
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
            // cv::cvtColor(image, image, cv::COLOR_BGRA2RGBA); // 会额外分配内存，影响性能

            const int height = image.rows;
            const int width = image.cols;
            for (int y = 0; y < height; ++y) {
                cv::Vec4b* rowPtr = image.ptr<cv::Vec4b>(y);
                for (int x = 0; x < width; ++x) {
                    cv::Vec4b& pixel = rowPtr[x];
                    std::swap(pixel[0], pixel[2]);  // B<->R
                }
            }

            imageAsset.frames.emplace_back(image);
            imageAsset.frameDurations.emplace_back(duration_ms);
        }
        else if (status == JXL_DEC_SUCCESS) {
            // All decoding successfully finished.
            // It's not required to call JxlDecoderReleaseInput(decoder.get()) here since
            // the decoder will be destroyed.

            break;
        }
        else {
            JARK_LOG("Unknown decoder status\n{}\n{}",
                jarkUtils::wstringToUtf8(path),
                jxlStatusCode2String(status));
            break;
        }
    }

    if (imageAsset.frames.empty()) {
        imageAsset.format = ImageFormat::None;
        imageAsset.primaryFrame = getErrorTipsMat();
    }
    else if (imageAsset.frames.size() == 1) {
        imageAsset.format = ImageFormat::Still;
        imageAsset.primaryFrame = std::move(imageAsset.frames[0]);
        imageAsset.frames.clear();
        imageAsset.frameDurations.clear();
    }
    else {
        imageAsset.format = ImageFormat::Animated;
    }
    return imageAsset;
}

static std::string statusExplain(WP2Status status) {
    switch (status) {
    case WP2_STATUS_OK:
        return "Operation completed successfully.";
    case WP2_STATUS_VERSION_MISMATCH:
        return "Version mismatch.";
    case WP2_STATUS_OUT_OF_MEMORY:
        return "Memory error allocating objects.";
    case WP2_STATUS_INVALID_PARAMETER:
        return "A parameter value is invalid.";
    case WP2_STATUS_NULL_PARAMETER:
        return "A pointer parameter is NULL.";
    case WP2_STATUS_BAD_DIMENSION:
        return "Picture has invalid width/height.";
    case WP2_STATUS_USER_ABORT:
        return "Abort request by user.";
    case WP2_STATUS_UNSUPPORTED_FEATURE:
        return "Unsupported feature.";
    case WP2_STATUS_BITSTREAM_ERROR:
        return "Bitstream has syntactic error.";
    case WP2_STATUS_NOT_ENOUGH_DATA:
        return "Premature EOF during decoding.";
    case WP2_STATUS_BAD_READ:
        return "Error while reading bytes.";
    case WP2_STATUS_NEURAL_DECODE_FAILURE:
        return "Neural decoder failed.";
    case WP2_STATUS_BITSTREAM_OUT_OF_MEMORY:
        return "Memory error while flushing bits.";
    case WP2_STATUS_INVALID_CONFIGURATION:
        return "Encoding configuration is invalid.";
    case WP2_STATUS_BAD_WRITE:
        return "Error while flushing bytes.";
    case WP2_STATUS_FILE_TOO_BIG:
        return "File is bigger than 4G.";
    case WP2_STATUS_INVALID_COLORSPACE:
        return "Encoder called with bad colorspace.";
    case WP2_STATUS_NEURAL_ENCODE_FAILURE:
        return "Neural encoder failed.";
    default:
        return "Unknown status.";
    }
}

// https://chromium.googlesource.com/codecs/libwebp2  commit 96720e6410284ebebff2007d4d87d7557361b952  Date:   Mon Sep 9 18:11:04 2024 +0000
// 网络找的不少wp2图像无法解码，使用 libwebp2 的 cwp2.exe 工具编码的 .wp2 图片可以正常解码
ImageAsset ImageDatabase::loadWP2(wstring_view path, const std::vector<uint8_t>& buf) {
    ImageAsset imageAsset;

    WP2::ArrayDecoder decoder(buf.data(), buf.size());
    uint32_t duration_ms = 0;

    while (decoder.ReadFrame(&duration_ms)) {
        auto& output_buffer = decoder.GetPixels();
        cv::Mat img; // Need BGRA or BGR

        switch (output_buffer.format())
        {
        case WP2_Argb_32:
        case WP2_ARGB_32:
        case WP2_XRGB_32: {// A-RGB -> BGR-A 大小端互转
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC4, (void*)output_buffer.GetRow(0), output_buffer.stride());
            auto srcPtr = (uint32_t*)img.ptr();
            auto pixelCount = (size_t)img.cols * img.rows;
            for (size_t i = 0; i < pixelCount; ++i) {
                srcPtr[i] = swap_endian(srcPtr[i]);
            }
        }break;

        case WP2_rgbA_32:
        case WP2_RGBA_32:
        case WP2_RGBX_32: {
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC4, (void*)output_buffer.GetRow(0), output_buffer.stride());
            cv::cvtColor(img, img, cv::COLOR_RGBA2BGRA);
        }break;

        case WP2_bgrA_32:
        case WP2_BGRA_32:
        case WP2_BGRX_32: {
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC4, (void*)output_buffer.GetRow(0), output_buffer.stride());
        }break;

        case WP2_RGB_24: {
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC3, (void*)output_buffer.GetRow(0), output_buffer.stride());
            cv::cvtColor(img, img, cv::COLOR_RGB2BGR);
        }break;

        case WP2_BGR_24: {
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC3, (void*)output_buffer.GetRow(0), output_buffer.stride());
        }break;

        case WP2_Argb_38: { // HDR format: 8 bits for A, 10 bits per RGB.
            img = cv::Mat(output_buffer.height(), output_buffer.width(), CV_8UC4); // 8-bit BGRA

            for (uint32_t y = 0; y < output_buffer.height(); ++y) {
                const uint8_t* src_row = (const uint8_t*)output_buffer.GetRow(y);
                cv::Vec4b* dst_row = img.ptr<cv::Vec4b>(y);

                for (uint32_t x = 0; x < output_buffer.width(); ++x) {
                    // src_row contains 5 bytes per pixel: A (8 bits), R (10 bits), G (10 bits), B (10 bits)
                    const uint8_t A = src_row[0];           // 8 bits for alpha
                    const uint16_t R = ((src_row[1] << 2) | (src_row[2] >> 6)); // 10 bits for red
                    const uint16_t G = ((src_row[2] & 0x3F) << 4) | (src_row[3] >> 4); // 10 bits for green
                    const uint16_t B = ((src_row[3] & 0x0F) << 6) | (src_row[4] >> 2); // 10 bits for blue

                    // Map 10-bit values (0-1023) to 8-bit values (0-255)
                    dst_row[x] = cv::Vec4b(
                        B >> 2,  // Blue (10 -> 8 bits)
                        G >> 2,  // Green (10 -> 8 bits)
                        R >> 2,  // Red (10 -> 8 bits)
                        A        // Alpha (already 8 bits)
                    );

                    src_row += 5; // Move to next pixel (5 bytes per pixel in WP2_Argb_38)
                }
            }
        } break;
        }

        if (!img.empty()) {
            imageAsset.frames.emplace_back(img.clone());
            imageAsset.frameDurations.emplace_back(duration_ms);
        }
    }

#ifndef NDEBUG
    auto status = decoder.GetStatus();
    if (status != WP2_STATUS_OK) {
        JARK_LOG("ERROR: {}\n{}", jarkUtils::wstringToUtf8(path), statusExplain(status));
    }
#endif

    if (imageAsset.frames.empty()) {
        imageAsset.format = ImageFormat::None;
        imageAsset.primaryFrame = getErrorTipsMat();
    }
    else if (imageAsset.frames.size() == 1) {
        imageAsset.format = ImageFormat::Still;
        imageAsset.primaryFrame = std::move(imageAsset.frames[0]);
        imageAsset.frames.clear();
        imageAsset.frameDurations.clear();
    }
    else {
        imageAsset.format = ImageFormat::Animated;
    }
    return imageAsset;
}


ImageAsset ImageDatabase::loadBPG(wstring_view path, const std::vector<uchar>& buf) {
    auto decoderContext = bpg_decoder_open();
    if (bpg_decoder_decode(decoderContext, buf.data(), (int)buf.size()) < 0) {
        JARK_LOG("cvMat cannot decode: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    ImageAsset imageAsset;
    BPGImageInfo img_info{};
    bpg_decoder_get_info(decoderContext, &img_info);

    auto width = img_info.width;
    auto height = img_info.height;

    cv::Mat frame(height, width, CV_8UC4);

    if (img_info.has_animation) {
        while (true) {
            if (bpg_decoder_start(decoderContext, BPG_OUTPUT_FORMAT_RGBA32) == 0) {
                for (uint32_t y = 0; y < height; y++) {
                    bpg_decoder_get_line(decoderContext, frame.ptr<uchar>(y));
                }
                cv::cvtColor(frame, frame, cv::COLOR_RGBA2BGRA);

                int num, den;
                bpg_decoder_get_frame_duration(decoderContext, &num, &den);
                imageAsset.frames.emplace_back(frame.clone());
                imageAsset.frameDurations.emplace_back(den == 0 ? 100 : (num * 1000 / den));
            }
            else {
                break;
            }
        }

        if (imageAsset.frames.empty()) {
            imageAsset.format = ImageFormat::None;
            imageAsset.primaryFrame = getErrorTipsMat();
        }
        else if (imageAsset.frames.size() == 1) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = std::move(imageAsset.frames[0]);
            imageAsset.frames.clear();
            imageAsset.frameDurations.clear();
        }
        else {
            imageAsset.format = ImageFormat::Animated;
        }
    }
    else {
        if (bpg_decoder_start(decoderContext, BPG_OUTPUT_FORMAT_RGBA32) == 0) {
            for (uint32_t y = 0; y < height; y++) {
                bpg_decoder_get_line(decoderContext, frame.ptr<uchar>(y));
            }
            cv::cvtColor(frame, frame, cv::COLOR_RGBA2BGRA);

            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = std::move(frame);
        }
        else {
            imageAsset.format = ImageFormat::None;
            imageAsset.primaryFrame = getErrorTipsMat();
        }
    }

    bpg_decoder_close(decoderContext);
    return imageAsset;
}


// https://github.com/strukturag/libheif
// vcpkg install libheif:x64-windows-static
// vcpkg install libheif[hevc]:x64-windows-static
cv::Mat ImageDatabase::loadHeic(wstring_view path, const vector<uint8_t>& buf) {
    if (buf.empty())
        return {};

    auto filetype_check = heif_check_filetype(buf.data(), 12);
    if (filetype_check == heif_filetype_no) {
        JARK_LOG("Input file is not an HEIF file: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    if (filetype_check == heif_filetype_yes_unsupported) {
        JARK_LOG("Input file is an unsupported HEIF file type: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    heif_context* ctx = heif_context_alloc();
    auto err = heif_context_read_from_memory_without_copy(ctx, buf.data(), buf.size(), nullptr);
    if (err.code) {
        JARK_LOG("heif_context_read_from_memory_without_copy error: {} {}", jarkUtils::wstringToUtf8(path), err.message);
        return {};
    }

    // get a handle to the primary image
    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code) {
        JARK_LOG("heif_context_get_primary_image_handle error: {} {}", jarkUtils::wstringToUtf8(path), err.message);
        if (ctx) heif_context_free(ctx);
        if (handle) heif_image_handle_release(handle);
        return {};
    }

    // decode the image and convert colorspace to RGB, saved as 24bit interleaved
    heif_image* img = nullptr;
    err = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (err.code) {
        JARK_LOG("Error: {}", jarkUtils::wstringToUtf8(path));
        JARK_LOG("heif_decode_image error: {}", err.message);
        if (ctx) heif_context_free(ctx);
        if (handle) heif_image_handle_release(handle);
        if (img) heif_image_release(img);
        return {};
    }

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

    auto width = heif_image_handle_get_width(handle);
    auto height = heif_image_handle_get_height(handle);

    cv::Mat matImg;
    cv::cvtColor(cv::Mat(height, width, CV_8UC4, (uint8_t*)data, stride), matImg, cv::COLOR_RGBA2BGRA);

    // clean up resources
    heif_context_free(ctx);
    heif_image_release(img);
    heif_image_handle_release(handle);

    return matImg;
}


// vcpkg install libavif[core,aom,dav1d]:x64-windows-static
// https://github.com/AOMediaCodec/libavif/issues/1451#issuecomment-1606903425
// TODO 部分图像仍不能正常解码
ImageAsset ImageDatabase::loadAvif(wstring_view path, const std::vector<uint8_t>& fileBuf) {
    ImageAsset imageAsset;

    if (fileBuf.empty()) {
        JARK_LOG("Empty input buffer");
        return imageAsset;
    }

    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) {
        JARK_LOG("Failed to create AVIF decoder");
        return imageAsset;
    }

    // 使用RAII管理解码器
    auto decoderGuard = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>(
        decoder, avifDecoderDestroy);

    // 配置解码器
    decoder->ignoreExif = AVIF_TRUE;
    decoder->ignoreXMP = AVIF_TRUE;
    decoder->strictFlags = AVIF_STRICT_DISABLED;
    decoder->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
    decoder->imageDimensionLimit = AVIF_DEFAULT_IMAGE_DIMENSION_LIMIT;

    avifResult result = avifDecoderSetIOMemory(decoder, fileBuf.data(), fileBuf.size());
    if (result != AVIF_RESULT_OK) {
        JARK_LOG("Failed to set IO memory: {}",avifResultToString(result));
        return imageAsset;
    }

    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK) {
        JARK_LOG("Failed to parse AVIF: {}", avifResultToString(result));
        return imageAsset;
    }

    imageAsset.frames.reserve(decoder->imageCount);
    imageAsset.frameDurations.reserve(decoder->imageCount);

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);

    while (avifDecoderNextImage(decoder) == AVIF_RESULT_OK) {
        bool hasAlpha = decoder->image->alphaPlane != nullptr &&
            decoder->image->alphaRowBytes > 0;

        // 配置RGB输出格式
        rgb.depth = 8;  // 强制8位输出
        rgb.format = hasAlpha ? AVIF_RGB_FORMAT_BGRA : AVIF_RGB_FORMAT_BGR;
        rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_BEST_QUALITY;
        rgb.avoidLibYUV = AVIF_FALSE;  // 使用libyuv加速（如果可用）

        result = avifRGBImageAllocatePixels(&rgb);
        if (result != AVIF_RESULT_OK) {
            JARK_LOG("Failed to AllocatePixels: {}", avifResultToString(result));
            break;
        }

        result = avifImageYUVToRGB(decoder->image, &rgb);
        if (result != AVIF_RESULT_OK) {
            avifRGBImageFreePixels(&rgb);
            JARK_LOG("Failed to convert YUV to RGB: {}", avifResultToString(result));
            break;
        }

        cv::Mat frame;
        if (hasAlpha) {
            frame = cv::Mat(decoder->image->height, decoder->image->width, CV_8UC4,
                rgb.pixels, rgb.rowBytes).clone();
        }
        else {
            frame = cv::Mat(decoder->image->height, decoder->image->width, CV_8UC3,
                rgb.pixels, rgb.rowBytes).clone();
        }

        imageAsset.frames.emplace_back(std::move(frame));

        // 计算帧时长
        if (decoder->imageCount > 1) {
            const int durationMs = (int)(decoder->imageTiming.duration * 1000 + 0.5);
            imageAsset.frameDurations.emplace_back(std::max(16, durationMs));  // 至少16ms
        }
        else {
            imageAsset.frameDurations.emplace_back(33);  // 静态图像
        }

        // 释放当前帧的RGB缓冲
        avifRGBImageFreePixels(&rgb);
    }

    if (imageAsset.frames.empty()) {
        imageAsset.format = ImageFormat::None;
        imageAsset.primaryFrame = getErrorTipsMat();
    }
    else if (imageAsset.frames.size() == 1) {
        imageAsset.format = ImageFormat::Still;
        imageAsset.primaryFrame = std::move(imageAsset.frames[0]);
        imageAsset.frames.clear();
        imageAsset.frameDurations.clear();
    }
    else {
        imageAsset.format = ImageFormat::Animated;
    }

    return imageAsset;
}


cv::Mat ImageDatabase::loadRaw(wstring_view path, const vector<uint8_t>& buf) {
    if (buf.empty()) {
        JARK_LOG("Buf is empty: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    auto rawProcessor = std::make_unique<LibRaw>();

    int ret = rawProcessor->open_buffer(buf.data(), buf.size());
    if (ret != LIBRAW_SUCCESS) {
        JARK_LOG("Cannot open RAW file: {} {}", jarkUtils::wstringToUtf8(path), libraw_strerror(ret));
        return {};
    }

    ret = rawProcessor->unpack();
    if (ret != LIBRAW_SUCCESS) {
        JARK_LOG("Cannot unpack RAW file: {} {}", jarkUtils::wstringToUtf8(path), libraw_strerror(ret));
        return {};
    }

    ret = rawProcessor->dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        JARK_LOG("Cannot process RAW file: {} {}", jarkUtils::wstringToUtf8(path), libraw_strerror(ret));
        return {};
    }

    libraw_processed_image_t* image = rawProcessor->dcraw_make_mem_image(&ret);
    if (image == nullptr) {
        JARK_LOG("Cannot make image from RAW data: {} {}", jarkUtils::wstringToUtf8(path), libraw_strerror(ret));
        return {};
    }

    cv::Mat retImg;
    cv::cvtColor(cv::Mat(image->height, image->width, CV_8UC3, image->data), retImg, cv::COLOR_RGB2BGRA);

    // Clean up
    LibRaw::dcraw_clear_mem(image);
    rawProcessor->recycle();

    return retImg;
}


cv::Mat ImageDatabase::readDibFromMemory(const uint8_t* data, size_t size) {
    // 确保有足够的数据用于DIB头
    if (size < sizeof(DibHeader)) {
        JARK_LOG("Insufficient data for DIB header. {}", size);
        return {};
    }

    // 读取DIB头
    DibHeader& header = *((DibHeader*)data);

    // 验证头大小是否符合预期
    if (header.headerSize != sizeof(DibHeader)) {
        JARK_LOG("Unsupported DIB header size {}", header.headerSize);
        return {};
    }

    // 计算调色板大小（如果存在）
    int paletteSize = 0;
    if (header.bitCount <= 8) {
        paletteSize = (header.clrUsed == 0 ? 1 << header.bitCount : header.clrUsed) * 4;
    }

    // 确保有足够的数据用于调色板和像素数据
    //const size_t requireSize = (size_t)header.headerSize + paletteSize + header.imageSize; // header.imageSize偶尔大于size
    //if (size < requireSize) {
    //    JARK_LOG("Insufficient data for image.");
    //    JARK_LOG("requireSize {} givenSize {}", requireSize, size);
    //    return {};
    //}

    // 读取调色板（如果存在）
    std::vector<cv::Vec4b> palette;
    if (paletteSize > 0) {
        palette.resize(paletteSize / 4);
        std::memcpy(palette.data(), data + sizeof(DibHeader), paletteSize);
    }

    // 指向像素数据的指针
    const uint8_t* pixelData = data + header.headerSize + paletteSize;

    // 检查是否存在透明度掩码
    bool hasAlphaMask = (header.height == 2 * header.width);
    int imageHeight = hasAlphaMask ? header.height / 2 : header.height;

    // 创建cv::Mat对象
    cv::Mat image(imageHeight, header.width, CV_8UC4);

    // 根据位深度处理像素数据
    switch (header.bitCount) {
    case 1: {
        // 处理1位图像
        int byteWidth = (header.width + 7) / 8; // 每行字节数
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int byteIndex = (imageHeight - y - 1) * byteWidth + x / 8;
                int bitIndex = x % 8;
                int colorIndex = (pixelData[byteIndex] >> (7 - bitIndex)) & 0x01;
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(palette[colorIndex][0], palette[colorIndex][1], palette[colorIndex][2], 255);
            }
        }
        break;
    }
    case 4: {
        // 处理4位图像
        int byteWidth = (header.width + 1) / 2; // 每行字节数
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int byteIndex = (imageHeight - y - 1) * byteWidth + x / 2;
                int nibbleIndex = x % 2;
                int colorIndex = (pixelData[byteIndex] >> (4 * (1 - nibbleIndex))) & 0x0F;
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(palette[colorIndex][0], palette[colorIndex][1], palette[colorIndex][2], 255);
            }
        }
        break;
    }
    case 8: {
        // 处理8位图像
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int byteIndex = (imageHeight - y - 1) * header.width + x;
                int colorIndex = pixelData[byteIndex];
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(palette[colorIndex][0], palette[colorIndex][1], palette[colorIndex][2], 255);
            }
        }
        break;
    }
    case 16: {
        // 处理16位图像
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int pixelIndex = (imageHeight - y - 1) * header.width + x;
                uint16_t pixel = ((uint16_t*)pixelData)[pixelIndex];
                uint8_t r = (pixel >> 10) & 0x1F;
                uint8_t g = (pixel >> 5) & 0x1F;
                uint8_t b = pixel & 0x1F;
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(b << 3, g << 3, r << 3, 255);
            }
        }
        break;
    }
    case 24: {
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int byteIndex = ((imageHeight - y - 1) * header.width + x) * 3;
                uint8_t r = pixelData[byteIndex];
                uint8_t g = pixelData[byteIndex + 1];
                uint8_t b = pixelData[byteIndex + 2];
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(b, g, r, 255);
            }
        }
        break;
    }
    case 32: {
        memcpy(image.ptr(), pixelData, 4ULL * imageHeight * header.width);
        cv::flip(image, image, 0); // 上下翻转
        break;
    }
    default:
        JARK_LOG("Unsupported bit count {}", header.bitCount);
        return {};
    }

    // 如果存在透明度掩码
    if (hasAlphaMask) {
        if (image.channels() == 3)
            cv::cvtColor(image, image, cv::COLOR_BGR2BGRA);

        const uint8_t* maskData = pixelData + header.width * header.width * header.bitCount / 8;

        int rowSize = (header.width + 31) / 32 * 4;
        int totalSize = rowSize * header.height;

        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < header.width; ++x) {
                int byteIndex = ((imageHeight - y - 1) * rowSize) + (x / 8);
                int bitIndex = 7 - (x % 8);

                // 读取透明掩码
                int transparencyBit = (maskData[byteIndex] >> bitIndex) & 1;
                if (transparencyBit == 1)
                    image.at<cv::Vec4b>(y, x) = cv::Vec4b(0, 0, 0, 0);

                // 读取图像掩码
                //int imageBit = (maskData[totalSize + byteIndex] >> bitIndex) & 1;
                //if (imageBit == 0)
                //    image.at<cv::Vec4b>(y, x) = cv::Vec4b(0, 0, 0, 0);
            }
        }
    }
    return image;
}


// https://github.com/corkami/pics/blob/master/binary/ico_bmp.png
std::tuple<cv::Mat, string> ImageDatabase::loadICO(wstring_view path, const vector<uint8_t>& buf) {
    if (buf.size() < 6) {
        JARK_LOG("Invalid ICO file: {}", jarkUtils::wstringToUtf8(path));
        return { cv::Mat(),"" };
    }

    uint16_t numImages = *reinterpret_cast<const uint16_t*>(&buf[4]);
    std::vector<IconDirEntry> entries;

    if (numImages > 255) {
        JARK_LOG("numImages Error: {} {}", jarkUtils::wstringToUtf8(path), numImages);
        return { cv::Mat(),"" };
    }

    size_t offset = 6;
    for (int i = 0; i < numImages; ++i) {
        if (offset + sizeof(IconDirEntry) > buf.size()) {
            JARK_LOG("Invalid ICO file structure: {}", jarkUtils::wstringToUtf8(path));
            return { cv::Mat(),"" };
        }

        entries.push_back(*reinterpret_cast<const IconDirEntry*>(&buf[offset]));
        offset += sizeof(IconDirEntry);
    }

    vector<cv::Mat> imgs;
    for (const auto& entry : entries) {
        int width = entry.width == 0 ? 256 : entry.width;
        int height = entry.height == 0 ? 256 : entry.height;

        const size_t endOffset = size_t(entry.dataOffset) + entry.dataSize;
        if (endOffset > buf.size()) {
            JARK_LOG("Invalid image data offset or size: {}", jarkUtils::wstringToUtf8(path));
            JARK_LOG("endOffset {} fileBuf.size(): {}", endOffset, buf.size());
            JARK_LOG("entry.dataOffset {}", entry.dataOffset);
            JARK_LOG("entry.dataSize: {}", entry.dataSize);
            continue;
        }

        cv::Mat rawData(1, entry.dataSize, CV_8UC1, (uint8_t*)(buf.data() + entry.dataOffset));

        // PNG BMP
        if (memcmp(rawData.ptr(), "\x89PNG", 4) == 0 || memcmp(rawData.ptr(), "BM", 2) == 0) {
            imgs.emplace_back(cv::imdecode(rawData, cv::IMREAD_UNCHANGED));
            continue;
        }

        DibHeader* dibHeader = (DibHeader*)(buf.data() + entry.dataOffset);
        if (dibHeader->headerSize != 0x28)
            continue;

        imgs.emplace_back(readDibFromMemory((uint8_t*)(buf.data() + entry.dataOffset), entry.dataSize));
    }

    int totalWidth = 0;
    int maxHeight = 0;
    for (auto& img : imgs) {
        if (img.rows == 0 || img.cols == 0)
            continue;

        if (img.channels() == 1) {
            cv::cvtColor(img, img, cv::COLOR_GRAY2BGRA);
        }
        else if (img.channels() == 3) {
            cv::cvtColor(img, img, cv::COLOR_BGR2BGRA);
        }
        else if (img.channels() == 4) {
            // Already BGRA
        }
        else {
            JARK_LOG("Unsupported image format");
            img = getErrorTipsMat();
        }

        totalWidth += img.cols;
        maxHeight = std::max(maxHeight, img.rows);
    }

    if (totalWidth == 0)
        return { cv::Mat(),"" };

    cv::Mat result(maxHeight, totalWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));

    string resolutionStr = std::format("\n{}:", getUIString(41));
    int imageCnt = 0;
    int currentX = 0;
    for (const auto& img : imgs) {
        if (img.rows == 0 || img.cols == 0)
            continue;

        img.copyTo(result(cv::Rect(currentX, 0, img.cols, img.rows)));
        currentX += img.cols;

        resolutionStr += std::format(" {}x{}", img.cols, img.rows);
        imageCnt++;
    }
    resolutionStr += std::format("\n{}: {}", getUIString(51), imageCnt);

    return { result,ExifParse::getSimpleInfo(path, result.cols, result.rows, buf.data(), buf.size()) + resolutionStr };
}


// https://github.com/MolecularMatters/psd_sdk
cv::Mat ImageDatabase::loadPSD(wstring_view path, const vector<uint8_t>& buf) {
    const int32_t CHANNEL_NOT_FOUND = UINT_MAX;

    cv::Mat img;

    psd::MallocAllocator allocator;
    psd::NativeFile file(&allocator);

    if (!file.OpenRead(path.data())) {
        JARK_LOG("Cannot open file {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    psd::Document* document = CreateDocument(&file, &allocator);
    if (!document) {
        JARK_LOG("Cannot create document {}", jarkUtils::wstringToUtf8(path));
        file.Close();
        return {};
    }

    // the sample only supports RGB colormode
    if (document->colorMode != psd::colorMode::RGB)
    {
        JARK_LOG("Document is not in RGB color mode {}", jarkUtils::wstringToUtf8(path));
        DestroyDocument(document, &allocator);
        file.Close();
        return {};
    }

    // extract all layers and masks.
    bool hasTransparencyMask = false;
    psd::LayerMaskSection* layerMaskSection = ParseLayerMaskSection(document, &file, &allocator);
    if (layerMaskSection)
    {
        hasTransparencyMask = layerMaskSection->hasTransparencyMask;

        // extract all layers one by one. this should be done in parallel for maximum efficiency.
        for (unsigned int i = 0; i < layerMaskSection->layerCount; ++i)
        {
            psd::Layer* layer = &layerMaskSection->layers[i];
            ExtractLayer(document, &file, &allocator, layer);

            // check availability of R, G, B, and A channels.
            // we need to determine the indices of channels individually, because there is no guarantee that R is the first channel,
            // G is the second, B is the third, and so on.
            const unsigned int indexR = FindChannel(layer, psd::channelType::R);
            const unsigned int indexG = FindChannel(layer, psd::channelType::G);
            const unsigned int indexB = FindChannel(layer, psd::channelType::B);
            const unsigned int indexA = FindChannel(layer, psd::channelType::TRANSPARENCY_MASK);

            // note that channel data is only as big as the layer it belongs to, e.g. it can be smaller or bigger than the canvas,
            // depending on where it is positioned. therefore, we use the provided utility functions to expand/shrink the channel data
            // to the canvas size. of course, you can work with the channel data directly if you need to.
            void* canvasData[4] = {};
            unsigned int channelCount = 0u;
            if ((indexR != CHANNEL_NOT_FOUND) && (indexG != CHANNEL_NOT_FOUND) && (indexB != CHANNEL_NOT_FOUND))
            {
                // RGB channels were found.
                canvasData[0] = ExpandChannelToCanvas(document, &allocator, layer, &layer->channels[indexR]);
                canvasData[1] = ExpandChannelToCanvas(document, &allocator, layer, &layer->channels[indexG]);
                canvasData[2] = ExpandChannelToCanvas(document, &allocator, layer, &layer->channels[indexB]);
                channelCount = 3u;

                if (indexA != CHANNEL_NOT_FOUND)
                {
                    // A channel was also found.
                    canvasData[3] = ExpandChannelToCanvas(document, &allocator, layer, &layer->channels[indexA]);
                    channelCount = 4u;
                }
            }

            // interleave the different pieces of planar canvas data into one RGB or RGBA image, depending on what channels
            // we found, and what color mode the document is stored in.
            uint8_t* image8 = nullptr;
            uint16_t* image16 = nullptr;
            float32_t* image32 = nullptr;
            if (channelCount == 3u)
            {
                if (document->bitsPerChannel == 8)
                {
                    image8 = CreateInterleavedImage<uint8_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], document->width, document->height);
                }
                else if (document->bitsPerChannel == 16)
                {
                    image16 = CreateInterleavedImage<uint16_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], document->width, document->height);
                }
                else if (document->bitsPerChannel == 32)
                {
                    image32 = CreateInterleavedImage<float32_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], document->width, document->height);
                }
            }
            else if (channelCount == 4u)
            {
                if (document->bitsPerChannel == 8)
                {
                    image8 = CreateInterleavedImage<uint8_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], canvasData[3], document->width, document->height);
                }
                else if (document->bitsPerChannel == 16)
                {
                    image16 = CreateInterleavedImage<uint16_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], canvasData[3], document->width, document->height);
                }
                else if (document->bitsPerChannel == 32)
                {
                    image32 = CreateInterleavedImage<float32_t>(&allocator, canvasData[0], canvasData[1], canvasData[2], canvasData[3], document->width, document->height);
                }
            }

            allocator.Free(canvasData[0]);
            allocator.Free(canvasData[1]);
            allocator.Free(canvasData[2]);
            allocator.Free(canvasData[3]);

            // get the layer name.
            // Unicode data is preferred because it is not truncated by Photoshop, but unfortunately it is optional.
            // fall back to the ASCII name in case no Unicode name was found.
            std::wstringstream layerName;
            if (layer->utf16Name)
            {
                layerName << reinterpret_cast<wchar_t*>(layer->utf16Name);
            }
            else
            {
                layerName << layer->name.c_str();
            }
        }

        DestroyLayerMaskSection(layerMaskSection, &allocator);
    }

    // extract the image data section, if available. the image data section stores the final, merged image, as well as additional
    // alpha channels. this is only available when saving the document with "Maximize Compatibility" turned on.
    if (document->imageDataSection.length != 0)
    {
        psd::ImageDataSection* imageData = ParseImageDataSection(document, &file, &allocator);
        if (imageData)
        {
            // interleave the planar image data into one RGB or RGBA image.
            // store the rest of the (alpha) channels and the transparency mask separately.
            const unsigned int imageCount = imageData->imageCount;

            // note that an image can have more than 3 channels, but still no transparency mask in case all extra channels
            // are actual alpha channels.
            bool isRgb = false;
            if (imageCount == 3)
            {
                // imageData->images[0], imageData->images[1] and imageData->images[2] contain the R, G, and B channels of the merged image.
                // they are always the size of the canvas/document, so we can interleave them using imageUtil::InterleaveRGB directly.
                isRgb = true;
            }
            else if (imageCount >= 4)
            {
                // check if we really have a transparency mask that belongs to the "main" merged image.
                if (hasTransparencyMask)
                {
                    // we have 4 or more images/channels, and a transparency mask.
                    // this means that images 0-3 are RGBA, respectively.
                    isRgb = false;
                }
                else
                {
                    // we have 4 or more images stored in the document, but none of them is the transparency mask.
                    // this means we are dealing with RGB (!) data, and several additional alpha channels.
                    isRgb = true;
                }
            }

            uint8_t* image8 = nullptr;
            uint16_t* image16 = nullptr;
            float32_t* image32 = nullptr;
            if (isRgb)
            {
                // RGB
                if (document->bitsPerChannel == 8)
                {
                    image8 = CreateInterleavedImage<uint8_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, document->width, document->height);
                }
                else if (document->bitsPerChannel == 16)
                {
                    image16 = CreateInterleavedImage<uint16_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, document->width, document->height);
                }
                else if (document->bitsPerChannel == 32)
                {
                    image32 = CreateInterleavedImage<float32_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, document->width, document->height);
                }
            }
            else
            {
                // RGBA
                if (document->bitsPerChannel == 8)
                {
                    image8 = CreateInterleavedImage<uint8_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, imageData->images[3].data, document->width, document->height);
                }
                else if (document->bitsPerChannel == 16)
                {
                    image16 = CreateInterleavedImage<uint16_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, imageData->images[3].data, document->width, document->height);
                }
                else if (document->bitsPerChannel == 32)
                {
                    image32 = CreateInterleavedImage<float32_t>(&allocator, imageData->images[0].data, imageData->images[1].data, imageData->images[2].data, imageData->images[3].data, document->width, document->height);
                }
            }

            if (document->bitsPerChannel == 8) {
                img = cv::Mat(document->height, document->width, CV_8UC4, image8).clone();
            }
            else if (document->bitsPerChannel == 16) {
                cv::Mat(document->height, document->width, CV_16UC4, image16).convertTo(img, CV_8UC4, 255.0 / 65535.0);
            }
            else if (document->bitsPerChannel == 32) {
                cv::Mat(document->height, document->width, CV_32FC4, image32).convertTo(img, CV_8UC4, 255.0);
            }

            allocator.Free(image8);
            allocator.Free(image16);
            allocator.Free(image32);

            DestroyImageDataSection(imageData, &allocator);
        }
    }

    // don't forget to destroy the document, and close the file.
    DestroyDocument(document, &allocator);
    file.Close();

    return img;
}


cv::Mat ImageDatabase::loadTGA_HDR(wstring_view path, const vector<uint8_t>& buf) {
    int width, height, channels;

    // 使用stb_image从内存缓冲区加载图像
    uint8_t* pxData = stbi_load_from_memory(buf.data(), (int)buf.size(), &width, &height, &channels, 0);

    if (!pxData) {
        JARK_LOG("Failed to load image: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    // 确定OpenCV的色彩空间
    int cv_type;
    switch (channels) {
    case 1: cv_type = CV_8UC1; break;
    case 3: cv_type = CV_8UC3; break;
    case 4: cv_type = CV_8UC4; break;
    default:
        stbi_image_free(pxData);
        JARK_LOG("Unsupported number of channels:{} {}", channels, jarkUtils::wstringToUtf8(path));
        return {};
    }

    auto result = cv::Mat(height, width, cv_type, pxData).clone();
    stbi_image_free(pxData);

    return result;
}


cv::Mat ImageDatabase::loadSVG(wstring_view path, const vector<uint8_t>& buf) {
    const int maxEdge = 4000;
    static bool isInitFont = false;

    if (!isInitFont) {
        isInitFont = true;
        auto rc = jarkUtils::GetResource(IDR_MSYHMONO_TTF, L"TTF");
        JARK_LOG("loadSVG initFont: size:{} ptr:{:x}", rc.size, (size_t)rc.ptr);
        if (!lunasvg_add_font_face_from_data("", false, false, rc.ptr, rc.size, nullptr, nullptr)) {
            JARK_LOG("loadSVG initFont Fail !!!\nlunasvg_add_font_face_from_data");
        }
        else {
            JARK_LOG("loadSVG initFont Done!");
        }
    }

    SVGPreprocessor preprocessor;
    auto SVGData = preprocessor.preprocessSVG((const char*)buf.data(), buf.size());

    auto dataPtr = SVGData.empty() ? (const char*)buf.data() : SVGData.data();
    size_t dataBytes = SVGData.empty() ? buf.size() : SVGData.length();

    auto document = lunasvg::Document::loadFromData(dataPtr, dataBytes);
    if (!document) {
        JARK_LOG("Failed to load SVG data {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    if (document->height() == 0 || document->width() == 0) {
        JARK_LOG("Failed to load SVG: height/width == 0 {}", jarkUtils::wstringToUtf8(path));
        return {};
    }
    // 宽高比例
    const float AspectRatio = document->width() / document->height();
    int height, width;

    if (AspectRatio == 1) {
        height = width = maxEdge;
    }
    else if (AspectRatio > 1) {
        width = maxEdge;
        height = int(maxEdge / AspectRatio);
    }
    else {
        height = maxEdge;
        width = int(maxEdge * AspectRatio);
    }

    auto bitmap = document->renderToBitmap(width, height);
    if (bitmap.isNull()) {
        JARK_LOG("Failed to render SVG to bitmap {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    return cv::Mat(height, width, CV_8UC4, bitmap.data(), bitmap.stride()).clone();
}


cv::Mat ImageDatabase::loadImageWinCOM(wstring_view path, const vector<uint8_t>& buf) {
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        JARK_LOG("Failed to initialize COM library.");
        return {};
    }

    IWICImagingFactory* pFactory = NULL;
    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (LPVOID*)&pFactory);
    if (FAILED(hr)) {
        JARK_LOG("Failed to create WIC Imaging Factory.");
        CoUninitialize();
        return {};
    }

    IStream* pStream = NULL;
    hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (FAILED(hr)) {
        JARK_LOG("Failed to create stream.");
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    ULONG bytesWritten;
    hr = pStream->Write(buf.data(), (ULONG)buf.size(), &bytesWritten);
    if (FAILED(hr) || bytesWritten != buf.size()) {
        JARK_LOG("Failed to write to stream.");
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    LARGE_INTEGER li = { 0 };
    hr = pStream->Seek(li, STREAM_SEEK_SET, NULL);
    if (FAILED(hr)) {
        JARK_LOG("Failed to seek stream.");
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    IWICBitmapDecoder* pDecoder = NULL;
    hr = pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        JARK_LOG("Failed to create decoder from stream.");
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    IWICBitmapFrameDecode* pFrame = NULL;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) {
        JARK_LOG("Failed to get frame from decoder.");
        pDecoder->Release();
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    UINT width, height;
    hr = pFrame->GetSize(&width, &height);
    if (FAILED(hr)) {
        JARK_LOG("Failed to get image size.");
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    IWICFormatConverter* pConverter = NULL;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) {
        JARK_LOG("Failed to create format converter.");
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        JARK_LOG("Failed to initialize format converter.");
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    cv::Mat mat(height, width, CV_8UC4);
    hr = pConverter->CopyPixels(NULL, width * 4, width * height * 4, mat.data);
    if (FAILED(hr)) {
        JARK_LOG("Failed to copy pixels.");
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        pFactory->Release();
        CoUninitialize();
        return {};
    }

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pStream->Release();
    pFactory->Release();
    CoUninitialize();

    return mat;
}

static std::string parseImageAssetInfo(wstring_view path, ImageAsset& imageAsset) {
    std::ostringstream oss;
    oss << "图像信息：" << jarkUtils::wstringToUtf8(path) << '\n';

    auto& image = imageAsset.primaryFrame;

    if (image.empty() && imageAsset.frames.empty() && imageAsset.exifInfo.empty()) {
        oss << "图像为空，无法解析信息";
        return oss.str();
    }

    if (!image.empty()) {
        oss << "====== 主图信息 ======\n";

        oss << "宽度：" << image.cols << '\n';
        oss << "高度：" << image.rows << '\n';

        int channels = image.channels();
        oss << "通道：" << channels << '\n';

        int depth = image.depth();
        std::string depthStr;
        switch (depth) {
        case CV_8U:  depthStr = "8U (8位无符号)"; break;
        case CV_8S:  depthStr = "8S (8位有符号)"; break;
        case CV_16U: depthStr = "16U (16位无符号)"; break;
        case CV_16S: depthStr = "16S (16位有符号)"; break;
        case CV_32S: depthStr = "32S (32位有符号整数)"; break;
        case CV_32F: depthStr = "32F (32位浮点)"; break;
        case CV_64F: depthStr = "64F (64位浮点)"; break;
        default:     depthStr = "未知类型 (" + std::to_string(depth) + ")"; break;
        }
        oss << "位深度：" << depthStr << '\n';

        int type = image.type();
        oss << "数据类型：" << type << " (CV_"
            << ((depth == CV_8U) ? "8U" :
                (depth == CV_8S) ? "8S" :
                (depth == CV_16U) ? "16U" :
                (depth == CV_16S) ? "16S" :
                (depth == CV_32S) ? "32S" :
                (depth == CV_32F) ? "32F" :
                (depth == CV_64F) ? "64F" : "UNKNOWN")
            << "C" << channels << ")\n";

        oss << "总像素数：" << (static_cast<int64_t>(image.rows) * image.cols) << '\n';
        oss << "总字节数：" << image.total() * image.elemSize() << '\n';
        oss << "每像素字节数：" << image.elemSize() << '\n';
        oss << "每通道字节数：" << image.elemSize1() << '\n';
        oss << "内存连续：" << (image.isContinuous() ? "是" : "否") << '\n';
        oss << "步长(行距)：" << image.step << " 字节\n";

        if (channels == 1) {
            oss << "色彩空间：灰度图\n";
        }
        else if (channels == 3) {
            oss << "色彩空间：RGB/BGR（需根据读取方式确认）\n";
        }
        else if (channels == 4) {
            oss << "色彩空间：RGBA/BGRA（含Alpha通道）\n";
        }
        else {
            oss << "色彩空间：多通道(" << channels << "通道)\n";
        }

        if (depth == CV_8U) {
            oss << "注：通常由 imread 读取的图像为8位无符号整数\n";
        }
    }

    if (!imageAsset.frames.empty()) {
        oss << "====== 动态帧信息 ======\n";

        oss << "首帧宽度：" << imageAsset.frames[0].cols << '\n';
        oss << "首帧高度：" << imageAsset.frames[0].rows << '\n';
        oss << "帧数量：" << imageAsset.frames.size() << '\n';
        oss << "各帧时长(ms): ";
        int idx = 0;
        for (const auto& duration : imageAsset.frameDurations) {
            oss << std::format("[{}]{}, ", idx++, duration);
        }
        oss << '\n';
    }

    if (!imageAsset.exifInfo.empty()) {
        oss << imageAsset.exifInfo;
    }

    return oss.str();
}

// 只接受 8 位通道
static void convertMatToCV_8U(cv::Mat& mat) {
    if (mat.empty() || mat.depth() == CV_8U)
        return;

    switch (mat.depth()) {
    case CV_16U: {
        // 16位无符号整型：线性缩放到 [0, 255]
        mat.convertTo(mat, CV_8U, 255.0 / 65535.0);
        break;
    }
    case CV_32F: {
        // 浮点型：检测是否在 [0,1] 范围内，若是则直接乘255，否则动态缩放
        double minVal, maxVal;
        cv::minMaxLoc(mat, &minVal, &maxVal);

        if (minVal >= 0.0 && maxVal <= 1.0) {
            // 假设是标准归一化浮点图（如来自深度学习模型或HDR解码）
            mat.convertTo(mat, CV_8U, 255.0);
        }
        else if (minVal == maxVal) {
            mat.convertTo(mat, CV_8U, 0.0, 127.5);
        }
        else {
            // 通用动态范围缩放（如 [0,1000] 或 [-10, 200]，虽罕见但安全）
            mat.convertTo(mat, CV_8U, 255.0 / (maxVal - minVal), -minVal * 255.0 / (maxVal - minVal));
        }
        break;
    }
    default: {
        // 其他罕见情况（如 CV_64F），统一动态缩放（保守兜底）
        double minVal, maxVal;
        cv::minMaxLoc(mat, &minVal, &maxVal);
        if (minVal == maxVal) {
            mat.convertTo(mat, CV_8U, 0.0, 127.5);
        }
        else {
            mat.convertTo(mat, CV_8U, 255.0 / (maxVal - minVal), -minVal * 255.0 / (maxVal - minVal));
        }
        break;
    }
    }
}

static void convertImageAssetToCV_8U(ImageAsset& imageAsset) {
    convertMatToCV_8U(imageAsset.primaryFrame);
    for (auto& frame : imageAsset.frames) {
        convertMatToCV_8U(frame);
    }
}

// 已支持 gif apng png webp 动图
ImageAsset ImageDatabase::loadAnimation(wstring_view path, const vector<uint8_t>& buf) {
    cv::Animation animation;
    ImageAsset imageAsset;

    bool success = cv::imdecodeanimation(buf, animation);

    if (!success || animation.frames.empty()) {
        imageAsset.primaryFrame = getErrorTipsMat();
    }
    else if (animation.frames.size() == 1) {
        imageAsset.format = ImageFormat::Still;
        imageAsset.primaryFrame = std::move(animation.frames[0]);
    }
    else {
        imageAsset.format = ImageFormat::Animated;
        imageAsset.frames = std::move(animation.frames);
        imageAsset.frameDurations = std::move(animation.durations);
        for (auto& duration : imageAsset.frameDurations) {
            if (0 == duration)
                duration = 16;
        }
    }
    return imageAsset;
}


cv::Mat ImageDatabase::loadImageOpenCV(wstring_view path, const vector<uint8_t>& buf) {
    cv::Mat img;
    try {
        img = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    }
    catch (cv::Exception e) {
        JARK_LOG("cvMat cannot decode: {} [{}]", jarkUtils::wstringToUtf8(path), e.what());
        return {};
    }

    if (img.empty()) {
        JARK_LOG("cvMat cannot decode: {}", jarkUtils::wstringToUtf8(path));
        return {};
    }

    if (img.channels() == 1)
        cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);

    if (img.channels() != 3 && img.channels() != 4) {
        JARK_LOG("cvMat unsupport channel: {}", img.channels());
        return {};
    }

    convertMatToCV_8U(img);

    return img;
}


// 辅助函数，用于从 PFM 头信息中提取尺寸和比例因子
static bool parsePFMHeader(const vector<uint8_t>& buf, int& width, int& height, float& scaleFactor, bool& isColor, size_t& dataOffset) {
    string header(reinterpret_cast<const char*>(buf.data()), 2);

    // 判断是否是RGB（PF）或灰度（Pf）
    if (header == "PF") {
        isColor = true;
    }
    else if (header == "Pf") {
        isColor = false;
    }
    else {
        JARK_LOG("Invalid PFM format!");
        return false;
    }

    // 查找下一行的宽度和高度
    size_t maxOffset = buf.size() > 100 ? 100 : buf.size();
    size_t offset = 2;

    while ((buf[offset] == '\n' || buf[offset] == ' ') && offset < maxOffset) offset++;
    string dimLine;
    while (buf[offset] != '\n' && offset < maxOffset) dimLine += buf[offset++];

    switch (sscanf(dimLine.c_str(), "%d %d", &width, &height)) {
    case 1: {
        offset++;
        string dimLine;
        while (buf[offset] != '\n' && offset < maxOffset) dimLine += buf[offset++];
        if (sscanf(dimLine.c_str(), "%d", &height) != 1) {
            JARK_LOG("parsePFMHeader fail!");
            return false;
        }
    }break;

    case 2:break;

    default: {
        JARK_LOG("parsePFMHeader fail!");
        return false;
    }
    }

    // 查找比例因子
    string scaleLine;
    offset++;
    while (buf[offset] != '\n' && offset < maxOffset) scaleLine += buf[offset++];

    scaleFactor = std::stof(scaleLine);
    dataOffset = offset + 1;

    return true;
}


cv::Mat ImageDatabase::loadPFM(wstring_view path, const vector<uint8_t>& buf) {
    int width, height;
    float scaleFactor;
    bool isColor;
    size_t dataOffset;

    // 解析 PFM 头信息
    if (!parsePFMHeader(buf, width, height, scaleFactor, isColor, dataOffset)) {
        JARK_LOG("Failed to parse PFM header!");
        return {};
    }

    // 创建 OpenCV Mat，格式为 CV_32FC3（或 CV_32FC1 对于灰度图）
    cv::Mat image(height, width, isColor ? CV_32FC3 : CV_32FC1, (void*)(buf.data() + dataOffset));

    // 如果比例因子为负数，图像需要垂直翻转
    if (scaleFactor < 0) {
        cv::flip(image, image, 0);
        scaleFactor = -scaleFactor;
    }

    // 将图像从浮点数格式缩放到范围 [0, 255]
    image *= 255.0f / scaleFactor;

    // 将图像转换为 8 位格式
    image.convertTo(image, CV_8UC3);

    // 转换到 BGR 格式（如果是彩色图像）
    cv::cvtColor(image, image, isColor ? cv::COLOR_RGB2BGR : cv::COLOR_GRAY2BGR);

    return image;
}


cv::Mat ImageDatabase::loadQOI(wstring_view path, const vector<uint8_t>& buf) {
    cv::Mat mat;
    qoi_desc desc;
    auto pixels = qoi_decode(buf.data(), (int)buf.size(), &desc, 0);
    if (!pixels)
        return mat;

    switch (desc.channels) {
    case 3:  // RGB
        mat = cv::Mat(desc.height, desc.width, CV_8UC3, pixels);
        cvtColor(mat, mat, cv::COLOR_RGB2BGR);  // QOI使用RGB格式，OpenCV默认使用BGR
        break;
    case 4:  // RGBA
        mat = cv::Mat(desc.height, desc.width, CV_8UC4, pixels);
        cvtColor(mat, mat, cv::COLOR_RGBA2BGRA);  // 转换RGBA到BGRA
        break;
    }

    auto ret = mat.clone();
    free(pixels);// 释放QOI解码分配的内存
    return ret;
}

// PCX文件头结构
#pragma pack(push, 1)
struct PCXHeader {
    uint8_t manufacturer;      // 固定为0x0A
    uint8_t version;           // 版本号
    uint8_t encoding;          // 编码方式(1=RLE)
    uint8_t bitsPerPixel;      // 每像素位数
    uint16_t xMin, yMin;       // 图像左上角坐标
    uint16_t xMax, yMax;       // 图像右下角坐标
    uint16_t hDpi, vDpi;       // 水平和垂直DPI
    uint8_t colormap[48];      // 16色调色板
    uint8_t reserved;          // 保留字节
    uint8_t numPlanes;         // 颜色平面数
    uint16_t bytesPerLine;     // 每行字节数
    uint16_t paletteInfo;      // 调色板类型
    uint16_t hScreenSize;      // 水平屏幕尺寸
    uint16_t vScreenSize;      // 垂直屏幕尺寸
    uint8_t filler[54];        // 填充到128字节
};
#pragma pack(pop)

// RLE解码函数
static std::vector<uint8_t> decodeRLE(const uint8_t* data, size_t dataSize, size_t expectedSize) {
    std::vector<uint8_t> decoded;
    decoded.reserve(expectedSize);

    size_t pos = 0;
    while (pos < dataSize && decoded.size() < expectedSize) {
        uint8_t byte = data[pos++];

        // 如果高2位为11，表示是RLE编码
        if ((byte & 0xC0) == 0xC0) {
            uint8_t count = byte & 0x3F;  // 重复次数
            if (pos >= dataSize) break;
            uint8_t value = data[pos++];   // 要重复的值

            for (int i = 0; i < count && decoded.size() < expectedSize; i++) {
                decoded.push_back(value);
            }
        }
        else {
            decoded.push_back(byte);
        }
    }

    return decoded;
}

cv::Mat ImageDatabase::loadPCX(wstring_view path, const vector<uint8_t>& buf) {
    if (buf.size() < sizeof(PCXHeader)) {
        JARK_LOG("文件太小，不是有效的PCX文件");
        return cv::Mat();
    }

    PCXHeader header;
    std::memcpy(&header, buf.data(), sizeof(PCXHeader));

    // 验证
    if (header.manufacturer != 0x0A) {
        JARK_LOG("无效的PCX文件标识");
        return cv::Mat();
    }

    int width = header.xMax - header.xMin + 1;
    int height = header.yMax - header.yMin + 1;

    if (width <= 0 || height <= 0) {
        JARK_LOG("无效的图像尺寸 {} x {}", width, height);
        return cv::Mat();
    }

    // 解码图像数据
    const uint8_t* imageData = buf.data() + sizeof(PCXHeader);
    size_t imageDataSize = buf.size() - sizeof(PCXHeader);

    // 处理256色调色板(如果存在)
    std::vector<cv::Vec3b> palette;
    if (header.bitsPerPixel == 8 && header.numPlanes == 1) {
        // 256色调色板在文件末尾
        if (buf.size() >= 769 && buf[buf.size() - 769] == 0x0C) {
            imageDataSize -= 769;
            const uint8_t* paletteData = buf.data() + buf.size() - 768;

            for (int i = 0; i < 256; i++) {
                uint8_t r = paletteData[i * 3];
                uint8_t g = paletteData[i * 3 + 1];
                uint8_t b = paletteData[i * 3 + 2];
                palette.push_back(cv::Vec3b(b, g, r));  // OpenCV使用BGR
            }
        }
    }

    // 计算每行总字节数
    size_t bytesPerScanline = header.bytesPerLine * header.numPlanes;
    size_t expectedSize = bytesPerScanline * height;

    // RLE解码
    std::vector<uint8_t> decoded = decodeRLE(imageData, imageDataSize, expectedSize);

    if (decoded.size() < expectedSize) {
        JARK_LOG("解码数据不完整");
        return cv::Mat();
    }

    cv::Mat result;

    // 根据位深度和平面数处理图像
    if (header.bitsPerPixel == 8 && header.numPlanes == 1) {
        // 256色索引图像
        result = cv::Mat(height, width, CV_8UC3);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint8_t index = decoded[y * header.bytesPerLine + x];
                if (!palette.empty()) {
                    result.at<cv::Vec3b>(y, x) = palette[index];
                }
                else {
                    result.at<cv::Vec3b>(y, x) = cv::Vec3b(index, index, index);
                }
            }
        }
    }
    else if (header.bitsPerPixel == 8 && header.numPlanes == 3) {
        // 24位RGB图像(平面格式)
        result = cv::Mat(height, width, CV_8UC3);

        for (int y = 0; y < height; y++) {
            size_t rowOffset = y * bytesPerScanline;
            for (int x = 0; x < width; x++) {
                uint8_t r = decoded[rowOffset + x];
                uint8_t g = decoded[rowOffset + header.bytesPerLine + x];
                uint8_t b = decoded[rowOffset + header.bytesPerLine * 2 + x];
                result.at<cv::Vec3b>(y, x) = cv::Vec3b(b, g, r);
            }
        }
    }
    else if (header.bitsPerPixel == 8 && header.numPlanes == 4) {
        // 32位RGBA图像(平面格式)
        result = cv::Mat(height, width, CV_8UC4);

        for (int y = 0; y < height; y++) {
            size_t rowOffset = y * bytesPerScanline;
            for (int x = 0; x < width; x++) {
                uint8_t r = decoded[rowOffset + x];
                uint8_t g = decoded[rowOffset + header.bytesPerLine + x];
                uint8_t b = decoded[rowOffset + header.bytesPerLine * 2 + x];
                uint8_t a = decoded[rowOffset + header.bytesPerLine * 3 + x];
                result.at<cv::Vec4b>(y, x) = cv::Vec4b(b, g, r, a);
            }
        }
    }
    else if (header.bitsPerPixel == 1 && header.numPlanes == 1) {
        // 单色图像
        result = cv::Mat(height, width, CV_8UC1);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int byteIdx = y * header.bytesPerLine + (x / 8);
                int bitIdx = 7 - (x % 8);
                uint8_t pixel = (decoded[byteIdx] >> bitIdx) & 1;
                result.at<uint8_t>(y, x) = pixel ? 255 : 0;
            }
        }
    }
    else {
        JARK_LOG("不支持的PCX格式: {} bpp, {} planes", (int)header.bitsPerPixel, (int)header.numPlanes);
    }

    return result;
}

static std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::string> unzipLivp(const std::vector<uint8_t>& livpFileBuff) {
    zlib_filefunc_def memory_filefunc;
    memset(&memory_filefunc, 0, sizeof(zlib_filefunc_def));

    struct membuf {
        const std::vector<uint8_t>& buffer;
        size_t position;
    } mem = { livpFileBuff, 0 };

    memory_filefunc.opaque = (voidpf)&mem;

    memory_filefunc.zopen_file = [](voidpf opaque, const char* filename, int mode) -> voidpf {
        return opaque;
        };

    memory_filefunc.zread_file = [](voidpf opaque, voidpf stream, void* buf, uLong size) -> uLong {
        membuf* mem = (membuf*)opaque;
        size_t remaining = mem->buffer.size() - mem->position;
        size_t to_read = (size < remaining) ? size : remaining;
        if (to_read > 0) {
            memcpy(buf, mem->buffer.data() + mem->position, to_read);
            mem->position += to_read;
        }
        return (uLong)to_read;
        };

    memory_filefunc.zwrite_file = [](voidpf, voidpf, const void*, uLong) -> uLong {
        return 0;
        };

    memory_filefunc.ztell_file = [](voidpf opaque, voidpf stream) -> long {
        return (long)((membuf*)opaque)->position;
        };

    memory_filefunc.zseek_file = [](voidpf opaque, voidpf stream, uLong offset, int origin) -> long {
        membuf* mem = (membuf*)opaque;
        size_t new_position = 0;

        switch (origin) {
        case ZLIB_FILEFUNC_SEEK_CUR:
            new_position = mem->position + offset;
            break;
        case ZLIB_FILEFUNC_SEEK_END:
            new_position = mem->buffer.size() + offset;
            break;
        case ZLIB_FILEFUNC_SEEK_SET:
            new_position = offset;
            break;
        default:
            return -1;
        }

        if (new_position > mem->buffer.size()) {
            return -1;
        }

        mem->position = new_position;
        return 0;
        };

    memory_filefunc.zclose_file = [](voidpf, voidpf) -> int {
        return 0;
        };

    memory_filefunc.zerror_file = [](voidpf, voidpf) -> int {
        return 0;
        };

    unzFile zipfile = unzOpen2("__memory__", &memory_filefunc);
    if (!zipfile) {
        return {};
    }

    if (unzGoToFirstFile(zipfile) != UNZ_OK) {
        unzClose(zipfile);
        return {};
    }

    std::vector<uint8_t> image_data;
    std::vector<uint8_t> video_data;
    std::string imgExt;

    do {
        unz_file_info file_info;
        char filename[256] = { 0 };

        if (unzGetCurrentFileInfo(zipfile, &file_info, filename, sizeof(filename), nullptr, 0, nullptr, 0) != UNZ_OK) {
            continue;
        }

        std::string file_name(filename);
        if (file_name.empty() || file_name.length() < 4)
            continue;

        for (int i = (int)file_name.length() - 4; i < file_name.length(); i++)
            file_name[i] = std::tolower(file_name[i]);

        if (file_name.ends_with("jpg") || file_name.ends_with("jpeg") ||
            file_name.ends_with("heic") || file_name.ends_with("heif")) {

            if (unzOpenCurrentFile(zipfile) != UNZ_OK) {
                continue;
            }

            image_data.resize(file_info.uncompressed_size);
            int bytes_read = unzReadCurrentFile(zipfile, image_data.data(), file_info.uncompressed_size);

            unzCloseCurrentFile(zipfile);

            if (bytes_read > 0 && static_cast<uLong>(bytes_read) == file_info.uncompressed_size) {
                imgExt = (file_name.ends_with("heic") || file_name.ends_with("heif")) ? "heic" : "jpg";
            }
            else {
                image_data.clear();
            }
        }

        if (file_name.ends_with("mov") || file_name.ends_with("mp4")) {
            if (unzOpenCurrentFile(zipfile) != UNZ_OK) {
                continue;
            }

            video_data.resize(file_info.uncompressed_size);
            int bytes_read = unzReadCurrentFile(zipfile, video_data.data(), file_info.uncompressed_size);

            unzCloseCurrentFile(zipfile);

            if (bytes_read <= 0 || static_cast<uLong>(bytes_read) != file_info.uncompressed_size) {
                video_data.clear();
            }
        }
    } while (unzGoToNextFile(zipfile) == UNZ_OK);

    unzClose(zipfile);

    return { image_data, video_data, imgExt };
}


void ImageDatabase::handleExifOrientation(int orientation, cv::Mat& img) {
    if (img.empty())
        return;

    switch (orientation) {
    case 2: // 水平翻转
        cv::flip(img, img, 1);
        break;
    case 3: // 旋转180度
        cv::rotate(img, img, cv::ROTATE_180);
        break;
    case 4: // 垂直翻转
        cv::flip(img, img, 0);
        break;
    case 5: // 顺时针旋转90度后垂直翻转
        cv::rotate(img, img, cv::ROTATE_90_CLOCKWISE);
        cv::flip(img, img, 0);
        break;
    case 6: // 顺时针旋转90度
        cv::rotate(img, img, cv::ROTATE_90_CLOCKWISE);
        break;
    case 7: // 顺时针旋转90度后水平翻转
        cv::rotate(img, img, cv::ROTATE_90_CLOCKWISE);
        cv::flip(img, img, 1);
        break;
    case 8: // 逆时针旋转90度
        cv::rotate(img, img, cv::ROTATE_90_COUNTERCLOCKWISE);
        break;
    }
}

// 苹果实况照片
ImageAsset ImageDatabase::loadLivp(wstring_view path, const vector<uint8_t>& fileBuf) {
    auto [imageFileData, videoFileData, imageExt] = unzipLivp(fileBuf);
    if (imageFileData.empty()) {
        auto exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        return { ImageFormat::Still, getErrorTipsMat(), {}, {}, exifInfo };
    }

    cv::Mat img;
    if (imageExt == "heic" || imageExt == "heif") {
        img = loadHeic(path, imageFileData);
    }
    else if (imageExt == "jpg" || imageExt == "jpeg") {
        img = loadImageOpenCV(path, imageFileData);
    }

    auto exifTmp = ExifParse::getExif(path, imageFileData.data(), imageFileData.size());
    if (imageExt == "jpg" || imageExt == "jpeg") { //heic 已经在解码过程应用了裁剪/旋转/镜像等操作
        const size_t idx = exifTmp.find(getUIString(53));
        if (idx != string::npos) {
            handleExifOrientation(exifTmp[idx + strlen(getUIString(53))] - '0', img);
        }
    }
    auto exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size()) + exifTmp;

    if (videoFileData.empty()) {
        return { ImageFormat::Still, img, {}, {}, exifInfo };
    }

    auto frames = VideoDecoder::DecodeVideoFrames(videoFileData.data(), videoFileData.size());

    if (frames.empty()) {
        return { ImageFormat::Still, img, {}, {}, exifInfo };
    }

    return { ImageFormat::Animated, img, frames, std::vector<int>(frames.size(), 33),  exifInfo };
}

// 从xmp获取视频数据大小  https://developer.android.com/media/platform/motion-photo-format?hl=zh-cn
// 旧标准（MicroVideo）    Xmp.GCamera.MicroVideoOffset: xxxx
// 新标准（MotionPhoto）   Xmp.Container.Directory[xxx]/Item:Length: xxxx
static size_t getVideoSize(string_view exifStr) {
    constexpr std::string_view targetKey1 = "Xmp.GCamera.MicroVideoOffset: ";
    constexpr size_t targetKey1Len = targetKey1.length();

    size_t valueStart = 0;
    size_t pos = exifStr.find(targetKey1);
    if (pos != std::string::npos) {
        valueStart = pos + targetKey1Len;
    }
    else {
        pos = exifStr.find("Item:Semantic: MotionPhoto");
        if (pos == std::string::npos) {
            return 0;
        }
        constexpr std::string_view targetKey2 = "Item:Length: ";
        constexpr size_t targetKey2Len = targetKey2.length();
        pos = exifStr.find(targetKey2, pos);
        if (pos == std::string::npos) {
            return 0;
        }
        valueStart = pos + targetKey2Len;
    }

    size_t valueEnd = valueStart;
    while ('0' <= exifStr[valueEnd] && exifStr[valueEnd] <= '9') valueEnd++;

    if (valueEnd <= valueStart || valueEnd >= exifStr.length()) {
        return 0;
    }

    std::string_view valueStr(exifStr.data() + valueStart, valueEnd - valueStart);

    int value = 0;
    try {
        value = std::stoi(std::string(valueStr));
    }
    catch (const std::exception&) {
        return 0;
    }
    return value;
}

// Android 实况照片 jpg/jpeg/heic/heif
ImageAsset ImageDatabase::loadMotionPhoto(wstring_view path, const vector<uint8_t>& fileBuf, bool isJPG = false) {
    auto img = isJPG ? loadImageOpenCV(path, fileBuf) : loadHeic(path, fileBuf);
    if (img.empty())
        img = loadImageWinCOM(path, fileBuf);

    if (img.empty()) {
        auto exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        return { ImageFormat::Still, getErrorTipsMat(), {}, {}, exifInfo };
    }

    auto exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size()) +
        ExifParse::getExif(path, fileBuf.data(), fileBuf.size());

    if (isJPG) {
        const size_t idx = exifInfo.find(getUIString(53));
        if (idx != string::npos) {
            handleExifOrientation(exifInfo[idx + strlen(getUIString(53))] - '0', img);
        }
    }

    auto videoSize = getVideoSize(exifInfo);
    if (videoSize == 0 || videoSize >= fileBuf.size()) {
        return { ImageFormat::Still, img, {}, {}, exifInfo };
    }

    auto frames = VideoDecoder::DecodeVideoFrames(fileBuf.data() + fileBuf.size() - videoSize, videoSize);
    if (frames.empty()) {
        return { ImageFormat::Still, img, {}, {}, exifInfo };
    }

    return { ImageFormat::Animated, img, frames, std::vector<int>(frames.size(), 33),  exifInfo };
}

ImageAsset ImageDatabase::myLoader(const wstring& path) {
    FunctionTimeCount FunctionTimeCount(__func__);
    JARK_LOG("loading: {}", jarkUtils::wstringToUtf8(path));

    if (path.length() < 4) {
        JARK_LOG("path.length() < 4: {}", jarkUtils::wstringToUtf8(path));
        return { ImageFormat::Still, getErrorTipsMat(), {}, {}, "" };
    }

    auto f = _wfopen(path.data(), L"rb");
    if (f == nullptr) {
        JARK_LOG("path canot open: {}", jarkUtils::wstringToUtf8(path));
        return { ImageFormat::Still, getErrorTipsMat(), {}, {}, "" };
    }

    fseek(f, 0, SEEK_END);
    auto fileSize = ftell(f);

    if (fileSize < 16) {
        fclose(f);
        JARK_LOG("path fileSize < 16: {}", jarkUtils::wstringToUtf8(path));
        return { ImageFormat::Still, getErrorTipsMat(), {}, {}, "" };
    }

    fseek(f, 0, SEEK_SET);
    vector<uint8_t> fileBuf(fileSize, 0);
    fread(fileBuf.data(), 1, fileSize, f);
    fclose(f);

    auto dotPos = path.rfind(L'.');
    auto ext = wstring((dotPos != std::wstring::npos && dotPos < path.size() - 1) ?
        path.substr(dotPos + 1) : path);
    for (auto& c : ext)	c = std::tolower(c);

    // 动态图
    if (opencvAnimationExt.contains(ext)) {
        auto imageAsset = loadAnimation(path, fileBuf);

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = getErrorTipsMat();
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
            return imageAsset;
        }
        else if (imageAsset.format == ImageFormat::Still) {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.primaryFrame.cols, imageAsset.primaryFrame.rows,
                fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
            return imageAsset;
        }

        // 以下情况是动图
        if (ext == L"gif") {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size());
        }
        else {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        return imageAsset;
    }
    else if (ext == L"bpg") { //静态或动画
        auto imageAsset = loadBPG(path, fileBuf);

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = getErrorTipsMat();
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        }
        else if (imageAsset.format == ImageFormat::Still) {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.primaryFrame.cols, imageAsset.primaryFrame.rows,
                fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        else {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        return imageAsset;
    }
    else if (ext == L"jxl") { //静态或动画
        auto imageAsset = loadJXL(path, fileBuf);

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.primaryFrame = loadImageWinCOM(path, fileBuf);
            if (!imageAsset.primaryFrame.empty()) {
                imageAsset.format = ImageFormat::Still;
            }
        }

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = getErrorTipsMat();
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        }
        else if (imageAsset.format == ImageFormat::Still) {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.primaryFrame.cols, imageAsset.primaryFrame.rows,
                fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        else {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        return imageAsset;
    }
    else if (ext == L"wp2") { // webp2 静态或动画
        auto imageAsset = loadWP2(path, fileBuf);
        if (imageAsset.format == ImageFormat::None) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = getErrorTipsMat();
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        }
        else if (imageAsset.format == ImageFormat::Still) {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.primaryFrame.cols, imageAsset.primaryFrame.rows,
                fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        else {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        return imageAsset;
    }
    else if (ext == L"avif" || ext == L"avifs") { // avif 静态或动画
        auto imageAsset = loadAvif(path, fileBuf);

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.primaryFrame = loadImageWinCOM(path, fileBuf);
            if (!imageAsset.primaryFrame.empty()) {
                imageAsset.format = ImageFormat::Still;
            }
        }

        if (imageAsset.format == ImageFormat::None) {
            imageAsset.format = ImageFormat::Still;
            imageAsset.primaryFrame = getErrorTipsMat();
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        }
        else if (imageAsset.format == ImageFormat::Still) {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.primaryFrame.cols, imageAsset.primaryFrame.rows,
                fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());

            const size_t idx = imageAsset.exifInfo.find(getUIString(53));
            if (idx != string::npos) {
                handleExifOrientation(imageAsset.exifInfo[idx + strlen(getUIString(53))] - '0', imageAsset.primaryFrame);
            }
        }
        else {
            imageAsset.exifInfo = ExifParse::getSimpleInfo(path, imageAsset.frames[0].cols, imageAsset.frames[0].rows, fileBuf.data(), fileBuf.size())
                + ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        }
        return imageAsset;
    }

    // 实况照片 包含一张图片和一段简短视频
    if (ext == L"livp") {
        return loadLivp(path, fileBuf);
    }
    else if (ext == L"jpg" || ext == L"jpeg") {
        return loadMotionPhoto(path, fileBuf, true);
    }
    else if (ext == L"heic" || ext == L"heif") {
        return loadMotionPhoto(path, fileBuf);
    }

    //以下是静态图
    cv::Mat img;
    string exifInfo;

    if (ext == L"jxr") {
        img = loadImageWinCOM(path, fileBuf);
    }
    else if (ext == L"tga" || ext == L"hdr") {
        img = loadTGA_HDR(path, fileBuf);
        exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size());
    }
    else if (ext == L"svg") {
        img = loadSVG(path, fileBuf);
        exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size());
        if (img.empty()) {
            img = getErrorTipsMat();
        }
    }
    else if (ext == L"qoi") {
        img = loadQOI(path, fileBuf);
        exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size());
        if (img.empty()) {
            img = getErrorTipsMat();
        }
    }
    else if (ext == L"pcx") {
        img = loadPCX(path, fileBuf);
        exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size());
        if (img.empty()) {
            img = getErrorTipsMat();
        }
    }
    else if (ext == L"ico" || ext == L"icon") {
        std::tie(img, exifInfo) = loadICO(path, fileBuf);
    }
    else if (ext == L"psd") {
        img = loadPSD(path, fileBuf);
        if (img.empty())
            img = getErrorTipsMat();
    }
    else if (ext == L"pfm") {
        img = loadPFM(path, fileBuf);
        if (img.empty()) {
            img = getErrorTipsMat();
            exifInfo = ExifParse::getSimpleInfo(path, 0, 0, fileBuf.data(), fileBuf.size());
        }
        else {
            exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size());
        }
    }
    else if (supportRaw.contains(ext)) {
        img = loadRaw(path, fileBuf);
        if (img.empty())
            img = loadImageWinCOM(path, fileBuf);
        if (img.empty())
            img = getErrorTipsMat();
    }

    if (img.empty())
        img = loadImageOpenCV(path, fileBuf);
    if (img.empty())
        img = loadImageWinCOM(path, fileBuf);

    if (exifInfo.empty()) {
        auto exifTmp = ExifParse::getExif(path, fileBuf.data(), fileBuf.size());
        if (!supportRaw.contains(ext)) { // RAW 格式已经在解码过程应用了裁剪/旋转/镜像等操作
            const size_t idx = exifTmp.find(getUIString(53));
            if (idx != string::npos) {
                handleExifOrientation(exifTmp[idx + strlen(getUIString(53))] - '0', img);
            }
        }
        exifInfo = ExifParse::getSimpleInfo(path, img.cols, img.rows, fileBuf.data(), fileBuf.size()) + exifTmp;
    }

    if (img.empty())
        img = getErrorTipsMat();

    return { ImageFormat::Still, img, {}, {}, exifInfo };
}

ImageAsset ImageDatabase::loader(const wstring& path) {
    auto imageAsset = myLoader(path);
    JARK_LOG("{}", parseImageAssetInfo(path, imageAsset));
    convertImageAssetToCV_8U(imageAsset);
    return imageAsset;
}