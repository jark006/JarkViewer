#include "jarkUtils.h"

#include "BatchWindow.h"
#include "UiHost.h"
#include "EditorWindow.h"
#include "CanvasRenderer.h"
#include "InfoScreen.h"
#include "NavigationOverlay.h"
#include "ThumbnailService.h"
#include "DecodeProbe.h"
#include "Localization.h"
#include "MediaPlayer.h"
#include "VectorImage.h"
#include "TextRenderer.h"
#include "ImageDatabase.h"
#include "PrintWindow.h"
#include "RenameWindow.h"
#include "SettingWindow.h"

#include "D3D11App.h"
#include <optional>
#include <random>
#include <ppl.h>
#include <concrt.h>

/* TODO
1. 在鼠标光标位置缩放
1. 给系统提供缩略图缓存支持
1. 缩放策略加个线性插值
1. LunaSVG库支持度较差，考虑更换
1. 考虑加个按时间日期排序
1. 导出实况的视频
*/

std::wstring_view appName = L"JarkViewer";
std::wstring_view appVersion = L"v2.0";
constinit int appVersionCode = 20000; // 主版本*10000 + 次版本*100 + 修订版本

std::wstring_view jarkLink = L"https://github.com/jark006";
std::wstring_view RepositoryLink = L"https://github.com/jark006/JarkViewer";
std::wstring_view BaiduLink = L"https://pan.baidu.com/s/1ka7p__WVw2du3mnOfqWceQ?pwd=6666"; // 密码 6666
std::wstring_view LanzouLink = L"https://jark006.lanzout.com/b0ko7mczg"; // 密码 6666


static constexpr auto generate_zoom_list() {
    // 原始缩放级别数组（2^10 到 2^22）
    constexpr std::array<int64_t, 13> base = {
        1 << 10, 1 << 11, 1 << 12, 1 << 13, 1 << 14,
        1 << 15, 1 << 16, 1 << 17, 1 << 18, 1 << 19,
        1 << 20, 1 << 21, 1 << 22
    };
    constexpr double baseScale = 1.148698354997035;// std::pow(2.0, 0.2);

    std::array<int64_t, 5 * base.size() - 4> result{};

    size_t index = 0;
    for (size_t i = 0; i < base.size(); ++i) {
        result[index++] = base[i];

        if (i < base.size() - 1) {
            result[index++] = (int64_t)(base[i] * baseScale);
            result[index++] = (int64_t)(base[i] * baseScale * baseScale);
            result[index++] = (int64_t)(base[i] * baseScale * baseScale * baseScale);
            result[index++] = (int64_t)(base[i] * baseScale * baseScale * baseScale * baseScale);
        }
    }
    return result;
}


struct CurImageParameter {
    static constexpr auto ZOOM_LIST = generate_zoom_list();
    static constexpr int64_t ZOOM_BASE = ::ZOOM_BASE; // 100%缩放

    int64_t zoomTarget;     // 设定的缩放比例
    int64_t zoomCur;        // 动画播放过程的缩放比例，动画完毕后的值等于zoomTarget
    int curFrameIdx;        // 小于0则单张静态图像，否则为动画当前帧索引
    int curFrameIdxMax;     // 若是动画则为帧数量
    int curFrameDelay;      // 当前帧延迟
    Cood slideCur, slideTarget;
    std::shared_ptr<ImageAsset> imageAssetPtr;

    vector<int64_t> zoomList;
    int zoomIndex = 0;
    int zoomIndexFix = 0;
    int zoomIndex100percent = 0;
    bool isAnimationPause = false;
    int width = 0;
    int height = 0;
    int rotation = 0; // 旋转： 0正常， 1逆90度， 2：180度， 3顺90度

    CurImageParameter() {
        Init();
    }

    void Init(int winWidth = 0, int winHeight = 0) {

        curFrameIdx = 0;
        curFrameDelay = 0;

        slideCur = 0;
        slideTarget = 0;
        rotation = 0;
        isAnimationPause = false;

        if (imageAssetPtr) {
            curFrameIdxMax = imageAssetPtr->format == ImageFormat::Animated ? (int)imageAssetPtr->frames.size() - 1 : 1;

            int imageWidth = 0;
            int imageHeight = 0;
            if (imageAssetPtr->vectorSource) {
                // 矢量图以文档尺寸为基准（位图分辨率随后按缩放按需变化）
                imageWidth = imageAssetPtr->vectorSource->intrinsicWidth;
                imageHeight = imageAssetPtr->vectorSource->intrinsicHeight;
            }
            else if (imageAssetPtr->format == ImageFormat::Animated && !imageAssetPtr->frames.empty()) {
                imageWidth = imageAssetPtr->frames[0].cols;
                imageHeight = imageAssetPtr->frames[0].rows;
            }
            else {
                imageWidth = imageAssetPtr->primaryFrame.cols;
                imageHeight = imageAssetPtr->primaryFrame.rows;
            }

            applyViewForSize(imageWidth, imageHeight, winWidth, winHeight);
        }
        else {
            curFrameIdxMax = 0;
            applyViewForSize(0, 0, winWidth, winHeight);
        }
    }

    // 按给定名义尺寸重算缩放与居中（Init 使用图像尺寸，视频播放使用视频尺寸）
    void applyViewForSize(int imageWidth, int imageHeight, int winWidth, int winHeight) {
        width = imageWidth;
        height = imageHeight;

        if (width > 0 && height > 0 && winWidth > 0 && winHeight > 0) {
            //适应显示窗口宽高的缩放比例
            int64_t zoomFitWindow = std::min(winWidth * ZOOM_BASE / width, winHeight * ZOOM_BASE / height);
            zoomTarget = (height > winHeight || width > winWidth) ? zoomFitWindow :
                (GlobalVar::settingParameter.isOneToOnePreferred ? ZOOM_BASE : zoomFitWindow);
            zoomCur = zoomTarget;

            zoomList = std::vector<int64_t>(ZOOM_LIST.begin(), ZOOM_LIST.end());
            if (!std::ranges::binary_search(ZOOM_LIST, zoomFitWindow) || 
                zoomFitWindow < ZOOM_LIST.front() || 
                zoomFitWindow > ZOOM_LIST.back())
                zoomList.emplace_back(zoomFitWindow);
            std::sort(zoomList.begin(), zoomList.end());
            auto it = std::find(zoomList.begin(), zoomList.end(), zoomTarget);
            zoomIndex = (it != zoomList.end()) ? (int)std::distance(zoomList.begin(), it) : (int)(ZOOM_LIST.size() / 2);

            it = std::find(zoomList.begin(), zoomList.end(), zoomFitWindow);
            zoomIndexFix = (it != zoomList.end()) ? (int)std::distance(zoomList.begin(), it) : zoomIndex;
            it = std::find(zoomList.begin(), zoomList.end(), ZOOM_BASE);
            zoomIndex100percent = (it != zoomList.end()) ? (int)std::distance(zoomList.begin(), it) : zoomIndex;
        }
        else {
            zoomList = std::vector<int64_t>(ZOOM_LIST.begin(), ZOOM_LIST.end());
            zoomIndex = (int)(ZOOM_LIST.size() / 2);
            zoomIndexFix = zoomIndex;
            zoomIndex100percent = zoomIndex;
            zoomTarget = ZOOM_BASE;
            zoomCur = ZOOM_BASE;
        }
    }

    void updateZoomList(int winWidth = 0, int winHeight = 0) {
        if (winHeight == 0 || winWidth == 0 || imageAssetPtr == nullptr)
            return;

        //适应显示窗口宽高的缩放比例
        int64_t zoomFitWindow = (rotation == 0 || rotation == 2)?
            std::min(winWidth * ZOOM_BASE / width, winHeight * ZOOM_BASE / height):
            std::min(winWidth * ZOOM_BASE / height, winHeight * ZOOM_BASE / width);

        zoomList = std::vector<int64_t>(ZOOM_LIST.begin(), ZOOM_LIST.end());
        if (!std::ranges::binary_search(ZOOM_LIST, zoomFitWindow) ||
            zoomFitWindow < ZOOM_LIST.front() ||
            zoomFitWindow > ZOOM_LIST.back())
            zoomList.emplace_back(zoomFitWindow);
        else {
            if (zoomIndex >= zoomList.size())
                zoomIndex = (int)zoomList.size() - 1;
        }
        std::sort(zoomList.begin(), zoomList.end());

        auto it = std::find(zoomList.begin(), zoomList.end(), zoomFitWindow);
        zoomIndexFix = (it != zoomList.end()) ? (int)std::distance(zoomList.begin(), it) : zoomIndex;
        it = std::find(zoomList.begin(), zoomList.end(), ZOOM_BASE);
        zoomIndex100percent = (it != zoomList.end()) ? (int)std::distance(zoomList.begin(), it) : zoomIndex;
    }

    void slideTargetRotateLeft() {
        slideTarget = { slideTarget.y, -slideTarget.x };
        slideCur = slideTarget;
    }

    void slideTargetRotateRight() {
        slideTarget = { -slideTarget.y, slideTarget.x };
        slideCur = slideTarget;
    }
};


// 主窗口叠加按钮的图标资源：file/mainRes.png 是一张 200x200 的雪碧图，图标按 96DPI 设计。
// 贴图交给 ImGui 按当前 DPI 拉伸绘制（保持原有的缩放机制），这里只负责切片与上传。
class OverlayIcons {
public:
    struct Slice {
        float x, y, w, h; // 在雪碧图中的位置与尺寸（96DPI 基准）
    };

    static constexpr Slice rotateLeft{ 0.0f, 0.0f, 50.0f, 50.0f };
    static constexpr Slice rotateRight{ 50.0f, 0.0f, 50.0f, 50.0f };
    static constexpr Slice printer{ 0.0f, 50.0f, 50.0f, 50.0f };
    static constexpr Slice setting{ 50.0f, 50.0f, 50.0f, 50.0f };
    static constexpr Slice leftArrow{ 100.0f, 0.0f, 50.0f, 100.0f };
    static constexpr Slice rightArrow{ 150.0f, 0.0f, 50.0f, 100.0f };
    static constexpr Slice barPlaying{ 0.0f, 100.0f, 200.0f, 50.0f }; // 播放中：条上是暂停按钮
    static constexpr Slice barPaused{ 0.0f, 150.0f, 200.0f, 50.0f };  // 已暂停：条上是继续按钮

    // 上传雪碧图（只上传一次；贴图还没准备好时返回 0，调用方跳过绘制）
    ImTextureID texture() {
        if (texture_ == 0 && !sheet_.empty())
            texture_ = jark::ui::UiHost::instance().textureFromImage(sheet_, textureSlot);
        return texture_;
    }

    // uv 半像素内缩：放大绘制时边缘的双线性采样会掺进相邻格——播放条是亮的，
    // 会把打印/设置图标的下边缘拉出一条亮线
    static constexpr ImVec2 uv0(const Slice& slice) {
        return { (slice.x + 0.5f) / sheetWidth, (slice.y + 0.5f) / sheetHeight };
    }

    static constexpr ImVec2 uv1(const Slice& slice) {
        return { (slice.x + slice.w - 0.5f) / sheetWidth, (slice.y + slice.h - 0.5f) / sheetHeight };
    }

private:
    static constexpr float sheetWidth = 200.0f;
    static constexpr float sheetHeight = 200.0f;
    static constexpr int textureSlot = 3; // 纹理槽：1=打印预览、2=编辑画布

    static cv::Mat loadSheet() {
        auto rc = jarkUtils::GetResource(IDB_PNG_MAIN_RES, L"PNG");
        if (!rc.size || !rc.ptr)
            return {};

        cv::Mat pngData(1, static_cast<int>(rc.size), CV_8UC1, static_cast<uint8_t*>(rc.ptr));
        return cv::imdecode(pngData, cv::IMREAD_UNCHANGED);
    }

    cv::Mat sheet_ = loadSheet();
    ImTextureID texture_ = 0;
};


class JarkViewerApp : public D3D11App {
public:

    OperateQueue operateQueue;
    jark::ui::NavigationOverlay navigation;
    uint64_t navigationImageVersion = 1;
    uint64_t directoryVersion = 1;
    int homeButtonState = 0; // 主页「打开图片」按钮：0 普通 / 1 悬停 / 2 按下

    CursorPos cursorPos = CursorPos::centerArea;
    CursorPos cursorPosLast = CursorPos::centerArea;
    ShowExtraUI extraUIFlag = ShowExtraUI::none;
    bool mouseIsPressing = false;
    bool ctrlIsPressing = false;
    bool smoothShift = false;
    bool showExif = false;
    // EXIF 面板的附加信息与滚动状态（附加信息在切图时算一次，见 updateExifPanelExtras）
    const ImageAsset* panelInfoAsset_ = nullptr;
    std::array<std::array<uint32_t, 256>, 3> histogram_{};
    uint32_t histogramPeak_ = 0;
    bool histogramReady_ = false;
    std::string panelColorSpace_;
    cv::Rect2f exifPanelRect_{};
    float exifPanelScroll_ = 0.0f;
    float exifPanelMaxScroll_ = 0.0f;
    Cood mousePos, mousePressPos;
    ImageDatabase imgDB;

    int curFileIdx = -1;         // 文件在路径列表的索引
    vector<wstring> imgFileList; // 工作目录下所有图像文件路径

    TextRenderer textDrawer;       // 给Mat绘制文字
    std::unique_ptr<jark::MediaPlayer> mediaPlayer; // 实况照片/视频的实时播放
    cv::Mat playbackFrame;                          // 播放中的当前帧
    const ImageAsset* playedAsset = nullptr;        // 已播放过的资源（每张图只自动播一次）
    const ImageAsset* lastSeenAsset = nullptr;      // 用于检测切图
    int lastSeenFileIndex = -1;

    // 「实况」角标：贴图片左上角显示，悬停就重播并出声（主动操作，设置静音也出声）
    bool currentIsLivePhoto_ = false;
    bool liveBadgeVisible_ = false;
    bool liveBadgeHovered_ = false;
    bool liveReplaySound_ = false;
    cv::Rect2f liveBadgeRect_{};

    bool liveBadgeHit(int x, int y) const {
        return liveBadgeVisible_ &&
            x >= liveBadgeRect_.x && x < liveBadgeRect_.x + liveBadgeRect_.width &&
            y >= liveBadgeRect_.y && y < liveBadgeRect_.y + liveBadgeRect_.height;
    }

    CurImageParameter curPar;
    std::chrono::steady_clock::time_point lastClickTimestamp{}, lastWinResizeTimestamp{};

    JarkViewerApp() {
        m_wndCaption = std::format(L"{} {}", appName, appVersion);
    }

    // 文字尺寸跟随窗口所在显示器的 DPI（100%:16 150%:24 200%:32）
    void updateTextDrawerScale() {
        const UINT dpi = static_cast<UINT>(std::lround(96.0f * uiScale()));
        textDrawer.setSize(dpi >= 168 ? 32 : (dpi >= 144 ? 24 : 16));
    }

    void OnDpiChanged() override {
        updateTextDrawerScale();
        if (hasInitWinSize)
            operateQueue.push({ ActionENUM::refresh });
    }

    ~JarkViewerApp() {
        jark::ThumbnailService::instance().shutdown();
        navigation.releaseTextures();
    }

    // ---- 启动路径：首图解码与 D3D 设备创建并行 ----
    std::wstring startupFilePath_;
    bool startupFilePrepared_ = false;
    std::chrono::steady_clock::time_point appStart_ = std::chrono::steady_clock::now();

