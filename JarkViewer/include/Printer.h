#pragma once

#include "jarkUtils.h"
#include "TextDrawer.h"
#include "D2D1App.h"

// 全局变量存储UI状态
struct PrintParams {
    //double topMargin = 5.0;       // 上边距百分比
    //double bottomMargin = 5.0;    // 下边距百分比
    //double leftMargin = 5.0;      // 左边距百分比
    //double rightMargin = 5.0;     // 右边距百分比
    //int layoutMode = 0;           // 0=适应, 1=填充, 2=原始比例
    uint32_t brightness = 100;      // 亮度调整 (0 ~ 200)
    uint32_t contrast = 100;        // 对比度调整 (0 ~ 200)
    uint32_t colorMode = 1;         // 颜色模式 0:彩色  1:黑白  2:黑白文档 3:黑白抖动(二值像素)
    bool invertColors = false;      // 是否反相

    bool isParamsChange = false;
    bool confirmed = false;       // 用户点击确认
    bool saveToFile = false;      // 保存到文件
    bool mousePressing = false;   // 鼠标左键是否按住状态
    bool mousePressingBrightnessBar = false;
    bool mousePressingContrastBar = false;
    cv::Mat previewImage;         // 预览图像

    const char* windowsName = nullptr;
};

class Printer {
private:
    string windowsNameAnsi = jarkUtils::utf8ToAnsi(getUIString(29));
    const char* windowsName = windowsNameAnsi.c_str();

    PrintParams params{};
    TextDrawer textDrawer;
    cv::Mat printerRes, buttonPrint, buttonNormal, buttonInvert, trackbarBg;
    std::vector<cv::Mat> buttonColorMode;

    static inline volatile bool requestExitFlag = false;

    void Init() {
        textDrawer.setSize(24);
        params.windowsName = windowsNameAnsi.c_str();

        rcFileInfo rc;
        rc = jarkUtils::GetResource(IDB_PNG_PRINTER_RES, L"PNG");
        printerRes = cv::imdecode(cv::Mat(1, (int)rc.size, CV_8UC1, (uint8_t*)rc.ptr), cv::IMREAD_UNCHANGED);

        if (GlobalVar::settingParameter.UI_LANG == 0) {
            buttonColorMode.push_back(printerRes({ 0, 0, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 400, 0, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 0, 50, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 400, 50, 400, 50 }));

            buttonNormal = printerRes({ 0, 100, 200, 50 });
            buttonInvert = printerRes({ 200, 100, 200, 50 });

            buttonPrint = printerRes({ 400, 100, 200, 50 });
            trackbarBg = printerRes({ 0, 150, 800, 100 });
        }
        else {
            buttonColorMode.push_back(printerRes({ 800, 0, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 1200, 0, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 800, 50, 400, 50 }));
            buttonColorMode.push_back(printerRes({ 1200, 50, 400, 50 }));

            buttonNormal = printerRes({ 800, 100, 200, 50 });
            buttonInvert = printerRes({ 1000, 100, 200, 50 });

            buttonPrint = printerRes({ 1200, 100, 200, 50 });
            trackbarBg = printerRes({ 800, 150, 800, 100 });
        }

        params.brightness = GlobalVar::settingParameter.printerBrightness;
        params.contrast = GlobalVar::settingParameter.printerContrast;
        params.colorMode = GlobalVar::settingParameter.printercolorMode;
        params.invertColors = GlobalVar::settingParameter.printerInvertColors;

        // 异常情况则恢复默认值
        if (params.brightness > 200) params.brightness = 100;
        if (params.contrast > 200) params.contrast = 100;
        if (params.colorMode > 3) params.colorMode = 1;
    }

public:
    static inline volatile bool isWorking = false;
    static inline volatile HWND hwnd = nullptr;

