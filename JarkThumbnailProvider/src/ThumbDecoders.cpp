#include "ThumbDecoders.h"

#include "FormatSniffer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <wincodec.h>
#include <windows.h>

#include "avif/avif.h"
#include "DirectXTex.h"
#include "jxl/decode_cxx.h"
#include "jxl/resizable_parallel_runner_cxx.h"
#include "libheif/heif.h"
#include "libraw/libraw.h"
#include "liblepton.h"
#include "lunasvg.h"
#include "minizip/unzip.h"
#include "src/wp2/base.h"
#include "src/wp2/decode.h"

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "qoi.h"

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace JarkThumbnail {
namespace {
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* get() const noexcept { return ptr_; }
    T** put() noexcept {
        reset();
        return &ptr_;
    }

    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    void reset(T* value = nullptr) noexcept {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = value;
    }

private:
    T* ptr_ = nullptr;
};

bool checkedDimensions(uint32_t width, uint32_t height) noexcept {
    constexpr uint32_t kMaxDimension = 65536;
    constexpr uint64_t kMaxPixels = 512ULL * 1024ULL * 1024ULL;
    return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension &&
        static_cast<uint64_t>(width) * height <= kMaxPixels;
}

bool assignRgba(uint32_t width, uint32_t height, const uint8_t* rgba, bool hasAlpha, ThumbBitmap& out) {
    if (!checkedDimensions(width, height) || !rgba) {
        return false;
    }

    const size_t bytes = static_cast<size_t>(width) * height * 4ULL;
    out.width = width;
    out.height = height;
    out.hasAlpha = hasAlpha;
    out.bgra.resize(bytes);

    bool observedAlpha = false;
    for (size_t src = 0, dst = 0; src < bytes; src += 4, dst += 4) {
        const uint8_t alpha = rgba[src + 3];
        out.bgra[dst + 0] = rgba[src + 2];
        out.bgra[dst + 1] = rgba[src + 1];
        out.bgra[dst + 2] = rgba[src + 0];
        out.bgra[dst + 3] = alpha;
        observedAlpha = observedAlpha || alpha != 255;
    }

    out.hasAlpha = hasAlpha && observedAlpha;
    return true;
}

bool assignRgbaStrided(uint32_t width, uint32_t height, const uint8_t* rgba, size_t stride, bool hasAlpha, ThumbBitmap& out) {
    if (!checkedDimensions(width, height) || !rgba || stride < static_cast<size_t>(width) * 4ULL) {
        return false;
    }

    const size_t bytes = static_cast<size_t>(width) * height * 4ULL;
    out.width = width;
    out.height = height;
    out.hasAlpha = hasAlpha;
    out.bgra.resize(bytes);

    bool observedAlpha = false;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const size_t src = static_cast<size_t>(x) * 4ULL;
            const size_t dst = (static_cast<size_t>(y) * width + x) * 4ULL;
            const uint8_t alpha = row[src + 3];
            out.bgra[dst + 0] = row[src + 2];
            out.bgra[dst + 1] = row[src + 1];
            out.bgra[dst + 2] = row[src + 0];
            out.bgra[dst + 3] = alpha;
            observedAlpha = observedAlpha || alpha != 255;
        }
    }

    out.hasAlpha = hasAlpha && observedAlpha;
    return true;
}

bool decodeQoi(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    qoi_desc desc{};
    std::unique_ptr<void, decltype(&free)> pixels(qoi_decode(data.data(), static_cast<int>(data.size()), &desc, 4), free);
    if (!pixels) {
        return false;
    }

    return assignRgba(desc.width, desc.height, static_cast<const uint8_t*>(pixels.get()), desc.channels == 4, out);
}

bool decodeStb(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width, &height, &channels, STBI_rgb_alpha),
        stbi_image_free);
    if (!pixels || width <= 0 || height <= 0) {
        return false;
    }

    return assignRgba(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels.get(), channels == 4, out);
}

bool decodeJxl(std::span<const uint8_t> data, ThumbBitmap& out) {
    auto runner = JxlResizableParallelRunnerMake(nullptr);
    auto decoder = JxlDecoderMake(nullptr);
    if (!runner || !decoder) {
        return false;
    }

    JxlDecoderStatus status = JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    if (status != JXL_DEC_SUCCESS) {
        return false;
    }

    status = JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get());
    if (status != JXL_DEC_SUCCESS) {
        return false;
    }

    JxlBasicInfo basicInfo{};
    bool gotBasicInfo = false;
    JxlPixelFormat pixelFormat{ 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    std::vector<uint8_t> rgba;

    JxlDecoderSetInput(decoder.get(), data.data(), data.size());
    JxlDecoderCloseInput(decoder.get());

    for (;;) {
        status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(decoder.get(), &basicInfo) != JXL_DEC_SUCCESS ||
                !checkedDimensions(basicInfo.xsize, basicInfo.ysize)) {
                return false;
            }

            gotBasicInfo = true;
            JxlResizableParallelRunnerSetThreads(
                runner.get(),
                JxlResizableParallelRunnerSuggestThreads(basicInfo.xsize, basicInfo.ysize));
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (!gotBasicInfo) {
                return false;
            }

            size_t bufferSize = 0;
            if (JxlDecoderImageOutBufferSize(decoder.get(), &pixelFormat, &bufferSize) != JXL_DEC_SUCCESS ||
                bufferSize != static_cast<size_t>(basicInfo.xsize) * basicInfo.ysize * 4ULL) {
                return false;
            }

            rgba.resize(bufferSize);
            if (JxlDecoderSetImageOutBuffer(decoder.get(), &pixelFormat, rgba.data(), rgba.size()) != JXL_DEC_SUCCESS) {
                return false;
            }
        }
        else if (status == JXL_DEC_FULL_IMAGE) {
            return assignRgba(basicInfo.xsize, basicInfo.ysize, rgba.data(), basicInfo.alpha_bits > 0, out);
        }
        else if (status == JXL_DEC_SUCCESS) {
            return false;
        }
        else if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
            return false;
        }
    }
}

struct HeifContextDeleter {
    void operator()(heif_context* ctx) const noexcept { if (ctx) heif_context_free(ctx); }
};

