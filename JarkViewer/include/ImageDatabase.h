#pragma once
#include "jarkUtils.h"
#include "LRU.h"

#include "videoDecoder.h"
#include "SVGPreprocessor.h"

// libbpg v0.9.8 End on 2018  https://bellard.org/bpg/
#include "libbpg.h"

// stb_image v2.30  https://github.com/nothings/stb
#include "stb_image.h"

// QOI v2025.4.29  https://github.com/phoboslab/qoi
#include "qoi.h"

// opencv 4.12.0  https://github.com/opencv/opencv
#pragma comment(lib, "IlmImf.lib")
#pragma comment(lib, "ipphal.lib")
#pragma comment(lib, "ippicvmt.lib")
#pragma comment(lib, "ippiw.lib")
#pragma comment(lib, "ittnotify.lib")
#pragma comment(lib, "libjpeg-turbo.lib")
#pragma comment(lib, "libopenjp2.lib")
#pragma comment(lib, "libpng.lib")
#pragma comment(lib, "libprotobuf.lib")
#pragma comment(lib, "libtiff.lib")
#pragma comment(lib, "opencv_world4120.lib")
#pragma comment(lib, "zlib.lib")
#pragma comment(lib, "libwebp.lib")

// webp2 v0.0.1  https://chromium.googlesource.com/codecs/libwebp2
#include "src/wp2/base.h"
#include "src/wp2/decode.h"
#pragma comment(lib, "webp2.lib")
#pragma comment(lib, "imageio.lib")

// heif v1.20.1#1  https://github.com/strukturag/libheif
#include "libheif/heif.h"
#pragma comment(lib, "heif.lib")
#pragma comment(lib, "libde265.lib")
#pragma comment(lib, "x265-static.lib")

// avif v1.3.0  https://github.com/AOMediaCodec/libavif
#include "avif/avif.h"
#pragma comment(lib, "avif.lib")
#pragma comment(lib, "yuv.lib")
#pragma comment(lib, "dav1d.lib")
#pragma comment(lib, "aom.lib")

// libraw  v0.21.4  https://www.libraw.org/
#include "libraw/libraw.h"
#pragma comment(lib, "raw_r.lib")
#pragma comment(lib, "freeglut.lib")
#pragma comment(lib, "jasper.lib")
#pragma comment(lib, "lcms2.lib")

// exiv2  v0.28.5  https://exiv2.org/download.html
#include "exifParse.h"
#pragma comment(lib, "exiv2.lib")
#pragma comment(lib, "charset.lib")
#pragma comment(lib, "iconv.lib")
#pragma comment(lib, "inih.lib")
#pragma comment(lib, "INIReader.lib")
#pragma comment(lib, "intl.lib")
#pragma comment(lib, "libexpatMT.lib")
#pragma comment(lib, "brotlicommon.lib")
#pragma comment(lib, "brotlidec.lib")
#pragma comment(lib, "brotlienc.lib")

// libjxl v0.11.1  https://github.com/libjxl/libjxl
#include "jxl/decode_cxx.h"
#include "jxl/resizable_parallel_runner_cxx.h"
#include "jxl/types.h"
#pragma comment(lib, "jxl.lib")
#pragma comment(lib, "jxl_cms.lib")
#pragma comment(lib, "jxl_extras_codec.lib")
#pragma comment(lib, "jxl_threads.lib")

#pragma comment(lib, "hwy.lib")
#pragma comment(lib, "FreeImage.lib")
#pragma comment(lib, "FreeImagePlus.lib")
#pragma comment(lib, "pixman-1.lib")
#pragma comment(lib, "fontconfig.lib")

// psdsdk  https://github.com/MolecularMatters/psd_sdk
#include "psdsdk.h"
#pragma comment(lib, "Psd_MT.lib")

// lunasvg v3.4.0  https://github.com/sammycage/lunasvg
#include "lunasvg.h"
#pragma comment(lib, "lunasvg.lib")
#pragma comment(lib, "plutovg.lib")

// minizip  1.3.1#1
#include "minizip/unzip.h"
#pragma comment(lib, "minizip.lib")
#pragma comment(lib, "bz2.lib")


class ImageDatabase :public LRU<wstring, ImageAsset> {
public:

    // 自 OpenCV 4.12 起支持的动态图像格式 gif png webp
    static inline const unordered_set<wstring_view> opencvAnimationExt{
        L"gif", L"png", L"apng", L"webp",
    };

    static inline const unordered_set<wstring_view> supportExt{
        L"jpg", L"jp2", L"jpeg", L"jpe", L"bmp", L"dib", L"png", L"apng",
        L"pbm", L"pgm", L"ppm", L"pxm",L"pnm",L"sr", L"ras",
        L"exr", L"tiff", L"tif", L"webp", L"hdr", L"pic",
        L"heic", L"heif", L"avif", L"avifs", L"gif", L"jxl",
        L"ico", L"icon", L"psd", L"tga", L"svg", L"jfif",
        L"jxr", L"wp2", L"pfm", L"bpg", L"livp", L"qoi",
    };

