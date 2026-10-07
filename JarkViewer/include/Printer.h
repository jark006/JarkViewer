#pragma once

#include "MatWindow.h"
#include "Localization.h"
#include "TextDrawer.h"
#include "ImageAdjust.h"
#include "UiFramework.h"

// 全局变量存储UI状态
struct PrintParams {
    //double topMargin = 5.0;       // 上边距百分比
    //double bottomMargin = 5.0;    // 下边距百分比
    //double leftMargin = 5.0;      // 左边距百分比
    //double rightMargin = 5.0;     // 右边距百分比
    //int layoutMode = 0;           // 0=适应, 1=填充, 2=原始比例
    int brightness = 100;           // 亮度调整 (0 ~ 200)
    int contrast = 100;             // 对比度调整 (0 ~ 200)
    uint32_t colorMode = 1;         // 颜色模式 0:彩色  1:黑白  2:黑白文档 3:黑白抖动(二值像素)
    bool invertColors = false;      // 是否反相

    bool confirmed = false;       // 用户点击确认
    bool saveToFile = false;      // 保存到文件
    bool mousePressing = false;   // 鼠标左键是否按住状态
    bool mousePressingBrightnessBar = false;
    bool mousePressingContrastBar = false;
    cv::Mat previewImage;         // 预览图像

};

class Printer : public MatWindow {
private:
    static inline const wchar_t* windowsClassName = L"JarkPrinterWnd";

    // 逻辑尺寸：物理像素由 MatWindow/UiCanvas 按 DPI 换算
    static constexpr int kLogicalWidth = 800;
    static constexpr int kLogicalHeight = 950;

    PrintParams params{};
    TextDrawer textDrawer;
    cv::Mat printerRes, buttonPrint, buttonNormal, buttonInvert, trackbarBg;
    std::vector<cv::Mat> buttonColorMode;
    cv::Mat m_inputBgrMat;  // 输入的图像
    cv::Mat m_uiCanvas;     // UI 画布

    // 控件树：顶栏（模式/正反色/打印）+ 两条拖动条 + 打印按钮热区
    std::unique_ptr<jark::ui::Panel> root;   // 控件树（控件由它持有）

    void Init() {
        rcFileInfo rc = jarkUtils::GetResource(IDB_PNG_PRINTER_RES, L"PNG");
        printerRes = cv::imdecode(cv::Mat(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr), cv::IMREAD_UNCHANGED);

        const bool chinese = jark::prefersChineseResources();
        const int baseX = chinese ? 0 : 800;

        buttonColorMode = {
            printerRes({ baseX, 0, 400, 50 }),
            printerRes({ baseX + 400, 0, 400, 50 }),
            printerRes({ baseX, 50, 400, 50 }),
            printerRes({ baseX + 400, 50, 400, 50 }),
        };
        buttonNormal = printerRes({ baseX, 100, 200, 50 });
        buttonInvert = printerRes({ baseX + 200, 100, 200, 50 });
        buttonPrint = printerRes({ baseX + 400, 100, 200, 50 });
        trackbarBg = printerRes({ baseX, 150, 800, 100 });

        params.brightness = GlobalVar::settingParameter.printerBrightness;
        params.contrast = GlobalVar::settingParameter.printerContrast;
        params.colorMode = GlobalVar::settingParameter.printercolorMode;
        params.invertColors = GlobalVar::settingParameter.printerInvertColors;

        // 异常情况则恢复默认值
        if (params.brightness > 200) params.brightness = 100;
        if (params.contrast > 200) params.contrast = 100;
        if (params.colorMode > 3) params.colorMode = 1;
    }

