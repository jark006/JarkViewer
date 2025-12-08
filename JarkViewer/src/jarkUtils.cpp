#pragma once

#include "jarkUtils.h"


std::string jarkUtils::bin2Hex(const void* bytes, const size_t len) {
    auto charList = "0123456789ABCDEF";
    if (len == 0) return "";
    string res(len * 3 - 1, ' ');
    for (size_t i = 0; i < len; i++) {
        const uint8_t value = reinterpret_cast<const uint8_t*>(bytes)[i];
        res[i * 3] = charList[value >> 4];
        res[i * 3 + 1] = charList[value & 0x0f];
    }
    return res;
}

std::wstring jarkUtils::ansiToWstring(string_view str) {
    if (str.empty())return L"";

    int wcharLen = MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.length(), nullptr, 0);
    if (wcharLen == 0) return L"";

    std::wstring ret(wcharLen, 0);
    MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.length(), ret.data(), wcharLen);

    return ret;
}

std::string jarkUtils::wstringToAnsi(wstring_view wstr) {
    if (wstr.empty())return "";

    int byteLen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), (int)wstr.length(), nullptr, 0, nullptr, nullptr);
    if (byteLen == 0) return "";

    std::string ret(byteLen, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.data(), (int)wstr.length(), ret.data(), byteLen, nullptr, nullptr);
    return ret;
}

std::wstring jarkUtils::utf8ToWstring(string_view str) {
    if (str.empty())return L"";

    int wcharLen = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.length(), nullptr, 0);
    if (wcharLen == 0) return L"";

    std::wstring ret(wcharLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.length(), ret.data(), wcharLen);

    return ret;
}

std::string jarkUtils::wstringToUtf8(wstring_view wstr) {
    if (wstr.empty())return "";

    int byteLen = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.length(), nullptr, 0, nullptr, nullptr);
    if (byteLen == 0) return "";

    std::string ret(byteLen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.length(), ret.data(), byteLen, nullptr, nullptr);
    return ret;
}

std::wstring jarkUtils::latin1ToWstring(string_view str) {
    if (str.empty())return L"";

    int wcharLen = MultiByteToWideChar(1252, 0, str.data(), (int)str.length(), nullptr, 0);
    if (wcharLen == 0) return L"";

    std::wstring ret(wcharLen, 0);
    MultiByteToWideChar(1252, 0, str.data(), (int)str.length(), ret.data(), wcharLen);

    return ret;
}

std::string jarkUtils::utf8ToAnsi(string_view str) {
    return wstringToAnsi(utf8ToWstring(str));
}

std::string jarkUtils::ansiToUtf8(string_view str) {
    return wstringToUtf8(ansiToWstring(str));
}

rcFileInfo jarkUtils::GetResource(unsigned int idi, const wchar_t* type) {
    rcFileInfo rc{};

    HMODULE ghmodule = GetModuleHandleW(nullptr);
    if (!ghmodule) {
        log("ghmodule nullptr");
        return rc;
    }

    HRSRC hrsrc = FindResourceW(ghmodule, MAKEINTRESOURCEW(idi), type);
    if (!hrsrc) {
        log("hrstc nullptr");
        return rc;
    }

    HGLOBAL hg = LoadResource(ghmodule, hrsrc);
    if (!hg) {
        log("hg nullptr");
        return rc;
    }

    rc.ptr = (unsigned char*)(LockResource(hg));
    rc.size = SizeofResource(ghmodule, hrsrc);
    return rc;
}

std::string jarkUtils::size2Str(const size_t fileSize) {
    if (fileSize < 1024) return std::format("{} Bytes", fileSize);
    if (fileSize < 1024ULL * 1024) return std::format("{:.1f} KiB", fileSize / 1024.0);
    if (fileSize < 1024ULL * 1024 * 1024) return std::format("{:.1f} MiB", fileSize / (1024.0 * 1024));
    return std::format("{:.1f} GiB", fileSize / (1024.0 * 1024 * 1024));
}

