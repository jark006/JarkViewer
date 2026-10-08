#include "ColorManager.h"
#include "exifParse.h"
#include "lcms2.h"

#include <fstream>

namespace {

class CmsProfile {
public:
    explicit CmsProfile(cmsHPROFILE profile = nullptr) : profile(profile) {}

    ~CmsProfile() {
        if (profile)
            cmsCloseProfile(profile);
    }

    CmsProfile(const CmsProfile&) = delete;
    CmsProfile& operator=(const CmsProfile&) = delete;

    void reset(cmsHPROFILE newProfile = nullptr) {
        if (profile)
            cmsCloseProfile(profile);
        profile = newProfile;
    }

    operator cmsHPROFILE() const {
        return profile;
    }

private:
    cmsHPROFILE profile = nullptr;
};

class CmsTransform {
public:
    explicit CmsTransform(cmsHTRANSFORM transform = nullptr) : transform(transform) {}

    ~CmsTransform() {
        if (transform)
            cmsDeleteTransform(transform);
    }

    CmsTransform(const CmsTransform&) = delete;
    CmsTransform& operator=(const CmsTransform&) = delete;

    operator cmsHTRANSFORM() const {
        return transform;
    }

private:
    cmsHTRANSFORM transform = nullptr;
};

// profile 的自述名（"Display P3"、"sRGB IEC61966-2.1"…），只能拿句柄问
std::string profileName(cmsHPROFILE profile) {
    if (!profile)
        return "?";

    char buffer[256] = {};
    if (cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", buffer, sizeof(buffer)) > 0)
        return buffer;

    return "?";
}

std::vector<uint8_t> readFileBytes(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
        return {};

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size <= 0)
        return {};

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file)
        return {};

    return bytes;
}

}

void ColorManager::setWindow(HWND hwnd) {
    this->hwnd = hwnd;
}

std::vector<uint8_t> ColorManager::readEmbeddedIccProfile(std::wstring_view path, std::span<const uint8_t> buf) {
    if (buf.empty())
        return {};

    try {
        auto image = Exiv2::ImageFactory::open(buf.data(), buf.size());
        if (!image)
            return {};

        image->readMetadata();
        if (!image->iccProfileDefined())
            return {};

        const auto& profile = image->iccProfile();
        if (profile.empty())
            return {};

        return { profile.c_data(), profile.c_data() + profile.size() };
    }
    catch ([[maybe_unused]] const Exiv2::Error& e) {
        JARK_LOG("Read ICC failed: {} [{}]", jarkUtils::wstringToUtf8(path), e.what());
    }
    catch ([[maybe_unused]] const std::exception& e) {
        JARK_LOG("Read ICC failed: {} [{}]", jarkUtils::wstringToUtf8(path), e.what());
    }

    return {};
}

std::vector<uint8_t> ColorManager::readMonitorIccProfile() const {
    if (!hwnd)
        return {};

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return {};

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return {};

    HDC hdc = CreateDCW(L"DISPLAY", monitorInfo.szDevice, nullptr, nullptr);
    if (!hdc)
        return {};

    DWORD pathLen = MAX_PATH;
    std::wstring profilePath(pathLen, L'\0');
    bool ok = GetICMProfileW(hdc, &pathLen, profilePath.data());
    if (!ok && pathLen > MAX_PATH) {
        profilePath.assign(pathLen, L'\0');
        ok = GetICMProfileW(hdc, &pathLen, profilePath.data());
    }
    DeleteDC(hdc);

    if (!ok)
        return {};

    profilePath.resize(pathLen);
    if (!profilePath.empty() && profilePath.back() == L'\0')
        profilePath.pop_back();

    return readFileBytes(profilePath);
}

std::string ColorManager::profileDescription(const std::vector<uint8_t>& icc) {
    if (icc.empty())
        return {};

    cmsHPROFILE profile = cmsOpenProfileFromMem(icc.data(), static_cast<cmsUInt32Number>(icc.size()));
    if (!profile)
        return {};

    char buffer[256] = {};
    std::string name;
    if (cmsGetProfileInfoASCII(profile, cmsInfoDescription, "en", "US", buffer, sizeof(buffer)) > 0)
        name = buffer;
    cmsCloseProfile(profile);
    return name;
}

std::vector<uint8_t>& ColorManager::readMonitorIccProfileCached() {
    static std::mutex mutex;
    static std::vector<uint8_t> cachedProfile;

    std::lock_guard<std::mutex> lock(mutex);
    if (cachedProfile.empty()) {
        cachedProfile = readMonitorIccProfile();
    }
    return cachedProfile;
}