    // 画布与控件树依赖窗口 DPI，必须在窗口创建之后建立
    void initCanvas() {
        textDrawer.setSize(dp(24));
        m_uiCanvas = cv::Mat(dp(kLogicalHeight), dp(kLogicalWidth), CV_8UC4,
            jarkUtils::to_cv_scalar(GlobalVar::currentTheme.BG));

        const bool dark = GlobalVar::isCurrentUIDarkMode;

        root = std::make_unique<jark::ui::Panel>();

        // 顶栏：颜色模式（四选一，只画选中项）
        auto colorModeGroup = std::make_unique<jark::ui::ImageRadioGroup>(
            buttonColorMode,
            [this]() { return static_cast<int>(params.colorMode); },
            [this](int index) {
                // 从黑白文档/抖动切回彩色或黑白时恢复默认亮度对比度
                if (params.colorMode >= 2 && index <= 1) {
                    params.brightness = 100;
                    params.contrast = 100;
                }
                params.colorMode = static_cast<uint32_t>(index);
                if (index == 2) { // 黑白文档
                    params.brightness = 160;
                    params.contrast = 180;
                }
                else if (index == 3) { // 黑白抖动
                    params.brightness = 80;
                    params.contrast = 100;
                }
                isNeedRefreshUI = true;
            },
            dark, true);
        root->overlay(std::move(colorModeGroup), { 0, 0, 400, 50 });

        // 正色 / 反色
        auto invertGroup = std::make_unique<jark::ui::ImageRadioGroup>(
            std::vector<cv::Mat>{ buttonNormal, buttonInvert },
            [this]() { return params.invertColors ? 1 : 0; },
            [this](int index) {
                params.invertColors = index == 1;
                isNeedRefreshUI = true;
            },
            dark);
        root->overlay(std::move(invertGroup), { 400, 0, 200, 50 });

        // 打印按钮（图片 + 两个热区：另存为 / 确定）
        root->overlay(std::make_unique<jark::ui::ImageView>(&buttonPrint), { 600, 0, 200, 50 });
        root->overlay(std::make_unique<jark::ui::HotArea>([this]() { params.saveToFile = true; }), { 600, 0, 100, 50 });
        root->overlay(std::make_unique<jark::ui::HotArea>([this]() {
            params.confirmed = true;
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }), { 700, 0, 100, 50 });

        // 拖动条背景图（深色主题下反相；成员持有以保证 ImageView 引用的生命周期）
        trackBackgroundImage = trackbarBg;
        if (dark) {
            std::vector<cv::Mat> channels(4);
            cv::split(trackbarBg, channels);
            for (int i = 0; i < 3; ++i)
                channels[i] = 255 - channels[i];
            cv::merge(channels, trackBackgroundImage);
        }
        root->overlay(std::make_unique<jark::ui::ImageView>(&trackBackgroundImage), { 0, 50, 800, 100 });

        // 亮度 / 对比度（轨道与原资源图对齐：x 250 ~ 750）
        root->overlay(std::make_unique<jark::ui::Slider>("", &params.brightness, 200, 250, 500), { 0, 50, 800, 50 });
        root->overlay(std::make_unique<jark::ui::Slider>("", &params.contrast, 200, 250, 500), { 0, 100, 800, 50 });
    }

    cv::Mat trackBackgroundImage; // 拖动条背景

public:
    static inline volatile bool isWorking = false;
    static inline volatile HWND hwnd = nullptr;

    Printer(const cv::Mat& image) {
        requestExitFlag = false;
        isWorking = true;

        Init();
        PrintMatImage(image);
        GlobalVar::settingParameter.printerBrightness = params.brightness;
        GlobalVar::settingParameter.printerContrast = params.contrast;
        GlobalVar::settingParameter.printercolorMode = params.colorMode;
        GlobalVar::settingParameter.printerInvertColors = params.invertColors;

        requestExitFlag = false;
        isWorking = false;
        hwnd = nullptr;
    }

    ~Printer() { }