struct HeifImageHandleDeleter {
    void operator()(heif_image_handle* handle) const noexcept { if (handle) heif_image_handle_release(handle); }
};

struct HeifImageDeleter {
    void operator()(heif_image* image) const noexcept { if (image) heif_image_release(image); }
};

bool decodeHeif(std::span<const uint8_t> data, ThumbBitmap& out) {
    std::unique_ptr<heif_context, HeifContextDeleter> ctx(heif_context_alloc());
    if (!ctx) {
        return false;
    }

    heif_error err = heif_context_read_from_memory_without_copy(ctx.get(), data.data(), data.size(), nullptr);
    if (err.code) {
        return false;
    }

    heif_image_handle* rawHandle = nullptr;
    err = heif_context_get_primary_image_handle(ctx.get(), &rawHandle);
    if (err.code) {
        return false;
    }
    std::unique_ptr<heif_image_handle, HeifImageHandleDeleter> handle(rawHandle);

    heif_image* rawImage = nullptr;
    err = heif_decode_image(handle.get(), &rawImage, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (err.code) {
        return false;
    }
    std::unique_ptr<heif_image, HeifImageDeleter> image(rawImage);

    int stride = 0;
    const uint8_t* pixels = heif_image_get_plane_readonly(image.get(), heif_channel_interleaved, &stride);
    const int width = heif_image_handle_get_width(handle.get());
    const int height = heif_image_handle_get_height(handle.get());
    return width > 0 && height > 0 && stride > 0 &&
        assignRgbaStrided(static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels, static_cast<size_t>(stride), true, out);
}

bool decodeAvif(std::span<const uint8_t> data, ThumbBitmap& out) {
    std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(), avifDecoderDestroy);
    if (!decoder) {
        return false;
    }

    decoder->ignoreExif = AVIF_TRUE;
    decoder->ignoreXMP = AVIF_TRUE;
    decoder->strictFlags = AVIF_STRICT_DISABLED;

    avifResult result = avifDecoderSetIOMemory(decoder.get(), data.data(), data.size());
    if (result != AVIF_RESULT_OK) {
        return false;
    }

    result = avifDecoderParse(decoder.get());
    if (result != AVIF_RESULT_OK || avifDecoderNextImage(decoder.get()) != AVIF_RESULT_OK) {
        return false;
    }

    if (!checkedDimensions(decoder->image->width, decoder->image->height)) {
        return false;
    }

    const bool hasAlpha = decoder->image->alphaPlane != nullptr && decoder->image->alphaRowBytes > 0;
    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.depth = 8;
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_BEST_QUALITY;
    rgb.avoidLibYUV = AVIF_FALSE;

    result = avifRGBImageAllocatePixels(&rgb);
    if (result != AVIF_RESULT_OK) {
        return false;
    }

    const auto freePixels = [&]() noexcept { avifRGBImageFreePixels(&rgb); };
    result = avifImageYUVToRGB(decoder->image, &rgb);
    if (result != AVIF_RESULT_OK) {
        freePixels();
        return false;
    }

    const bool ok = assignRgbaStrided(decoder->image->width, decoder->image->height, rgb.pixels, rgb.rowBytes, hasAlpha, out);
    freePixels();
    return ok;
}

struct ZipMemoryBuffer {
    std::span<const uint8_t> data;
    size_t position = 0;
};

voidpf zipOpenMemory(voidpf opaque, const char*, int) {
    auto* memory = static_cast<ZipMemoryBuffer*>(opaque);
    if (!memory) {
        return nullptr;
    }

    memory->position = 0;
    return opaque;
}

uLong zipReadMemory(voidpf opaque, voidpf, void* buffer, uLong size) {
    auto* memory = static_cast<ZipMemoryBuffer*>(opaque);
    if (!memory || !buffer) {
        return 0;
    }

    const size_t remaining = memory->data.size() - memory->position;
    const size_t bytesToRead = std::min<size_t>(remaining, size);
    if (bytesToRead > 0) {
        std::memcpy(buffer, memory->data.data() + memory->position, bytesToRead);
        memory->position += bytesToRead;
    }

    return static_cast<uLong>(bytesToRead);
}

long zipTellMemory(voidpf opaque, voidpf) {
    auto* memory = static_cast<ZipMemoryBuffer*>(opaque);
    if (!memory || memory->position > static_cast<size_t>(std::numeric_limits<long>::max())) {
        return -1;
    }

    return static_cast<long>(memory->position);
}

long zipSeekMemory(voidpf opaque, voidpf, uLong offset, int origin) {
    auto* memory = static_cast<ZipMemoryBuffer*>(opaque);
    if (!memory) {
        return -1;
    }

    size_t base = 0;
    switch (origin) {
    case ZLIB_FILEFUNC_SEEK_CUR:
        base = memory->position;
        break;
    case ZLIB_FILEFUNC_SEEK_END:
        base = memory->data.size();
        break;
    case ZLIB_FILEFUNC_SEEK_SET:
        base = 0;
        break;
    default:
        return -1;
    }

    if (offset > std::numeric_limits<size_t>::max() - base) {
        return -1;
    }

    const size_t newPosition = base + offset;
    if (newPosition > memory->data.size()) {
        return -1;
    }

    memory->position = newPosition;
    return 0;
}

int zipCloseMemory(voidpf, voidpf) {
    return 0;
}

int zipErrorMemory(voidpf, voidpf) {
    return 0;
}

bool hasLivpStillExtension(std::string_view filename) {
    std::string lower(filename);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
        lower.ends_with(".heic") || lower.ends_with(".heif");
}

bool readCurrentZipFile(unzFile zipFile, const unz_file_info& fileInfo, std::vector<uint8_t>& out) {
    constexpr uLong kMaxLivpStillBytes = 512UL * 1024UL * 1024UL;
    if (fileInfo.uncompressed_size == 0 || fileInfo.uncompressed_size > kMaxLivpStillBytes ||
        fileInfo.uncompressed_size > static_cast<uLong>(std::numeric_limits<unsigned>::max()) ||
        fileInfo.uncompressed_size > static_cast<uLong>(std::numeric_limits<int>::max())) {
        return false;
    }

    if (unzOpenCurrentFile(zipFile) != UNZ_OK) {
        return false;
    }

    std::vector<uint8_t> bytes(fileInfo.uncompressed_size);
    const int bytesRead = unzReadCurrentFile(zipFile, bytes.data(), static_cast<unsigned>(bytes.size()));
    const int closeResult = unzCloseCurrentFile(zipFile);
    if (bytesRead != static_cast<int>(bytes.size()) || closeResult != UNZ_OK) {
        return false;
    }

    out = std::move(bytes);
    return true;
}