bool ColorManager::applyToMat(cv::Mat& mat, const std::vector<uint8_t>& sourceIcc, const std::vector<uint8_t>& monitorIcc) {
    if (mat.empty())
        return false;

    const uint32_t pixelType = mat.type() == CV_8UC3 ? TYPE_BGR_8 :
        mat.type() == CV_8UC4 ? TYPE_BGRA_8 : 0;
    if (pixelType == 0)
        return false;

    // 源和目标同一色彩空间时变换是恒等的，跑一遍只把每个像素原样写回去（还带舍入）。
    // 最常见的情况：文件没嵌 profile（按 sRGB 处理）且显示器没设 profile（也按 sRGB 处理）。
    // 一亿像素白跑一趟要两三百毫秒，而这步紧跟在解码之后、直接顶在出图时间上。
    if (sourceIcc.empty() && monitorIcc.empty())
        return false; // 恒等：无需变换
    if (!sourceIcc.empty() && sourceIcc == monitorIcc)
        return false; // 恒等：图文 profile 与显示器 profile 逐字节相同

    CmsProfile sourceProfile(sourceIcc.empty() ?
        cmsCreate_sRGBProfile() :
        cmsOpenProfileFromMem(sourceIcc.data(), static_cast<cmsUInt32Number>(sourceIcc.size())));
    if (!sourceProfile)
        sourceProfile.reset(cmsCreate_sRGBProfile());
    if (!sourceProfile)
        return false;

    CmsProfile outputProfile(monitorIcc.empty() ?
        cmsCreate_sRGBProfile() :
        cmsOpenProfileFromMem(monitorIcc.data(), static_cast<cmsUInt32Number>(monitorIcc.size())));
    if (!outputProfile)
        outputProfile.reset(cmsCreate_sRGBProfile());
    if (!outputProfile)
        return false;

    CmsTransform transform(cmsCreateTransform(
        sourceProfile,
        pixelType,
        outputProfile,
        pixelType,
        INTENT_PERCEPTUAL,
        mat.channels() == 4 ? cmsFLAGS_COPY_ALPHA : 0));
    if (!transform)
        return false;

    // 换了哪两套色彩空间、转的是什么尺寸的图——"颜色不对"的报告全靠这一行判断是
    // 色彩管理在按配置文件干活（超色域的颜色转窄色域会被剪裁，看起来像串色），
    // 还是根本没生效。没开日志时实参不会求值。
    JARK_LOG("色彩管理: {} → {} ({}x{} {}ch)",
        profileName(sourceProfile), profileName(outputProfile),
        mat.cols, mat.rows, mat.channels());

    // 大图按行分块并行：lcms2 的 transform 句柄可被多线程共用（只读），各线程处理
    // 互不重叠的行，结果与单线程逐行调用逐字节相同。小图不分线程（调度开销更大）。
    constexpr size_t parallelPixelThreshold = 2u << 20; // 约 200 万像素
    if (mat.total() < parallelPixelThreshold) {
        if (mat.isContinuous()) {
            cmsDoTransform(transform, mat.ptr(), mat.ptr(), static_cast<cmsUInt32Number>(mat.total()));
        }
        else {
            for (int y = 0; y < mat.rows; ++y)
                cmsDoTransform(transform, mat.ptr(y), mat.ptr(y), static_cast<cmsUInt32Number>(mat.cols));
        }
        return true;
    }

    const int rowsPerBlock = (std::max)(1, mat.rows / 64);
    const int blockCount = (mat.rows + rowsPerBlock - 1) / rowsPerBlock;
    const cmsHTRANSFORM rawTransform = transform;
    cv::parallel_for_(cv::Range(0, blockCount), [&](const cv::Range& range) {
        for (int block = range.start; block < range.end; ++block) {
            const int firstRow = block * rowsPerBlock;
            const int lastRow = (std::min)(firstRow + rowsPerBlock, mat.rows);
            for (int y = firstRow; y < lastRow; ++y) {
                cmsDoTransform(rawTransform, mat.ptr(y), mat.ptr(y), static_cast<cmsUInt32Number>(mat.cols));
            }
        }
    });
    return true;
}

void ColorManager::applyToImageAsset(ImageAsset& imageAsset) {
    if (!GlobalVar::settingParameter.enableColorManagement)
        return;

    std::vector<uint8_t>& monitorIcc = readMonitorIccProfileCached();
    applyToMat(imageAsset.primaryFrame, imageAsset.iccProfile, monitorIcc);
}