std::string jarkUtils::timeStamp2Str(time_t timeStamp) {
    timeStamp += 8ULL * 3600; // UTC+8
    std::tm* ptm = std::gmtime(&timeStamp);
    std::stringstream ss;
    ss << std::put_time(ptm, "%Y-%m-%d %H:%M:%S UTC+8");
    return ss.str();
}

WinSize jarkUtils::getWindowSize(HWND hwnd) {
    RECT rect;
    if (GetClientRect(hwnd, &rect)) {
        return { rect.right,rect.bottom };
    }
    else {
        log("getWindowSize fail.");
        return { 0,0 };
    }
}

// 设置窗口图标
void jarkUtils::setWindowIcon(HWND hWnd, WORD wIconId) {
    HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCE(wIconId));
    if (hIcon) {
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
}

// 禁止窗口调整尺寸
void jarkUtils::disableWindowResize(HWND hwnd) {
    if (hwnd) {
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        style &= ~WS_THICKFRAME;   // 移除 WS_THICKFRAME 样式即可禁止拉伸
        style &= ~WS_MAXIMIZEBOX;  // 移除最大化按钮
        SetWindowLongW(hwnd, GWL_STYLE, style);
    }
}

bool jarkUtils::copyToClipboard(wstring_view text) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }

    EmptyClipboard();

    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!hGlob) {
        CloseClipboard();
        return false;
    }

    auto desPtr = GlobalLock(hGlob);
    if (desPtr) {
        memcpy(desPtr, text.data(), (text.size() + 1) * sizeof(wchar_t));
        ((wchar_t*)desPtr)[text.size() + 1] = 0; // wstring_view 尾部不能保证有 '\0' 终止符
    }
    GlobalUnlock(hGlob);

    if (!SetClipboardData(CF_UNICODETEXT, hGlob)) {
        CloseClipboard();
        GlobalFree(hGlob);
        return false;
    }

    CloseClipboard();
    return true;
}

bool jarkUtils::limitSizeTo16K(cv::Mat& image) {
    // 检查并缩放图像（保持宽高比）
    if (image.cols > 16384 || image.rows > 16384) {
        double scale = std::min(16384.0 / image.cols, 16384.0 / image.rows);
        int newWidth = static_cast<int>(image.cols * scale);
        int newHeight = static_cast<int>(image.rows * scale);
        cv::resize(image, image, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_LINEAR);
        MessageBoxW(nullptr, std::format(L"{} {}x{}", getUIStringW(16), image.cols, image.rows).c_str(), getUIStringW(15), MB_OK | MB_ICONWARNING);
    }
    return true;
}

// Alpha透明通道混合白色背景
void jarkUtils::flattenRGBAonWhite(cv::Mat& image) {
    if (image.empty() || image.type() != CV_8UC4)
        return;

    const int width = image.cols;
    const int height = image.rows;

    for (int y = 0; y < height; y++) {
        BYTE* srcRow = image.ptr<BYTE>(y);

        for (int x = 0; x < width; x++) {
            BYTE b = srcRow[x * 4];
            BYTE g = srcRow[x * 4 + 1];
            BYTE r = srcRow[x * 4 + 2];
            BYTE a = srcRow[x * 4 + 3];

            if (a == 0) {
                srcRow[x * 4] = 255;
                srcRow[x * 4 + 1] = 255;
                srcRow[x * 4 + 2] = 255;
                srcRow[x * 4 + 3] = 255;
            }
            else if (a != 255) {
                srcRow[x * 4] = static_cast<BYTE>((b * a + 255 * (255 - a) + 255) >> 8);
                srcRow[x * 4 + 1] = static_cast<BYTE>((g * a + 255 * (255 - a) + 255) >> 8);
                srcRow[x * 4 + 2] = static_cast<BYTE>((r * a + 255 * (255 - a) + 255) >> 8);
                srcRow[x * 4 + 3] = 255;
            }
        }
    }
}