bool extractLivpStillImage(std::span<const uint8_t> data, std::vector<uint8_t>& out) {
    zlib_filefunc_def memoryFileFunc{};
    ZipMemoryBuffer memory{ data, 0 };
    memoryFileFunc.opaque = &memory;
    memoryFileFunc.zopen_file = zipOpenMemory;
    memoryFileFunc.zread_file = zipReadMemory;
    memoryFileFunc.zwrite_file = [](voidpf, voidpf, const void*, uLong) -> uLong { return 0; };
    memoryFileFunc.ztell_file = zipTellMemory;
    memoryFileFunc.zseek_file = zipSeekMemory;
    memoryFileFunc.zclose_file = zipCloseMemory;
    memoryFileFunc.zerror_file = zipErrorMemory;

    unzFile zipFile = unzOpen2("__memory__", &memoryFileFunc);
    if (!zipFile) {
        return false;
    }

    const auto closeZip = [](unzFile file) noexcept {
        if (file) {
            unzClose(file);
        }
    };
    std::unique_ptr<void, decltype(closeZip)> zipGuard(zipFile, closeZip);

    if (unzGoToFirstFile(zipFile) != UNZ_OK) {
        return false;
    }

    do {
        unz_file_info fileInfo{};
        char filename[1024] = {};
        if (unzGetCurrentFileInfo(zipFile, &fileInfo, filename, sizeof(filename), nullptr, 0, nullptr, 0) != UNZ_OK) {
            continue;
        }

        if (!hasLivpStillExtension(filename)) {
            continue;
        }

        if (readCurrentZipFile(zipFile, fileInfo, out)) {
            return true;
        }
    } while (unzGoToNextFile(zipFile) == UNZ_OK);

    return false;
}

bool decodeLivp(std::span<const uint8_t> data, uint32_t maxEdge, ThumbBitmap& out) {
    std::vector<uint8_t> stillImage;
    if (!extractLivpStillImage(data, stillImage)) {
        return false;
    }

    return decodeThumbnail(stillImage, maxEdge, out);
}

bool decodeWp2(std::span<const uint8_t> data, ThumbBitmap& out) {
    WP2::ArgbBuffer buffer(WP2_Argb_32);
    if (WP2::Decode(data.data(), data.size(), &buffer) != WP2_STATUS_OK ||
        buffer.IsEmpty() || !checkedDimensions(buffer.width(), buffer.height())) {
        return false;
    }

    out.width = buffer.width();
    out.height = buffer.height();
    out.hasAlpha = buffer.HasTransparency();
    out.bgra.assign(static_cast<size_t>(out.width) * out.height * 4ULL, 255);

    for (uint32_t y = 0; y < out.height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(buffer.GetRow(y));
        for (uint32_t x = 0; x < out.width; ++x) {
            const size_t src = static_cast<size_t>(x) * 4ULL;
            const size_t dst = (static_cast<size_t>(y) * out.width + x) * 4ULL;
            const uint8_t a = row[src + 0];
            uint32_t r = row[src + 1];
            uint32_t g = row[src + 2];
            uint32_t b = row[src + 3];
            if (a != 0 && a != 255) {
                r = std::min<uint32_t>(255, (r * 255U + a / 2U) / a);
                g = std::min<uint32_t>(255, (g * 255U + a / 2U) / a);
                b = std::min<uint32_t>(255, (b * 255U + a / 2U) / a);
            }

            out.bgra[dst + 0] = static_cast<uint8_t>(b);
            out.bgra[dst + 1] = static_cast<uint8_t>(g);
            out.bgra[dst + 2] = static_cast<uint8_t>(r);
            out.bgra[dst + 3] = a;
        }
    }

    return true;
}

bool decodeWic(std::span<const uint8_t> data, ThumbBitmap& out);

bool decodeDds(std::span<const uint8_t> data, ThumbBitmap& out) {
    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage scratch;
    HRESULT hr = DirectX::LoadFromDDSMemory(data.data(), data.size(), DirectX::DDS_FLAGS_NONE, &metadata, scratch);
    if (FAILED(hr)) {
        return false;
    }

    DirectX::ScratchImage workspace;
    const DirectX::ScratchImage* finalScratch = &scratch;

    if (DirectX::IsCompressed(metadata.format)) {
        hr = DirectX::Decompress(scratch.GetImages(), scratch.GetImageCount(), metadata,
            DXGI_FORMAT_B8G8R8A8_UNORM, workspace);
        if (FAILED(hr)) {
            return false;
        }
        finalScratch = &workspace;
    }
    else if (metadata.format != DXGI_FORMAT_B8G8R8A8_UNORM && metadata.format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        hr = DirectX::Convert(scratch.GetImages(), scratch.GetImageCount(), metadata,
            DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, workspace);
        if (FAILED(hr)) {
            return false;
        }
        finalScratch = &workspace;
    }

    const DirectX::Image* image = finalScratch->GetImage(0, 0, 0);
    if (!image || !image->pixels || !checkedDimensions(static_cast<uint32_t>(image->width), static_cast<uint32_t>(image->height))) {
        return false;
    }

    out.width = static_cast<uint32_t>(image->width);
    out.height = static_cast<uint32_t>(image->height);
    out.hasAlpha = true;
    out.bgra.assign(static_cast<size_t>(out.width) * out.height * 4ULL, 255);

    bool observedAlpha = false;
    const size_t rowBytes = static_cast<size_t>(out.width) * 4ULL;
    for (uint32_t y = 0; y < out.height; ++y) {
        const uint8_t* src = image->pixels + static_cast<size_t>(y) * image->rowPitch;
        uint8_t* dst = out.bgra.data() + static_cast<size_t>(y) * rowBytes;
        std::memcpy(dst, src, rowBytes);
        for (uint32_t x = 0; x < out.width; ++x) {
            observedAlpha = observedAlpha || dst[static_cast<size_t>(x) * 4ULL + 3] != 255;
        }
    }

    out.hasAlpha = observedAlpha;
    return true;
}

