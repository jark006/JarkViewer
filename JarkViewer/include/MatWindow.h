#pragma once

#include "jarkUtils.h"

// 轻量基类：为 Setting/Printer 提供 Win32 窗口骨架
// 使用 GDI StretchDIBits 渲染 cv::Mat
class MatWindow {
protected:
    HWND m_hwnd = nullptr;
    std::wstring m_className;
    std::wstring m_title;
    int m_width = 0;
    int m_height = 0;
    int m_x = 0;
    int m_y = 0;

    bool createWindow(int width, int height, const wchar_t* className, const wchar_t* title) {
        m_width = width;
        m_height = height;
        m_className = className;
        m_title = title;

        HINSTANCE hInstance = GetModuleHandleW(NULL);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = staticWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = className;
        wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_JARKVIEWER));

        RegisterClassExW(&wc);

        // 固定尺寸窗口
        DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

        // 计算窗口尺寸以获得指定客户区大小
        RECT rc = { 0, 0, width, height };
        AdjustWindowRect(&rc, style, FALSE);

        m_hwnd = CreateWindowExW(
            0,
            className,
            title,
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            NULL, NULL, hInstance, this  // 传递 this 指针
        );

        if (!m_hwnd)
            return false;

        // 依据窗口所在显示器的缩放比例调整到物理像素，避免高 DPI 下界面过小
        {
            const UINT dpi = ::GetDpiForWindow(m_hwnd);
            if (dpi >= 48) {
                m_uiScale = dpi / 96.0f;
                if (m_uiScale > 1.0f) {
                    RECT scaled = { 0, 0,
                        static_cast<LONG>(std::lround(width * m_uiScale)),
                        static_cast<LONG>(std::lround(height * m_uiScale)) };
                    AdjustWindowRect(&scaled, style, FALSE);
                    SetWindowPos(m_hwnd, nullptr, 0, 0, scaled.right - scaled.left, scaled.bottom - scaled.top,
                        SWP_NOMOVE | SWP_NOZORDER);
                }
            }
        }

        jarkUtils::disableWindowResize(m_hwnd);

        // 设置图标
        HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_JARKVIEWER));
        if (hIcon) {
            SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }

        applyDarkModeAttribute();

        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);

        return true;
    }

    virtual void drawingUI() = 0;

    // 界面缩放：逻辑像素 -> 物理像素（高 DPI 屏幕下 > 1）
    float uiScale() const noexcept { return m_uiScale; }
    int dp(int logical) const noexcept {
        return static_cast<int>(std::lround(logical * m_uiScale));
    }

    void runMessageLoop() {
        MSG msg;

        std::thread drawThread([this]() {
            while (isDrawThreadRuning) {
                if (isNeedRefreshUI) {
                    isNeedRefreshUI = false;
                    drawingUI();
                    isDrawDone = true;
                    if (m_hwnd)
                        PostMessageW(m_hwnd, WM_MATWINDOW_DRAW_DONE, 0, 0);
                }
                else {
                    Sleep(10);
                }
            }
            });

        while (GetMessageW(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            idleTask();
        }

        isDrawThreadRuning = false;
        drawThread.join();
    }

    virtual void idleTask() {}

    void invalidate() const {
        if (m_hwnd)
            InvalidateRect(m_hwnd, NULL, FALSE);
    }

    void blitMat(HDC hdc, const cv::Mat& bgra) {
        if (bgra.empty())
            return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bgra.cols;
        bmi.bmiHeader.biHeight = -bgra.rows;  // 负数表示自顶向下
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(hdc,
            0, 0, bgra.cols, bgra.rows,
            0, 0, bgra.cols, bgra.rows,
            bgra.data, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    void applyDarkModeAttribute() {
        if (!m_hwnd)
            return;
        BOOL themeMode = GlobalVar::isCurrentUIDarkMode;
        DwmSetWindowAttribute(m_hwnd, DWMWINDOWATTRIBUTE::DWMWA_USE_IMMERSIVE_DARK_MODE, &themeMode, sizeof(BOOL));
    }

    void applyIconAndTitle(const wchar_t* title) {
        if (m_hwnd)
            SetWindowTextW(m_hwnd, title);
    }

    // 子类实现
    virtual void onPaint(HDC hdc) = 0;
    virtual void onLButtonDown() {}
    virtual void onLButtonUp() {}
    virtual void onRButtonUp() {}
    virtual void onMouseMove(WPARAM keyState) {}
    virtual void onMouseWheel(int delta) {}
    virtual void onKeyDown(WPARAM key) {}

    // 字符输入（文本框等需要）；界面已禁用 IME，收到的多为 ASCII
    virtual void onKeyChar(wchar_t character) {}
    virtual void onClose() {
        if (m_hwnd)
            DestroyWindow(m_hwnd);
    }

    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MatWindow* pThis = nullptr;

        if (msg == WM_CREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pThis = reinterpret_cast<MatWindow*>(pCreate->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        else {
            pThis = reinterpret_cast<MatWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (pThis)
            return pThis->wndProc(msg, wParam, lParam);

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT wndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(m_hwnd, &ps);
            onPaint(hdc);
            EndPaint(m_hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            m_x = LOWORD(lParam);
            m_y = HIWORD(lParam);
            onLButtonDown();
            return 0;

        case WM_LBUTTONUP:
            // 用消息自带坐标：避免没有移动过的点击使用过期坐标
            m_x = LOWORD(lParam);
            m_y = HIWORD(lParam);
            onLButtonUp();
            return 0;

        case WM_RBUTTONUP:
            m_x = LOWORD(lParam);
            m_y = HIWORD(lParam);
            onRButtonUp();
            return 0;

        case WM_MOUSEMOVE:
            m_x = LOWORD(lParam);
            m_y = HIWORD(lParam);
            onMouseMove(wParam);
            return 0;

        case WM_CHAR:
            onKeyChar(static_cast<wchar_t>(wParam));
            return 0;

        case WM_MOUSEWHEEL:
            onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_KEYDOWN:
            onKeyDown(wParam);
            return 0;

        case WM_CLOSE:
            onClose();
            return 0;

        case WM_DESTROY:
            m_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;

        case WM_MATWINDOW_DRAW_REQUEST:
            isNeedRefreshUI = true;
            return 0;

        case WM_MATWINDOW_DRAW_DONE:
            if (isDrawDone) {
                isDrawDone = false;
                invalidate();
            }
            return 0;
        }

        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }

public:
    static constexpr UINT WM_MATWINDOW_DRAW_REQUEST = WM_APP + 1;
    static constexpr UINT WM_MATWINDOW_DRAW_DONE = WM_APP + 2;
    volatile bool requestExitFlag = false;
    float m_uiScale = 1.0f;
    volatile bool isNeedRefreshUI = true;
    volatile bool isDrawThreadRuning = true;
    volatile bool isDrawDone = false;

    virtual ~MatWindow() {
        m_hwnd = nullptr;
    }

    HWND getHwnd() const { return m_hwnd; }
};
