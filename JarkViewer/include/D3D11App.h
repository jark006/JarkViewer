#pragma once

#include "jarkUtils.h"


class D3D11App {
public:
    D3D11App();
    virtual ~D3D11App();

    virtual HRESULT Initialize(HINSTANCE hInstance);
    virtual void DrawScene() = 0;

    void Run();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    static HMENU CreateContextMenu(HWND hwnd);
    static void ShowContextMenu(HWND hwnd, int x, int y);

    virtual void OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) = 0;
    virtual void OnMouseUp(WPARAM btnState, int x, int y, WPARAM wParam) = 0;
    // 不受 ImGui 捕获状态影响的手势收尾，仅消费自己拥有的抬起。
    virtual bool OnMouseRelease(WPARAM button, int x, int y, WPARAM state) { return false; }
    virtual void OnPointerCancel() {}
    virtual void OnMouseMove(WPARAM btnState, int x, int y) = 0;
    virtual void OnMouseLeave() = 0;
    virtual void OnMouseWheel(UINT nFlags, short zDelta, int x, int y) = 0;
    virtual void OnKeyDown(WPARAM keyValue) = 0;
    virtual void OnKeyUp(WPARAM keyValue) = 0;
    virtual void OnDropFiles(WPARAM wParam) = 0;
    virtual void OnContextMenuCommand(WPARAM wParam) = 0;

    virtual void OnResize(UINT width, UINT height) = 0;
    virtual void OnDpiChanged() { (void)0; } // 默认只需刷新缩放；子类可顺带重建按 DPI 缩放的资源
    virtual void OnRequestExitOtherWindows() = 0;
    virtual void OnDestroy();

    // 窗口句柄刚创建、D3D 设备尚未创建时回调：子类可把耗时工作（首图解码）
    // 先派发出去，与随后的设备/交换链创建并行（建 D3D 设备要几十毫秒）。
    virtual void OnWindowCreated() {}

    // 界面（ImGui）：每帧在 PresentFrame() 里被调用，子类在这里提交窗口与控件
    virtual void DrawUi() {}

    // 是否有界面窗口在显示（设置/批量/打印/编辑）。ImGui 只在有窗口时才独占鼠标键盘：
    // 窗口关掉后 ImGui 自己的 WantCapture* 不会跟着复位，只看它会永久吞掉主窗口的输入。
    virtual bool hasVisibleWindows() const { return false; }

protected:
    HRESULT CreateDeviceResources();
    void CreateWindowSizeDependentResources();
    void DiscardDeviceResources();

    // CPU 画布数据上传到暂存纹理
    void PresentCanvas(const uint8_t* data, int width, int height, int stride);

    // 把最近上传的画布贴到后缓冲，叠加 ImGui 界面后 Present（每帧一次）
    void PresentFrame();
    // 画布内容没有变化时只重画界面
    void PresentUiOnly();

    // 交换链重建/收到 WM_PAINT 后需要重新呈现一次（否则空闲时会停在空白后缓冲上）
    void markPresentRequested() { m_presentRequested = true; }

    bool consumePresentRequest() {
        const bool requested = m_presentRequested;
        m_presentRequested = false;
        return requested;
    }

    template<class Interface>
    void SafeRelease(Interface*& pInterfaceToRelease);

    void loadSettings();
    void saveSettings() const;

    // 主窗口是 PerMonitorHighDPIAware，拿到的就是物理像素：叠加 UI（悬停按钮、
    // EXIF 面板）的尺寸与命中区域都要按所在显示器的 DPI 换算，否则高 DPI 下按钮只有一半大。
    void refreshUiScale() {
        UINT dpi = m_hWnd ? GetDpiForWindow(m_hWnd) : 0;
        if (dpi == 0)
            dpi = 96;
        m_uiScale = static_cast<float>(dpi) / 96.0f;
    }

    float uiScale() const { return m_uiScale; }
    int dp(int logical) const { return static_cast<int>(std::lround(logical * m_uiScale)); }

protected:
    HINSTANCE m_hAppInst = nullptr;
    HWND m_hWnd = nullptr;
    std::wstring m_wndCaption = L"D3D11App";
    BOOL m_fRunning = TRUE;

    // D3D11 设备
    ID3D11Device* m_pD3DDevice = nullptr;
    ID3D11DeviceContext* m_pD3DDeviceContext = nullptr;
    // DXGI 交换链 (Win7 兼容)
    IDXGISwapChain* m_pSwapChain = nullptr;
    // CPU 可写暂存纹理，用于 CPU→GPU 数据传输
    ID3D11Texture2D* m_pStagingTexture = nullptr;
    // 暂存纹理尺寸
    UINT m_stagingWidth = 0;
    UINT m_stagingHeight = 0;
    // 后缓冲渲染目标（ImGui 需要绑定 RTV 才能绘制）
    ID3D11RenderTargetView* m_pBackBufferRTV = nullptr;
    bool m_presentRequested = true;
    bool m_processingMouseRelease = false; // 区分 backend 正常释放捕获与异常失捕获
    // 所创设备特性等级
    D3D_FEATURE_LEVEL m_featureLevel;

    int winWidth = 800;
    int winHeight = 600;
    float m_uiScale = 1.0f;
    bool hasInitWinSize = false;
    cv::Mat mainCanvas;
};