    Printer(cv::Mat image) {
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
        requestExitFlag = true;
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
    static cv::Mat balancedImageBrightness(const cv::Mat& input_img) {
        if (input_img.type() != CV_8UC3) {
            MessageBoxW(nullptr, L"balancedImageBrightness转换图像错误: 只接受BGR/CV_8UC3类型图像", getUIStringW(14), MB_OK | MB_ICONERROR);
            return {};
        }

        int kernel_size = std::max(input_img.cols, input_img.rows) / 20;
        if (kernel_size < 3)
            kernel_size = 3;

        // 确保核大小为奇数
        kernel_size |= 1;

        // 转换为灰度图像
        cv::Mat gray;
        if (input_img.channels() == 3) {
            cvtColor(input_img, gray, cv::COLOR_BGR2GRAY);
        }
        else {
            gray = input_img.clone();
        }

        // 估计背景（使用大核模糊）
        cv::Mat background;
        GaussianBlur(gray, background, cv::Size(kernel_size, kernel_size), 0);

        // 从原始图像中减去背景
        cv::Mat corrected;
        // 相当于：corrected = gray * 1 + background * (-1) + 128
        addWeighted(gray, 1.0, background, -1.0, 128, corrected);

        // 增强对比度（归一化到0-255范围）
        normalize(corrected, corrected, 0, 255, cv::NORM_MINMAX);
        corrected.convertTo(corrected, CV_8U);

        cvtColor(corrected, corrected, cv::COLOR_GRAY2BGR);
        return corrected;
    }


    // 将cv::Mat转换为HBITMAP
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

    static void adjustBrightnessContrast(cv::Mat& src, uint32_t brightnessInt, uint32_t contrastInt) {
        if (src.empty() || src.type() != CV_8UC3)
            return;

        // 取值不能极端
        if (brightnessInt < 1)
            brightnessInt = 1;
        else if (brightnessInt > 199)
            brightnessInt = 199;

        if (contrastInt > 200)
            contrastInt = 200;

        // brightness: 0 ~ 200 映射到 0 ~ 2.0
        // contrast:   0 ~ 200 映射到 0 ~ 2.0
        double brightness = brightnessInt / 100.0;
        double contrast = contrastInt / 100.0;

        brightness = pow(2.0 - brightness, 3); // 增大对比度比例
        contrast = pow(contrast, 3); // 增大对比度比例

        for (int y = 0; y < src.rows; y++) {
            for (int x = 0; x < src.cols; x++) {
                cv::Vec3b pixel = src.at<cv::Vec3b>(y, x);
                for (int c = 0; c < 3; c++) {
                    // 以128为中心进行对比度调整
                    double adjusted = (pixel[c] - 128.0) * contrast + 128.0;

                    // 添加亮度偏移
                    adjusted = pow(adjusted / 255.0, brightness) * 255;

                    // 确保值在0-255范围内
                    pixel[c] = cv::saturate_cast<uchar>(adjusted);
                }
                src.at<cv::Vec3b>(y, x) = pixel;
            }
        }
    }

    // 图像处理 调整对比度 彩色 黑白
    static void ApplyImageAdjustments(cv::Mat& image, uint32_t brightness, uint32_t contrast, uint32_t colorMode, bool invertColors) {
        if (image.empty()) return;

        if (colorMode == 1 || colorMode == 3) { // 黑白、黑白抖动也需要先灰度
            cv::Mat gray;
            cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, image, cv::COLOR_GRAY2BGR);
        }else if (colorMode == 2) {
            image = balancedImageBrightness(image);
        }

        // brightness: 0 ~ 200, 100是中间值，亮度不增不减
        // contrast:   0 ~ 200, 100是中间值，对比度不增不减
        adjustBrightnessContrast(image, brightness, contrast);

        if (colorMode == 3) // 最后处理 黑白抖动
            floydSteinbergDithering(image);

        if (invertColors) {
            cv::bitwise_not(image, image);
        }
    }