    static void requestExit() {
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    // 灰度/BGR/BGRA统一到BGR
    cv::Mat matToBGR(const cv::Mat& image) {
        if (image.empty())
            return {};

        cv::Mat bgrMat;
        if (image.channels() == 1) {
            cv::cvtColor(image, bgrMat, cv::COLOR_GRAY2BGR);
        }
        else if (image.channels() == 3) {
            bgrMat = image.clone();
        }
        else if (image.channels() == 4) {
            // Alpha混合白色背景 (255, 255, 255)
            const int width = image.cols;
            const int height = image.rows;
            bgrMat = cv::Mat(height, width, CV_8UC3);
            for (int y = 0; y < height; y++) {
                const BYTE* srcRow = image.ptr<BYTE>(y);
                BYTE* desRow = bgrMat.ptr<BYTE>(y);

                for (int x = 0; x < width; x++) {
                    BYTE b = srcRow[x * 4];
                    BYTE g = srcRow[x * 4 + 1];
                    BYTE r = srcRow[x * 4 + 2];
                    BYTE a = srcRow[x * 4 + 3];

                    if (a == 0) {
                        desRow[x * 3] = 255;
                        desRow[x * 3 + 1] = 255;
                        desRow[x * 3 + 2] = 255;
                    }
                    else if (a == 255) {
                        desRow[x * 3] = b;
                        desRow[x * 3 + 1] = g;
                        desRow[x * 3 + 2] = r;
                    }
                    else {
                        desRow[x * 3] = static_cast<BYTE>((b * a + 255 * (255 - a) + 255) >> 8);     // B
                        desRow[x * 3 + 1] = static_cast<BYTE>((g * a + 255 * (255 - a) + 255) >> 8); // G
                        desRow[x * 3 + 2] = static_cast<BYTE>((r * a + 255 * (255 - a) + 255) >> 8); // R
                    }
                }
            }
        }

        return bgrMat;
    }

    // 均衡全图亮度 再调整亮度对比度 适合打印文档
    HBITMAP MatToHBITMAP(const cv::Mat& image) {
        if (image.empty()) {
            MessageBoxW(nullptr, L"MatToHBITMAP转换图像错误: 空图像", getUIStringW(14), MB_OK | MB_ICONERROR);
            return nullptr;
        }
        if (image.type() != CV_8UC3) {
            MessageBoxW(nullptr, L"MatToHBITMAP转换图像错误: 只接受BGR/CV_8UC3类型图像", getUIStringW(14), MB_OK | MB_ICONERROR);
            return nullptr;
        }

        const int width = image.cols;
        const int height = image.rows;

        const int stride = (width * 3 + 3) & ~3;  // 4字节对齐

        // 准备BITMAPINFO结构
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;          // 24位RGB
        bmi.bmiHeader.biCompression = BI_RGB;
        bmi.bmiHeader.biSizeImage = stride * height;

        // 创建DIB并复制数据
        HDC hdcScreen = GetDC(nullptr);
        void* pBits;
        HBITMAP hBitmap = CreateDIBSection(
            hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdcScreen);

        if (hBitmap) {
            for (int y = 0; y < height; y++) {
                const BYTE* srcRow = image.ptr<BYTE>(height - 1 - y);
                BYTE* dstRow = static_cast<BYTE*>(pBits) + y * stride;
                memcpy(dstRow, srcRow, stride);
            }
        }

        return hBitmap;
    }

    void onPaint(HDC hdc) override {
        if (!m_uiCanvas.empty())
            blitMat(hdc, m_uiCanvas);
    }

    // 鼠标事件统一交给控件树处理（拖动、点击都在控件内部完成）
    void onLButtonDown() override {
        if (!root)
            return;

        jark::ui::UiCanvas canvas(m_uiCanvas, textDrawer, GlobalVar::currentTheme, uiScale());
        rebuildLayout(canvas);
        if (root->onMouseDown(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onLButtonUp() override {
        if (!root)
            return;

        if (root->onMouseUp(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onMouseMove(WPARAM keyState) override {
        (void)keyState;
        if (!root)
            return;

        if (root->onMouseMove(m_x, m_y))
            isNeedRefreshUI = true;
    }

    void onMouseWheel(int delta) override {
        if (!root)
            return;

        jark::ui::UiCanvas canvas(m_uiCanvas, textDrawer, GlobalVar::currentTheme, uiScale());
        rebuildLayout(canvas);
        if (root->onWheel(m_x, m_y, delta))
            isNeedRefreshUI = true;
    }

    void onRButtonUp() override {
        if (GlobalVar::settingParameter.rightClickAction == 1)
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }

    void onKeyDown(WPARAM key) override {
        if (key == VK_ESCAPE)
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }

    // 控件树每次绘制前排布一次（Panel 在 draw 时给子控件分配矩形）
    void rebuildLayout(jark::ui::UiCanvas& canvas) {
        root->bounds = { 0, 0, canvas.width(), canvas.height() };
    }

    void drawingUI() override {
        jark::ui::UiCanvas canvas(m_uiCanvas, textDrawer, GlobalVar::currentTheme, uiScale());
        canvas.fill({ 0, 0, canvas.width(), canvas.height() }, GlobalVar::currentTheme.BG);

        // 预览图：按当前参数处理后在下方居中等比显示
        if (!params.previewImage.empty()) {
            cv::Mat adjusted = params.previewImage.clone();
            jark::applyImageAdjustments(adjusted, params.brightness, params.contrast, params.colorMode, params.invertColors);
            cv::cvtColor(adjusted, adjusted, cv::COLOR_BGR2BGRA);

            const int offsetX = (canvas.width() - adjusted.cols) / 2;
            const int offsetY = canvas.dp(150) + ((canvas.height() - canvas.dp(150)) - adjusted.rows) / 2;
            if (offsetX >= 0 && offsetY >= canvas.dp(150) &&
                offsetX + adjusted.cols <= canvas.width() && offsetY + adjusted.rows <= canvas.height()) {
                adjusted.copyTo(canvas.raw()(cv::Rect(offsetX, offsetY, adjusted.cols, adjusted.rows)));
            }
        }

        rebuildLayout(canvas);
        root->draw(canvas);
    }

    void idleTask() override {
        if (params.saveToFile) {
            params.saveToFile = false;

            std::thread saveImageThread([](cv::Mat image, PrintParams* params) {
                auto [filePath, isJPG] = jarkUtils::saveImageDialogW(getUIStringW(23));
                if (filePath.empty())
                    return;

                jark::applyImageAdjustments(image, params->brightness, params->contrast, params->colorMode, params->invertColors);

                std::vector<uchar> buffer;
                if (cv::imencode(isJPG ? ".jpg" : ".png", image, buffer)) {
                    std::ofstream file(filePath, std::ios::binary);
                    if (file.is_open()) {
                        file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                        file.close();
                    }
                }
                }, m_inputBgrMat.clone(), &params);
            saveImageThread.detach();
        }
    }

    // 打印前预处理
    bool ImagePreprocessingForPrint() {
        if (m_inputBgrMat.empty() || m_inputBgrMat.type() != CV_8UC3) {
            return false;
        }

        // 预览按物理像素宽度缩放（高 DPI 下画布是物理像素）
        const int previewWidth = dp(kLogicalWidth);
        double scale = (double)previewWidth / std::max(m_inputBgrMat.rows, m_inputBgrMat.cols);
        cv::resize(m_inputBgrMat, params.previewImage, cv::Size(), scale, scale);

        // 若长宽差距很极端，超长或超宽，缩放可能异常
        if (params.previewImage.empty() || params.previewImage.rows <= 0 || params.previewImage.cols <= 0) {
            return false;
        }

        // 创建UI窗口
        if (!createWindow(kLogicalWidth, kLogicalHeight, windowsClassName, getUIStringW(40)))
            return false;

        hwnd = m_hwnd;
        initCanvas();
        isNeedRefreshUI = true;
        runMessageLoop();

        // 用户是否确定打印
        return params.confirmed;
    }


    // 打印图像函数
    void PrintMatImage(const cv::Mat& image) {
        m_inputBgrMat = matToBGR(image);

        if (m_inputBgrMat.empty()) {
            MessageBoxW(nullptr, L"图像转换到BGR发生错误", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        if (!jarkUtils::limitSizeTo16K(m_inputBgrMat)) {
            MessageBoxW(nullptr, L"调整图像尺寸发生错误", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        auto confirmFlag = ImagePreprocessingForPrint();

        if (!confirmFlag) { // 取消打印
            return;
        }

        // 初始化打印对话框
        PRINTDLGW pd{};
        pd.lStructSize = sizeof(pd);
        pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;

        // 显示打印对话框
        if (!PrintDlgW(&pd))
            return;
        if (!pd.hDC) {
            MessageBoxW(nullptr, L"无法获得打印机参数", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        // 准备文档信息
        DOCINFOW di{};
        di.cbSize = sizeof(di);
        di.lpszDocName = L"JarkViewer Printed Image";
        di.lpszOutput = nullptr;

        // 开始打印作业
        if (StartDocW(pd.hDC, &di) <= 0) {
            DeleteDC(pd.hDC);
            return;
        }

        // 开始新页面
        if (StartPage(pd.hDC) <= 0) {
            EndDoc(pd.hDC);
            DeleteDC(pd.hDC);
            MessageBoxW(nullptr, L"无法初始化打印业StartPage", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        // 获取打印页面尺寸
        int pageWidth = GetDeviceCaps(pd.hDC, HORZRES);
        int pageHeight = GetDeviceCaps(pd.hDC, VERTRES);

        // 0.9 留5%边距
        double scale = 0.9 * std::min(static_cast<double>(pageWidth) / m_inputBgrMat.cols,
            static_cast<double>(pageHeight) / m_inputBgrMat.rows);
        int newWidth = static_cast<int>(std::round(m_inputBgrMat.cols * scale));
        int newHeight = static_cast<int>(std::round(m_inputBgrMat.rows * scale));

        // 缩放图像
        cv::Mat resized;
        cv::resize(m_inputBgrMat, resized, cv::Size(newWidth, newHeight), 0, 0);
        // 使用之前调整的参数处理图像
        jark::applyImageAdjustments(resized, params.brightness, params.contrast, params.colorMode, params.invertColors);

        cv::Mat output(pageHeight, pageWidth, m_inputBgrMat.type(), cv::Scalar(255, 255, 255));
        int offsetX = (pageWidth - newWidth + 1) / 2;  // +1确保偶数差时居中
        int offsetY = (pageHeight - newHeight + 1) / 2;

        cv::Mat roi(output, cv::Rect(offsetX, offsetY, newWidth, newHeight));
        resized.copyTo(roi);

        HBITMAP hBitmap = MatToHBITMAP(output);
        if (!hBitmap) {
            MessageBoxW(nullptr, L"无法转换图像到打印格式HBITMAP", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        // 创建内存DC
        HDC hdcMem = CreateCompatibleDC(pd.hDC);
        SelectObject(hdcMem, hBitmap);

        SetStretchBltMode(pd.hDC, COLORONCOLOR); // 尺寸一致，无需缩放
        SetBrushOrgEx(pd.hDC, 0, 0, nullptr);

        // 绘制到打印机DC
        StretchBlt(
            pd.hDC, 0, 0, pageWidth, pageHeight,
            hdcMem, 0, 0, pageWidth, pageHeight, SRCCOPY
        );

        // 清理资源
        DeleteDC(hdcMem);
        DeleteObject(hBitmap);

        // 结束页面和文档
        EndPage(pd.hDC);
        EndDoc(pd.hDC);
        DeleteDC(pd.hDC);

        return;
    }

};