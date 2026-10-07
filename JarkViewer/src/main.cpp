#include "jarkUtils.h"

#include "BatchWindow.h"
#include "CanvasRenderer.h"
#include "DecodeProbe.h"
#include "Localization.h"
#include "MediaPlayer.h"
#include "VectorImage.h"
#include "TextDrawer.h"
#include "ImageDatabase.h"
#include "Printer.h"
#include "Setting.h"

#include "D3D11App.h"
#include <optional>
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
std::wstring_view appVersion = L"v1.35";
constinit int appVersionCode = 13500; // 主版本*10000 + 次版本*100 + 修订版本

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


class ExtraUIRes {
public:
    cv::Mat mainRes, leftArrow, rightArrow, leftRotate, rightRotate, printer, setting, animationBarPlaying, animationBarPausing;

    ExtraUIRes() {
        rcFileInfo rc;

        rc = jarkUtils::GetResource(IDB_PNG_MAIN_RES, L"PNG");
        mainRes = cv::imdecode(cv::Mat(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr), cv::IMREAD_UNCHANGED);

        rebuild(1.0f);
    }
    ~ExtraUIRes() {}

    // 资源切片是 96DPI 下的尺寸；窗口在高 DPI 显示器上时要放大后再贴图，
    // 否则按钮只有设计尺寸的一半大。缩放值不变时直接复用。
    void rebuild(float scale) {
        if (hasBuilt && std::abs(scale - buildScale) < 0.01f)
            return;

        hasBuilt = true;
        buildScale = scale;

        auto slice = [&](cv::Rect rect) -> cv::Mat {
            cv::Mat part = mainRes(rect);
            if (std::abs(scale - 1.0f) < 0.01f)
                return part.clone();

            cv::Mat scaled;
            cv::resize(part, scaled, cv::Size(), scale, scale,
                scale > 1.0f ? cv::INTER_LINEAR : cv::INTER_AREA);
            return scaled;
            };

        leftRotate = slice({ 0, 0, 50, 50 });
        rightRotate = slice({ 50, 0, 50, 50 });

        printer = slice({ 0, 50, 50, 50 });
        setting = slice({ 50, 50, 50, 50 });

        leftArrow = slice({ 100, 0, 50, 100 });
        rightArrow = slice({ 150, 0, 50, 100 });

        animationBarPlaying = slice({ 0, 100, 200, 50 });
        animationBarPausing = slice({ 0, 150, 200, 50 });
    }

private:
    float buildScale = 0.0f;
    bool hasBuilt = false;
};

class JarkViewerApp : public D3D11App {
public:

    OperateQueue operateQueue;

    CursorPos cursorPos = CursorPos::centerArea;
    CursorPos cursorPosLast = CursorPos::centerArea;
    ShowExtraUI extraUIFlag = ShowExtraUI::none;
    bool mouseIsPressing = false;
    bool ctrlIsPressing = false;
    bool smoothShift = false;
    bool showExif = false;
    Cood mousePos, mousePressPos;
    ImageDatabase imgDB;

    int curFileIdx = -1;         // 文件在路径列表的索引
    vector<wstring> imgFileList; // 工作目录下所有图像文件路径

    TextDrawer textDrawer;       // 给Mat绘制文字
    std::unique_ptr<jark::MediaPlayer> mediaPlayer; // 实况照片/视频的实时播放
    cv::Mat playbackFrame;                          // 播放中的当前帧
    const ImageAsset* playedAsset = nullptr;        // 已播放过的资源（每张图只自动播一次）
    const ImageAsset* lastSeenAsset = nullptr;      // 用于检测切图
    int lastSeenFileIndex = -1;