bool decodeRaw(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.empty()) {
        return false;
    }

    LibRaw processor;
    processor.imgdata.params.half_size = 1;
    processor.imgdata.params.use_camera_wb = 1;
    processor.imgdata.params.no_auto_bright = 1;
    processor.imgdata.params.output_bps = 8;

    int ret = processor.open_buffer(data.data(), data.size());
    if (ret != LIBRAW_SUCCESS) {
        return false;
    }

    ret = processor.unpack_thumb();
    if (ret == LIBRAW_SUCCESS && processor.imgdata.thumbnail.thumb && processor.imgdata.thumbnail.tlength > 0 &&
        processor.imgdata.thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG) {
        const uint8_t* thumb = reinterpret_cast<const uint8_t*>(processor.imgdata.thumbnail.thumb);
        if (decodeWic(std::span<const uint8_t>(thumb, processor.imgdata.thumbnail.tlength), out)) {
            processor.recycle();
            return true;
        }
    }

    ret = processor.unpack();
    if (ret != LIBRAW_SUCCESS) {
        processor.recycle();
        return false;
    }

    ret = processor.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        processor.recycle();
        return false;
    }

    libraw_processed_image_t* image = processor.dcraw_make_mem_image(&ret);
    if (!image || ret != LIBRAW_SUCCESS) {
        if (image) {
            LibRaw::dcraw_clear_mem(image);
        }
        processor.recycle();
        return false;
    }

    const bool valid = image->type == LIBRAW_IMAGE_BITMAP &&
        (image->colors == 3 || image->colors == 4) &&
        checkedDimensions(image->width, image->height);
    if (!valid) {
        LibRaw::dcraw_clear_mem(image);
        processor.recycle();
        return false;
    }

    out.width = image->width;
    out.height = image->height;
    out.hasAlpha = image->colors == 4;
    out.bgra.assign(static_cast<size_t>(out.width) * out.height * 4ULL, 255);

    const uint8_t* srcPixels = image->data;
    bool observedAlpha = false;
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const size_t src = (static_cast<size_t>(y) * out.width + x) * image->colors;
            const size_t dst = (static_cast<size_t>(y) * out.width + x) * 4ULL;
            out.bgra[dst + 0] = srcPixels[src + 2];
            out.bgra[dst + 1] = srcPixels[src + 1];
            out.bgra[dst + 2] = srcPixels[src + 0];
            if (image->colors == 4) {
                out.bgra[dst + 3] = srcPixels[src + 3];
                observedAlpha = observedAlpha || srcPixels[src + 3] != 255;
            }
        }
    }

    out.hasAlpha = observedAlpha;
    LibRaw::dcraw_clear_mem(image);
    processor.recycle();
    return true;
}

bool decodeLep(std::span<const uint8_t> data, ThumbBitmap& out) {
    uint8_t* jpegData = nullptr;
    int jpegSize = 0;
    if (!decode_lepton(data.data(), data.size(), &jpegData, &jpegSize) || !jpegData || jpegSize <= 0) {
        return false;
    }

    const bool ok = decodeWic(std::span<const uint8_t>(jpegData, static_cast<size_t>(jpegSize)), out) ||
        decodeStb(std::span<const uint8_t>(jpegData, static_cast<size_t>(jpegSize)), out);
    free_lepton_buffer(jpegData, jpegSize);
    return ok;
}

#pragma pack(push, 1)
struct PcxHeader {
    uint8_t manufacturer;
    uint8_t version;
    uint8_t encoding;
    uint8_t bitsPerPixel;
    uint16_t xMin;
    uint16_t yMin;
    uint16_t xMax;
    uint16_t yMax;
    uint16_t hDpi;
    uint16_t vDpi;
    uint8_t colormap[48];
    uint8_t reserved;
    uint8_t numPlanes;
    uint16_t bytesPerLine;
    uint16_t paletteInfo;
    uint16_t hScreenSize;
    uint16_t vScreenSize;
    uint8_t filler[54];
};

struct Blp1Header {
    uint32_t magic;
    uint32_t compression;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t pictureType;
    uint32_t pictureSubType;
    uint32_t mipmapOffset[16];
    uint32_t mipmapSize[16];
};

struct Blp2Header {
    uint32_t magic;
    uint32_t type;
    uint8_t encoding;
    uint8_t alphaDepth;
    uint8_t alphaEncoding;
    uint8_t hasMipmaps;
    uint32_t width;
    uint32_t height;
    uint32_t mipmapOffset[16];
    uint32_t mipmapSize[16];
};
#pragma pack(pop)

static_assert(sizeof(PcxHeader) == 128);
static_assert(sizeof(Blp1Header) == 156);
static_assert(sizeof(Blp2Header) == 148);

uint16_t readLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> decodePcxRle(const uint8_t* data, size_t dataSize, size_t expectedSize) {
    std::vector<uint8_t> decoded;
    decoded.reserve(expectedSize);

    size_t pos = 0;
    while (pos < dataSize && decoded.size() < expectedSize) {
        const uint8_t byte = data[pos++];
        if ((byte & 0xC0) == 0xC0) {
            const uint8_t count = byte & 0x3F;
            if (pos >= dataSize) {
                break;
            }
            const uint8_t value = data[pos++];
            for (uint8_t i = 0; i < count && decoded.size() < expectedSize; ++i) {
                decoded.push_back(value);
            }
        }
        else {
            decoded.push_back(byte);
        }
    }

    return decoded;
}