    static inline const unordered_set<wstring_view> supportRaw{
        L"crw", L"cr2", L"cr3", // Canon
        L"arw", L"srf", L"sr2", // Sony
        L"raw", L"dng", // Leica
        L"nef", // Nikon
        L"pef", // Pentax
        L"orf", // Olympus
        L"rw2", // Panasonic
        L"raf", // Fujifilm
        L"kdc", // Kodak
        L"x3f", // Sigma
        L"mrw", // Minolta
        L"3fr", // Hasselblad
        L"ari", // ARRIRAW
        L"bay", // Casio
        L"cap", // Phase One
        L"dcr", // Kodak
        L"dcs", // Kodak
        L"drf", // DNG+
        L"eip", // Enhanced Image Package, Phase One
        L"erf", // Epson
        L"fff", // Imacon/Hasselblad
        L"gpr", // GoPro
        L"iiq", // Phase One
        L"k25", // Kodak
        L"mdc", // Minolta
        L"mef", // Mamiya
        L"mos", // Leaf
        L"nrw", // Nikon
        L"ptx", // Pentax
        L"r3d", // Red Digital Cinema
        L"rwl", // Leica
        L"rwz", // Leica
        L"srw", // Samsung
    };

    ImageDatabase() = default;

    cv::Mat errorTipsMatDeep, errorTipsMatLight, homeMatDeep, homeMatLight;

    cv::Mat getErrorTipsMat() {
        if (errorTipsMatDeep.empty()) {
            auto rc = jarkUtils::GetResource(IDB_PNG_TIPS, L"PNG");
            cv::Mat imgData(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr);
            auto errorTipsMat = cv::imdecode(imgData, cv::IMREAD_UNCHANGED);
            if (GlobalVar::settingParameter.UI_LANG == 0) {
                errorTipsMatLight = errorTipsMat({ 0, 0, 800, 600 }).clone();
                errorTipsMatDeep = errorTipsMat({ 0, 600, 800, 600 }).clone();
            }
            else {
                errorTipsMatLight = errorTipsMat({ 800, 0, 800, 600 }).clone();
                errorTipsMatDeep = errorTipsMat({ 800, 600, 800, 600 }).clone();
            }
        }
        return GlobalVar::CURRENT_UI_MODE == 1 ? errorTipsMatLight : errorTipsMatDeep;
    }


    cv::Mat getHomeMat() {
        if (homeMatDeep.empty()) {
            auto rc = jarkUtils::GetResource(IDB_PNG_HOME, L"PNG");
            cv::Mat imgData(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr);
            auto homeMat = cv::imdecode(imgData, cv::IMREAD_UNCHANGED);
            if (GlobalVar::settingParameter.UI_LANG == 0) {
                homeMatLight = homeMat({ 0, 0, 800, 600 }).clone();
                homeMatDeep = homeMat({ 0, 600, 800, 600 }).clone();
            }
            else {
                homeMatLight = homeMat({ 800, 0, 800, 600 }).clone();
                homeMatDeep = homeMat({ 800, 600, 800, 600 }).clone();
            }
        }
        return GlobalVar::CURRENT_UI_MODE == 1 ? homeMatLight : homeMatDeep;
    }


    static uint32_t swap_endian(uint32_t value) {
        return (value >> 24) |
            ((value >> 8) & 0x0000FF00) |
            ((value << 8) & 0x00FF0000) |
            (value << 24);
    }

    struct IconDirEntry {
        uint8_t width;
        uint8_t height;
        uint8_t colorCount;
        uint8_t reserved;
        uint16_t planes;
        uint16_t bitsPerPixel;
        uint32_t dataSize;
        uint32_t dataOffset;
    };

    struct DibHeader {
        uint32_t headerSize;
        int32_t width;
        int32_t height;
        uint16_t planes;
        uint16_t bitCount;
        uint32_t compression;
        uint32_t imageSize;
        int32_t xPelsPerMeter;
        int32_t yPelsPerMeter;
        uint32_t clrUsed;
        uint32_t clrImportant;
    };

    cv::Mat readDibFromMemory(const uint8_t* data, size_t size);

    // https://github.com/corkami/pics/blob/master/binary/ico_bmp.png
    std::tuple<cv::Mat, string> loadICO(wstring_view path, const vector<uint8_t>& buf);


    template <typename T, typename DataHolder>
    void* ExpandChannelToCanvas(psd::Allocator* allocator, const DataHolder* layer, const void* data, unsigned int canvasWidth, unsigned int canvasHeight)
    {
        T* canvasData = static_cast<T*>(allocator->Allocate(sizeof(T) * canvasWidth * canvasHeight, 16u));
        memset(canvasData, 0u, sizeof(T) * canvasWidth * canvasHeight);

        psd::imageUtil::CopyLayerData(static_cast<const T*>(data), canvasData, layer->left, layer->top, layer->right, layer->bottom, canvasWidth, canvasHeight);

        return canvasData;
    }