    CurImageParameter curPar;
    ExtraUIRes extraUIRes;
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
        extraUIRes.rebuild(uiScale());
        if (hasInitWinSize)
            operateQueue.push({ ActionENUM::refresh });
    }

    ~JarkViewerApp() {
    }

    HRESULT InitWindow(HINSTANCE hInstance) {
        if (!SUCCEEDED(D3D11App::Initialize(hInstance)))
            return S_FALSE;

        if (m_pD3DDevice == nullptr)
            return S_FALSE;

        imgDB.setColorManagementWindow(m_hWnd);

        updateTextDrawerScale();
        extraUIRes.rebuild(uiScale());

        return S_OK;
    }

    void initOpenFile(wstring filePath) {
        namespace fs = std::filesystem;

        curFileIdx = -1;
        imgFileList.clear();
        imgDB.clear();

        if (filePath.empty()) {
            imgFileList.emplace_back(m_wndCaption);
            curFileIdx = 0;
            imgDB.put(m_wndCaption, { ImageFormat::Still, imgDB.getHomeMat(), {}, {}, getUIString(32) });
            curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[curFileIdx]);
            curPar.Init(winWidth, winHeight);
            return;
        }

        fs::path fullPath = fs::absolute(filePath);
        wstring openFileName = fullPath.filename().wstring();

        auto workDir = fullPath.parent_path();
        if (fs::exists(workDir)) {
            std::vector<std::wstring> fileNameList;
            for (const auto& entry : fs::directory_iterator(workDir)) {
                if (!entry.is_regular_file())continue;

                std::wstring ext = entry.path().extension().wstring();
                if (ext.length() < 2)continue;
                
                ext = ext.substr(1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

                if (ImageDatabase::supportExt.contains(ext) || ImageDatabase::supportRaw.contains(ext)) {
                    fileNameList.emplace_back(entry.path().filename().wstring());
                }
            }

            // 自然排序 数字感知排序
            std::sort(fileNameList.begin(), fileNameList.end(), [](std::wstring_view a, std::wstring_view b) -> bool {
                return StrCmpLogicalW(a.data(), b.data()) < 0; });

            for (auto& fileName : fileNameList) {
                auto fullpath = (workDir / fileName).wstring();
                imgFileList.emplace_back(std::move(fullpath));
                if (curFileIdx == -1 && openFileName == fileName) {
                    curFileIdx = (int)imgFileList.size() - 1;
                }
            }
        }
        else {
            curFileIdx = -1;
        }

        if (curFileIdx < 0) {
            if (filePath.empty()) { //直接打开软件，没有传入参数
                imgFileList.emplace_back(m_wndCaption);
                curFileIdx = 0;
                imgDB.put(m_wndCaption, { ImageFormat::Still, imgDB.getHomeMat(), {}, {}, getUIString(32) });
            }
            else { // 打开的文件不支持，默认加到尾部
                imgFileList.emplace_back(fullPath.wstring());
                curFileIdx = (int)imgFileList.size() - 1;

                auto dotPos = filePath.rfind(L'.');
                auto ext = wstring((dotPos != std::wstring::npos && dotPos < filePath.size() - 1) ?
                    filePath.substr(dotPos + 1) : filePath);
                for (auto& c : ext)	c = std::tolower(c);

                if (!ImageDatabase::videoExt.contains(ext)) // 非视频文件直接提示错误。若是视频文件则尝试当做动态照片处理(仅解码前 MAX_VIDEO_FRAMES 帧)
                    imgDB.put(fullPath.wstring(), { ImageFormat::Still, imgDB.getErrorTipsMat(), {}, {}, getUIString(33) });
            }
        }

        curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[(curFileIdx + 1) % imgFileList.size()]);
        curPar.Init(winWidth, winHeight);
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
                auto [filePath, isJPG] = jarkUtils::saveImageDialogW(getUIStringW(4));
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

    void OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) override {
        switch ((uint64_t)btnState)
        {
        case WM_LBUTTONDOWN: {//左键
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
        cursorPosLast = cursorPos = CursorPos::centerArea;
        extraUIFlag = ShowExtraUI::none;
        mouseIsPressing = false;
        operateQueue.push({ ActionENUM::refresh });
    }

    void OnMouseWheel(UINT nFlags, short zDelta, int x, int y) override {
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
            }break;

            case 'S': { // Ctrl + S  动图或实况图视频 批量保存每一帧到png图片
                auto& frames = curPar.imageAssetPtr->frames;
                if (frames.empty())
                    break;

                if (IDYES == MessageBoxW(
                    m_hWnd,
                    std::format(L"{}{}", getUIStringW(5), frames.size()).c_str(),
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

            case VK_ESCAPE: { // ESC
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
            drawExifInfo(mainCanvas);
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

    void drawCanvas(const cv::Mat& srcImg, cv::Mat& canvas) const {
        jark::ViewState view;
        view.imageWidth = curPar.width;
        view.imageHeight = curPar.height;
        view.zoom = curPar.zoomCur;
        view.zoomBase = curPar.ZOOM_BASE;
        view.slideX = curPar.slideCur.x;
        view.slideY = curPar.slideCur.y;
        view.rotation = curPar.rotation;

        jark::drawImageToCanvas(srcImg, canvas, view);
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
            drawExifInfo(mainCanvas);
            drawExtraUI(mainCanvas);

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
            drawExifInfo(mainCanvas);
            drawExtraUI(mainCanvas);

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
        drawExifInfo(nextmainCanvas);

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
            drawExtraUI(mainCanvas);

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
        drawExifInfo(nextmainCanvas);

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
            drawExtraUI(mainCanvas);

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
        drawExifInfo(nextmainCanvas);

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
            drawExtraUI(mainCanvas);

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
        drawExifInfo(nextmainCanvas);

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
            drawExtraUI(mainCanvas);

            updateMainCanvas();

            if (duration_cast<milliseconds>(steady_clock::now() - start_clock).count() < 10)
                Sleep(1);
        }
    }

    void drawExifInfo(cv::Mat& canvas) {
        if (showExif) {
            const int padding = dp(10);
            const int areaWidth = (canvas.cols - 2 * padding) / 4;
            cv::Rect rect{ padding, padding, std::max(areaWidth, dp(400)), canvas.rows - 2 * padding };
            textDrawer.putAlignLeft(canvas, rect, curPar.imageAssetPtr->exifInfo.c_str(), GlobalVar::currentTheme.FG, true);
        }
    }

    void drawExtraUI(cv::Mat& canvas) {
        int canvasHeight = canvas.rows;
        int canvasWidth = canvas.cols;

        //窗口尺寸太小则直接退出
        if (canvasWidth < dp(100) || canvasHeight < dp(100) || extraUIFlag == ShowExtraUI::none)
            return;

        switch (extraUIFlag)
        {
        case ShowExtraUI::rotateLeftButton: {
            auto& img = extraUIRes.leftRotate;
            jarkUtils::overlayImg(canvas, img, 0, (canvasHeight / 4 - img.rows) / 2);
        } break;

        case ShowExtraUI::leftArrow: {
            auto& img = extraUIRes.leftArrow;
            jarkUtils::overlayImg(canvas, img, 0, (canvasHeight - img.rows) / 2);
        } break;

        case ShowExtraUI::printer: {
            auto& img = extraUIRes.printer;
            jarkUtils::overlayImg(canvas, img, 0, (canvasHeight * 7 / 4 - img.rows) / 2);
        } break;

        case ShowExtraUI::setting: {
            auto& img = extraUIRes.setting;
            jarkUtils::overlayImg(canvas, img, canvasWidth - img.cols, (canvasHeight * 7 / 4 - img.rows) / 2);
        } break;

        case ShowExtraUI::rightArrow: {
            auto& img = extraUIRes.rightArrow;
            jarkUtils::overlayImg(canvas, img, canvasWidth - img.cols, (canvasHeight - img.rows) / 2);
        } break;

        case ShowExtraUI::rotateRightButton: {
            auto& img = extraUIRes.rightRotate;
            jarkUtils::overlayImg(canvas, img, canvasWidth - img.cols, (canvasHeight / 4 - img.rows) / 2);
        } break;

        case ShowExtraUI::animationBar: {
            auto& img = curPar.isAnimationPause ? extraUIRes.animationBarPausing : extraUIRes.animationBarPlaying;
            jarkUtils::overlayImg(canvas, img, (canvasWidth - img.cols)/2, 0);
        } break;
        }
    }

    void updateMainCanvas() {
        PresentCanvas(mainCanvas.ptr(), mainCanvas.cols, mainCanvas.rows, (int)mainCanvas.step);
    }


    int64_t delayRemain = 0;
    const std::chrono::milliseconds frameDuration{ 10 };
    std::chrono::steady_clock::time_point lastTimestamp = std::chrono::steady_clock::now();


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
        if (!player || !player->start(asset.videoSource->data)) {
            JARK_LOG("视频播放启动失败，保持静态图");
            playedAsset = assetPtr;
            return;
        }

        JARK_LOG("视频播放开始，音频={}", player->hasAudio());
        mediaPlayer = std::move(player);

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

    void DrawScene() {
        updateMediaPlayback(); // 实时播放推进（含音频时钟驱动的帧切换）

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

                if (currentPath == m_wndCaption) {
                    imgDB.put(m_wndCaption, { ImageFormat::Still, imgDB.getHomeMat(), {}, {}, getUIString(32) });
                    curPar.imageAssetPtr = imgDB.getSafePtr(currentPath, currentPath);
                }
                else {
                    const auto& nextPath = imgFileList[(curFileIdx + 1) % imgFileList.size()];
                    curPar.imageAssetPtr = imgDB.getSafePtr(currentPath, nextPath);
                }

                curPar.Init(winWidth, winHeight);
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

            Sleep(1); // Windows机制限制，实际时长最小只能 15.6ms
            return;
        }

        if (operateAction.action == ActionENUM::batchProcess) {
            JARK_LOG("批量处理请求: 文件列表 {} 项, 已在运行={}", imgFileList.size(), static_cast<bool>(BatchWindow::isWorking));
            if (BatchWindow::isWorking) {
                if (BatchWindow::hwnd)
                    jarkUtils::activateWindow(BatchWindow::hwnd);
            }
            else {
                // 以当前目录里已识别的图片作为待处理列表
                std::vector<std::wstring> batchFiles;
                for (const auto& file : imgFileList) {
                    if (file != m_wndCaption)
                        batchFiles.push_back(file);
                }

                if (batchFiles.empty()) {
                    MessageBoxW(m_hWnd, getUIStringW(33), getUIStringW(15), MB_OK | MB_ICONINFORMATION);
                }
                else {
                    OnRequestExitOtherWindows();
                    std::thread batchThread([files = std::move(batchFiles)]() {
                        BatchWindow window(files);
                        });
                    batchThread.detach();
                }
            }
        }

        if (operateAction.action == ActionENUM::printImage) {
            if (Printer::isWorking) {
                jarkUtils::activateWindow(Printer::hwnd);
            }
            else {
                cv::Mat srcImg = currentSourceImage();

                std::thread printerThread([](cv::Mat image, int rotation) {
                    cv::Mat rotatedImage;

                    switch (rotation) {
                    case 1:
                        cv::rotate(image, rotatedImage, cv::ROTATE_90_COUNTERCLOCKWISE);
                        break;
                    case 2:
                        cv::rotate(image, rotatedImage, cv::ROTATE_180);
                        break;
                    case 3:
                        cv::rotate(image, rotatedImage, cv::ROTATE_90_CLOCKWISE);
                        break;
                    default:
                        rotatedImage = image;
                        break;
                    }

                    Printer printer(rotatedImage);
                    }, srcImg, curPar.rotation);
                printerThread.detach();
            }
            return;
        }

        if (operateAction.action == ActionENUM::setting) {
            if (Setting::isWorking) {
                Setting::curTabIdx = operateAction.value1;
                PostMessageW(Setting::hwnd, MatWindow::WM_MATWINDOW_DRAW_REQUEST, 0, 0);
                jarkUtils::activateWindow(Setting::hwnd);
            }
            else {
                std::thread settingThread([](int tabIdx) {
                    Setting setting(tabIdx);
                    }, operateAction.value1);
                settingThread.detach();
            }
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
        case ActionENUM::preImg: {
            if (imgFileList.size() <= 1)
                break;

            if (GlobalVar::settingParameter.switchImageAnimationMode) {// 开动画时才需要
                cv::Mat srcImg = currentSourceImage();

                drawCanvas(srcImg, mainCanvas); //先更新无额外按钮UI的原图
                drawExifInfo(mainCanvas);
            }
            
            // 播放过的实况图，状态会变成静态图，切走前恢复一下
            if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
                curPar.imageAssetPtr->format = ImageFormat::Animated;
            }

            if (--curFileIdx < 0)
                curFileIdx = (int)imgFileList.size() - 1;
            curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[(curFileIdx + imgFileList.size() - 1) % imgFileList.size()]);
            curPar.Init(winWidth, winHeight);

            if (GlobalVar::settingParameter.switchImageAnimationMode == 1)
                mainCanvasSlideToPreAnimationVertical();      // 竖直滑动
            else if (GlobalVar::settingParameter.switchImageAnimationMode == 2)
                mainCanvasSlideToPreAnimationHorizontal();    // 水平滑动

            lastTimestamp = std::chrono::steady_clock::now();
            delayRemain = 0;
        } break;

        case ActionENUM::nextImg: {
            if (imgFileList.size() <= 1)
                break;

            if (GlobalVar::settingParameter.switchImageAnimationMode) {// 开动画时才需要
                cv::Mat srcImg = currentSourceImage();

                drawCanvas(srcImg, mainCanvas); //先更新无额外按钮UI的原图
                drawExifInfo(mainCanvas);
            }

            // 播放过的实况图，状态会变成静态图，切走前恢复一下
            if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
                curPar.imageAssetPtr->format = ImageFormat::Animated;
            }

            if (++curFileIdx >= (int)imgFileList.size())
                curFileIdx = 0;
            curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[(curFileIdx + 1) % imgFileList.size()]);
            curPar.Init(winWidth, winHeight);

            if (GlobalVar::settingParameter.switchImageAnimationMode == 1)
                mainCanvasSlideToNextAnimationVertical();   // 竖直滑动
            else if (GlobalVar::settingParameter.switchImageAnimationMode == 2)
                mainCanvasSlideToNextAnimationHorizontal(); // 水平滑动

            lastTimestamp = std::chrono::steady_clock::now();
            delayRemain = 0;
        } break;

        case ActionENUM::firstImg: {
            if (imgFileList.size() == 1 or curFileIdx == 0)
                break;

            if (GlobalVar::settingParameter.switchImageAnimationMode) {// 开动画时才需要
                cv::Mat srcImg = currentSourceImage();

                drawCanvas(srcImg, mainCanvas); //先更新无额外按钮UI的原图
                drawExifInfo(mainCanvas);
            }

            // 播放过的实况图，状态会变成静态图，切走前恢复一下
            if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
                curPar.imageAssetPtr->format = ImageFormat::Animated;
            }

            curFileIdx = 0;
            curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[(curFileIdx + imgFileList.size() - 1) % imgFileList.size()]);
            curPar.Init(winWidth, winHeight);

            if (GlobalVar::settingParameter.switchImageAnimationMode == 1)
                mainCanvasSlideToPreAnimationVertical();      // 竖直滑动
            else if (GlobalVar::settingParameter.switchImageAnimationMode == 2)
                mainCanvasSlideToPreAnimationHorizontal();    // 水平滑动

            lastTimestamp = std::chrono::steady_clock::now();
            delayRemain = 0;
        } break;

        case ActionENUM::finalImg: {
            if (imgFileList.size() == 1 or curFileIdx == ((int)imgFileList.size() - 1))
                break;

            if (GlobalVar::settingParameter.switchImageAnimationMode) {// 开动画时才需要
                cv::Mat srcImg = currentSourceImage();

                drawCanvas(srcImg, mainCanvas); //先更新无额外按钮UI的原图
                drawExifInfo(mainCanvas);
            }

            // 播放过的实况图，状态会变成静态图，切走前恢复一下
            if (curPar.imageAssetPtr->format == ImageFormat::Still && !curPar.imageAssetPtr->frames.empty()) {
                curPar.imageAssetPtr->format = ImageFormat::Animated;
            }

            curFileIdx = (int)imgFileList.size() - 1;
            curPar.imageAssetPtr = imgDB.getSafePtr(imgFileList[curFileIdx], imgFileList[(curFileIdx + 1) % imgFileList.size()]);
            curPar.Init(winWidth, winHeight);

            if (GlobalVar::settingParameter.switchImageAnimationMode == 1)
                mainCanvasSlideToNextAnimationVertical();   // 竖直滑动
            else if (GlobalVar::settingParameter.switchImageAnimationMode == 2)
                mainCanvasSlideToNextAnimationHorizontal(); // 水平滑动

            lastTimestamp = std::chrono::steady_clock::now();
            delayRemain = 0;
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
            if (GlobalVar::settingParameter.isAllowRotateAnimation) {
                rotateLeftAnimation();
            }
            curPar.rotation = (curPar.rotation + 1) & 0b11;
            curPar.slideTargetRotateLeft();
            curPar.updateZoomList(winWidth, winHeight);
        } break;

        case ActionENUM::rotateRight: {
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
                auto tips = std::format(L"{}\n\n{}", getUIStringW(7), target);
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
                auto errMsg = std::format(L"{} 0x{:08X}", getUIStringW(8), lastError);
                MessageBoxW(m_hWnd, errMsg.c_str(), getUIStringW(1), MB_OK | MB_ICONERROR);
                break;
            }

            imgFileList.erase(imgFileList.begin() + curFileIdx);

            if (imgFileList.empty()) {
                imgFileList.emplace_back(m_wndCaption);
                curFileIdx = 0;
                imgDB.put(m_wndCaption, { ImageFormat::Still, imgDB.getHomeMat(), {}, {}, getUIString(32) });
            }
            else if (curFileIdx >= (int)imgFileList.size()) {
                curFileIdx = (int)imgFileList.size() - 1;
            }

            curPar.imageAssetPtr = imgDB.getSafePtr(
                imgFileList[curFileIdx],
                imgFileList[(curFileIdx + 1) % imgFileList.size()]);
            curPar.Init(winWidth, winHeight);
        } break;

        case ActionENUM::requestExit: {
            PostMessageW(m_hWnd, WM_DESTROY, 0, 0);
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
        drawExifInfo(mainCanvas);
        drawExtraUI(mainCanvas);

        if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause) {
            wstring str = std::format(L"{} [{}/{}] {}% {}  ",
                getUIStringW(9),
                curPar.curFrameIdx + 1, curPar.curFrameIdxMax + 1,
                curPar.zoomCur * 100ULL / curPar.ZOOM_BASE,
                imgFileList[curFileIdx]);
            if (curPar.rotation)
                str += (curPar.rotation == 1 ? getUIStringW(10) : (curPar.rotation == 3 ? getUIStringW(11) : getUIStringW(12)));
            SetWindowTextW(m_hWnd, str.c_str());
        }
        else {
            wstring str = std::format(L" [{}/{}] {}% {}  ",
                curFileIdx + 1, imgFileList.size(),
                curPar.zoomCur * 100ULL / curPar.ZOOM_BASE,
                imgFileList[curFileIdx]);
            if (curPar.rotation)
                str += (curPar.rotation == 1 ? getUIStringW(10) : (curPar.rotation == 3 ? getUIStringW(11) : getUIStringW(12)));
            SetWindowTextW(m_hWnd, str.c_str());
        }

        updateMainCanvas();

        if (curPar.imageAssetPtr->format == ImageFormat::Animated && curPar.isAnimationPause == false) {
            if (delayRemain <= 0)
                delayRemain = curPar.curFrameDelay;

            auto nowTimestamp = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(nowTimestamp - lastTimestamp);
            lastTimestamp = nowTimestamp;

            if (frameDuration > elapsed)
                std::this_thread::sleep_for(frameDuration - elapsed);

            delayRemain -= elapsed.count();
            if (delayRemain <= 0) {
                delayRemain = curPar.curFrameDelay;
                curPar.curFrameIdx++;
                if (curPar.curFrameIdx > curPar.curFrameIdxMax) {
                    curPar.curFrameIdx = 0;

                    // 动态帧播放完，若有主图，则是当前实况图像
                    if (!curPar.imageAssetPtr->primaryFrame.empty()) {
                        curPar.imageAssetPtr->format = ImageFormat::Still;
                        curPar.Init(winWidth, winHeight);
                        operateQueue.push({ ActionENUM::refresh });
                    }
                }
            }
        }
    }

    void OnRequestExitOtherWindows() {
        Printer::requestExit();
        Setting::requestExit();
        BatchWindow::requestExit();
    }
};

void test();

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
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
    ::ImmDisableIME(GetCurrentThreadId()); // 禁用输入法，防止干扰按键操作

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
                    // 临时指定界面语言：0简体 1繁體 2English 3日本語 4한국어
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
    if (SUCCEEDED(app.InitWindow(hInstance))) {
        // 设置文件已在初始化时读入，此处再应用命令行指定的语言
        if (languageOverride.has_value()) {
            GlobalVar::settingParameter.UI_LANG =
                static_cast<uint32_t>(jark::languageFromSetting(*languageOverride));
        }

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