void jarkUtils::copyImageToClipboard(const cv::Mat& image) {
    if (image.empty()) {
        MessageBoxW(nullptr, getUIStringW(17), getUIStringW(14), MB_OK | MB_ICONERROR);
        return;
    }

    cv::Mat processedImage;

    // 转换为BGRA格式
    try {
        if (image.channels() == 1) {
            cv::cvtColor(image, processedImage, cv::COLOR_GRAY2BGRA);
        }
        else if (image.channels() == 3) {
            cv::cvtColor(image, processedImage, cv::COLOR_BGR2BGRA);
        }
        else if (image.channels() == 4) {
            processedImage = image.clone();
        }
        else {
            MessageBoxW(nullptr, std::format(L"{} {}", getUIStringW(18), processedImage.channels()).c_str(), getUIStringW(19), MB_OK | MB_ICONERROR);
            return;
        }
    }
    catch (const cv::Exception& e) {
        MessageBoxW(nullptr, jarkUtils::utf8ToWstring(e.what()).c_str(), getUIStringW(20), MB_OK | MB_ICONERROR);
        return;
    }

    // 检查并缩放图像（保持宽高比）
    if (!limitSizeTo16K(processedImage))
        return;

    const int width = processedImage.cols;
    const int height = processedImage.rows;


    // 打开剪贴板
    if (!OpenClipboard(nullptr)) {
        MessageBoxW(nullptr, getUIStringW(21), getUIStringW(14), MB_OK | MB_ICONERROR);
        return;
    }

    // 清空剪贴板
    if (!EmptyClipboard()) {
        CloseClipboard();
        MessageBoxW(nullptr, getUIStringW(22), getUIStringW(14), MB_OK | MB_ICONERROR);
        return;
    }

    int dibStride = width * 4;
    int imageDataSize = dibStride * height;

    // 创建CF_DIBV5格式数据
    int dibv5Size = sizeof(BITMAPV5HEADER) + imageDataSize;
    HGLOBAL hDibV5 = GlobalAlloc(GMEM_MOVEABLE, dibv5Size);
    if (hDibV5) {
        LPVOID pDibV5 = GlobalLock(hDibV5);
        if (pDibV5) {
            // 填充BITMAPV5HEADER
            BITMAPV5HEADER* pBmpV5Header = static_cast<BITMAPV5HEADER*>(pDibV5);
            memset(pBmpV5Header, 0, sizeof(BITMAPV5HEADER));

            pBmpV5Header->bV5Size = sizeof(BITMAPV5HEADER);
            pBmpV5Header->bV5Width = width;
            pBmpV5Header->bV5Height = height;
            pBmpV5Header->bV5Planes = 1;
            pBmpV5Header->bV5BitCount = 32;
            pBmpV5Header->bV5Compression = BI_BITFIELDS;
            pBmpV5Header->bV5SizeImage = imageDataSize;
            pBmpV5Header->bV5XPelsPerMeter = 0;
            pBmpV5Header->bV5YPelsPerMeter = 0;
            pBmpV5Header->bV5ClrUsed = 0;
            pBmpV5Header->bV5ClrImportant = 0;

            // 设置颜色掩码 (BGRA格式)
            pBmpV5Header->bV5RedMask = 0x00FF0000;
            pBmpV5Header->bV5GreenMask = 0x0000FF00;
            pBmpV5Header->bV5BlueMask = 0x000000FF;
            pBmpV5Header->bV5AlphaMask = 0xFF000000;

            // 设置颜色空间
            pBmpV5Header->bV5CSType = LCS_sRGB;
            pBmpV5Header->bV5Intent = LCS_GM_GRAPHICS;

            // 复制图像数据
            BYTE* pImageData = static_cast<BYTE*>(pDibV5) + sizeof(BITMAPV5HEADER);
            for (int y = 0; y < height; y++) {
                const BYTE* srcRow = processedImage.ptr<BYTE>(height - 1 - y);
                BYTE* dstRow = pImageData + y * dibStride;
                memcpy(dstRow, srcRow, width * 4ULL);
            }

            GlobalUnlock(hDibV5);

            // 设置到剪贴板
            if (!SetClipboardData(CF_DIBV5, hDibV5)) {
                GlobalFree(hDibV5);
            }
        }
        else {
            GlobalFree(hDibV5);
        }
    }

    // 将透明像素混合白色背景
    flattenRGBAonWhite(processedImage);

    // 创建CF_DIB格式数据
    int dibSize = sizeof(BITMAPINFOHEADER) + imageDataSize;
    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (hDib) {
        LPVOID pDib = GlobalLock(hDib);
        if (pDib) {
            // 填充BITMAPINFOHEADER
            BITMAPINFOHEADER* pBmpHeader = static_cast<BITMAPINFOHEADER*>(pDib);
            ZeroMemory(pBmpHeader, sizeof(BITMAPINFOHEADER));

            pBmpHeader->biSize = sizeof(BITMAPINFOHEADER);
            pBmpHeader->biWidth = width;
            pBmpHeader->biHeight = height;
            pBmpHeader->biPlanes = 1;
            pBmpHeader->biBitCount = 32;
            pBmpHeader->biCompression = BI_RGB; // CF_DIB通常使用BI_RGB
            pBmpHeader->biSizeImage = imageDataSize;

            // 复制图像数据
            BYTE* pImageData = static_cast<BYTE*>(pDib) + sizeof(BITMAPINFOHEADER);
            for (int y = 0; y < height; y++) {
                const BYTE* srcRow = processedImage.ptr<BYTE>(height - 1 - y);
                BYTE* dstRow = pImageData + y * dibStride;
                memcpy(dstRow, srcRow, width * 4ULL);
            }

            GlobalUnlock(hDib);

            // 设置到剪贴板
            if (!SetClipboardData(CF_DIB, hDib)) {
                GlobalFree(hDib);
            }
        }
        else {
            GlobalFree(hDib);
        }
    }

    // 关闭剪贴板
    CloseClipboard();
}