    void* ExpandChannelToCanvas(const psd::Document* document, psd::Allocator* allocator, psd::Layer* layer, psd::Channel* channel)
    {
        if (document->bitsPerChannel == 8)
            return ExpandChannelToCanvas<uint8_t>(allocator, layer, channel->data, document->width, document->height);
        else if (document->bitsPerChannel == 16)
            return ExpandChannelToCanvas<uint16_t>(allocator, layer, channel->data, document->width, document->height);
        else if (document->bitsPerChannel == 32)
            return ExpandChannelToCanvas<float32_t>(allocator, layer, channel->data, document->width, document->height);

        return nullptr;
    }


    template <typename T>
    void* ExpandMaskToCanvas(const psd::Document* document, psd::Allocator* allocator, T* mask)
    {
        if (document->bitsPerChannel == 8)
            return ExpandChannelToCanvas<uint8_t>(allocator, mask, mask->data, document->width, document->height);
        else if (document->bitsPerChannel == 16)
            return ExpandChannelToCanvas<uint16_t>(allocator, mask, mask->data, document->width, document->height);
        else if (document->bitsPerChannel == 32)
            return ExpandChannelToCanvas<float32_t>(allocator, mask, mask->data, document->width, document->height);

        return nullptr;
    }


    unsigned int FindChannel(psd::Layer* layer, int16_t channelType) {
        const int32_t CHANNEL_NOT_FOUND = UINT_MAX;

        for (unsigned int i = 0; i < layer->channelCount; ++i)
        {
            psd::Channel* channel = &layer->channels[i];
            if (channel->data && channel->type == channelType)
                return i;
        }

        return CHANNEL_NOT_FOUND;
    }

    template <typename T>
    struct TmpValue;

    // uint8_t的特化
    template <>
    struct TmpValue<uint8_t> {
        static constexpr uint8_t alphaMax = 255;
    };

    // uint16_t的特化
    template <>
    struct TmpValue<uint16_t> {
        static constexpr uint16_t alphaMax = 65535;
    };

    // float的特化
    template <>
    struct TmpValue<float> {
        static constexpr float alphaMax = 1.0f;
    };

    template <typename T>
    T* CreateInterleavedImage(psd::Allocator* allocator, const void* srcR, const void* srcG, const void* srcB, unsigned int width, unsigned int height)
    {
        T* image = static_cast<T*>(allocator->Allocate(4ULL * width * height * sizeof(T), 16u));

        const T* r = static_cast<const T*>(srcR);
        const T* g = static_cast<const T*>(srcG);
        const T* b = static_cast<const T*>(srcB);
        psd::imageUtil::InterleaveRGB(b, g, r, TmpValue<T>::alphaMax, image, width, height); // RGB -> BGR

        return image;
    }


    template <typename T>
    T* CreateInterleavedImage(psd::Allocator* allocator, const void* srcR, const void* srcG, const void* srcB, const void* srcA, unsigned int width, unsigned int height)
    {
        T* image = static_cast<T*>(allocator->Allocate(4ULL * width * height * sizeof(T), 16u));

        const T* r = static_cast<const T*>(srcR);
        const T* g = static_cast<const T*>(srcG);
        const T* b = static_cast<const T*>(srcB);
        const T* a = static_cast<const T*>(srcA);
        psd::imageUtil::InterleaveRGBA(b, g, r, a, image, width, height); // RGB -> BGR

        return image;
    }


    // https://github.com/MolecularMatters/psd_sdk
    cv::Mat loadPSD(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadTGA_HDR(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadSVG(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadPFM(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadQOI(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadHeic(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadRaw(wstring_view path, const vector<uint8_t>& buf);

    cv::Mat loadImageWinCOM(wstring_view path, const vector<uint8_t>& buf);
    cv::Mat loadImageOpenCV(wstring_view path, const vector<uint8_t>& buf);

    ImageAsset loadAvif(wstring_view path, const vector<uint8_t>& buf);
    ImageAsset loadJXL(wstring_view path, const vector<uint8_t>& buf);
    ImageAsset loadWP2(wstring_view path, const std::vector<uint8_t>& buf);
    ImageAsset loadBPG(wstring_view path, const std::vector<uchar>& buf);
    ImageAsset loadLivp(wstring_view path, const std::vector<uchar>& buf);
    ImageAsset loadMotionPhoto(wstring_view path, const std::vector<uchar>& buf, bool isJPG);
    ImageAsset loadAnimation(wstring_view path, const vector<uint8_t>& buf);

    void handleExifOrientation(int orientation, cv::Mat& img);
    ImageAsset myLoader(const wstring& path);
    ImageAsset loader(const wstring& path);
};