bool decodePcx(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.size() < sizeof(PcxHeader)) {
        return false;
    }

    PcxHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.manufacturer != 0x0A || header.encoding != 1) {
        return false;
    }

    const int width = static_cast<int>(header.xMax) - static_cast<int>(header.xMin) + 1;
    const int height = static_cast<int>(header.yMax) - static_cast<int>(header.yMin) + 1;
    if (width <= 0 || height <= 0 || !checkedDimensions(static_cast<uint32_t>(width), static_cast<uint32_t>(height)) ||
        header.bytesPerLine == 0 || header.numPlanes == 0) {
        return false;
    }

    const uint8_t* imageData = data.data() + sizeof(PcxHeader);
    size_t imageDataSize = data.size() - sizeof(PcxHeader);

    std::array<std::array<uint8_t, 3>, 256> palette{};
    bool hasPalette = false;
    if (header.bitsPerPixel == 8 && header.numPlanes == 1 && data.size() >= 769 && data[data.size() - 769] == 0x0C) {
        hasPalette = true;
        imageDataSize -= 769;
        const uint8_t* paletteData = data.data() + data.size() - 768;
        for (size_t i = 0; i < palette.size(); ++i) {
            palette[i] = { paletteData[i * 3 + 0], paletteData[i * 3 + 1], paletteData[i * 3 + 2] };
        }
    }

    const size_t bytesPerScanline = static_cast<size_t>(header.bytesPerLine) * header.numPlanes;
    const size_t expectedSize = bytesPerScanline * static_cast<size_t>(height);
    const auto decoded = decodePcxRle(imageData, imageDataSize, expectedSize);
    if (decoded.size() < expectedSize) {
        return false;
    }

    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.hasAlpha = header.bitsPerPixel == 8 && header.numPlanes == 4;
    out.bgra.assign(static_cast<size_t>(width) * height * 4ULL, 255);

    bool observedAlpha = false;
    if (header.bitsPerPixel == 8 && header.numPlanes == 1) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const uint8_t index = decoded[static_cast<size_t>(y) * header.bytesPerLine + x];
                const size_t dst = (static_cast<size_t>(y) * width + x) * 4ULL;
                if (hasPalette) {
                    out.bgra[dst + 0] = palette[index][2];
                    out.bgra[dst + 1] = palette[index][1];
                    out.bgra[dst + 2] = palette[index][0];
                }
                else {
                    out.bgra[dst + 0] = index;
                    out.bgra[dst + 1] = index;
                    out.bgra[dst + 2] = index;
                }
            }
        }
        return true;
    }

    if (header.bitsPerPixel == 8 && (header.numPlanes == 3 || header.numPlanes == 4)) {
        for (int y = 0; y < height; ++y) {
            const size_t rowOffset = static_cast<size_t>(y) * bytesPerScanline;
            for (int x = 0; x < width; ++x) {
                const uint8_t r = decoded[rowOffset + x];
                const uint8_t g = decoded[rowOffset + header.bytesPerLine + x];
                const uint8_t b = decoded[rowOffset + static_cast<size_t>(header.bytesPerLine) * 2ULL + x];
                const size_t dst = (static_cast<size_t>(y) * width + x) * 4ULL;
                out.bgra[dst + 0] = b;
                out.bgra[dst + 1] = g;
                out.bgra[dst + 2] = r;
                if (header.numPlanes == 4) {
                    const uint8_t a = decoded[rowOffset + static_cast<size_t>(header.bytesPerLine) * 3ULL + x];
                    out.bgra[dst + 3] = a;
                    observedAlpha = observedAlpha || a != 255;
                }
            }
        }
        out.hasAlpha = observedAlpha;
        return true;
    }

    if (header.bitsPerPixel == 1 && header.numPlanes == 1) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t byteIndex = static_cast<size_t>(y) * header.bytesPerLine + (x / 8);
                const int bitIndex = 7 - (x % 8);
                const uint8_t value = ((decoded[byteIndex] >> bitIndex) & 1) ? 255 : 0;
                const size_t dst = (static_cast<size_t>(y) * width + x) * 4ULL;
                out.bgra[dst + 0] = value;
                out.bgra[dst + 1] = value;
                out.bgra[dst + 2] = value;
            }
        }
        return true;
    }

    out = {};
    return false;
}