// 创建路径几何图形
ID2D1PathGeometry* jarkUtils::GetPathGeometry(ID2D1Factory4* pD2DFactory, D2D1_POINT_2F* points, UINT pointsCount)
{
    ID2D1PathGeometry* geometry = nullptr;
    HRESULT hr = pD2DFactory->CreatePathGeometry(&geometry);

    if (SUCCEEDED(hr))
    {
        ID2D1GeometrySink* pSink = nullptr;
        hr = geometry->Open(&pSink); // 获取Sink对象

        if (SUCCEEDED(hr))
        {
            pSink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);

            pSink->AddLines(points, pointsCount);

            pSink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }

        pSink->Close(); // 关闭Sink对象

        return geometry;
    }

    return nullptr;
}

void jarkUtils::ToggleFullScreen(HWND hwnd) {
    static RECT preRect{};
    static DWORD preStyle = 0;
    static DWORD preExStyle = 0;

    static bool isFullScreen = false;

    if (isFullScreen) {
        // 退出全屏模式，恢复之前的窗口状态
        SetWindowLong(hwnd, GWL_STYLE, preStyle);
        SetWindowLong(hwnd, GWL_EXSTYLE, preExStyle);
        SetWindowPos(hwnd, nullptr, preRect.left, preRect.top,
            preRect.right - preRect.left,
            preRect.bottom - preRect.top,
            SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    else {
        // 保存当前窗口状态
        GetWindowRect(hwnd, &preRect);
        preStyle = GetWindowLong(hwnd, GWL_STYLE);
        preExStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

        // 切换到全屏模式
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        GetMonitorInfo(monitor, &monitorInfo);

        SetWindowLong(hwnd, GWL_STYLE, preStyle & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowLong(hwnd, GWL_EXSTYLE, preExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        SetWindowPos(hwnd, HWND_TOP,
            monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    isFullScreen = !isFullScreen;
}

// 假设 canvas 完全没有透明像素
void jarkUtils::overlayImg(cv::Mat& canvas, cv::Mat& img, int xOffset, int yOffset) {
    if (canvas.type() != CV_8UC4 || img.type() != CV_8UC4)
        return;

    int canvasHeight = canvas.rows;
    int canvasWidth = canvas.cols;
    int imgHeight = img.rows;
    int imgWidth = img.cols;

    for (int y = 0; y < imgHeight; y++) {
        int canvasY = yOffset + y;
        if (canvasY < 0)
            continue;
        if (canvasHeight <= canvasY)
            break;

        auto canvasPtr = canvas.ptr<intUnion>(canvasY);
        auto imgPtr = img.ptr<intUnion>(y);
        for (int x = 0; x < imgWidth; x++) {
            int canvasX = xOffset + x;
            if (canvasX < 0)
                continue;
            if (canvasWidth <= canvasX)
                break;

            auto& canvasPx = canvasPtr[canvasX];
            auto imgPx = imgPtr[x];
            uint32_t alpha = imgPx[3];

            if (alpha == 255) {
                canvasPx = imgPx;
            }
            else if (alpha) {
                uint32_t inv_alpha = 255 - alpha;
                canvasPx = {
                (uint8_t)(((canvasPx[0] * inv_alpha + imgPx[0] * alpha) + 128) >> 8), // +128 四舍五入
                (uint8_t)(((canvasPx[1] * inv_alpha + imgPx[1] * alpha) + 128) >> 8),
                (uint8_t)(((canvasPx[2] * inv_alpha + imgPx[2] * alpha) + 128) >> 8),
                255 };
            }
        }
    }
}

std::wstring jarkUtils::SelectFile(HWND hWnd) {
    OPENFILENAMEW ofn;
    wchar_t szFile[1024] = { 0 };

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.hwndOwner = hWnd;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"All Files\0*.*\0"; // 过滤器
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = nullptr;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = nullptr;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return std::wstring(szFile);
    }
    else {
        return L"";
    }
}

// ext需要带点 ".png"
static bool isExt(const std::wstring& path, const std::wstring& ext) {
    if (ext.empty() || path.length() <= ext.length())
        return false;

    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos || dotPos != (path.length() - ext.length()))
        return false;

    std::wstring fileExt = path.substr(dotPos);
    return std::equal(fileExt.begin(), fileExt.end(), ext.begin(),
        [](wchar_t a, wchar_t b) {
            return std::tolower(a) == b;
        });
}

// 图像另存为 选取文件路径 bool: isJPG
std::pair<std::wstring, bool> jarkUtils::saveImageDialogW(wstring_view title) {
    OPENFILENAMEW ofn{};
    wchar_t szFile[520] = { 0 };

    // 配置对话框参数
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"JPG\0*.jpg\0PNG\0*.png\0All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = title.data();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    // 显示保存对话框
    if (!GetSaveFileNameW(&ofn)) {
        return { L"", false};  // 用户取消操作
    }

    // 确保文件扩展名正确
    std::wstring filePath = szFile;
    bool isJPG = true;

    if (ofn.nFilterIndex == 1) {
        if (!isExt(filePath, L".jpg") and !isExt(filePath, L".jpeg"))
            filePath += L".jpg";
    }
    else if (ofn.nFilterIndex == 2) {
        isJPG = false;
        if(!isExt(filePath, L".png"))
            filePath += L".png";
    }
    else {
        filePath += L".jpg";  // 默认添加 .jpg 扩展名
    }

    return { filePath, isJPG };
}

void jarkUtils::openUrl(const wchar_t* url) {
    SHELLEXECUTEINFOW sei = {
        .cbSize = sizeof(sei),
        .lpVerb = L"open",
        .lpFile = url,
        .nShow = SW_SHOWNORMAL,
    };

    ShellExecuteExW(&sei);
}


std::vector<std::string> jarkUtils::splitString(std::string_view str, std::string_view delim) {
    if (str.empty()) return {};
    if (delim.empty()) return { std::string(str) };

    std::vector<std::string> res;
    size_t nextDelimIdx, targetBeginIdx = 0;
    while ((nextDelimIdx = str.find(delim, targetBeginIdx)) != std::string::npos) {
        if (nextDelimIdx == targetBeginIdx) {
            targetBeginIdx += delim.length();
            continue;
        }
        res.emplace_back(str.substr(targetBeginIdx, nextDelimIdx - targetBeginIdx));
        targetBeginIdx = nextDelimIdx + delim.length();
    }
    if (targetBeginIdx < str.length())
        res.emplace_back(str.substr(targetBeginIdx, str.length() - targetBeginIdx));
    return res;
}

void jarkUtils::stringReplace(std::string& src, std::string_view oldBlock, std::string_view newBlock) {
    if (oldBlock.empty()) return;

    size_t nextBeginIdx = 0, foundIdx;
    while ((foundIdx = src.find(oldBlock, nextBeginIdx)) != std::string::npos) {
        src.replace(foundIdx, oldBlock.length(), newBlock);
        nextBeginIdx = foundIdx + newBlock.length();
    }
}

std::vector<std::wstring> jarkUtils::splitWstring(std::wstring_view str, std::wstring_view delim) {
    if (str.empty()) return {};
    if (delim.empty()) return { std::wstring(str) };

    std::vector<std::wstring> res;
    size_t nextDelimIdx, targetBeginIdx = 0;
    while ((nextDelimIdx = str.find(delim, targetBeginIdx)) != std::wstring::npos) {
        if (nextDelimIdx == targetBeginIdx) {
            targetBeginIdx += delim.length();
            continue;
        }
        res.emplace_back(str.substr(targetBeginIdx, nextDelimIdx - targetBeginIdx));
        targetBeginIdx = nextDelimIdx + delim.length();
    }
    if (targetBeginIdx < str.length())
        res.emplace_back(str.substr(targetBeginIdx, str.length() - targetBeginIdx));
    return res;
}

void jarkUtils::wstringReplace(std::wstring& src, std::wstring_view oldBlock, std::wstring_view newBlock) {
    if (oldBlock.empty()) return;

    size_t nextBeginIdx = 0, foundIdx;
    while ((foundIdx = src.find(oldBlock, nextBeginIdx)) != std::wstring::npos) {
        src.replace(foundIdx, oldBlock.length(), newBlock);
        nextBeginIdx = foundIdx + newBlock.length();
    }
}

void jarkUtils::activateWindow(HWND hwnd) {
    if (hwnd) {
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        SetForegroundWindow(hwnd);
    }
}

// 获取当前程序的完整路径
std::wstring jarkUtils::getCurrentAppPath() {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path);
}

// 调用 Windows Explorer 来打开文件所在的文件夹并选中该文件
void jarkUtils::openFileLocation(wstring_view filePath) {
    if (!std::filesystem::exists(filePath)) {
        return;
    }

    ShellExecuteW(
        nullptr,
        L"open",
        L"explorer.exe",
        std::format(L"/select,\"{}\"", filePath).c_str(),
        nullptr,
        SW_SHOWNORMAL
    );
}

void jarkUtils::openFileProperties(wstring_view filePath) {
    if (!std::filesystem::exists(filePath)) {
        return;
    }

    SHELLEXECUTEINFOW sei = { sizeof(SHELLEXECUTEINFOW) };
    sei.lpVerb = L"properties";
    sei.lpFile = filePath.data();
    sei.nShow = SW_SHOW;
    sei.fMask = SEE_MASK_INVOKEIDLIST;

    ShellExecuteExW(&sei);
}