    long long startupMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - appStart_).count();
    }

    void setStartupFile(std::wstring path) { startupFilePath_ = std::move(path); }

    // 窗口句柄刚创建、D3D 设备还没建：先把首图解码派发出去（只派发不等待），
    // 建 D3D 设备/交换链的几十毫秒里解码在后台线程上并行跑。等待与收尾仍由 initOpenFile 完成。
    void OnWindowCreated() override {
        imgDB.setColorManagementWindow(m_hWnd); // 显示器 ICC 靠窗口所在显示器，派发前先设好
        prepareFileList(startupFilePath_);
        if (curFileIdx >= 0 && curFileIdx < static_cast<int>(imgFileList.size())) {
            imgDB.requestPreloadBatch({ imgFileList[curFileIdx],
                imgFileList[static_cast<size_t>(curFileIdx + 1) % imgFileList.size()] });
        }
        startupFilePrepared_ = true;
        jarkUtils::startupTraceMark("window ready, decode dispatched");
        JARK_LOG("startup: decode dispatched at {} ms (before device creation)", startupMs());
    }

    HRESULT InitWindow(HINSTANCE hInstance) {
        if (!SUCCEEDED(D3D11App::Initialize(hInstance)))
            return S_FALSE;

        if (m_pD3DDevice == nullptr)
            return S_FALSE;

        imgDB.setColorManagementWindow(m_hWnd);
        jark::ThumbnailService::instance().initialize(
            std::filesystem::path(GlobalVar::settingPath).parent_path() / L"JarkViewer.thumbnail");

        updateTextDrawerScale();
        jarkUtils::startupTraceMark("device and UI ready");

        return S_OK;
    }

    // 主页/解码失败等占位资源：不再是资源图，画面由 InfoScreen 按当前语言与主题实时绘制
    static ImageAsset placeholderAsset(PlaceholderKind kind, std::wstring detail = {}, std::string exifInfo = {}) {
        ImageAsset asset;
        asset.format = ImageFormat::Still;
        asset.placeholder = kind;
        asset.placeholderDetail = std::move(detail);
        asset.exifInfo = std::move(exifInfo);
        return asset;
    }

    bool isHomeScreen() const {
        return curPar.imageAssetPtr && curPar.imageAssetPtr->placeholder == PlaceholderKind::Home;
    }

    // 主页「打开图片」按钮命中：把客户区坐标逆变换回画布像素（考虑缩放/平移/旋转）
    bool homeButtonHit(int x, int y) const {
        if (!isHomeScreen() || curPar.imageAssetPtr->primaryFrame.empty())
            return false;

        const auto geometry = jark::imageGeometry(viewState(), { winWidth, winHeight });
        if (geometry.scale <= 0.0 || geometry.nominalSize.empty())
            return false;

        const int nominalX = static_cast<int>((x - geometry.origin.x) / geometry.scale);
        const int nominalY = static_cast<int>((y - geometry.origin.y) / geometry.scale);
        const cv::Size bitmap = curPar.imageAssetPtr->primaryFrame.size();
        cv::Point source;
        switch (curPar.rotation & 3) {
        case 1: source = { bitmap.width - 1 - nominalY, nominalX }; break;
        case 2: source = { bitmap.width - 1 - nominalX, bitmap.height - 1 - nominalY }; break;
        case 3: source = { nominalY, bitmap.height - 1 - nominalX }; break;
        default: source = { nominalX, nominalY }; break;
        }
        return jark::homeButtonRect({ winWidth, winHeight }, uiScale()).contains(source);
    }

    void setHomeButtonState(int state) {
        if (homeButtonState == state)
            return;
        homeButtonState = state;
        if (isHomeScreen()) {
            curPar.imageAssetPtr->placeholderStamp = 0; // 交给 DrawScene 按新状态重绘按钮
            markPresentRequested();
        }
    }

    void openImageFromHome() {
        wstring filePath = jarkUtils::SelectFile(m_hWnd);
        if (!filePath.empty()) {
            initOpenFile(filePath);
            operateQueue.push({ ActionENUM::refresh });
        }
    }

    // 尺寸、DPI、语言、主题或按钮状态变化时重新绘制当前占位画面。
    // 返回 0=无变化；1=仅内容变化；2=尺寸也变化（调用方需要重新 Init 视图）
    int updatePlaceholderImage() {
        ImageAsset* asset = curPar.imageAssetPtr.get();
        if (!asset || asset->placeholder == PlaceholderKind::None)
            return 0;

        const cv::Size size{ winWidth, winHeight };
        const int interaction = asset->placeholder == PlaceholderKind::Home ? homeButtonState : 0;
        const uint64_t stamp = jark::infoScreenStamp(size, uiScale(), interaction);
        if (asset->placeholderStamp == stamp)
            return 0;

        const bool sizeChanged = asset->primaryFrame.empty() || asset->primaryFrame.size() != size;
        asset->placeholderStamp = stamp;
        asset->primaryFrame = jark::renderInfoScreen(asset->placeholder, asset->placeholderDetail,
            size, uiScale(), interaction);
        return sizeChanged ? 2 : 1;
    }

    // 打开图片的前半段：清缓存、扫同目录、建列表、放占位（不碰视图状态）。
    // 启动路径在 OnWindowCreated() 里先跑这一段并派发解码，随后 initOpenFile 只消费结果；
    // 其它时机的打开由 initOpenFile 连着后半段一起跑。
    void prepareFileList(wstring filePath) {
        namespace fs = std::filesystem;

        curFileIdx = -1;
        imgFileList.clear();
        imgDB.clear();
        homeButtonState = 0;
        stopMediaPlayback();
        updateNavigationDirectory();

        if (filePath.empty()) {
            imgFileList.emplace_back(m_wndCaption);
            curFileIdx = 0;
            imgDB.put(m_wndCaption, placeholderAsset(PlaceholderKind::Home, {}, getUIString(32)));
            return;
        }

        fs::path fullPath = fs::absolute(filePath);
        wstring openFileName = fullPath.filename().wstring();

        auto workDir = fullPath.parent_path();
        if (fs::exists(workDir)) {
            std::vector<std::wstring> filePaths;
            for (const auto& entry : fs::directory_iterator(workDir)) {
                if (!entry.is_regular_file())continue;

                std::wstring ext = entry.path().extension().wstring();
                if (ext.length() < 2)continue;

                ext = ext.substr(1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                if (ImageDatabase::supportExt.contains(ext) || ImageDatabase::supportRaw.contains(ext)) {
                    filePaths.emplace_back(entry.path().wstring());
                }
            }

            // 排序方式由设置决定（名称自然序 / 修改时间 / 文件大小）；先把打开的这张定位出来，
            // 排序时把它的下标一起带过去
            int index = -1;
            for (size_t i = 0; i < filePaths.size(); ++i) {
                if (fs::path(filePaths[i]).filename() == openFileName) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            jarkUtils::sortImageFileList(filePaths, GlobalVar::settingParameter.sortMode, index);
            curFileIdx = index;

            for (auto& filePath : filePaths)
                imgFileList.emplace_back(std::move(filePath));
        }
        else {
            curFileIdx = -1;
        }

        if (curFileIdx < 0) { // 文件不在支持列表里：区分"不支持的格式"和"文件根本不存在/打不开"
            imgFileList.emplace_back(fullPath.wstring());
            curFileIdx = (int)imgFileList.size() - 1;

            auto dotPos = filePath.rfind(L'.');
            auto ext = wstring((dotPos != std::wstring::npos && dotPos < filePath.size() - 1) ?
                filePath.substr(dotPos + 1) : filePath);
            for (auto& c : ext) c = std::tolower(c);

            // 视频文件不加占位，交给加载器当动态照片处理(仅解码前 MAX_VIDEO_FRAMES 帧)
            if (!ImageDatabase::videoExt.contains(ext)) {
                const bool readable = std::filesystem::exists(fullPath) &&
                    GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES;
                imgDB.put(fullPath.wstring(), placeholderAsset(
                    readable ? PlaceholderKind::UnsupportedFormat : PlaceholderKind::FileMissing,
                    fullPath.wstring(), getUIString(33)));
            }
        }
    }

    // 打开图片的后半段：请求解码（不阻塞；未就绪则主页垫底 + "加载中"浮标，
    // 就绪后由 updatePendingLoad 走 adoptCurrentImage 收尾）、刷新占位、初始化视图
    void finishOpenFile() {
        if (auto asset = requestCurrentImage(imgFileList[(curFileIdx + 1) % imgFileList.size()]))
            curPar.imageAssetPtr = std::move(asset);
        updatePlaceholderImage();
        curPar.Init(winWidth, winHeight);
        updateNavigationDirectory();
    }

    void initOpenFile(wstring filePath) {
        if (startupFilePrepared_ && filePath == startupFilePath_) {
            // 启动路径：OnWindowCreated() 已抢在 D3D 设备创建前扫描目录并派发解码。
            // 这里不能再跑前半段——imgDB.clear() 会把在途解码作废——直接等结果收尾。
            startupFilePrepared_ = false;
            finishOpenFile();
            jarkUtils::startupTraceMark("open handled");
            JARK_LOG("startup: open handled at {} ms", startupMs());
            return;
        }

        prepareFileList(std::move(filePath));
        finishOpenFile();
    }

    inline void handleAnimationControl(int x, int y) {
        // 按钮ID  0:上一帧  1:暂停/继续  2:下一帧  3:保存该帧（宽度按 DPI 缩放，与播放条资源图一致）
        int buttonIdx = (x + dp(100) - winWidth / 2) / dp(50);
        if (buttonIdx < 0 || 3 < buttonIdx || curPar.imageAssetPtr->format != ImageFormat::Animated)
            return;

        if (curPar.isAnimationPause) {
            switch (buttonIdx) {
            case 0: {
                if (--curPar.curFrameIdx < 0)
                    curPar.curFrameIdx = curPar.curFrameIdxMax;
                operateQueue.push({ ActionENUM::refresh });
            }break;
            case 1: {
                curPar.isAnimationPause = !curPar.isAnimationPause;
                operateQueue.push({ ActionENUM::refresh });
            }break;
            case 2: {
                if (++curPar.curFrameIdx > curPar.curFrameIdxMax)
                    curPar.curFrameIdx = 0;
                operateQueue.push({ ActionENUM::refresh });
            }break;
            case 3: {
                auto [filePath, isJPG] = jarkUtils::saveImageDialogW(getUIStringW(4).c_str());
                if (filePath.length() <= 2)
                    break;

                cv::Mat img = currentSourceImage();

                std::vector<uchar> buffer;
                if (cv::imencode(isJPG ? ".jpg" : ".png", img, buffer)) {
                    std::ofstream file(filePath, std::ios::binary);
                    if (file.is_open()) {
                        file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                        file.close();
                    }
                }
            }break;
            }
        }
        else {
            if (buttonIdx == 1) {
                curPar.isAnimationPause = !curPar.isAnimationPause;
                operateQueue.push({ ActionENUM::refresh });
            }
        }
    }

    void syncNavigation() {
        const bool hasImage = curFileIdx >= 0 && curFileIdx < static_cast<int>(imgFileList.size()) &&
            imgFileList[curFileIdx] != m_wndCaption;
        navigation.sync(viewState(), { winWidth, winHeight }, uiScale(),
            !GlobalVar::settingParameter.hideNavigator, anyWindowVisible() || !hasImage,
            navigationImageVersion, curFileIdx);
    }

    void updateNavigationDirectory() {
        ++directoryVersion;
        ++navigationImageVersion;
        navigation.setDirectory(imgFileList.size() == 1 && imgFileList[0] == m_wndCaption
            ? std::vector<std::wstring>{} : imgFileList, curFileIdx);
    }

    static unsigned pointerButton(WPARAM message) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) return 1;
        if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) return 2;
        if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) return 4;
        return 8;
    }

    bool applyNavigationEvent(const jark::ui::NavigationOverlay::Event& event) {
        if (event.redraw)
            markPresentRequested();
        if (event.handled) {
            extraUIFlag = ShowExtraUI::none;
            cursorPosLast = cursorPos = CursorPos::centerArea;
        }
        // 鸟瞰面板右上角的 ✕：收起鸟瞰图，等同取消设置里的「显示鸟瞰图」勾选（随退出统一写盘）
        if (event.closeNavigator)
            GlobalVar::settingParameter.hideNavigator = true;
        if (event.selected >= 0 && event.selected != curFileIdx)
            operateQueue.push({ ActionENUM::jumpToImage, event.selected, 0, directoryVersion });
        if (event.slide)
            operateQueue.push({ ActionENUM::navigateImage, event.slide->x, event.slide->y, navigationImageVersion });
        return event.handled;
    }

    bool OnMouseRelease(WPARAM button, int x, int y, WPARAM state) override {
        const auto event = navigation.mouseUp(pointerButton(button));
        if (event.handled) {
            mouseIsPressing = false;
            applyNavigationEvent(event);
            applyNavigationEvent(navigation.mouseMove({ x, y }, false));
            return true;
        }
        if (button == WM_LBUTTONUP)
            mouseIsPressing = false; // 即使这次抬起被二级窗口拦截，也结束旧的画布拖动
        return false;
    }

    void OnPointerCancel() override {
        navigation.cancel();
        setHomeButtonState(0);
        mouseIsPressing = false;
        ctrlIsPressing = false;
        extraUIFlag = ShowExtraUI::none;
        cursorPosLast = cursorPos = CursorPos::centerArea;
        markPresentRequested();
    }

    void OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) override {
        syncNavigation();
        if (!mouseIsPressing && applyNavigationEvent(navigation.mouseDown({ x, y }, pointerButton(btnState))))
            return;
        // 按下前重算旧按钮命中，不能依赖上一条 mousemove 的位置。
        OnMouseMove(btnState, x, y);

        // 「实况」角标消费自己的左键按下（悬停已经触发重播），不进入画布拖动/双击全屏
        if ((uint64_t)btnState == WM_LBUTTONDOWN && liveBadgeHit(x, y))
            return;

        switch ((uint64_t)btnState)
        {
        case WM_LBUTTONDOWN: {//左键
            if (isHomeScreen() && homeButtonHit(x, y)) { // 主页「打开图片」按钮：按下并显示按压态
                setHomeButtonState(2);
                return;
            }
            if (cursorPos == CursorPos::centerArea) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = duration_cast<std::chrono::milliseconds>(now - lastClickTimestamp).count();
                lastClickTimestamp = now;

                if (10 < elapsed && elapsed < 300) { // 10 ~ 300 ms
                    jarkUtils::ToggleFullScreen(m_hWnd);
                }
                else {
                    mouseIsPressing = true;
                }
            }

            mousePressPos = { x, y };

            if (cursorPos == CursorPos::leftEdge)
                operateQueue.push({ ActionENUM::preImg });
            else if (cursorPos == CursorPos::rightEdge)
                operateQueue.push({ ActionENUM::nextImg });
            else if (cursorPos == CursorPos::leftUp)
                operateQueue.push({ ActionENUM::rotateLeft });
            else if (cursorPos == CursorPos::rightUp)
                operateQueue.push({ ActionENUM::rotateRight });
            else if (cursorPos == CursorPos::leftDown)
                operateQueue.push({ ActionENUM::printImage });
            else if (cursorPos == CursorPos::rightDown)
                operateQueue.push({ ActionENUM::setting, 0 });
            else if (cursorPos == CursorPos::centerTop) {
                handleAnimationControl(x, y);
            }
            return;
        }

        case WM_RBUTTONDOWN: {//右键
            return;
        }

        case WM_MBUTTONDOWN: {//中键
            operateQueue.push({ ActionENUM::toggleExif });
            return;
        }

        case WM_XBUTTONDOWN: {//侧键
            WORD xButton = GET_XBUTTON_WPARAM(wParam);
            if (xButton == XBUTTON1) {
                operateQueue.push({ ActionENUM::nextImg });
            }
            else if (xButton == XBUTTON2) {
                operateQueue.push({ ActionENUM::preImg });
            }
            return;
        }

        default: {
            JARK_LOG("{} KeyValue: 0x{:04x}", __FUNCTION__, (uint64_t)btnState);
        }break;
        }
    }

    void OnMouseUp(WPARAM btnState, int x, int y, WPARAM wParam) override {
        switch ((uint64_t)btnState)
        {
        case WM_LBUTTONUP: {//左键
            mouseIsPressing = false;
            if (homeButtonState == 2) { // 主页按钮：在按钮内抬起才算点击，等同 Ctrl+O
                const bool inside = homeButtonHit(x, y);
                setHomeButtonState(inside ? 1 : 0);
                if (inside)
                    openImageFromHome();
                return;
            }
            operateQueue.push({ ActionENUM::refresh });
            return;
        }

        case WM_RBUTTONUP: {//右键
            if (GlobalVar::settingParameter.rightClickAction == 0) {
                PostMessageW(m_hWnd, WM_CONTEXTMENU, 0, MAKELPARAM(x, y));
            }
            else {
                operateQueue.push({ ActionENUM::requestExit });
            }
            return;
        }

        case WM_MBUTTONUP: {//中键
            return;
        }

        case WM_XBUTTONUP: {//侧键
            //WORD xButton = GET_XBUTTON_WPARAM(wParam);
            //if (xButton == XBUTTON1){}
            //else if (xButton == XBUTTON2){}
            return;
        }

        default: {
            JARK_LOG("{} KeyValue: 0x{:04x}", __FUNCTION__, (uint64_t)btnState);
        }break;
        }
    }

    void OnMouseMove(WPARAM btnState, int x, int y) override {
        mousePos = { x, y };
        syncNavigation();
        if (applyNavigationEvent(navigation.mouseMove({ x, y }, mouseIsPressing)))
            return;

        // 「实况」角标悬停：进入的瞬间重播并出声（悬停本身就是"想听"的主动操作，设置静音也出声）
        const bool overBadge = liveBadgeHit(x, y);
        if (overBadge != liveBadgeHovered_) {
            liveBadgeHovered_ = overBadge;
            markPresentRequested(); // 高亮反馈/擦除
            if (overBadge && currentIsLivePhoto_ && curPar.imageAssetPtr && curPar.imageAssetPtr->videoSource) {
                stopMediaPlayback();
                playedAsset = nullptr;   // 从头再播
                liveReplaySound_ = true; // 这次出声
                operateQueue.push({ ActionENUM::refresh });
            }
        }

        if (isHomeScreen()) { // 主页「打开图片」按钮的悬停/按下反馈
            const int desired = homeButtonHit(x, y)
                ? (homeButtonState == 2 && mouseIsPressing ? 2 : 1) : 0;
            setHomeButtonState(desired);
        }

        const int edgeWidth = dp(50);   // 悬停热区宽度按 DPI 缩放，与按钮资源图一致
        const int edgeHeight = dp(50);
        const int centerTopWidth = dp(100);

        if (mouseIsPressing) {
            cursorPos = CursorPos::centerArea;
        }
        else {
            if (winWidth >= dp(500)) {
                if (0 <= x && x < edgeWidth) {
                    if (0 <= y && y < (winHeight / 4)) {
                        cursorPos = CursorPos::leftUp;
                    }
                    else if ((winHeight / 4) <= y && y < (winHeight * 3 / 4)) {
                        cursorPos = CursorPos::leftEdge;
                    }
                    else if ((winHeight * 3 / 4) <= y && y < (winHeight)) {
                        cursorPos = CursorPos::leftDown;
                    }
                }
                else if (((winWidth - edgeWidth) < x && x <= winWidth)) {
                    if (0 <= y && y < (winHeight / 4)) {
                        cursorPos = CursorPos::rightUp;
                    }
                    else if ((winHeight / 4) <= y && y < (winHeight * 3 / 4)) {
                        cursorPos = CursorPos::rightEdge;
                    }
                    else if ((winHeight * 3 / 4) <= y && y < (winHeight)) {
                        cursorPos = CursorPos::rightDown;
                    }
                }
                else {
                    cursorPos = CursorPos::centerArea;
                }
            }
            else {
                if (0 <= x && x < (winWidth / 4)) {
                    if (0 <= y && y < (winHeight / 4)) {
                        cursorPos = CursorPos::leftUp;
                    }
                    else if ((winHeight / 4) <= y && y < (winHeight * 3 / 4)) {
                        cursorPos = CursorPos::leftEdge;
                    }
                    else if ((winHeight * 3 / 4) <= y && y < (winHeight)) {
                        cursorPos = CursorPos::leftDown;
                    }
                }
                else if ((winWidth * 3 / 4) < x && x <= winWidth) {
                    if (0 <= y && y < (winHeight / 4)) {
                        cursorPos = CursorPos::rightUp;
                    }
                    else if ((winHeight / 4) <= y && y < (winHeight * 3 / 4)) {
                        cursorPos = CursorPos::rightEdge;
                    }
                    else if ((winHeight * 3 / 4) <= y && y < (winHeight)) {
                        cursorPos = CursorPos::rightDown;
                    }
                }
                else {
                    cursorPos = CursorPos::centerArea;
                }
            }

            if (y < edgeHeight && abs(x - winWidth / 2) < centerTopWidth) {
                cursorPos = CursorPos::centerTop;
            }
        }

        if (cursorPosLast != cursorPos) {
            switch (cursorPos)
            {
            case CursorPos::leftUp:
                extraUIFlag = ShowExtraUI::rotateLeftButton;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::leftDown:
                extraUIFlag = ShowExtraUI::printer;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::leftEdge:
                extraUIFlag = ShowExtraUI::leftArrow;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::centerTop:
                if (curPar.imageAssetPtr->format == ImageFormat::Animated) {
                    extraUIFlag = ShowExtraUI::animationBar;
                    operateQueue.push({ ActionENUM::refresh });
                }
                break;

            case CursorPos::centerArea:
                extraUIFlag = ShowExtraUI::none;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::rightEdge:
                extraUIFlag = ShowExtraUI::rightArrow;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::rightDown:
                extraUIFlag = ShowExtraUI::setting;
                operateQueue.push({ ActionENUM::refresh });
                break;

            case CursorPos::rightUp:
                extraUIFlag = ShowExtraUI::rotateRightButton;
                operateQueue.push({ ActionENUM::refresh });
                break;
            }

            cursorPosLast = cursorPos;
        }

        if (mouseIsPressing) {
            auto slideDelta = mousePos - mousePressPos;
            mousePressPos = mousePos;
            operateQueue.push({ ActionENUM::slide, slideDelta.x, slideDelta.y });
        }
    }

    void OnMouseLeave() override {
        navigation.mouseLeave();
        setHomeButtonState(0);
        liveBadgeHovered_ = false;
        cursorPosLast = cursorPos = CursorPos::centerArea;
        extraUIFlag = ShowExtraUI::none;
        if (GetCapture() != m_hWnd)
            mouseIsPressing = false;
        markPresentRequested();
    }

    void OnMouseWheel(UINT nFlags, short zDelta, int x, int y) override {
        POINT clientPoint{ x, y };
        ScreenToClient(m_hWnd, &clientPoint);
        syncNavigation();
        if (applyNavigationEvent(navigation.mouseWheel({ clientPoint.x, clientPoint.y }, zDelta)))
            return;

        // EXIF 面板内的滚轮只滚面板内容，不穿透成画布缩放
        if (showExif &&
            static_cast<float>(clientPoint.x) >= exifPanelRect_.x &&
            static_cast<float>(clientPoint.x) < exifPanelRect_.x + exifPanelRect_.width &&
            static_cast<float>(clientPoint.y) >= exifPanelRect_.y &&
            static_cast<float>(clientPoint.y) < exifPanelRect_.y + exifPanelRect_.height) {
            exifPanelScroll_ -= zDelta / static_cast<float>(WHEEL_DELTA) * dp(66);
            exifPanelScroll_ = (std::clamp)(exifPanelScroll_, 0.0f, exifPanelMaxScroll_);
            markPresentRequested();
            return;
        }

        switch (cursorPos)
        {
        case CursorPos::centerArea:
            operateQueue.push({ zDelta < 0 ? ActionENUM::zoomOut : ActionENUM::zoomIn });
            break;

        case CursorPos::leftEdge:
        case CursorPos::rightEdge:
            operateQueue.push({ zDelta < 0 ? ActionENUM::nextImg : ActionENUM::preImg });
            break;

        case CursorPos::leftDown:
        case CursorPos::rightDown:
            break;

        case CursorPos::leftUp:
        case CursorPos::rightUp:
            operateQueue.push({ zDelta < 0 ? ActionENUM::rotateRight : ActionENUM::rotateLeft });
            break;
        }
    }

    void OnKeyDown(WPARAM keyValue) override {
        if (ctrlIsPressing) {
            switch (keyValue)
            {
            case 'O': { // Ctrl + O  打开图片
                wstring filePath = jarkUtils::SelectFile(m_hWnd);
                if (!filePath.empty()) {
                    initOpenFile(filePath);
                    operateQueue.push({ ActionENUM::refresh });
                }
                ctrlIsPressing = false; // 上面弹出窗口导致收不到CTRL键释放的消息
            }break;

            case 'B': { // Ctrl + B 批量处理
                operateQueue.push({ ActionENUM::batchProcess });
                ctrlIsPressing = false; // 上面弹出窗口导致收不到CTRL键释放的消息
            }break;

            case 'E': { // Ctrl + E 图像编辑与标注
                operateQueue.push({ ActionENUM::editImage });
                ctrlIsPressing = false; // 上面弹出窗口导致收不到CTRL键释放的消息
            }break;

            case 'S': { // Ctrl + S  动图或实况图视频 批量保存每一帧到png图片
                auto& frames = curPar.imageAssetPtr->frames;
                if (frames.empty())
                    break;

                if (IDYES == MessageBoxW(
                    m_hWnd,
                    std::format(L"{}{}", getUIStringW(5).c_str(), frames.size()).c_str(),
                    getUIStringW(6),
                    MB_YESNO | MB_ICONQUESTION
                )) {
                    std::thread saveThread([](std::wstring filePath, std::shared_ptr<ImageAsset> imageAssetPtr) {
                        auto& frames = imageAssetPtr->frames;
                        auto dotIdx = filePath.find_last_of(L".");
                        if (dotIdx == string::npos)
                            dotIdx = filePath.size();

                        for (int i = 0; i < frames.size(); i++) {
                            std::vector<uchar> buffer;
                            if (cv::imencode(".png", frames[i], buffer)) {
                                std::ofstream file(std::format(L"{}_{:04d}.png", filePath.substr(0, dotIdx), i + 1), std::ios::binary);
                                if (file.is_open()) {
                                    file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                                    file.close();
                                }
                            }
                        }
                        }, imgFileList[curFileIdx], curPar.imageAssetPtr);

                    saveThread.detach();
                }
                ctrlIsPressing = false; // 上面弹出窗口导致收不到CTRL键释放的消息
            }break;

            case 'C': { // Ctrl + C  复制到剪贴板
                cv::Mat srcImg = currentSourceImage();

                jarkUtils::copyImageToClipboard(srcImg);
                ctrlIsPressing = false;
            }break;

            case 'P': { // Ctrl + P 打印
                operateQueue.push({ ActionENUM::printImage });
                ctrlIsPressing = false;
            }break;

            case 'W': { // Ctrl + W 退出
                operateQueue.push({ ActionENUM::requestExit });
                ctrlIsPressing = false;
            }break;

            case 'R': { // Ctrl + R 重命名当前图片
                operateQueue.push({ ActionENUM::renameImage });
            }break;

            default: {
                // 没绑定的组合键：顺手清掉状态。界面窗口会吃掉 CTRL 释放消息，
                // 不清的话后续所有按键都会被当成“按住 Ctrl 的组合键”而失灵。
                ctrlIsPressing = false;
            }break;
            }
        }
        else {
            switch (keyValue)
            {

            case 'J': { // 上一帧
                if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause) {
                    curPar.curFrameIdx--;
                    if (curPar.curFrameIdx < 0)
                        curPar.curFrameIdx = curPar.curFrameIdxMax;
                    operateQueue.push({ ActionENUM::refresh });
                }
            }break;

            case 'K': { // 动图 暂停/继续
                if (curPar.imageAssetPtr->format == ImageFormat::Animated) {
                    curPar.isAnimationPause = !curPar.isAnimationPause;
                    operateQueue.push({ ActionENUM::refresh });
                }
            }break;

            case 'L': { // 下一帧
                if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause) {
                    curPar.curFrameIdx++;
                    if (curPar.curFrameIdx > curPar.curFrameIdxMax)
                        curPar.curFrameIdx = 0;
                    operateQueue.push({ ActionENUM::refresh });
                }
            }break;

            case VK_CONTROL: {
                ctrlIsPressing = true;
            }break;

            case 'C': { // 复制图像信息到剪贴板
                jarkUtils::copyToClipboard(jarkUtils::utf8ToWstring(curPar.imageAssetPtr->exifInfo));
            }break;

            case 'F':
            case VK_F11: {
                jarkUtils::ToggleFullScreen(m_hWnd);
            }break;

            case 'P': { // 幻灯片播放开关
                operateQueue.push({ ActionENUM::slideshow });
            }break;

            case 'Q': {
                operateQueue.push({ ActionENUM::rotateLeft });
            }break;

            case 'E': {
                operateQueue.push({ ActionENUM::rotateRight });
            }break;

            case 'W': {
                const int newTargetYMax = (int)(((curPar.rotation == 0 or curPar.rotation == 2) ?
                    curPar.height : curPar.width) * curPar.zoomTarget / 2 / curPar.ZOOM_BASE);
                int newTargetY = curPar.slideTarget.y + ((winHeight + winWidth) / 16);
                newTargetY = std::clamp(newTargetY, -newTargetYMax, newTargetYMax);
                curPar.slideTarget.y = newTargetY;
                smoothShift = true;
            }break;

            case 'S': {
                const int newTargetYMax = (int)(((curPar.rotation == 0 or curPar.rotation == 2) ?
                    curPar.height : curPar.width) * curPar.zoomTarget / 2 / curPar.ZOOM_BASE);
                int newTargetY = curPar.slideTarget.y - ((winHeight + winWidth) / 16);
                newTargetY = std::clamp(newTargetY, -newTargetYMax, newTargetYMax);
                curPar.slideTarget.y = newTargetY;
                smoothShift = true;
            }break;

            case 'A': {
                const int newTargetXMax = (int)(((curPar.rotation == 0 || curPar.rotation == 2) ?
                    curPar.width : curPar.height) * curPar.zoomTarget / 2 / curPar.ZOOM_BASE);
                int newTargetX = curPar.slideTarget.x + ((winHeight + winWidth) / 16);
                newTargetX = std::clamp(newTargetX, -newTargetXMax, newTargetXMax);
                curPar.slideTarget.x = newTargetX;
                smoothShift = true;
            }break;

            case 'D': {
                const int newTargetXMax = (int)(((curPar.rotation == 0 || curPar.rotation == 2) ?
                    curPar.width : curPar.height) * curPar.zoomTarget / 2 / curPar.ZOOM_BASE);
                int newTargetX = curPar.slideTarget.x - ((winHeight + winWidth) / 16);
                newTargetX = std::clamp(newTargetX, -newTargetXMax, newTargetXMax);
                curPar.slideTarget.x = newTargetX;
                smoothShift = true;
            }break;

            case VK_UP: {
                operateQueue.push({ ActionENUM::zoomIn });
            }break;

            case VK_DOWN: {
                operateQueue.push({ ActionENUM::zoomOut });
            }break;

            case '5':
            case VK_NUMPAD5: {
                operateQueue.push({ ActionENUM::zoomFix });
            }break;

            case VK_PRIOR:
            case VK_LEFT: {
                operateQueue.push({ ActionENUM::preImg });
            }break;

            case VK_NEXT:
            case VK_RIGHT: {
                operateQueue.push({ ActionENUM::nextImg });
            }break;

            case VK_HOME: {
                operateQueue.push({ ActionENUM::firstImg });
            }break;

            case VK_END: {
                operateQueue.push({ ActionENUM::finalImg });
            }break;

            case VK_SPACE: {
                if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
                    curPar.imageAssetPtr->format = ImageFormat::Animated;
                    curPar.Init(winWidth, winHeight);
                    operateQueue.push({ ActionENUM::refresh });
                }
                else if (curPar.imageAssetPtr->format == ImageFormat::Animated) {
                    curPar.isAnimationPause = !curPar.isAnimationPause;
                    operateQueue.push({ ActionENUM::refresh });
                }
                else if (currentIsLivePhoto_ && curPar.imageAssetPtr->videoSource &&
                    !curPar.imageAssetPtr->videoSource->data.empty()) {
                    // 实况照片：播放中按空格马上切回静态图；正在显示静态图则从头播放（主动操作，出声）
                    if (mediaPlayer) {
                        JARK_LOG("实况播放手动停止，切回静态图");
                        stopMediaPlayback();
                        playedAsset = curPar.imageAssetPtr.get(); // 手动停止后不再自动续播
                        liveReplaySound_ = false;
                        curPar.Init(winWidth, winHeight); // 恢复静态图的名义尺寸与适应缩放
                    }
                    else {
                        playedAsset = nullptr;
                        liveReplaySound_ = true;
                    }
                    operateQueue.push({ ActionENUM::refresh });
                }
                else {
                    operateQueue.push({ ActionENUM::nextImg });
                }
            }break;

            case VK_TAB:
            case 'I': {
                operateQueue.push({ ActionENUM::toggleExif });
            }break;

            case VK_F1: {
                operateQueue.push({ ActionENUM::setting, 0 });
            }break;

            case VK_F2: {
                operateQueue.push({ ActionENUM::setting, 1 });
            }break;

            case VK_F3: {
                operateQueue.push({ ActionENUM::setting, 2 });
            }break;

            case VK_F4: {
                operateQueue.push({ ActionENUM::setting, 3 });
            }break;

            case VK_ESCAPE: { // ESC：有窗口先关窗口（失焦时 ImGui 收不到 Esc），播放中先停止播放，否则退出
                if (closeTopWindow())
                    operateQueue.push({ ActionENUM::refresh });
                else if (slideshowActive)
                    operateQueue.push({ ActionENUM::slideshow });
                else
                    operateQueue.push({ ActionENUM::requestExit });
            }break;

            case VK_DELETE: { //DELETE
                operateQueue.push({ ActionENUM::deleteImg });
            }

            default: {
                JARK_LOG("{} KeyValue: 0x{:04x}", __FUNCTION__, (uint64_t)keyValue);
            }break;
            }
        }
    }

    void OnKeyUp(WPARAM keyValue) override {
        switch (keyValue)
        {
        case VK_CONTROL: {
            ctrlIsPressing = false;
        }break;

        default: {
            JARK_LOG("{} KeyValue: 0x{:04x}", __FUNCTION__, (uint64_t)keyValue);
        }break;
        }
    }

    void OnDropFiles(WPARAM wParam) override {
        wstring path;
        HDROP hDrop = (HDROP)wParam;
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

        if (0 < fileCount) { // 拖入多文件时，只接受第一个
            wchar_t filePath[4096] = {};
            DragQueryFileW(hDrop, 0, filePath, 4096);
            path = filePath;
        }
        DragFinish(hDrop);

        if (!path.empty()) {
            initOpenFile(path);
            operateQueue.push({ ActionENUM::refresh });
        }
    }

    void OnContextMenuCommand(WPARAM wParam) override {
        switch ((ContextMenu)wParam) {
        case ContextMenu::openNewImage: {
            wstring filePath = jarkUtils::SelectFile(m_hWnd);
            if (!filePath.empty()) {
                initOpenFile(filePath);
                operateQueue.push({ ActionENUM::refresh });
            }
        }break;

        case ContextMenu::copyImageInfo: {
            jarkUtils::copyToClipboard(jarkUtils::utf8ToWstring(curPar.imageAssetPtr->exifInfo));
        }break;

        case ContextMenu::copyImagePath: {
            jarkUtils::copyToClipboard(imgFileList[curFileIdx]);
        }break;

        case ContextMenu::copyImageData: {
            cv::Mat srcImg = currentSourceImage();
            jarkUtils::copyImageToClipboard(srcImg);
        }break;

        case ContextMenu::toggleExifDisplay: {
            operateQueue.push({ ActionENUM::toggleExif });
        }break;

        case ContextMenu::openContainerFloder: {
            jarkUtils::openFileLocation(imgFileList[curFileIdx]);
        }break;

        case ContextMenu::renameImage: {
            operateQueue.push({ ActionENUM::renameImage });
        }break;

        case ContextMenu::copyToTarget: {
            copyOrMoveCurrentImage(false);
        }break;

        case ContextMenu::moveToTarget: {
            copyOrMoveCurrentImage(true);
        }break;

        case ContextMenu::chooseTargetDir: {
            const std::wstring chosen = jarkUtils::SelectFolder(m_hWnd);
            if (!chosen.empty())
                wcscpy_s(GlobalVar::settingParameter.copyTargetDir, chosen.c_str());
        }break;

        case ContextMenu::openWithEditor: {
            openWithExternalEditor();
        }break;

        case ContextMenu::chooseEditor: {
            const std::wstring chosen = jarkUtils::SelectFile(m_hWnd);
            if (!chosen.empty())
                wcscpy_s(GlobalVar::settingParameter.externalEditor, chosen.c_str());
        }break;

        case ContextMenu::deleteImage: {
            operateQueue.push({ ActionENUM::deleteImg });
        }break;

        case ContextMenu::openFileProperties: {
            jarkUtils::openFileProperties(imgFileList[curFileIdx]);
        }break;

        case ContextMenu::printImage: {
            operateQueue.push({ ActionENUM::printImage });
        }break;

        case ContextMenu::batchProcess: {
            operateQueue.push({ ActionENUM::batchProcess });
        }break;

        case ContextMenu::editImage: {
            operateQueue.push({ ActionENUM::editImage });
        }break;

        case ContextMenu::slideshow: {
            operateQueue.push({ ActionENUM::slideshow });
        }break;

        case ContextMenu::toggleFullScreen: {
            jarkUtils::ToggleFullScreen(m_hWnd);
        }break;

        case ContextMenu::openSetting: {
            operateQueue.push({ ActionENUM::setting, 0 });
        }break;

        case ContextMenu::openHelp: {
            operateQueue.push({ ActionENUM::setting, 2 });
        }break;

        case ContextMenu::aboutSoftware: {
            operateQueue.push({ ActionENUM::setting, 3 });
        }break;

        case ContextMenu::exitSoftware: {
            operateQueue.push({ ActionENUM::requestExit });
        }break;

        default:
            break;
        }
    }

    void OnResize(UINT width, UINT height) override {
        ++navigationImageVersion;
        navigation.cancel();
        if (width == 0 || height == 0)
            return;

        if (winWidth == width && winHeight == height)
            return;

        winWidth = width;
        winHeight = height;

        if (winWidth != mainCanvas.cols || winHeight != mainCanvas.rows) {
            mainCanvas = cv::Mat(winHeight, winWidth, CV_8UC4);
            CreateWindowSizeDependentResources();
        }

        if (hasInitWinSize) {
            curPar.updateZoomList(winWidth, winHeight);

            cv::Mat srcImg = currentSourceImage();

            drawCanvas(srcImg, mainCanvas);
        }
        else {
            hasInitWinSize = true;
            curPar.Init(winWidth, winHeight);

            uint32_t* ptrStart = (uint32_t*)mainCanvas.ptr();
            uint32_t* ptrEnd = ptrStart + winHeight * winWidth;
            std::fill(ptrStart, ptrEnd, GlobalVar::currentTheme.BG);
        }

        updateMainCanvas();
        operateQueue.push({ ActionENUM::refresh });
    }

    jark::ViewState viewState() const {
        jark::ViewState view;
        view.imageWidth = curPar.width;
        view.imageHeight = curPar.height;
        view.zoom = curPar.zoomCur;
        view.zoomBase = curPar.ZOOM_BASE;
        view.slideX = curPar.slideCur.x;
        view.slideY = curPar.slideCur.y;
        view.rotation = curPar.rotation;
        // 主页/解码失败是界面画面，不画"图像边框"
        view.border = !(curPar.imageAssetPtr && curPar.imageAssetPtr->placeholder != PlaceholderKind::None);
        return view;
    }

    void drawCanvas(const cv::Mat& srcImg, cv::Mat& canvas) const {
        jark::drawImageToCanvas(srcImg, canvas, viewState());
    }


    cv::Mat rotateImage(const cv::Mat& image, double angle) {
        int width = image.cols;
        int height = image.rows;
        cv::Point2f center(width / 2.0f, height / 2.0f);

        cv::Mat rotationMatrix = cv::getRotationMatrix2D(center, angle, 1.0);

        cv::Mat rotatedImage;
        cv::warpAffine(image, rotatedImage, rotationMatrix, image.size(),
            cv::INTER_LINEAR, cv::BORDER_CONSTANT, 
            cv::Scalar(GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG));

        return rotatedImage;
    }

    // 取对角线作为新画布的宽高，绘制好内容再旋转，最后截取画布。
    // 若直接使用原尺寸画布进行旋转，宽或高其中较小的那边旋转到较长那边时，会缺失部分内容
    void rotateLeftAnimation() {
        using namespace std::chrono;

        int maxEdge = (int)std::ceil(std::sqrt(winWidth * winWidth + winHeight * winHeight));
        if (maxEdge < 2)
            return;
        auto tmpCanvas = cv::Mat(maxEdge, maxEdge, CV_8UC4, 
            cv::Vec4b(GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG));

        cv::Mat srcImg = currentSourceImage();

        drawCanvas(srcImg, tmpCanvas);
        cv::resize(tmpCanvas, tmpCanvas, cv::Size(tmpCanvas.cols / 2, tmpCanvas.cols / 2));

        for (int i = 0; i <= 90; i += ((100 - i) / 6)) {
            auto start_clock = steady_clock::now();
            auto view = rotateImage(tmpCanvas, i)(cv::Rect((maxEdge - winWidth) / 4, (maxEdge - winHeight) / 4, winWidth / 2, winHeight / 2));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    void rotateRightAnimation() {
        using namespace std::chrono;

        int maxEdge = (int)std::ceil(std::sqrt(winWidth * winWidth + winHeight * winHeight));
        if (maxEdge < 2)
            return;
        auto tmpCanvas = cv::Mat(maxEdge, maxEdge, CV_8UC4, 
            cv::Vec4b(GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG, GlobalVar::currentTheme.BG));

        cv::Mat srcImg = currentSourceImage();

        drawCanvas(srcImg, tmpCanvas);
        cv::resize(tmpCanvas, tmpCanvas, cv::Size(tmpCanvas.cols / 2, tmpCanvas.cols / 2));

        for (int i = 0; i >= -90; i -= ((100 + i) / 6)) {
            auto start_clock = steady_clock::now();
            auto view = rotateImage(tmpCanvas, i)(cv::Rect((maxEdge - winWidth) / 4, (maxEdge - winHeight) / 4, winWidth / 2, winHeight / 2));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    // 添加水平运动模糊
    void applyHorizontalMotionBlur(cv::Mat& src, cv::Mat& dst, int kernelSize = 15, int direction = 1) {
        // 创建水平运动模糊核 (1行 x kernelSize列)
        cv::Mat kernel = cv::Mat::zeros(1, kernelSize, CV_32F);

        // 设置方向 (1.0 = 右移模糊, -1.0 = 左移模糊)
        int start = (direction > 0) ? 0 : kernelSize - 1;
        int end = (direction > 0) ? kernelSize : -1;
        int step = (direction > 0) ? 1 : -1;

        // 填充核 (线性衰减效果)
        float sum = 0.0f;
        for (int i = start; i != end; i += step) {
            float weight = 1.0f - std::abs(static_cast<float>(i - start) / (kernelSize - 1));
            kernel.at<float>(0, i) = weight;
            sum += weight;
        }
        // 归一化核
        kernel /= sum;
        // 应用水平卷积 (仅水平方向)
        cv::filter2D(src, dst, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
    }

    // 添加竖直运动模糊
    void applyVerticalMotionBlur(cv::Mat& src, cv::Mat& dst, int kernelSize = 15, float direction = 1.0f) {
        // 创建水平运动模糊核 (1行 x kernelSize列)
        cv::Mat kernel = cv::Mat::zeros(kernelSize, 1, CV_32F);

        // 设置方向 (1.0 = 下移模糊, -1.0 = 上移模糊)
        int start = (direction > 0) ? 0 : kernelSize - 1;
        int end = (direction > 0) ? kernelSize : -1;
        int step = (direction > 0) ? 1 : -1;

        // 填充核 (线性衰减效果)
        float sum = 0.0f;
        for (int i = start; i != end; i += step) {
            float weight = 1.0f - std::abs(static_cast<float>(i - start) / (kernelSize - 1));
            kernel.at<float>(i, 0) = weight;
            sum += weight;
        }
        // 归一化核
        kernel /= sum;
        // 应用水平卷积 (仅水平方向)
        cv::filter2D(src, dst, -1, kernel, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
    }

    // 水平滑动 上一张图
    void mainCanvasSlideToPreAnimationHorizontal() {
        using namespace std::chrono;

        cv::Mat srcImg = currentSourceImage();

        auto nextmainCanvas = cv::Mat(mainCanvas.size(), mainCanvas.type());
        drawCanvas(srcImg, nextmainCanvas);

        cv::Mat smallMainCanvas;
        cv::resize(mainCanvas, smallMainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);
        cv::resize(nextmainCanvas, nextmainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);

        cv::Mat panorama(nextmainCanvas.rows, nextmainCanvas.cols * 2, nextmainCanvas.type());
        nextmainCanvas.copyTo(panorama(cv::Rect(0, 0, smallMainCanvas.cols, smallMainCanvas.rows)));
        smallMainCanvas.copyTo(panorama(cv::Rect(smallMainCanvas.cols, 0, nextmainCanvas.cols, nextmainCanvas.rows)));

        cv::Mat blurred;
        applyHorizontalMotionBlur(panorama, blurred, 15, 1);
        panorama = blurred;

        const int frame_width = nextmainCanvas.cols;
        const int frame_height = nextmainCanvas.rows;
        for (int x = frame_width; x > 0; x -= (int)((frame_width * 1.5 - x) / 8)) {
            auto start_clock = steady_clock::now();

            cv::Mat view = panorama(cv::Rect(x, 0, frame_width, frame_height));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    // 水平滑动 下一张图
    void mainCanvasSlideToNextAnimationHorizontal() {
        using namespace std::chrono;

        cv::Mat srcImg = currentSourceImage();

        auto nextmainCanvas = cv::Mat(mainCanvas.size(), mainCanvas.type());
        drawCanvas(srcImg, nextmainCanvas);

        cv::Mat smallMainCanvas;
        cv::resize(mainCanvas, smallMainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);
        cv::resize(nextmainCanvas, nextmainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);

        cv::Mat panorama(nextmainCanvas.rows, nextmainCanvas.cols * 2, nextmainCanvas.type());
        smallMainCanvas.copyTo(panorama(cv::Rect(0, 0, smallMainCanvas.cols, smallMainCanvas.rows)));
        nextmainCanvas.copyTo(panorama(cv::Rect(smallMainCanvas.cols, 0, nextmainCanvas.cols, nextmainCanvas.rows)));

        cv::Mat blurred;
        applyHorizontalMotionBlur(panorama, blurred, 15, -1);
        panorama = blurred;

        const int frame_width = nextmainCanvas.cols;
        const int frame_height = nextmainCanvas.rows;
        for (int x = 0; x <= frame_width; x += (int)((frame_width*1.5 - x) / 8)) {
            auto start_clock = steady_clock::now();

            cv::Mat view = panorama(cv::Rect(x, 0, frame_width, frame_height));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    // 竖直滑动 上一张图
    void mainCanvasSlideToPreAnimationVertical() {
        using namespace std::chrono;

        cv::Mat srcImg = currentSourceImage();

        auto nextmainCanvas = cv::Mat(mainCanvas.size(), mainCanvas.type());
        drawCanvas(srcImg, nextmainCanvas);

        cv::Mat smallMainCanvas;
        cv::resize(mainCanvas, smallMainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);
        cv::resize(nextmainCanvas, nextmainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);

        cv::Mat panorama(nextmainCanvas.rows * 2, nextmainCanvas.cols, nextmainCanvas.type());
        nextmainCanvas.copyTo(panorama(cv::Rect(0, 0, smallMainCanvas.cols, smallMainCanvas.rows)));
        smallMainCanvas.copyTo(panorama(cv::Rect(0, smallMainCanvas.rows, nextmainCanvas.cols, nextmainCanvas.rows)));

        cv::Mat blurred;
        applyVerticalMotionBlur(panorama, blurred, 15, 1);
        panorama = blurred;

        const int frame_width = nextmainCanvas.cols;
        const int frame_height = nextmainCanvas.rows;
        for (int y = frame_height; y >= 0; y -= (int)((frame_height * 1.5 - y) / 8)) {
            auto start_clock = steady_clock::now();

            cv::Mat view = panorama(cv::Rect(0, y, frame_width, frame_height));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }
    
    // 竖直滑动 下一张图
    void mainCanvasSlideToNextAnimationVertical() {
        using namespace std::chrono;

        cv::Mat srcImg = currentSourceImage();

        auto nextmainCanvas = cv::Mat(mainCanvas.size(), mainCanvas.type());
        drawCanvas(srcImg, nextmainCanvas);

        cv::Mat smallMainCanvas;
        cv::resize(mainCanvas, smallMainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);
        cv::resize(nextmainCanvas, nextmainCanvas, cv::Size(winWidth / 4, winHeight / 4), 0, 0, cv::INTER_NEAREST);

        cv::Mat panorama(nextmainCanvas.rows*2, nextmainCanvas.cols, nextmainCanvas.type());
        smallMainCanvas.copyTo(panorama(cv::Rect(0, 0, smallMainCanvas.cols, smallMainCanvas.rows)));
        nextmainCanvas.copyTo(panorama(cv::Rect(0, smallMainCanvas.rows, nextmainCanvas.cols, nextmainCanvas.rows)));

        cv::Mat blurred;
        applyVerticalMotionBlur(panorama, blurred, 15, -1);
        panorama = blurred;

        const int frame_width = nextmainCanvas.cols;
        const int frame_height = nextmainCanvas.rows;
        for (int y = 0; y <= frame_height; y += (int)((frame_height * 1.5 - y) / 8)) {
            auto start_clock = steady_clock::now();

            cv::Mat view = panorama(cv::Rect(0, y, frame_width, frame_height));
            cv::resize(view, mainCanvas, mainCanvas.size(), 0, 0, cv::INTER_NEAREST);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    void updateMainCanvas() {
        PresentCanvas(mainCanvas.ptr(), mainCanvas.cols, mainCanvas.rows, (int)mainCanvas.step);
        PresentFrame();
    }


    int64_t delayRemain = 0; // 当前帧剩余时长（微秒）
    const std::chrono::milliseconds frameDuration{ 10 };
    std::chrono::steady_clock::time_point lastTimestamp = std::chrono::steady_clock::now();
    bool animClockArmed = false; // 动画计时是否已启动：起播/恢复播放时重新对齐计时起点，避免把加载或暂停的时长算进第一帧


    // 标题栏：[序号/总数] 文件名 宽x高 (大小) 缩放% 旋转状态。
    // 文件名放最前（任务栏与 Alt+Tab 里一眼可见），全路径太长只会把有用信息挤出可视区；
    // 序号在总数到两位时补零，比例字体下不补零会随数字宽度左右跳动；
    // 动图逐帧浏览时把序号换成帧号；幻灯片播放时前面加播放标记。
    void updateWindowCaption() {
        if (curFileIdx < 0 || curFileIdx >= (int)imgFileList.size() || !curPar.imageAssetPtr)
            return;

        const std::wstring& path = imgFileList[curFileIdx];
        // 文件大小只在切图后查一次（标题每帧都重建，磁盘查询不能跟着每帧走）
        if (path != captionPathCache_) {
            captionPathCache_ = path;
            std::error_code errorCode;
            const auto bytes = std::filesystem::file_size(path, errorCode);
            captionBytes_ = errorCode ? 0 : bytes;
        }

        std::wstring counter;
        if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause) {
            counter = std::format(L"{} [{}/{}]",
                getUIStringW(9).c_str(),
                curPar.curFrameIdx + 1, curPar.curFrameIdxMax + 1);
        }
        else if (imgFileList.size() >= 10) {
            counter = std::format(L"[{:02}/{:02}]", curFileIdx + 1, imgFileList.size());
        }
        else {
            counter = std::format(L"[{}/{}]", curFileIdx + 1, imgFileList.size());
        }

        std::wstring str = std::format(L"{} {} {}x{}",
            counter,
            std::filesystem::path(path).filename().wstring(),
            curPar.width, curPar.height);

        const std::wstring sizeText = formatFileSize(captionBytes_);
        if (!sizeText.empty())
            str += std::format(L" ({})", sizeText);

        str += std::format(L" {}%", curPar.zoomCur * 100ULL / curPar.ZOOM_BASE);

        if (curPar.rotation) {
            str += L" ";
            str += (curPar.rotation == 1 ? getUIStringW(10) : (curPar.rotation == 3 ? getUIStringW(11) : getUIStringW(12)));
        }

        if (slideshowActive)
            str = L"▶ " + str;

        SetWindowTextW(m_hWnd, str.c_str());
    }

    // 标题栏文件大小缓存：路径没变就不重复查磁盘
    std::wstring captionPathCache_;
    uintmax_t captionBytes_ = 0;
    bool firstSceneDrawn_ = false; // 启动分段计时：首帧绘制时刻只记一次

    // —— 渐进加载：当前图的解码不阻塞主循环，先给预览或旧图，就绪后再无缝换清晰图 ——
    bool pendingLoad_ = false;                // 当前图正在后台解码
    std::wstring pendingLoadPath_;
    std::chrono::steady_clock::time_point pendingLoadStart_{};
    bool allowPreviewSwap_ = false;           // 等待期间是否允许缩略图预览顶替当前画面
    uint64_t previewVersion_ = 0;             // 已顶上画面的缩略图版本（防重复换）

    // 文件大小的短文本（B/KB/MB/GB），0 表示取不到、不显示
    static std::wstring formatFileSize(uintmax_t bytes) {
        if (bytes == 0)
            return {};
        if (bytes < 1024)
            return std::format(L"{}B", bytes);
        if (bytes < 1024ull * 1024)
            return std::format(L"{:.1f}KB", bytes / 1024.0);
        if (bytes < 1024ull * 1024 * 1024)
            return std::format(L"{:.1f}MB", bytes / (1024.0 * 1024.0));
        return std::format(L"{:.2f}GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }

    // —— 幻灯片播放 ——

    bool slideshowActive = false;
    std::chrono::steady_clock::time_point slideshowNextAdvance{};

    // 幻灯片自己开的窗口全屏（进来之前就全屏的话，退出播放时不要还原）
    bool slideshowOwnsFullScreen = false;

    void toggleSlideshow() {
        slideshowActive = !slideshowActive;
        if (slideshowActive) {
            slideshowNextAdvance = std::chrono::steady_clock::now() +
                std::chrono::seconds(std::clamp<uint32_t>(GlobalVar::settingParameter.pptTimeout, 1, 300));

            // 幻灯片全屏播放
            slideshowOwnsFullScreen = !jarkUtils::IsFullScreen();
            if (slideshowOwnsFullScreen)
                jarkUtils::SetFullScreen(m_hWnd, true);

            JARK_LOG("幻灯片播放开始：顺序={} 间隔={}s", GlobalVar::settingParameter.pptOrder,
                GlobalVar::settingParameter.pptTimeout);
        }
        else {
            if (slideshowOwnsFullScreen) {
                jarkUtils::SetFullScreen(m_hWnd, false);
                slideshowOwnsFullScreen = false;
            }
            JARK_LOG("幻灯片播放结束");
        }
        updateWindowCaption();
    }

    // 播放结束（到点发现只有一张图时也要还原全屏）
    void stopSlideshow() {
        if (slideshowActive)
            toggleSlideshow();
    }

    // 到点后切换到下一张（顺序/逆序/随机由设置决定）
    void updateSlideshow() {
        if (!slideshowActive)
            return;

        if (imgFileList.size() <= 1) {
            stopSlideshow();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < slideshowNextAdvance)
            return;

        slideshowNextAdvance = now +
            std::chrono::seconds(std::clamp<uint32_t>(GlobalVar::settingParameter.pptTimeout, 1, 300));

        int direction = 1;
        switch (std::clamp<uint32_t>(GlobalVar::settingParameter.pptOrder, 0, 2)) {
        case 1: // 逆序
            direction = -1;
            if (curFileIdx <= 0) {
                if (GlobalVar::settingParameter.stopAtListEnd) {
                    stopSlideshow(); // 已放完第一张：结束播放，不绕到末尾
                    return;
                }
                curFileIdx = (int)imgFileList.size() - 1;
            }
            else {
                --curFileIdx;
            }
            break;

        case 2: { // 随机（不重复当前）
            static std::mt19937 generator(std::random_device{}());
            std::uniform_int_distribution<int> distribution(0, (int)imgFileList.size() - 2);
            const int candidate = distribution(generator);
            const int next = candidate >= curFileIdx ? candidate + 1 : candidate;
            direction = next > curFileIdx ? 1 : -1;
            curFileIdx = next;
        } break;

        default: // 顺序
            if (curFileIdx >= (int)imgFileList.size() - 1) {
                if (GlobalVar::settingParameter.stopAtListEnd) {
                    stopSlideshow(); // 已放完最后一张：结束播放，不绕回开头
                    return;
                }
                curFileIdx = 0;
            }
            else {
                ++curFileIdx;
            }
            break;
        }

        switchToFile(curFileIdx, direction);
    }

    // 切换当前图片到指定索引，并把画面按方向滑动切过去（direction: +1 下一张 / -1 上一张 / 0 直接切）
    void switchToFile(int newIndex, int direction) {
        if (imgFileList.size() <= 1 || newIndex < 0 || newIndex >= (int)imgFileList.size())
            return;

        if (direction != 0 && GlobalVar::settingParameter.switchImageAnimationMode) { // 直接选图不准备动画画布
            cv::Mat srcImg = currentSourceImage();

            drawCanvas(srcImg, mainCanvas); //先更新无额外按钮UI的原图
        }

        // 播放过的实况图，状态会变成静态图，切走前恢复一下
        if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
            curPar.imageAssetPtr->format = ImageFormat::Animated;
        }

        stopMediaPlayback(); // 本次绘制就必须丢弃旧视频帧，不能等下一个主循环
        ++navigationImageVersion;
        curFileIdx = newIndex;

        const size_t nextIndex = direction > 0
            ? (curFileIdx + 1) % imgFileList.size()
            : (curFileIdx + imgFileList.size() - 1) % imgFileList.size();
        auto loadedAsset = requestCurrentImage(imgFileList[nextIndex]);
        if (loadedAsset)
            curPar.imageAssetPtr = std::move(loadedAsset);

        updatePlaceholderImage();
        curPar.Init(winWidth, winHeight);

        // 未就绪时不播切图动画（保留旧图停留，等 adoptCurrentImage 换入新图）
        const int animationMode = (!loadedAsset || direction == 0) ? 0 : GlobalVar::settingParameter.switchImageAnimationMode;
        if (animationMode == 1)
            direction > 0 ? mainCanvasSlideToNextAnimationVertical() : mainCanvasSlideToPreAnimationVertical();
        else if (animationMode == 2)
            direction > 0 ? mainCanvasSlideToNextAnimationHorizontal() : mainCanvasSlideToPreAnimationHorizontal();

        lastTimestamp = std::chrono::steady_clock::now();
        delayRemain = 0;
        animClockArmed = false; // 新图重新对齐动画计时（下一帧起第一帧获得完整延迟）

        // 必须主动要求重画：切换可能在空闲状态发生（画面已稳定），否则主循环继续走空闲分支，
        // 屏幕停在上一张上——幻灯片播放到动图之后“就不再换图”就是这个原因
        operateQueue.push({ ActionENUM::refresh });
    }

    // 当前应显示的图像：视频播放中优先用播放帧，否则取动图当前帧/静态图
    cv::Mat currentSourceImage() const {
        if (!curPar.imageAssetPtr)
            return {};

        if (!playbackFrame.empty())
            return playbackFrame;

        if (curPar.imageAssetPtr->format == ImageFormat::Animated && !curPar.imageAssetPtr->frames.empty())
            return curPar.imageAssetPtr->frames[curPar.curFrameIdx];

        return curPar.imageAssetPtr->primaryFrame;
    }

    // 实况照片/视频：需要时启动实时播放（含声音），一次播完后回到静态图
    void updateMediaPlayback() {
        if (!curPar.imageAssetPtr)
            return;

        const ImageAsset* assetPtr = curPar.imageAssetPtr.get();

        // 切换图片：重置播放状态，使重新进入本图时可以再播一次
        if (assetPtr != lastSeenAsset || curFileIdx != lastSeenFileIndex) {
            stopMediaPlayback();
            playedAsset = nullptr;
            lastSeenAsset = assetPtr;
            lastSeenFileIndex = curFileIdx;
            liveBadgeHovered_ = false;

            // 「实况」角标只给实况照片；直接打开的视频文件不带角标、也不受静音设置影响
            currentIsLivePhoto_ = false;
            if (assetPtr->videoSource && !assetPtr->videoSource->data.empty() &&
                curFileIdx >= 0 && curFileIdx < static_cast<int>(imgFileList.size())) {
                std::wstring ext = std::filesystem::path(imgFileList[curFileIdx]).extension().wstring();
                if (!ext.empty())
                    ext = ext.substr(1);
                for (auto& c : ext)
                    c = static_cast<wchar_t>(std::towlower(c));
                currentIsLivePhoto_ = !ImageDatabase::videoExt.contains(ext);
            }
        }

        const auto& asset = *curPar.imageAssetPtr;
        if (!asset.videoSource || asset.videoSource->data.empty())
            return;

        if (mediaPlayer) {
            if (mediaPlayer->hasFinished()) {
                JARK_LOG("视频播放结束");
                stopMediaPlayback();
                playedAsset = assetPtr;              // 本图只自动播放一次
                curPar.Init(winWidth, winHeight);    // 恢复静态图的名义尺寸与适应缩放
                operateQueue.push({ ActionENUM::refresh });
                return;
            }

            cv::Mat frame;
            if (mediaPlayer->acquireFrame(frame)) {
                playbackFrame = std::move(frame);
                operateQueue.push({ ActionENUM::refresh });
            }
            return;
        }

        if (playedAsset == assetPtr)
            return; // 已播放过（切换图片时会重置）

        auto player = jark::MediaPlayer::create();
        // 音量：视频文件是用户主动打开的、照常出声；实况照片按设置（默认静音）；
        // 悬停「实况」角标触发的重播一律出声
        const float volume = liveReplaySound_ ? 1.0f
            : (currentIsLivePhoto_ && !GlobalVar::settingParameter.livePhotoAutoPlaySound ? 0.0f : 1.0f);
        if (!player || !player->start(asset.videoSource->data, volume)) {
            JARK_LOG("视频播放启动失败，保持静态图");
            playedAsset = assetPtr;
            liveReplaySound_ = false;
            return;
        }

        JARK_LOG("视频播放开始，音频={} 音量={:.1f}", player->hasAudio(), volume);
        mediaPlayer = std::move(player);
        liveReplaySound_ = false;

        // 播放期间以视频尺寸作为名义尺寸（实况照片的静态图与视频尺寸可能不同）
        int videoWidth = 0;
        int videoHeight = 0;
        if (mediaPlayer->getVideoSize(videoWidth, videoHeight) && videoWidth > 0 && videoHeight > 0 &&
            (videoWidth != curPar.width || videoHeight != curPar.height)) {
            curPar.applyViewForSize(videoWidth, videoHeight, winWidth, winHeight);
        }
    }

    void stopMediaPlayback() {
        if (mediaPlayer) {
            mediaPlayer->stop();
            mediaPlayer.reset();
        }
        playbackFrame = cv::Mat();
    }

    // 请求加载当前图（非阻塞）：命中缓存立即返回；否则进入等待状态并返回空，
    // 主循环由 updatePendingLoad() 每帧轮询收尾（超过 60 秒退回一次阻塞等待兜底）。
    std::shared_ptr<ImageAsset> requestCurrentImage(const std::wstring& nextPath) {
        const std::wstring& path = imgFileList[curFileIdx];
        imgDB.requestPreloadBatch({ path, nextPath });
        if (auto ptr = imgDB.tryGetPtr(path)) {
            pendingLoad_ = false;
            return ptr;
        }
        beginPendingLoad(path);
        return nullptr;
    }

    void beginPendingLoad(const std::wstring& path) {
        pendingLoad_ = true;
        pendingLoadPath_ = path;
        pendingLoadStart_ = std::chrono::steady_clock::now();
        previewVersion_ = 0;

        // 当前没有真图可看（启动/主页/占位）时：主页画面垫底，缩略图就绪后顶上当模糊预览；
        // 切图时保留旧图更平滑，不给预览
        allowPreviewSwap_ = !(curPar.imageAssetPtr &&
            curPar.imageAssetPtr->placeholder == PlaceholderKind::None &&
            curPar.imageAssetPtr->format != ImageFormat::None);
        if (allowPreviewSwap_)
            curPar.imageAssetPtr = std::make_shared<ImageAsset>(
                placeholderAsset(PlaceholderKind::Home, {}, getUIString(32)));

        jark::ThumbnailService::instance().updateRequests({ path });
    }

    void updatePendingLoad() {
        // 解码完成（失败也会以空帧落缓存，交给占位逻辑显示失败原因）
        if (auto asset = imgDB.tryGetPtr(pendingLoadPath_)) {
            adoptCurrentImage(std::move(asset));
            return;
        }

        if (std::chrono::steady_clock::now() - pendingLoadStart_ > std::chrono::seconds(60)) {
            adoptCurrentImage(imgDB.getSafePtr(pendingLoadPath_, pendingLoadPath_));
            return;
        }

        // 缩略图先到：仅在没有真图可看时顶上（浅拷贝共享缩略图服务的只读像素）
        if (allowPreviewSwap_) {
            const auto thumbnail = jark::ThumbnailService::instance().get(pendingLoadPath_);
            if (!thumbnail.image.empty() && thumbnail.version != previewVersion_) {
                previewVersion_ = thumbnail.version;
                ImageAsset previewAsset{ ImageFormat::Still, thumbnail.image };
                curPar.imageAssetPtr = std::make_shared<ImageAsset>(std::move(previewAsset));
                updatePlaceholderImage();
                curPar.Init(winWidth, winHeight);
                operateQueue.push({ ActionENUM::refresh });
                JARK_LOG("progressive: 缩略图预览已顶上（原图仍在解码）");
            }
        }
    }

    // 挂起的加载完成：接管真图并做与同步路径相同的收尾
    void adoptCurrentImage(std::shared_ptr<ImageAsset> asset) {
        pendingLoad_ = false;
        pendingLoadPath_.clear();
        previewVersion_ = 0;
        allowPreviewSwap_ = false;
        curPar.imageAssetPtr = std::move(asset);
        updatePlaceholderImage();
        curPar.Init(winWidth, winHeight);
        operateQueue.push({ ActionENUM::refresh });
        jarkUtils::startupTraceMark("image adopted"); // 每次切图/加载完成都留点，便于对照首帧时刻
        JARK_LOG("progressive: 原图接管");
    }

    // 重命名确认后执行：改磁盘、同步列表（重排后的新位置）、作废缓存并重新装载
    void applyRename(const std::wstring& newPath) {
        if (curFileIdx < 0 || curFileIdx >= (int)imgFileList.size())
            return;

        const std::wstring oldPath = imgFileList[curFileIdx];
        stopMediaPlayback();

        std::error_code errorCode;
        std::filesystem::rename(oldPath, newPath, errorCode);
        if (errorCode) {
            auto errMsg = std::format(L"{} 0x{:08X}", getUIStringW(51).c_str(),
                static_cast<unsigned>(errorCode.value()));
            MessageBoxW(m_hWnd, errMsg.c_str(), getUIStringW(1), MB_OK | MB_ICONERROR);
            return;
        }

        jark::ThumbnailService::instance().invalidate(oldPath);
        imgFileList[curFileIdx] = newPath;

        // 文件名变了，按当前排序设置把这张挪到新位置（与 initOpenFile 同一套规则）
        jarkUtils::sortImageFileList(imgFileList, GlobalVar::settingParameter.sortMode, curFileIdx);

        imgDB.clear();            // 旧路径的缓存（连同相邻预读）一起作废，重新装载
        ++navigationImageVersion; // 导航里按旧坐标的换图请求作废
        if (auto asset = requestCurrentImage(imgFileList[(curFileIdx + 1) % imgFileList.size()])) {
            curPar.imageAssetPtr = std::move(asset);
            updatePlaceholderImage();
            curPar.Init(winWidth, winHeight);
        }
        updateNavigationDirectory();
        operateQueue.push({ ActionENUM::refresh });
    }

    // 复制/移动当前图片到目标文件夹（首次使用时选择并记住）：目标不存在自动创建，
    // 重名按资源管理器习惯让到 "名 (2).ext"，绝不覆盖已有文件；移动成功后与删除一样
    // 从列表摘掉当前项并显示下一张。
    void copyOrMoveCurrentImage(bool move) {
        if (curFileIdx < 0 || curFileIdx >= (int)imgFileList.size() ||
            imgFileList[curFileIdx] == m_wndCaption)
            return;

        if (GlobalVar::settingParameter.copyTargetDir[0] == 0) {
            const std::wstring chosen = jarkUtils::SelectFolder(m_hWnd);
            if (chosen.empty())
                return;
            wcscpy_s(GlobalVar::settingParameter.copyTargetDir, chosen.c_str());
        }

        const std::wstring target = GlobalVar::settingParameter.copyTargetDir;
        const std::filesystem::path source(imgFileList[curFileIdx]);
        std::error_code errorCode;

        const auto showError = [&]() {
            auto errMsg = std::format(L"{} 0x{:08X}", getUIStringW(move ? 56 : 55).c_str(),
                static_cast<unsigned>(errorCode.value()));
            MessageBoxW(m_hWnd, errMsg.c_str(), getUIStringW(1), MB_OK | MB_ICONERROR);
        };

        std::filesystem::create_directories(target, errorCode); // 目标不存在自动创建（含多层）
        if (errorCode) {
            showError();
            return;
        }

        // 重名让路：a.png -> "a (2).png"
        std::filesystem::path destination = std::filesystem::path(target) / source.filename();
        for (int index = 2; std::filesystem::exists(destination, errorCode) && index < 10000; ++index) {
            destination = std::filesystem::path(target) / (source.stem().wstring() +
                std::format(L" ({}).", index) + source.extension().wstring().substr(1));
        }

        if (move) {
            std::filesystem::rename(source, destination, errorCode);
            if (errorCode) {
                // 跨盘移动 rename 会失败，退化为复制 + 删除
                errorCode.clear();
                std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, errorCode);
                if (!errorCode)
                    std::filesystem::remove(source, errorCode);
            }
        }
        else {
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, errorCode);
        }

        if (errorCode) {
            showError();
            return;
        }

        JARK_LOG("{}: {} -> {}", move ? "move" : "copy",
            jarkUtils::wstringToUtf8(source.wstring()), jarkUtils::wstringToUtf8(destination.wstring()));

        if (move) {
            // 与删除相同的收尾：从列表摘掉当前项、显示下一张
            jark::ThumbnailService::instance().invalidate(source.wstring());
            stopMediaPlayback();
            imgFileList.erase(imgFileList.begin() + curFileIdx);

            if (imgFileList.empty()) {
                imgFileList.emplace_back(m_wndCaption);
                curFileIdx = 0;
                imgDB.put(m_wndCaption, placeholderAsset(PlaceholderKind::Home, {}, getUIString(32)));
            }
            else if (curFileIdx >= (int)imgFileList.size()) {
                curFileIdx = (int)imgFileList.size() - 1;
            }

            if (auto asset = requestCurrentImage(imgFileList[(curFileIdx + 1) % imgFileList.size()])) {
                curPar.imageAssetPtr = std::move(asset);
                updatePlaceholderImage();
                curPar.Init(winWidth, winHeight);
            }
            updateNavigationDirectory();
            operateQueue.push({ ActionENUM::refresh });
        }
    }

    // 用外部编辑器打开当前图片；未设置过编辑器时先选择并记住。图片路径整体加引号
    // 传给编辑器（带空格的路径是常态），编辑器路径不经过命令行拆分。
    void openWithExternalEditor() {
        if (curFileIdx < 0 || curFileIdx >= (int)imgFileList.size() ||
            imgFileList[curFileIdx] == m_wndCaption)
            return;

        if (GlobalVar::settingParameter.externalEditor[0] == 0) {
            const std::wstring chosen = jarkUtils::SelectFile(m_hWnd);
            if (chosen.empty())
                return;
            wcscpy_s(GlobalVar::settingParameter.externalEditor, chosen.c_str());
        }

        const std::wstring editor = GlobalVar::settingParameter.externalEditor;
        const std::wstring file = imgFileList[curFileIdx];
        const INT_PTR result = reinterpret_cast<INT_PTR>(ShellExecuteW(m_hWnd, L"open",
            editor.c_str(), (L"\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL));
        if (result <= 32) {
            auto errMsg = std::format(L"{} {}", getUIStringW(59).c_str(), result);
            MessageBoxW(m_hWnd, errMsg.c_str(), getUIStringW(1), MB_OK | MB_ICONERROR);
        }
    }

    // 矢量图（SVG）在缩放稳定后按需重新光栅化；返回 true 表示位图已更新、需要重绘
    bool refreshVectorRasterIfNeeded() {
        if (!curPar.imageAssetPtr || !curPar.imageAssetPtr->vectorSource)
            return false;

        const int targetEdge = jark::vectorTargetEdge(
            *curPar.imageAssetPtr, curPar.zoomCur, CurImageParameter::ZOOM_BASE);
        if (!jark::refreshVectorRaster(*curPar.imageAssetPtr, targetEdge))
            return false;

        operateQueue.push({ ActionENUM::refresh });
        return true;
    }

    // —— 叠加界面：用 ImGui 的前景绘制列表贴 mainRes 雪碧图与信息面板 ——
    // 命中区域仍由 cursorPos 决定（见 OnMouseMove），这里只负责画。

    OverlayIcons overlayIcons;

    static ImU32 imColor(uint32_t argb, float alphaScale = 1.0f) {
        const int alpha = static_cast<int>(((argb >> 24) & 0xFF) * alphaScale);
        return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, alpha);
    }

    bool hasOverlayUi() const {
        return extraUIFlag != ShowExtraUI::none && winWidth >= dp(100) && winHeight >= dp(100);
    }

    // 客户区坐标 → ImGui 坐标。多视口模式下主视口的原点是"客户区左上角在屏幕上的位置"，
    // 直接按客户区坐标绘制会让整个叠加层偏移（动画控制条甚至会有一半被顶到客户区上边）。
    static ImVec2 uiPos(float x, float y) {
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        return { origin.x + x, origin.y + y };
    }

    // 把一块资源切片贴到客户区坐标 (x, y)：尺寸按 DPI 缩放，与热区保持一致
    void drawOverlayIcon(const OverlayIcons::Slice& slice, float x, float y) {
        const ImTextureID texture = overlayIcons.texture();
        if (texture == 0)
            return;

        const float scale = uiScale();
        ImGui::GetForegroundDrawList()->AddImage(texture, uiPos(x, y),
            uiPos(x + slice.w * scale, y + slice.h * scale),
            OverlayIcons::uv0(slice), OverlayIcons::uv1(slice));
    }

    void drawOverlayUi() {
        if (!hasOverlayUi())
            return;

        const float scale = uiScale();
        const float winW = static_cast<float>(winWidth);
        const float winH = static_cast<float>(winHeight);

        // 左右边缘的图标：水平贴边，垂直居中于给定的中心线
        auto drawEdgeIcon = [&](const OverlayIcons::Slice& slice, bool rightEdge, float centerY) {
            const float x = rightEdge ? winW - slice.w * scale : 0.0f;
            drawOverlayIcon(slice, x, centerY - slice.h * scale * 0.5f);
        };

        switch (extraUIFlag) {
        case ShowExtraUI::rotateLeftButton:
            drawEdgeIcon(OverlayIcons::rotateLeft, false, winH * 0.125f);
            break;

        case ShowExtraUI::leftArrow:
            drawEdgeIcon(OverlayIcons::leftArrow, false, winH * 0.5f);
            break;

        case ShowExtraUI::printer:
            drawEdgeIcon(OverlayIcons::printer, false, winH * 0.875f);
            break;

        case ShowExtraUI::setting:
            drawEdgeIcon(OverlayIcons::setting, true, winH * 0.875f);
            break;

        case ShowExtraUI::rightArrow:
            drawEdgeIcon(OverlayIcons::rightArrow, true, winH * 0.5f);
            break;

        case ShowExtraUI::rotateRightButton:
            drawEdgeIcon(OverlayIcons::rotateRight, true, winH * 0.125f);
            break;

        case ShowExtraUI::animationBar: {
            // 整条 200x50：上一帧 / 暂停继续 / 下一帧 / 保存当前帧（热区按 4 段 50 宽切分）
            const OverlayIcons::Slice& bar = curPar.isAnimationPause
                ? OverlayIcons::barPaused : OverlayIcons::barPlaying;
            drawOverlayIcon(bar, (winW - bar.w * scale) * 0.5f, 0.0f);
        } break;
        }
    }

    // 等待解码的浮标：贴客户区左上角显示已等待秒数；"画面"本身由主页垫底或旧图停留
    void drawLoadingBadge() {
        if (!pendingLoad_)
            return;

        const float scale = uiScale();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pendingLoadStart_).count();
        const std::string label = std::format("{} {:.1f}s", getUIString(182), elapsedMs / 1000.0);

        const float padX = 9.0f * scale;
        const float padY = 4.0f * scale;
        const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
            ImGui::GetFontSize(), FLT_MAX, 0.0f, label.c_str());
        const float x = 12.0f * scale;
        const float y = 12.0f * scale;

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddRectFilled(uiPos(x, y), uiPos(x + textSize.x + padX * 2.0f, y + textSize.y + padY * 2.0f),
            imColor(GlobalVar::currentTheme.BG_DEEP, 0.82f), 6.0f * scale);
        drawList->AddText(uiPos(x + padX, y + padY), imColor(GlobalVar::currentTheme.FG), label.c_str());
    }

    // 「实况」角标：贴图片左上角（放大裁切时夹回可视区内），半透明底 + 文字，悬停高亮；
    // 悬停的重播动作在 OnMouseMove 里（见 liveBadgeHit）。只标实况照片，视频文件不带。
    void drawLiveBadge() {
        liveBadgeVisible_ = false;

        if (!currentIsLivePhoto_ || !curPar.imageAssetPtr ||
            curPar.imageAssetPtr->placeholder != PlaceholderKind::None)
            return;

        const auto geometry = jark::imageGeometry(viewState(), { winWidth, winHeight });
        if (geometry.scale <= 0)
            return;

        const float scale = uiScale();
        const std::string label = getUIString(172);
        const float padX = 9.0f * scale;
        const float padY = 4.0f * scale;
        const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
            ImGui::GetFontSize(), FLT_MAX, 0.0f, label.c_str());
        const float badgeW = textSize.x + padX * 2.0f;
        const float badgeH = textSize.y + padY * 2.0f;
        const float margin = 8.0f * scale;

        const float x = std::clamp(static_cast<float>(geometry.origin.x) + margin, margin,
            (std::max)(margin, winWidth - badgeW - margin));
        const float y = std::clamp(static_cast<float>(geometry.origin.y) + margin, margin,
            (std::max)(margin, winHeight - badgeH - margin));

        liveBadgeRect_ = { x, y, badgeW, badgeH };
        liveBadgeVisible_ = true;

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const ImU32 background = liveBadgeHovered_
            ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
            : imColor(GlobalVar::currentTheme.BG_DEEP, 0.72f);
        drawList->AddRectFilled(uiPos(x, y), uiPos(x + badgeW, y + badgeH), background, 6.0f * scale);
        drawList->AddText(uiPos(x + padX, y + padY), imColor(GlobalVar::currentTheme.FG), label.c_str());
    }

    // EXIF 面板的附加信息（直方图/内嵌 ICC 简介名）：切图时算一次（直方图要遍历像素）
    void updateExifPanelExtras() {
        if (!curPar.imageAssetPtr || curPar.imageAssetPtr->placeholder != PlaceholderKind::None)
            return;
        if (panelInfoAsset_ == curPar.imageAssetPtr.get())
            return;
        panelInfoAsset_ = curPar.imageAssetPtr.get();

        histogramReady_ = false;
        histogramPeak_ = 0;
        panelColorSpace_.clear();

        // RGB 直方图：超过约四百万像素就按步长采样（够画 128 列）
        const cv::Mat& image = currentSourceImage();
        cv::Mat bgr;
        if (image.channels() == 4)
            cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
        else if (image.channels() == 3)
            bgr = image;
        if (!bgr.empty()) {
            for (auto& channel : histogram_)
                channel.fill(0);
            const int step = (std::max)(1, static_cast<int>(bgr.total() / (4u << 20)));
            for (int y = 0; y < bgr.rows; y += step) {
                const uint8_t* row = bgr.ptr<uint8_t>(y);
                for (int x = 0; x < bgr.cols; x += step) {
                    const uint8_t* px = row + static_cast<size_t>(x) * 3;
                    ++histogram_[0][px[2]]; // R
                    ++histogram_[1][px[1]]; // G
                    ++histogram_[2][px[0]]; // B
                }
            }
            for (const auto& channel : histogram_) {
                for (uint32_t count : channel)
                    histogramPeak_ = (std::max)(histogramPeak_, count);
            }
            histogramReady_ = histogramPeak_ > 0;
        }

        if (!curPar.imageAssetPtr->iccProfile.empty())
            panelColorSpace_ = ColorManager::profileDescription(curPar.imageAssetPtr->iccProfile);
    }

    // RGB 直方图：三条通道半透明叠加（128 列、两档合一），底部一条基线
    void drawHistogram(ImDrawList* drawList, float left, float top, float right, float height) const {
        constexpr int kColumns = 128;
        constexpr ImU32 kChannelColors[3] = {
            IM_COL32(255, 82, 82, 128),
            IM_COL32(82, 255, 82, 128),
            IM_COL32(82, 82, 255, 128),
        };

        const float width = right - left;
        const float baseY = top + height;
        drawList->AddRectFilled(uiPos(left, top), uiPos(right, baseY), imColor(GlobalVar::currentTheme.BG, 0.45f));

        const float columnWidth = width / kColumns;
        for (int channel = 0; channel < 3; ++channel) {
            for (int column = 0; column < kColumns; ++column) {
                const uint32_t value = (std::max)(histogram_[channel][column * 2], histogram_[channel][column * 2 + 1]);
                if (value == 0)
                    continue;
                const float barHeight = static_cast<float>(value) / histogramPeak_ * (height - 2.0f);
                drawList->AddRectFilled(
                    uiPos(left + column * columnWidth, baseY - barHeight),
                    uiPos(left + (column + 1) * columnWidth, baseY), kChannelColors[channel]);
            }
        }
        drawList->AddLine(uiPos(left, baseY), uiPos(right, baseY), imColor(GlobalVar::currentTheme.FG, 0.35f));
    }

    // EXIF/AI 提示词面板：半透明底 + 直方图/色彩空间/质量头部 + 可滚动的折行文本
    void drawExifPanel() {
        if (!showExif || winWidth < dp(100) || winHeight < dp(100))
            return;

        if (!curPar.imageAssetPtr || curPar.imageAssetPtr->exifInfo.empty())
            return;

        updateExifPanelExtras();

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const float scale = uiScale();
        const float padding = static_cast<float>(dp(12));
        const float panelWidth = (winWidth - padding * 2.0f) / 4.0f;
        const float panelHeight = winHeight - padding * 2.0f;
        exifPanelRect_ = { padding, padding, panelWidth, panelHeight };

        drawList->AddRectFilled(uiPos(padding, padding), uiPos(padding + panelWidth, padding + panelHeight),
            imColor(GlobalVar::currentTheme.BG_DEEP, 0.82f), 8.0f * scale);

        const float contentLeft = padding + dp(10);
        const float contentRight = padding + panelWidth - dp(10);
        const bool chinese = jark::prefersChineseResources();
        float contentTop = padding + dp(8);

        // 头部：直方图与内嵌 ICC 的名字、JPEG 质量（都是"当前这张图"的附加信息）
        if (histogramReady_) {
            drawHistogram(drawList, contentLeft, contentTop, contentRight, dp(56));
            contentTop += dp(56) + dp(6);
        }
        if (!panelColorSpace_.empty()) {
            const std::string line = (chinese ? "色彩空间: " : "Color space: ") + panelColorSpace_;
            drawList->AddText(uiPos(contentLeft, contentTop), imColor(GlobalVar::currentTheme.FG), line.c_str());
            contentTop += ImGui::GetTextLineHeight();
        }
        if (curPar.imageAssetPtr->jpegQuality > 0) {
            const std::string line = chinese
                ? std::format("质量: 约 {}（由量化表反推）", curPar.imageAssetPtr->jpegQuality)
                : std::format("Quality: approx. {} (from quantization table)", curPar.imageAssetPtr->jpegQuality);
            drawList->AddText(uiPos(contentLeft, contentTop), imColor(GlobalVar::currentTheme.FG), line.c_str());
            contentTop += ImGui::GetTextLineHeight();
        }
        if (contentTop > padding + dp(8)) {
            contentTop += dp(4);
            drawList->AddLine(uiPos(contentLeft, contentTop), uiPos(contentRight, contentTop),
                imColor(GlobalVar::currentTheme.FG, 0.25f));
            contentTop += dp(6);
        }

        // 文本区：先量总高再画（滚动偏移 + 裁剪区间），屏外的行直接跳过
        const float textBottom = padding + panelHeight - dp(8);
        const float textViewHeight = textBottom - contentTop;
        const std::string& text = curPar.imageAssetPtr->exifInfo;
        const float totalHeight = drawWrappedText(nullptr, contentLeft, 0.0f, contentRight,
            0.0f, 0.0f, text, 0, false);
        exifPanelMaxScroll_ = (std::max)(0.0f, totalHeight - textViewHeight);
        exifPanelScroll_ = (std::clamp)(exifPanelScroll_, 0.0f, exifPanelMaxScroll_);

        drawList->PushClipRect(uiPos(contentLeft, contentTop), uiPos(contentRight, textBottom), true);
        drawWrappedText(drawList, contentLeft, contentTop - exifPanelScroll_, contentRight,
            contentTop, textBottom, text, imColor(GlobalVar::currentTheme.FG), true);
        drawList->PopClipRect();

        // 滚动条：贴在面板右缘
        if (exifPanelMaxScroll_ > 0.5f) {
            const float thumbHeight = (std::max)(static_cast<float>(dp(24)), textViewHeight * textViewHeight / totalHeight);
            const float thumbTop = contentTop + (textViewHeight - thumbHeight) * (exifPanelScroll_ / exifPanelMaxScroll_);
            drawList->AddRectFilled(uiPos(contentRight + dp(2), contentTop),
                uiPos(contentRight + dp(4), textBottom), imColor(GlobalVar::currentTheme.FG, 0.15f));
            drawList->AddRectFilled(uiPos(contentRight + dp(2), thumbTop),
                uiPos(contentRight + dp(4), thumbTop + thumbHeight), imColor(GlobalVar::currentTheme.FG, 0.45f));
        }
    }

    // 按宽度折行（CJK 逐字断行即可；拉丁文尽量在空格处断开）。draw=false 时只测量总高；
    // draw=true 时从 top 起画、clipTop/clipBottom 之外的行跳过（y 仍推进），配合滚动使用。
    float drawWrappedText(ImDrawList* drawList, float left, float top, float right,
        float clipTop, float clipBottom, const std::string& text, ImU32 color, bool draw) {
        const float lineHeight = ImGui::GetTextLineHeight();
        const float wrapWidth = right - left;
        if (wrapWidth <= 8.0f)
            return 0.0f;

        float y = top;
        size_t index = 0;
        std::string line;

        auto flushLine = [&](const std::string& value) {
            if (value.empty())
                return;
            if (draw && y + lineHeight >= clipTop && y <= clipBottom)
                drawList->AddText(uiPos(left, y), color, value.c_str());
            y += lineHeight;
        };

        while (index < text.size()) {
            const size_t charStart = index;
            const unsigned char byte = static_cast<unsigned char>(text[index]);

            size_t charLength = 1;
            if ((byte & 0xE0) == 0xC0) charLength = 2;
            else if ((byte & 0xF0) == 0xE0) charLength = 3;
            else if ((byte & 0xF8) == 0xF0) charLength = 4;
            charLength = (std::min)(charLength, text.size() - index);

            const std::string character = text.substr(charStart, charLength);
            const int codePoint = charLength == 1 ? byte : -1;

            if (codePoint == '\n') {
                flushLine(line);
                line.clear();
                index += charLength;
                continue;
            }

            line += character;

            if (ImGui::CalcTextSize(line.c_str()).x > wrapWidth) {
                // 超宽：退掉最后一个字符，输出当前行，把它挪到下一行
                line.resize(line.size() - character.size());
                flushLine(line);
                line = character;
            }

            index += charLength;
        }

        flushLine(line);
        return y - top;
    }

    // 是否有界面窗口（设置/批量/打印/编辑/重命名）在显示
    static bool anyWindowVisible() {
        return SettingWindow::instance().visible() || BatchWindow::instance().visible() ||
            PrintWindow::instance().visible() || EditorWindow::instance().visible() ||
            RenameWindow::instance().visible();
    }

    bool hasVisibleWindows() const override { return anyWindowVisible(); }

    // ESC 关掉最前面的一个界面窗口（窗口失焦时 ImGui 收不到 Esc，这里兜底）；返回是否关掉了
    static bool closeTopWindow() {
        if (RenameWindow::instance().visible()) {
            RenameWindow::instance().close();
            return true;
        }
        if (EditorWindow::instance().visible()) {
            EditorWindow::instance().close();
            return true;
        }
        if (PrintWindow::instance().visible()) {
            PrintWindow::instance().close();
            return true;
        }
        if (BatchWindow::instance().visible()) {
            BatchWindow::instance().close();
            return true;
        }
        if (SettingWindow::instance().visible()) {
            SettingWindow::instance().close();
            return true;
        }
        return false;
    }

    void DrawUi() override {
        const bool windowVisibleBefore = anyWindowVisible();
        syncNavigation();

        drawOverlayUi();
        drawExifPanel();
        drawLiveBadge();
        drawLoadingBadge();
        navigation.draw(currentSourceImage(), uiPos(0.0f, 0.0f));
        SettingWindow::instance().draw();
        BatchWindow::instance().draw();
        PrintWindow::instance().draw();
        EditorWindow::instance().draw();
        RenameWindow::instance().draw();

        // 重命名弹窗确认后由主窗口统一执行（改名 + 列表/缓存同步）
        if (auto renamed = RenameWindow::instance().takeConfirmedPath())
            applyRename(*renamed);

        // 在窗口绘制之后取可见性：本帧被关掉的窗口立刻把输入还给画布
        const bool windowVisible = anyWindowVisible();

        auto& uiHost = jark::ui::UiHost::instance();
        uiHost.setUiVisible(hasOverlayUi() || showExif || windowVisible);

        // 本帧刚被关掉的窗口还画在这一帧的画面里，要再补一帧把它擦掉；
        // 否则主循环直接进空闲分支，屏幕停在旧画面上，看起来就是“点了关闭按钮卡住”。
        if (windowVisibleBefore && !windowVisible)
            markPresentRequested();
    }

    void DrawScene() {
        if (!firstSceneDrawn_) {
            firstSceneDrawn_ = true;
            jarkUtils::startupTraceMark("first scene drawn");
        }

        if (pendingLoad_)
            updatePendingLoad();

        updateMediaPlayback(); // 实时播放推进（含音频时钟驱动的帧切换）
        updateSlideshow();     // 幻灯片按间隔自动切换

        // 主页/解码失败占位画面：尺寸、DPI、语言、主题或按钮状态变化时重新绘制
        if (const int placeholderChange = updatePlaceholderImage(); placeholderChange != 0) {
            if (placeholderChange == 2)
                curPar.Init(winWidth, winHeight); // 只有尺寸变化才需要重新布局视图（悬停反馈不动缩放）
            operateQueue.push({ ActionENUM::refresh });
        }

        if (jark::ThumbnailService::instance().consumeChanged())
            markPresentRequested();
        syncNavigation();

        if (GlobalVar::isNeedUpdateTheme) {
            GlobalVar::isNeedUpdateTheme = false;
            BOOL themeMode = GlobalVar::isCurrentUIDarkMode;
            DwmSetWindowAttribute(m_hWnd, 20, &themeMode, sizeof(BOOL));
            operateQueue.push({ ActionENUM::refresh });
        }

        if (GlobalVar::isNeedReloadImageCache) {
            GlobalVar::isNeedReloadImageCache = false;
            if (curFileIdx >= 0 && curFileIdx < (int)imgFileList.size()) {
                const auto currentPath = imgFileList[curFileIdx];
                imgDB.clear();
                ++navigationImageVersion;
                if (currentPath != m_wndCaption)
                    jark::ThumbnailService::instance().invalidate(currentPath);

                if (currentPath == m_wndCaption) {
                    imgDB.put(m_wndCaption, placeholderAsset(PlaceholderKind::Home, {}, getUIString(32)));
                    if (auto asset = requestCurrentImage(currentPath))
                        curPar.imageAssetPtr = std::move(asset);
                }
                else if (auto asset = requestCurrentImage(imgFileList[(curFileIdx + 1) % imgFileList.size()])) {
                    curPar.imageAssetPtr = std::move(asset);
                }

                updatePlaceholderImage();
                curPar.Init(winWidth, winHeight);
                operateQueue.push({ ActionENUM::refresh });
            }
        }

        if (GlobalVar::isNeedSortFileList) {
            GlobalVar::isNeedSortFileList = false;
            if (imgFileList.size() > 1) {
                // 只换顺序、不换图：缓存按路径索引，无需作废；当前图片的下标跟着走
                jarkUtils::sortImageFileList(imgFileList, GlobalVar::settingParameter.sortMode, curFileIdx);
                updateNavigationDirectory();
                operateQueue.push({ ActionENUM::refresh });
            }
        }

        auto operateAction = operateQueue.get();
        if (operateAction.action == ActionENUM::none &&
            curPar.zoomCur == curPar.zoomTarget &&
            curPar.slideCur == curPar.slideTarget &&
            (curPar.imageAssetPtr->format != ImageFormat::Animated ||
                (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause))) {

            // 画面已经稳定：此时才把矢量图升级到当前缩放需要的分辨率
            if (refreshVectorRasterIfNeeded())
                return;

            // 有界面在显示、或系统要求重绘时要继续出帧（不能停在空白后缓冲上）。
            // 窗口的可见性要单独看：刚被打开的窗口还没经过一帧，uiVisible() 还是旧值，
            // 少了这一项就要等鼠标动了才会画出来。
            const bool presentRequested = consumePresentRequest();
            // 等图期间也要持续出帧：浮标的秒数要跳、轮询也要靠 DrawScene 每帧执行
            if (anyWindowVisible() || jark::ui::UiHost::instance().uiVisible() || presentRequested || pendingLoad_)
                PresentUiOnly();

            Sleep(1); // Windows机制限制，实际时长最小只能 15.6ms
            return;
        }

        if (operateAction.action == ActionENUM::batchProcess) {
            // 以当前目录里已识别的图片作为待处理列表
            std::vector<std::wstring> batchFiles;
            for (const auto& file : imgFileList) {
                if (file != m_wndCaption)
                    batchFiles.push_back(file);
            }

            if (batchFiles.empty())
                // 49：当前目录下没有可批量处理的图片（原来写的是 33，那是“关于”菜单项）
                MessageBoxW(m_hWnd, getUIStringW(49), getUIStringW(15), MB_OK | MB_ICONINFORMATION);
            else
                BatchWindow::instance().open(std::move(batchFiles));
        }

        if (operateAction.action == ActionENUM::editImage) {
            cv::Mat srcImg = currentSourceImage();
            if (!srcImg.empty()) {
                const std::wstring path = (curFileIdx >= 0 && curFileIdx < (int)imgFileList.size())
                    ? imgFileList[curFileIdx] : std::wstring();
                EditorWindow::instance().open(path, srcImg);
                operateQueue.push({ ActionENUM::refresh });
            }
            return;
        }

        if (operateAction.action == ActionENUM::printImage) {
            cv::Mat srcImg = currentSourceImage();
            if (!srcImg.empty())
                PrintWindow::instance().open(srcImg, curPar.rotation);
            return;
        }

        if (operateAction.action == ActionENUM::setting) {
            SettingWindow::instance().open(operateAction.value1);
            operateQueue.push({ ActionENUM::refresh });
            return;
        }

        // 以下action均需要刷新画面
        auto clampSlideForZoom = [&](Cood slide, int64_t zoom) {
            const int srcW = (curPar.rotation == 0 || curPar.rotation == 2) ? curPar.width : curPar.height;
            const int srcH = (curPar.rotation == 0 || curPar.rotation == 2) ? curPar.height : curPar.width;
            const int slideXMax = (int)(srcW * zoom / 2 / curPar.ZOOM_BASE);
            const int slideYMax = (int)(srcH * zoom / 2 / curPar.ZOOM_BASE);

            slide.x = std::clamp(slide.x, -slideXMax, slideXMax);
            slide.y = std::clamp(slide.y, -slideYMax, slideYMax);
            return slide;
        };

        auto computeZoomSlide = [&](int64_t zoomNext) {
            const int srcW = (curPar.rotation == 0 || curPar.rotation == 2) ? curPar.width : curPar.height;
            const int srcH = (curPar.rotation == 0 || curPar.rotation == 2) ? curPar.height : curPar.width;
            const double halfDiffW_old = (winWidth - (double)srcW * curPar.zoomCur / curPar.ZOOM_BASE) / 2.0;
            const double halfDiffH_old = (winHeight - (double)srcH * curPar.zoomCur / curPar.ZOOM_BASE) / 2.0;

            const int imgLeft = (int)std::round(curPar.slideCur.x + halfDiffW_old);
            const int imgTop = (int)std::round(curPar.slideCur.y + halfDiffH_old);
            const int imgRight = (int)std::round(imgLeft + (double)srcW * curPar.zoomCur / curPar.ZOOM_BASE);
            const int imgBottom = (int)std::round(imgTop + (double)srcH * curPar.zoomCur / curPar.ZOOM_BASE);

            Cood slideNext = curPar.slideCur;
            if (mousePos.x >= imgLeft && mousePos.x < imgRight && mousePos.y >= imgTop && mousePos.y < imgBottom) {
                const double halfDiffW_new = (winWidth - (double)srcW * zoomNext / curPar.ZOOM_BASE) / 2.0;
                const double halfDiffH_new = (winHeight - (double)srcH * zoomNext / curPar.ZOOM_BASE) / 2.0;
                const double srcX = ((double)mousePos.x - curPar.slideCur.x - halfDiffW_old) * curPar.ZOOM_BASE / curPar.zoomCur;
                const double srcY = ((double)mousePos.y - curPar.slideCur.y - halfDiffH_old) * curPar.ZOOM_BASE / curPar.zoomCur;
                slideNext.x = (int)std::round(mousePos.x - halfDiffW_new - srcX * zoomNext / curPar.ZOOM_BASE);
                slideNext.y = (int)std::round(mousePos.y - halfDiffH_new - srcY * zoomNext / curPar.ZOOM_BASE);
            }
            curPar.slideTarget = clampSlideForZoom(slideNext, zoomNext);
        };

        switch (operateAction.action) {
        case ActionENUM::jumpToImage: {
            if (operateAction.generation == directoryVersion && operateAction.value1 != curFileIdx)
                switchToFile(operateAction.value1, 0);
        } break;

        case ActionENUM::navigateImage: {
            if (operateAction.generation == navigationImageVersion && !anyWindowVisible()) {
                curPar.zoomTarget = curPar.zoomCur;
                curPar.slideTarget = curPar.slideCur = { operateAction.x, operateAction.y };
                smoothShift = false;
            }
        } break;

        case ActionENUM::preImg: {
            if (curFileIdx <= 0) {
                if (GlobalVar::settingParameter.stopAtListEnd)
                    break; // 已在第一张：停住，不绕到末尾
                curFileIdx = (int)imgFileList.size() - 1;
            }
            else {
                --curFileIdx;
            }
            switchToFile(curFileIdx, -1);
        } break;

        case ActionENUM::nextImg: {
            if (curFileIdx >= (int)imgFileList.size() - 1) {
                if (GlobalVar::settingParameter.stopAtListEnd)
                    break; // 已在最后一张：停住，不绕回开头
                curFileIdx = 0;
            }
            else {
                ++curFileIdx;
            }
            switchToFile(curFileIdx, +1);
        } break;

        case ActionENUM::firstImg: {
            if (curFileIdx == 0)
                break;
            switchToFile(0, -1);
        } break;

        case ActionENUM::finalImg: {
            if (curFileIdx == (int)imgFileList.size() - 1)
                break;
            switchToFile((int)imgFileList.size() - 1, +1);
        } break;

        case ActionENUM::slideshow: {
            toggleSlideshow();
        } break;

        case ActionENUM::slide: {
            curPar.slideTarget = clampSlideForZoom({
                curPar.slideTarget.x + operateAction.x,
                curPar.slideTarget.y + operateAction.y
                }, curPar.zoomTarget);
        } break;

        case ActionENUM::toggleExif: {
            showExif = !showExif;
        } break;

        case ActionENUM::zoomIn: {
            if (curPar.zoomIndex < curPar.zoomList.size() - 1) {
                curPar.zoomIndex++;

                auto zoomNext = curPar.zoomList[curPar.zoomIndex];
                if (curPar.zoomTarget && zoomNext != curPar.zoomTarget) {
                    computeZoomSlide(zoomNext);
                }
                curPar.zoomTarget = zoomNext;
                smoothShift = true;
            }
        } break;

        case ActionENUM::zoomOut: {
            // 不宜缩太小
            if (curPar.zoomTarget <= curPar.ZOOM_BASE && (curPar.zoomTarget * std::min(curPar.width, curPar.height) / curPar.ZOOM_BASE) < 4)
                break;

            if (curPar.zoomIndex > 0) {
                curPar.zoomIndex--;

                auto zoomNext = curPar.zoomList[curPar.zoomIndex];
                if (curPar.zoomTarget && zoomNext != curPar.zoomTarget) {
                    computeZoomSlide(zoomNext);
                }
                curPar.zoomTarget = zoomNext;
                smoothShift = true;
            }
        } break;

        case ActionENUM::zoomFix: {
            if (curPar.zoomIndex == curPar.zoomIndex100percent)
                curPar.zoomIndex = curPar.zoomIndexFix;
            else
                curPar.zoomIndex = curPar.zoomIndex100percent;

            auto zoomNext = curPar.zoomList[curPar.zoomIndex];
            if (curPar.zoomTarget && zoomNext != curPar.zoomTarget) {
                computeZoomSlide(zoomNext);
            }
            curPar.zoomTarget = zoomNext;
            smoothShift = true;
        } break;

        case ActionENUM::rotateLeft: {
            ++navigationImageVersion;
            navigation.cancel();
            if (GlobalVar::settingParameter.isAllowRotateAnimation) {
                rotateLeftAnimation();
            }
            curPar.rotation = (curPar.rotation + 1) & 0b11;
            curPar.slideTargetRotateLeft();
            curPar.updateZoomList(winWidth, winHeight);
        } break;

        case ActionENUM::rotateRight: {
            ++navigationImageVersion;
            navigation.cancel();
            if (GlobalVar::settingParameter.isAllowRotateAnimation) {
                rotateRightAnimation();
            }
            curPar.rotation = (curPar.rotation + 4 - 1) & 0b11;
            curPar.slideTargetRotateRight();
            curPar.updateZoomList(winWidth, winHeight);
        } break;

        case ActionENUM::deleteImg: {
            if (imgFileList.empty() || curFileIdx < 0 || curFileIdx >= (int)imgFileList.size()) { break; }

            std::wstring_view target = imgFileList[curFileIdx];
            if (target == m_wndCaption || !std::filesystem::exists(target)) {
                break;
            }

            bool shouldDelete = true;
            if (GlobalVar::settingParameter.isNoteBeforeDelete) {
                auto tips = std::format(L"{}\n\n{}", getUIStringW(7).c_str(), target);
                shouldDelete = MessageBoxW(m_hWnd, tips.c_str(), getUIStringW(1),
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
            }

            if (!shouldDelete)
                break;

            std::wstring pathBuffer(target);
            pathBuffer.push_back(L'\0'); // SHFileOperation 需要双零终止

            SHFILEOPSTRUCTW fileOp{};
            fileOp.hwnd = m_hWnd;
            fileOp.wFunc = FO_DELETE;
            fileOp.pFrom = pathBuffer.c_str();
            fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;

            int opResult = SHFileOperationW(&fileOp);
            if (opResult != 0 || fileOp.fAnyOperationsAborted) {
                DWORD lastError = opResult != 0 ? (DWORD)opResult : GetLastError();
                auto errMsg = std::format(L"{} 0x{:08X}", getUIStringW(8).c_str(), lastError);
                MessageBoxW(m_hWnd, errMsg.c_str(), getUIStringW(1), MB_OK | MB_ICONERROR);
                break;
            }

            jark::ThumbnailService::instance().invalidate(std::wstring(target));
            stopMediaPlayback();
            imgFileList.erase(imgFileList.begin() + curFileIdx);

            if (imgFileList.empty()) {
                imgFileList.emplace_back(m_wndCaption);
                curFileIdx = 0;
                imgDB.put(m_wndCaption, placeholderAsset(PlaceholderKind::Home, {}, getUIString(32)));
            }
            else if (curFileIdx >= (int)imgFileList.size()) {
                curFileIdx = (int)imgFileList.size() - 1;
            }

            if (auto asset = requestCurrentImage(imgFileList[(curFileIdx + 1) % imgFileList.size()])) {
                curPar.imageAssetPtr = std::move(asset);
                updatePlaceholderImage();
                curPar.Init(winWidth, winHeight);
            }
            updateNavigationDirectory();
        } break;

        case ActionENUM::requestExit: {
            PostMessageW(m_hWnd, WM_DESTROY, 0, 0);
        } break;

        case ActionENUM::renameImage: {
            if (curFileIdx >= 0 && curFileIdx < (int)imgFileList.size() && imgFileList[curFileIdx] != m_wndCaption)
                RenameWindow::instance().open(imgFileList[curFileIdx]);
        } break;
        }

        if (curPar.zoomCur != curPar.zoomTarget || curPar.slideCur != curPar.slideTarget) {
            if (GlobalVar::settingParameter.isAllowZoomAnimation && smoothShift) { // 简单缩放动画
                const int progressMax = 1 << 8;
                static int progressCnt = progressMax;
                static int64_t zoomInit = 0;
                static int64_t zoomTargetInit = 0;
                static Cood slideInit{}, slideTargetInit{};

                //未开始进行动画 或 动画未完成就有新缩放操作
                if (progressCnt >= progressMax || zoomTargetInit != curPar.zoomTarget || slideTargetInit != curPar.slideTarget) {
                    progressCnt = 1;
                    zoomInit = curPar.zoomCur;
                    zoomTargetInit = curPar.zoomTarget;
                    slideInit = curPar.slideCur;
                    slideTargetInit = curPar.slideTarget;
                }
                else {
                    auto addDelta = ((progressMax - progressCnt) / 4);
                    if (addDelta <= 1) {
                        progressCnt = progressMax;
                        curPar.zoomCur = curPar.zoomTarget;
                        curPar.slideCur = curPar.slideTarget;
                        smoothShift = false;
                    }
                    else {
                        progressCnt += addDelta;
                        curPar.zoomCur = zoomInit + (curPar.zoomTarget - zoomInit) * progressCnt / progressMax;
                        const double t = (double)progressCnt / progressMax;
                        curPar.slideCur.x = (int)std::round(slideInit.x + (curPar.slideTarget.x - slideInit.x) * t);
                        curPar.slideCur.y = (int)std::round(slideInit.y + (curPar.slideTarget.y - slideInit.y) * t);
                    }
                }
            }
            else {
                curPar.zoomCur = curPar.zoomTarget;
                curPar.slideCur = curPar.slideTarget;
            }
        }

        cv::Mat srcImg = currentSourceImage();
        if (curPar.imageAssetPtr->format == ImageFormat::Animated &&
            curPar.curFrameIdx < (int)curPar.imageAssetPtr->frameDurations.size()) {
            curPar.curFrameDelay = curPar.imageAssetPtr->frameDurations[curPar.curFrameIdx];
        }

        drawCanvas(srcImg, mainCanvas);

        updateWindowCaption();

        updateMainCanvas();

        if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause == false) {
            // 起播/恢复播放时先把计时起点对齐到当下：否则加载耗时或暂停时长会被算进第一帧，
            // 表现为第一帧长时间不动
            if (!animClockArmed) {
                animClockArmed = true;
                delayRemain = (std::max)(int64_t(1), int64_t(curPar.curFrameDelay)) * 1000;
                lastTimestamp = std::chrono::steady_clock::now();
            }

            auto nowTimestamp = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(nowTimestamp - lastTimestamp);
            lastTimestamp = nowTimestamp;

            if (frameDuration > elapsed)
                std::this_thread::sleep_for(frameDuration - elapsed);

            // 余量按微秒累计：一帧要跑七八轮主循环迭代，若每轮都按整毫秒截断、每轮丢不足 1ms，
            // 100ms 的帧会被拖成 ~103ms——动图整体越播越慢就是这么来的
            delayRemain -= elapsed.count();
            // 欠帧时循环推进并把“多走”的时间留给下一帧（加下一个帧延迟）。Windows 睡眠粒度
            // 约 15.6ms，若像原来那样每帧把余量丢弃重置，超时部分会逐帧累积成慢性慢放
            while (delayRemain <= 0) {
                if (curPar.imageAssetPtr->frameDurations.empty())
                    break;

                curPar.curFrameIdx++;
                if (curPar.curFrameIdx > curPar.curFrameIdxMax) {
                    curPar.curFrameIdx = 0;

                    // 动态帧播放完，若有主图，则是当前实况图像
                    if (!curPar.imageAssetPtr->primaryFrame.empty()) {
                        curPar.imageAssetPtr->format = ImageFormat::Still;
                        curPar.Init(winWidth, winHeight);
                        operateQueue.push({ ActionENUM::refresh });
                        break;
                    }
                }

                delayRemain += (std::max)(int64_t(1),
                    int64_t(curPar.imageAssetPtr->frameDurations[curPar.curFrameIdx])) * 1000;
            }
        }
        else {
            animClockArmed = false; // 暂停或非动画状态：作废旧计时，恢复播放时重新对齐
        }
    }

    void OnRequestExitOtherWindows() {
    }
};

void test();

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    jarkUtils::startupTraceMark("begin"); // 分段计时的起点（JARKVIEWER_STARTUP_TRACE=<文件> 时启用）

#ifndef NDEBUG
    AllocConsole();
    FILE* stream;
    freopen_s(&stream, "CON", "w", stdout);//重定向标准输出流
    freopen_s(&stream, "CON", "w", stderr);//重定向错误输出流

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    // Release 下也可开启日志：命令行 --log 或环境变量 JARKVIEWER_LOG=1
    // （Debug 默认开启；开启后日志同时写入 %TEMP%\JarkViewer.log）
    {
        wchar_t envValue[8] = {};
        const DWORD envLength = ::GetEnvironmentVariableW(L"JARKVIEWER_LOG", envValue, 8);
        if (envLength > 0 && envLength < 8 && envValue[0] != L'0' && envValue[0] != 0)
            jarkUtils::setLogEnabled(true);
    }

    //test();

    // 限制 PPL 默认调度器最多 4 线程, 必须在任何 concurrency::parallel_* 调用之前设置。
    {
        concurrency::SchedulerPolicy policy(2,
            concurrency::MinConcurrency, 1,
            concurrency::MaxConcurrency, 4);
        concurrency::Scheduler::SetDefaultSchedulerPolicy(policy);
    }

    Exiv2::enableBMFF();
    // 输入法不在这里禁用（ImmDisableIME 是线程级且不可撤销，那样界面里的文本框就永远打不了中文）：
    // 改由 UiHost 在窗口上挂/摘 IME 上下文——平时不挂（不干扰快捷键），有文本框聚焦时才挂回去。

    ::HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    if (!SUCCEEDED(::CoInitialize(nullptr)))
        return 0;

    // 命令行解析：跳过 --log 等开关，第一个普通参数视为要打开的文件
    wstring filePath = lpCmdLine;
    std::optional<int> languageOverride;
    {
        int argCount = 0;
        if (LPWSTR* rawArgv = ::CommandLineToArgvW(::GetCommandLineW(), &argCount)) {
            std::vector<std::wstring> argList(rawArgv, rawArgv + argCount);
            ::LocalFree(rawArgv);

            // 无界面解码自检：--probe <文件...>，用于在无人眼参与时验证解码路由
            if (argList.size() > 1 && argList[1] == L"--probe") {
                // 保持 COM 已初始化：音频输出（XAudio2）与部分解码器依赖它
                const int exitCode = jark::runDecodeProbe(argList);
                ::CoUninitialize();
                return exitCode;
            }

            filePath.clear();
            for (size_t i = 1; i < argList.size(); ++i) {
                if (argList[i] == L"--log") {
                    jarkUtils::setLogEnabled(true);
                    continue;
                }
                if (argList[i] == L"--lang" && i + 1 < argList.size()) {
                    // 临时指定界面语言：0简体 1繁體 2English 3日本語 4한국어 5Русский
                    // 记录待用：设置文件在窗口初始化时才会读入，那时才能覆盖
                    languageOverride = ::_wtoi(argList[++i].c_str());
                    continue;
                }
                if (filePath.empty())
                    filePath = argList[i];
            }
        }
        else {
            // 解析失败时退回旧的去引号逻辑
            if (!filePath.empty() && filePath.front() == '\"')
                filePath = filePath.substr(1);
            if (!filePath.empty() && filePath.back() == '\"')
                filePath.pop_back();
        }
    }

    JarkViewerApp app;
    app.setStartupFile(filePath);
    // 命令行指定的语言要在窗口创建前应用：窗口一就绪就会扫描目录、放占位并派发解码，
    // 占位文案随 UI_LANG 走
    if (languageOverride.has_value()) {
        GlobalVar::settingParameter.UI_LANG =
            static_cast<uint32_t>(jark::languageFromSetting(*languageOverride));
    }

    if (SUCCEEDED(app.InitWindow(hInstance))) {
        app.initOpenFile(filePath);
        app.Run();
    }
    else {
        MessageBoxW(NULL, getUIStringW(13), getUIStringW(14), MB_ICONERROR);
    }

    ::CoUninitialize();
    return 0;
}

void test() {
    std::ifstream file("D:\\Downloads\\test\\22.wp2", std::ios::binary);
    auto buf = std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});

    exit(0);
}