uint8_t expand5(uint8_t value) noexcept {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t expand6(uint8_t value) noexcept {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

void unpack565(uint16_t color, uint8_t& r, uint8_t& g, uint8_t& b) noexcept {
    r = expand5(static_cast<uint8_t>((color >> 11) & 0x1F));
    g = expand6(static_cast<uint8_t>((color >> 5) & 0x3F));
    b = expand5(static_cast<uint8_t>(color & 0x1F));
}

void decompressBc1Block(const uint8_t* src, uint8_t dst[64], bool hasBinaryAlpha) noexcept {
    const uint16_t c0 = readLe16(src + 0);
    const uint16_t c1 = readLe16(src + 2);
    const uint32_t bits = readLe32(src + 4);

    uint8_t r[4]{}, g[4]{}, b[4]{}, a[4]{ 255, 255, 255, 255 };
    unpack565(c0, r[0], g[0], b[0]);
    unpack565(c1, r[1], g[1], b[1]);

    if ((c0 > c1) || !hasBinaryAlpha) {
        r[2] = static_cast<uint8_t>((2 * r[0] + r[1] + 1) / 3);
        g[2] = static_cast<uint8_t>((2 * g[0] + g[1] + 1) / 3);
        b[2] = static_cast<uint8_t>((2 * b[0] + b[1] + 1) / 3);
        r[3] = static_cast<uint8_t>((r[0] + 2 * r[1] + 1) / 3);
        g[3] = static_cast<uint8_t>((g[0] + 2 * g[1] + 1) / 3);
        b[3] = static_cast<uint8_t>((b[0] + 2 * b[1] + 1) / 3);
    }
    else {
        r[2] = static_cast<uint8_t>((r[0] + r[1]) / 2);
        g[2] = static_cast<uint8_t>((g[0] + g[1]) / 2);
        b[2] = static_cast<uint8_t>((b[0] + b[1]) / 2);
        a[3] = 0;
    }

    for (int i = 0; i < 16; ++i) {
        const int idx = (bits >> (i * 2)) & 0x3;
        dst[i * 4 + 0] = b[idx];
        dst[i * 4 + 1] = g[idx];
        dst[i * 4 + 2] = r[idx];
        dst[i * 4 + 3] = a[idx];
    }
}

void decompressBc2Block(const uint8_t* src, uint8_t dst[64]) noexcept {
    decompressBc1Block(src + 8, dst, false);
    for (int i = 0; i < 8; ++i) {
        dst[(i * 2 + 0) * 4 + 3] = static_cast<uint8_t>((src[i] & 0x0F) * 17);
        dst[(i * 2 + 1) * 4 + 3] = static_cast<uint8_t>((src[i] >> 4) * 17);
    }
}

void decompressBc3Block(const uint8_t* src, uint8_t dst[64]) noexcept {
    const uint8_t a0 = src[0];
    const uint8_t a1 = src[1];
    uint8_t alphaTable[8]{ a0, a1 };

    if (a0 > a1) {
        alphaTable[2] = static_cast<uint8_t>((6 * a0 + a1 + 3) / 7);
        alphaTable[3] = static_cast<uint8_t>((5 * a0 + 2 * a1 + 3) / 7);
        alphaTable[4] = static_cast<uint8_t>((4 * a0 + 3 * a1 + 3) / 7);
        alphaTable[5] = static_cast<uint8_t>((3 * a0 + 4 * a1 + 3) / 7);
        alphaTable[6] = static_cast<uint8_t>((2 * a0 + 5 * a1 + 3) / 7);
        alphaTable[7] = static_cast<uint8_t>((a0 + 6 * a1 + 3) / 7);
    }
    else {
        alphaTable[2] = static_cast<uint8_t>((4 * a0 + a1 + 2) / 5);
        alphaTable[3] = static_cast<uint8_t>((3 * a0 + 2 * a1 + 2) / 5);
        alphaTable[4] = static_cast<uint8_t>((2 * a0 + 3 * a1 + 2) / 5);
        alphaTable[5] = static_cast<uint8_t>((a0 + 4 * a1 + 2) / 5);
        alphaTable[6] = 0;
        alphaTable[7] = 255;
    }

    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(src[2 + i]) << (i * 8);
    }

    decompressBc1Block(src + 8, dst, false);
    for (int i = 0; i < 16; ++i) {
        dst[i * 4 + 3] = alphaTable[(bits >> (i * 3)) & 0x7];
    }
}

bool decodeBlpDxt(const uint8_t* data, size_t size, uint32_t width, uint32_t height, int dxtType, ThumbBitmap& out) {
    const int blockBytes = dxtType == 1 ? 8 : 16;
    const int blocksX = (static_cast<int>(width) + 3) / 4;
    const int blocksY = (static_cast<int>(height) + 3) / 4;
    const size_t needed = static_cast<size_t>(blocksX) * blocksY * blockBytes;
    if (size < needed || !checkedDimensions(width, height)) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.hasAlpha = true;
    out.bgra.assign(static_cast<size_t>(width) * height * 4ULL, 255);

    bool observedAlpha = false;
    const uint8_t* src = data;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            uint8_t block[64]{};
            if (dxtType == 1) decompressBc1Block(src, block, true);
            else if (dxtType == 3) decompressBc2Block(src, block);
            else decompressBc3Block(src, block);
            src += blockBytes;

            for (int py = 0; py < 4; ++py) {
                const int y = by * 4 + py;
                if (y >= static_cast<int>(height)) continue;
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    if (x >= static_cast<int>(width)) continue;
                    const size_t srcOffset = static_cast<size_t>(py * 4 + px) * 4ULL;
                    const size_t dstOffset = (static_cast<size_t>(y) * width + x) * 4ULL;
                    out.bgra[dstOffset + 0] = block[srcOffset + 0];
                    out.bgra[dstOffset + 1] = block[srcOffset + 1];
                    out.bgra[dstOffset + 2] = block[srcOffset + 2];
                    out.bgra[dstOffset + 3] = block[srcOffset + 3];
                    observedAlpha = observedAlpha || block[srcOffset + 3] != 255;
                }
            }
        }
    }

    out.hasAlpha = observedAlpha;
    return true;
}

bool decodeBlpPalette(const uint8_t* indices, size_t indexCount, const uint8_t* alphaData, size_t alphaSize,
    int alphaDepth, const uint32_t* palette, uint32_t width, uint32_t height, ThumbBitmap& out) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (indexCount < pixelCount || !checkedDimensions(width, height)) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.hasAlpha = alphaDepth > 0;
    out.bgra.assign(pixelCount * 4ULL, 255);

    bool observedAlpha = false;
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t entry = palette[indices[i]];
        out.bgra[i * 4 + 0] = static_cast<uint8_t>(entry & 0xFF);
        out.bgra[i * 4 + 1] = static_cast<uint8_t>((entry >> 8) & 0xFF);
        out.bgra[i * 4 + 2] = static_cast<uint8_t>((entry >> 16) & 0xFF);

        uint8_t alpha = 255;
        if (alphaDepth == 1 && alphaData && i / 8 < alphaSize) {
            alpha = ((alphaData[i / 8] >> (i % 8)) & 1) ? 255 : 0;
        }
        else if (alphaDepth == 4 && alphaData && i / 2 < alphaSize) {
            const uint8_t value = (i % 2 == 0) ? (alphaData[i / 2] & 0x0F) : ((alphaData[i / 2] >> 4) & 0x0F);
            alpha = static_cast<uint8_t>(value * 17);
        }
        else if (alphaDepth == 8 && alphaData && i < alphaSize) {
            alpha = alphaData[i];
        }

        out.bgra[i * 4 + 3] = alpha;
        observedAlpha = observedAlpha || alpha != 255;
    }

    out.hasAlpha = observedAlpha;
    return true;
}

bool decodeBlpRawBgra(const uint8_t* pixels, size_t size, uint32_t width, uint32_t height, ThumbBitmap& out) {
    const size_t pixelBytes = static_cast<size_t>(width) * height * 4ULL;
    if (!checkedDimensions(width, height) || size < pixelBytes) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.hasAlpha = false;
    out.bgra.assign(pixels, pixels + pixelBytes);
    for (size_t i = 3; i < out.bgra.size(); i += 4) {
        out.hasAlpha = out.hasAlpha || out.bgra[i] != 255;
    }
    return true;
}