    // 误差扩散抖动算法
    static void floydSteinbergDithering(cv::Mat& image) {
        cv::cvtColor(image, image, cv::COLOR_BGR2GRAY);

        int height = image.rows;
        int width = image.cols;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                // 获取当前像素值
                uchar oldVal = image.at<uchar>(y, x);
                // 二值化：大于阈值设为255（白），否则0（黑）
                uchar newVal = (oldVal < 128) ? 0 : 255;
                image.at<uchar>(y, x) = newVal;

                // 计算误差
                int error = oldVal - newVal;

                // 将误差按比例分配到周围像素（Floyd-Steinberg权重）
                if (x + 1 < width) {
                    image.at<uchar>(y, x + 1) = cv::saturate_cast<uchar>(
                        image.at<uchar>(y, x + 1) + error * 7 / 16
                    );
                }
                if (y + 1 < height) {
                    if (x - 1 >= 0) {
                        image.at<uchar>(y + 1, x - 1) = cv::saturate_cast<uchar>(
                            image.at<uchar>(y + 1, x - 1) + error * 3 / 16
                        );
                    }
                    image.at<uchar>(y + 1, x) = cv::saturate_cast<uchar>(
                        image.at<uchar>(y + 1, x) + error * 5 / 16
                    );
                    if (x + 1 < width) {
                        image.at<uchar>(y + 1, x + 1) = cv::saturate_cast<uchar>(
                            image.at<uchar>(y + 1, x + 1) + error * 1 / 16
                        );
                    }
                }
            }
        }
        cv::cvtColor(image, image, cv::COLOR_GRAY2BGR);
    }

    void drawProgressBar(cv::Mat& image, cv::Rect rect, double progress) {
        cv::rectangle(image, rect, cv::Scalar(255, 0, 0, 255), 2);

        const int progresswidth = static_cast<int>(rect.width * progress);
        if (0 < progresswidth && progresswidth <= rect.width) {
            rect.width = progresswidth;
            cv::rectangle(image, rect, cv::Scalar(204, 72, 63, 255), -1);
        }
    }


    void refreshUI() {

        cv::Mat adjusted = params.previewImage.clone();
        ApplyImageAdjustments(adjusted, params.brightness, params.contrast, params.colorMode, params.invertColors);

        // 扩展为正方形画布
        int outputSize = std::max(adjusted.rows, adjusted.cols);
        int x = (outputSize - adjusted.cols) / 2;
        int y = (outputSize - adjusted.rows) / 2;

        // 画布高度+150 放置三行底栏：两行拖动条，一行按钮
        cv::Mat squareMat(outputSize + 150, outputSize, CV_8UC3, cv::Scalar(255, 255, 255));
        cv::Mat roi = squareMat(cv::Rect(x, y + 150, adjusted.cols, adjusted.rows));
        adjusted.copyTo(roi);

        cv::cvtColor(squareMat, squareMat, cv::COLOR_BGR2BGRA);

        jarkUtils::overlayImg(squareMat, buttonColorMode[params.colorMode], 0, 0);
        jarkUtils::overlayImg(squareMat, params.invertColors ? buttonInvert : buttonNormal, 400, 0);
        jarkUtils::overlayImg(squareMat, buttonPrint, 600, 0);
        jarkUtils::overlayImg(squareMat, trackbarBg, 0, 50);

        textDrawer.putAlignLeft(squareMat, { 120, 60, 200, 900 }, std::format("{:3} %", params.brightness).c_str(), { 0, 0, 0, 255 });
        textDrawer.putAlignLeft(squareMat, { 120, 110, 200, 900 }, std::format("{:3} %", params.contrast).c_str(), { 0, 0, 0, 255 });

        drawProgressBar(squareMat, { 250, 60, 500, 30 }, params.brightness / 200.0);
        drawProgressBar(squareMat, { 250, 110, 500, 30 }, params.contrast / 200.0);

        cv::imshow(windowsName, squareMat);
    }


    static void mouseCallback(int event, int x, int y, int flags, void* userdata) {
        PrintParams* params = static_cast<PrintParams*>(userdata);

        switch (event) {

        case cv::EVENT_RBUTTONUP: { // 右键
            if (GlobalVar::settingParameter.rightClickAction == 1) {
                requestExit();
                return;
            }
        }break;

        case cv::EVENT_MOUSEWHEEL: {
            auto wheelValue = cv::getMouseWheelDelta(flags);

            if ((100 < x) && (x < 800) && (50 < y) && (y < 100)) {
                params->brightness += (wheelValue > 0) ? 1 : -1;
                params->isParamsChange = true;
                if (params->brightness > INT_MAX)params->brightness = 0;
                if (params->brightness > 200)params->brightness = 200;
            }
            else if ((100 < x) && (x < 800) && (100 < y) && (y < 150)) {
                params->contrast += (wheelValue > 0) ? 1 : -1;
                params->isParamsChange = true;
                if (params->contrast > INT_MAX)params->contrast = 0;
                if (params->contrast > 200)params->contrast = 200;
            }
        }break;

        case cv::EVENT_MOUSEMOVE: {
            if (params->mousePressing) {
                if (params->mousePressingBrightnessBar) {
                    params->brightness = x < 250 ? 0 : (x > 750 ? 200 : ((x - 250) * 200 / 500));
                    params->isParamsChange = true;
                }
                else if (params->mousePressingContrastBar) {
                    params->contrast = x < 250 ? 0 : (x > 750 ? 200 : ((x - 250) * 200 / 500));
                    params->isParamsChange = true;
                }
            }
        }break;

        case cv::EVENT_LBUTTONDOWN: {
            params->mousePressing = true;

            if ((200 < x) && (x <= 800) && (50 < y) && (y < 100)) {
                params->mousePressingBrightnessBar = true;
                params->brightness = x < 250 ? 0 : (x > 750 ? 200 : ((x - 250) * 200 / 500));
                params->isParamsChange = true;
            }
            if ((200 < x) && (x <= 800) && (100 < y) && (y < 150)) {
                params->mousePressingContrastBar = true;
                params->contrast = x < 250 ? 0 : (x > 750 ? 200 : ((x - 250) * 200 / 500));
                params->isParamsChange = true;
            }
        }break;

        case cv::EVENT_LBUTTONUP: {
            params->mousePressing = false;
            params->mousePressingBrightnessBar = false;
            params->mousePressingContrastBar = false;

            if ((0 < x) && (x < 100) && (y < 50)) { // 彩色
                if (params->colorMode != 0) {
                    if (params->colorMode >= 2) { // 若之前是黑白文档/抖动模式，则恢复默认亮度对比度
                        params->brightness = 100;
                        params->contrast = 100;
                    }
                    params->colorMode = 0;
                    params->isParamsChange = true;
                }
            }

            if ((100 < x) && (x < 200) && (y < 50)) { // 黑白
                if (params->colorMode != 1) {
                    if (params->colorMode >= 2) { // 若之前是黑白文档/抖动模式，则恢复默认亮度对比度
                        params->brightness = 100;
                        params->contrast = 100;
                    }
                    params->colorMode = 1;
                    params->isParamsChange = true;
                }
            }

            if ((200 < x) && (x < 300) && (y < 50)) { // 黑白文档
                if (params->colorMode != 2) {
                    params->colorMode = 2;
                    params->brightness = 160;
                    params->contrast = 180;
                    params->isParamsChange = true;
                }
            }

            if ((300 < x) && (x < 400) && (y < 50)) { // 黑白抖动
                if (params->colorMode != 3) {
                    params->colorMode = 3;
                    params->brightness = 80;
                    params->contrast = 100;
                    params->isParamsChange = true;
                }
            }

            if ((400 < x) && (x < 500) && (y < 50)) { // 正色
                if (params->invertColors) {
                    params->invertColors = false;
                    params->isParamsChange = true;
                }
            }
            if ((500 < x) && (x < 600) && (y < 50)) { // 反色
                if (!params->invertColors) {
                    params->invertColors = true;
                    params->isParamsChange = true;
                }
            }
            if ((600 < x) && (x < 700) && (y < 50)) { //另存为
                params->saveToFile = true;
            }
            if ((700 < x) && (x < 800) && (y < 50)) { // 确定按钮
                params->confirmed = true;
                params->isParamsChange = true;
                requestExit();
            }
        }break;
        }
    }

    // 打印前预处理
    bool ImagePreprocessingForPrint(const cv::Mat& image) {
        if (image.empty() || image.type() != CV_8UC3) {
            return false;
        }

        // 创建预览图像
        const int previewWidth = 800;
        const int previewHeight = 950;

        double scale = (double)previewWidth / std::max(image.rows, image.cols);
        cv::resize(image, params.previewImage, cv::Size(), scale, scale);

        // 若长宽差距很极端，超长或超宽，缩放可能异常
        if (params.previewImage.empty() || params.previewImage.rows <= 0 || params.previewImage.cols <= 0) {
            return false;
        }

        // 创建UI窗口
        cv::namedWindow(windowsName, cv::WINDOW_AUTOSIZE);
        cv::resizeWindow(windowsName, previewWidth, previewHeight);

        cv::setMouseCallback(windowsName, mouseCallback, &params);

        // 初始预览
        refreshUI();

        hwnd = FindWindowA(NULL, windowsName);
        if (hwnd) {
            jarkUtils::disableWindowResize(hwnd);

            HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCE(IDI_JARKVIEWER));
            if (hIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }
            BOOL themeMode = GlobalVar::CURRENT_UI_MODE == 1 ? 0 : 1;
            DwmSetWindowAttribute(hwnd, DWMWINDOWATTRIBUTE::DWMWA_USE_IMMERSIVE_DARK_MODE, &themeMode, sizeof(BOOL));
        }

        // 事件循环
        while (cv::getWindowProperty(windowsName, cv::WND_PROP_VISIBLE) > 0) {
            if (params.isParamsChange) {
                params.isParamsChange = false;
                refreshUI();
            }

            if (cv::waitKey(10) == 27) // ESC
                requestExit();
            if (requestExitFlag || params.confirmed) {
                cv::destroyWindow(params.windowsName);
                break;
            }

            // 保存到文件
            if (params.saveToFile) {
                params.saveToFile = false;

                std::thread saveImageThread([](cv::Mat image, PrintParams* params) {
                    auto [filePath, isJPG] = jarkUtils::saveImageDialogW(getUIStringW(23));
                    if (filePath.empty())
                        return;

                    ApplyImageAdjustments(image, params->brightness, params->contrast, params->colorMode, params->invertColors);

                    std::vector<uchar> buffer;
                    if (cv::imencode(isJPG ? ".jpg" : ".png", image, buffer)) {
                        std::ofstream file(filePath, std::ios::binary);
                        if (file.is_open()) {
                            file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
                            file.close();
                        }
                    }
                    }, image.clone(), &params);
                saveImageThread.detach();
            }
        }

        // 用户是否确定打印
        return params.confirmed;
    }


    // 打印图像函数
    void PrintMatImage(const cv::Mat& image) {
        auto bgrMat = matToBGR(image);

        if (bgrMat.empty()) {
            MessageBoxW(nullptr, L"图像转换到BGR发生错误", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        if (!jarkUtils::limitSizeTo16K(bgrMat)) {
            MessageBoxW(nullptr, L"调整图像尺寸发生错误", getUIStringW(14), MB_OK | MB_ICONERROR);
            return;
        }

        auto confirmFlag = ImagePreprocessingForPrint(bgrMat);

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
        double scale = 0.9 * std::min(static_cast<double>(pageWidth) / bgrMat.cols,
            static_cast<double>(pageHeight) / bgrMat.rows);
        int newWidth = static_cast<int>(std::round(bgrMat.cols * scale));
        int newHeight = static_cast<int>(std::round(bgrMat.rows * scale));

        // 缩放图像
        cv::Mat resized;
        cv::resize(bgrMat, resized, cv::Size(newWidth, newHeight), 0, 0);

        // 使用之前调整的参数处理图像
        ApplyImageAdjustments(resized, params.brightness, params.contrast, params.colorMode, params.invertColors);

        cv::Mat output(pageHeight, pageWidth, bgrMat.type(), cv::Scalar(255, 255, 255));
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