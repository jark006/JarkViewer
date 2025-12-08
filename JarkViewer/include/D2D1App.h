 /***********************************************************************
 功能：Direct2D 1.1应用程序类，使用时可以继承这个类
 作者：Ray1024
 网址：http://www.cnblogs.com/Ray1024/
 ***********************************************************************/


#pragma once

#include "jarkUtils.h"


class D2D1App {
public:
    // 构造函数
    D2D1App();
    // 析构函数
    virtual ~D2D1App();

    // 初始化
    virtual HRESULT Initialize(HINSTANCE hInstance);
    // 可以重写此函数来实现你想做的每帧执行的操作
    virtual void UpdateScene(float dt) {};
    // 渲染
    virtual void DrawScene() = 0;

    // 消息循环
    void Run();
    // 处理消息
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    static HMENU CreateContextMenu(HWND hwnd);
    static void ShowContextMenu(HWND hwnd, int x, int y);

    virtual void OnMouseDown(WPARAM btnState, int x, int y, WPARAM wParam) = 0;
    virtual void OnMouseUp(WPARAM btnState, int x, int y, WPARAM wParam) = 0;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) = 0;
    virtual void OnMouseLeave() = 0;
    virtual void OnMouseWheel(UINT nFlags, short zDelta, int x, int y) = 0;
    virtual void OnKeyDown(WPARAM keyValue) = 0;
    virtual void OnKeyUp(WPARAM keyValue) = 0;
    virtual void OnDropFiles(WPARAM wParam) = 0;
    virtual void OnContextMenuCommand(WPARAM wParam) = 0;

    virtual void OnResize(UINT width, UINT height) = 0;
    virtual void OnDestroy();

protected:

    // 创建设备无关资源
    HRESULT CreateDeviceIndependentResources();
    // 创建设备有关资源
    HRESULT CreateDeviceResources();
    // 每次窗口大小更改时，都需要重新创建这些资源
    void CreateWindowSizeDependentResources();
    // 丢弃设备有关资源
    void DiscardDeviceResources();

    template<class Interface>
    void SafeRelease(Interface*& pInterfaceToRelease);

    void loadSettings();
    void saveSettings() const;

protected:
    // 应用程序实例句柄
    HINSTANCE							m_hAppInst = nullptr;
    // 窗口句柄
    HWND								m_hWnd = nullptr;
    // 窗口标题
    std::wstring						m_wndCaption = L"D2D1App";
    // 是否运行
    BOOL								m_fRunning = TRUE;
    // D3D 设备
    ID3D11Device* m_pD3DDevice;
    // D3D 设备上下文
    ID3D11DeviceContext* m_pD3DDeviceContext;

    // D2D 工厂
    ID2D1Factory4* m_pD2DFactory = nullptr;
    // D2D 设备
    ID2D1Device3* m_pD2DDevice = nullptr;
    // D2D 设备上下文
    ID2D1DeviceContext3* m_pD2DDeviceContext = nullptr;

    // WIC 工厂
    IWICImagingFactory2* m_pWICFactory = nullptr;
    // DWrite工厂
    IDWriteFactory1* m_pDWriteFactory = nullptr;
    // DXGI 交换链
    IDXGISwapChain1* m_pSwapChain = nullptr;
    // D2D 位图 储存当前显示的位图
    ID2D1Bitmap1* m_pD2DTargetBimtap = nullptr;
    //  所创设备特性等级
    D3D_FEATURE_LEVEL                   m_featureLevel;
    // 手动交换链
    DXGI_PRESENT_PARAMETERS             m_parameters;
};