bool decodeBlpJpeg(const uint8_t* header, size_t headerSize, const uint8_t* body, size_t bodySize, ThumbBitmap& out) {
    std::vector<uint8_t> jpeg;
    jpeg.reserve(headerSize + bodySize);
    jpeg.insert(jpeg.end(), header, header + headerSize);
    jpeg.insert(jpeg.end(), body, body + bodySize);
    return decodeWic(jpeg, out) || decodeStb(jpeg, out);
}

bool firstMip(const uint32_t (&offsets)[16], const uint32_t (&sizes)[16], uint32_t& offset, uint32_t& size) noexcept {
    for (int i = 0; i < 16; ++i) {
        if (offsets[i] != 0 && sizes[i] != 0) {
            offset = offsets[i];
            size = sizes[i];
            return true;
        }
    }
    return false;
}

bool rangeInside(size_t total, uint32_t offset, uint32_t size) noexcept {
    return size <= total && offset <= total - size;
}

bool decodeBlp(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.size() < 4) {
        return false;
    }

    if (data[0] == 'B' && data[1] == 'L' && data[2] == 'P' && data[3] == '1') {
        if (data.size() < sizeof(Blp1Header)) {
            return false;
        }

        Blp1Header header{};
        std::memcpy(&header, data.data(), sizeof(header));
        uint32_t mipOffset = 0;
        uint32_t mipSize = 0;
        if (!checkedDimensions(header.width, header.height) ||
            !firstMip(header.mipmapOffset, header.mipmapSize, mipOffset, mipSize) ||
            !rangeInside(data.size(), mipOffset, mipSize)) {
            return false;
        }

        const uint8_t* mipData = data.data() + mipOffset;
        if (header.compression == 0) {
            constexpr size_t jpegHeaderSizeOffset = sizeof(Blp1Header);
            if (data.size() < jpegHeaderSizeOffset + sizeof(uint32_t)) {
                return false;
            }
            const uint32_t sharedHeaderSize = readLe32(data.data() + jpegHeaderSizeOffset);
            const size_t sharedHeaderOffset = jpegHeaderSizeOffset + sizeof(uint32_t);
            if (sharedHeaderSize > data.size() || sharedHeaderOffset > data.size() - sharedHeaderSize) {
                return false;
            }
            return decodeBlpJpeg(data.data() + sharedHeaderOffset, sharedHeaderSize, mipData, mipSize, out);
        }

        if (header.compression == 1) {
            constexpr size_t paletteOffset = sizeof(Blp1Header);
            constexpr size_t paletteBytes = 256 * sizeof(uint32_t);
            if (data.size() < paletteOffset + paletteBytes) {
                return false;
            }

            const size_t pixelCount = static_cast<size_t>(header.width) * header.height;
            const uint8_t* alphaData = nullptr;
            size_t alphaSize = 0;
            int alphaDepth = 0;
            if (header.pictureType == 3) alphaDepth = 8;
            else if (header.pictureType == 5) alphaDepth = 1;
            else if (header.flags & 0x8u) alphaDepth = 8;
            if (alphaDepth > 0 && mipSize > pixelCount) {
                alphaData = mipData + pixelCount;
                alphaSize = mipSize - pixelCount;
            }

            return decodeBlpPalette(mipData, mipSize, alphaData, alphaSize, alphaDepth,
                reinterpret_cast<const uint32_t*>(data.data() + paletteOffset), header.width, header.height, out);
        }

        return false;
    }

    if (data[0] == 'B' && data[1] == 'L' && data[2] == 'P' && data[3] == '2') {
        if (data.size() < sizeof(Blp2Header)) {
            return false;
        }

        Blp2Header header{};
        std::memcpy(&header, data.data(), sizeof(header));
        uint32_t mipOffset = 0;
        uint32_t mipSize = 0;
        if (!checkedDimensions(header.width, header.height) ||
            !firstMip(header.mipmapOffset, header.mipmapSize, mipOffset, mipSize) ||
            !rangeInside(data.size(), mipOffset, mipSize)) {
            return false;
        }

        const uint8_t* mipData = data.data() + mipOffset;
        if (header.encoding == 1) {
            constexpr size_t paletteOffset = sizeof(Blp2Header);
            constexpr size_t paletteBytes = 256 * sizeof(uint32_t);
            if (data.size() < paletteOffset + paletteBytes) {
                return false;
            }

            const size_t pixelCount = static_cast<size_t>(header.width) * header.height;
            const uint8_t* alphaData = nullptr;
            size_t alphaSize = 0;
            if (header.alphaDepth > 0 && mipSize > pixelCount) {
                alphaData = mipData + pixelCount;
                alphaSize = mipSize - pixelCount;
            }

            return decodeBlpPalette(mipData, mipSize, alphaData, alphaSize, header.alphaDepth,
                reinterpret_cast<const uint32_t*>(data.data() + paletteOffset), header.width, header.height, out);
        }

        if (header.encoding == 2) {
            int dxtType = 0;
            if (header.alphaEncoding == 0) dxtType = 1;
            else if (header.alphaEncoding == 1) dxtType = 3;
            else if (header.alphaEncoding == 7) dxtType = 5;
            else return false;

            if (!decodeBlpDxt(mipData, mipSize, header.width, header.height, dxtType, out)) {
                return false;
            }
            if (dxtType == 1 && header.alphaDepth == 0) {
                for (size_t i = 3; i < out.bgra.size(); i += 4) {
                    out.bgra[i] = 255;
                }
                out.hasAlpha = false;
            }
            return true;
        }

        if (header.encoding == 3) {
            return decodeBlpRawBgra(mipData, mipSize, header.width, header.height, out);
        }
    }

    return false;
}

bool decodeSvg(std::span<const uint8_t> data, uint32_t maxEdge, ThumbBitmap& out) {
    const auto document = lunasvg::Document::loadFromData(reinterpret_cast<const char*>(data.data()), data.size());
    if (!document || document->width() <= 0.0f || document->height() <= 0.0f) {
        return false;
    }

    const float aspectRatio = document->width() / document->height();
    const int targetEdge = static_cast<int>(std::clamp<uint32_t>(maxEdge, 1, 4096));
    int width = targetEdge;
    int height = targetEdge;
    if (aspectRatio > 1.0f) {
        height = std::max(1, static_cast<int>(targetEdge / aspectRatio));
    }
    else if (aspectRatio < 1.0f) {
        width = std::max(1, static_cast<int>(targetEdge * aspectRatio));
    }

    auto bitmap = document->renderToBitmap(width, height);
    if (bitmap.isNull()) {
        return false;
    }

    bitmap.convertToRGBA();
    return assignRgba(static_cast<uint32_t>(width), static_cast<uint32_t>(height), bitmap.data(), true, out);
}

bool parsePfmToken(std::span<const uint8_t> data, size_t& offset, std::string& token) {
    token.clear();
    while (offset < data.size()) {
        const uint8_t ch = data[offset];
        if (ch == '#') {
            while (offset < data.size() && data[offset] != '\n') {
                ++offset;
            }
        }
        else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++offset;
        }
        else {
            break;
        }
    }

    while (offset < data.size()) {
        const uint8_t ch = data[offset];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++offset;
            break;
        }
        token.push_back(static_cast<char>(ch));
        ++offset;
    }

    return !token.empty();
}

uint32_t byteSwap32(uint32_t value) noexcept {
    return (value >> 24) |
        ((value >> 8) & 0x0000FF00u) |
        ((value << 8) & 0x00FF0000u) |
        (value << 24);
}

float readPfmFloat(const uint8_t* bytes, bool bigEndian) noexcept {
    uint32_t raw = 0;
    std::memcpy(&raw, bytes, sizeof(raw));
    if (bigEndian) {
        raw = byteSwap32(raw);
    }

    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

uint8_t floatToByte(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }

    return static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

bool decodePfm(std::span<const uint8_t> data, ThumbBitmap& out) {
    size_t offset = 0;
    std::string token;

    if (!parsePfmToken(data, offset, token)) {
        return false;
    }

    const bool color = token == "PF";
    if (!color && token != "Pf") {
        return false;
    }

    if (!parsePfmToken(data, offset, token)) return false;
    const int width = std::atoi(token.c_str());
    if (!parsePfmToken(data, offset, token)) return false;
    const int height = std::atoi(token.c_str());
    if (!parsePfmToken(data, offset, token)) return false;
    float scale = std::strtof(token.c_str(), nullptr);

    if (width <= 0 || height <= 0 || scale == 0.0f ||
        !checkedDimensions(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) {
        return false;
    }

    const int channels = color ? 3 : 1;
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t required = pixelCount * static_cast<size_t>(channels) * sizeof(float);
    if (offset > data.size() || required > data.size() - offset) {
        return false;
    }

    const bool bigEndian = scale > 0.0f;
    scale = std::fabs(scale);

    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.hasAlpha = false;
    out.bgra.assign(pixelCount * 4ULL, 255);

    const uint8_t* samples = data.data() + offset;
    for (int y = 0; y < height; ++y) {
        const int sourceY = bigEndian ? y : (height - 1 - y);
        for (int x = 0; x < width; ++x) {
            const size_t srcPixel = (static_cast<size_t>(sourceY) * width + x) * channels;
            const size_t dst = (static_cast<size_t>(y) * width + x) * 4ULL;

            if (color) {
                const float r = readPfmFloat(samples + (srcPixel + 0) * sizeof(float), bigEndian) * 255.0f / scale;
                const float g = readPfmFloat(samples + (srcPixel + 1) * sizeof(float), bigEndian) * 255.0f / scale;
                const float b = readPfmFloat(samples + (srcPixel + 2) * sizeof(float), bigEndian) * 255.0f / scale;
                out.bgra[dst + 0] = floatToByte(b);
                out.bgra[dst + 1] = floatToByte(g);
                out.bgra[dst + 2] = floatToByte(r);
            }
            else {
                const float gray = readPfmFloat(samples + srcPixel * sizeof(float), bigEndian) * 255.0f / scale;
                const uint8_t v = floatToByte(gray);
                out.bgra[dst + 0] = v;
                out.bgra[dst + 1] = v;
                out.bgra[dst + 2] = v;
            }
        }
    }

    return true;
}

bool decodeWic(std::span<const uint8_t> data, ThumbBitmap& out) {
    if (data.empty() || data.size() > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.put());
    if (FAILED(hr)) {
        return false;
    }

    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || !checkedDimensions(width, height)) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.put());
    if (FAILED(hr)) {
        return false;
    }

    hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        return false;
    }

    out.width = width;
    out.height = height;
    out.hasAlpha = true;
    out.bgra.resize(static_cast<size_t>(width) * height * 4ULL);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data());
    if (FAILED(hr)) {
        out = {};
        return false;
    }

    out.hasAlpha = std::ranges::any_of(out.bgra, [index = size_t{ 0 }](uint8_t value) mutable {
        const bool isAlpha = (index++ % 4) == 3;
        return isAlpha && value != 255;
    });
    return true;
}
}

bool decodeThumbnail(std::span<const uint8_t> data, uint32_t maxEdge, ThumbBitmap& out) noexcept {
    out = {};

    try {
        switch (sniffFormat(data)) {
        case Format::Jxl:
            return decodeJxl(data, out);
        case Format::Avif:
            return decodeAvif(data, out);
        case Format::Heif:
            return decodeHeif(data, out);
        case Format::Livp:
            return decodeLivp(data, maxEdge, out);
        case Format::Wp2:
            return decodeWp2(data, out);
        case Format::Qoi:
            return decodeQoi(data, out);
        case Format::Blp:
            return decodeBlp(data, out);
        case Format::Pcx:
            return decodePcx(data, out);
        case Format::Pfm:
            return decodePfm(data, out);
        case Format::Svg:
            return decodeSvg(data, maxEdge, out);
        case Format::Dds:
            return decodeDds(data, out);
        case Format::Lep:
            return decodeLep(data, out);
        case Format::Tga:
        case Format::Hdr:
        case Format::Pic:
        case Format::Pnm:
            return decodeStb(data, out);
        case Format::Jxr:
        case Format::RawTiff:
            return decodeRaw(data, out) || decodeWic(data, out);
        default:
            return decodeWic(data, out) || decodeStb(data, out) || decodeWp2(data, out) ||
                decodeLep(data, out) || decodeRaw(data, out);
        }
    }
    catch (...) {
        out = {};
        return false;
    }
}
}
