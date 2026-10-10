#include "D3D11App.h"

#include "UiHost.h"

#include <algorithm>

namespace {

enum class PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode appMode);
using FlushMenuThemesFn = void (WINAPI*)();

struct MenuThemeApi {
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
};

const MenuThemeApi& GetMenuThemeApi() {
    static const MenuThemeApi api = []() {
        MenuThemeApi result;
        HMODULE module = GetModuleHandleW(L"uxtheme.dll");
        if (!module) {
            module = LoadLibraryW(L"uxtheme.dll");
        }
        if (!module) {
            return result;
        }

        result.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(module, MAKEINTRESOURCEA(135)));
        result.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(module, MAKEINTRESOURCEA(136)));
        return result;
    }();
    return api;
}

class PopupMenuThemeScope {
public:
    explicit PopupMenuThemeScope(bool useDarkTheme) {
        const auto& api = GetMenuThemeApi();
        if (!api.setPreferredAppMode || !api.flushMenuThemes) {
            return;
        }

        setPreferredAppMode_ = api.setPreferredAppMode;
        flushMenuThemes_ = api.flushMenuThemes;
        previousMode_ = setPreferredAppMode_(useDarkTheme ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
        flushMenuThemes_();
        isActive_ = true;
    }

    ~PopupMenuThemeScope() {
        if (!isActive_) {
            return;
        }

        setPreferredAppMode_(previousMode_);
        flushMenuThemes_();
    }

private:
    bool isActive_ = false;
    PreferredAppMode previousMode_ = PreferredAppMode::Default;
    SetPreferredAppModeFn setPreferredAppMode_ = nullptr;
    FlushMenuThemesFn flushMenuThemes_ = nullptr;
};

}

D3D11App::D3D11App() {
    loadSettings();

    GlobalVar::isSystemDarkMode = jarkUtils::getSystemDarkMode();
    GlobalVar::isCurrentUIDarkMode = GlobalVar::settingParameter.UI_Mode == 0 ? GlobalVar::isSystemDarkMode : (GlobalVar::settingParameter.UI_Mode == 2);
    GlobalVar::currentTheme = GlobalVar::isCurrentUIDarkMode ? deepTheme : lightTheme;
}

D3D11App::~D3D11App() {
    this->DiscardDeviceResources();
}

template<class Interface>
void D3D11App::SafeRelease(Interface*& pInterfaceToRelease) {
    if (pInterfaceToRelease == nullptr)
        return;

    pInterfaceToRelease->Release();
    pInterfaceToRelease = nullptr;
}

void D3D11App::loadSettings() {
    auto exePath = jarkUtils::getCurrentAppPath();
    size_t lastSlash = exePath.find_last_of(L'\\');
    if (lastSlash == std::wstring::npos) {
        GlobalVar::settingPath = L"JarkViewer.db";
        return;
    }

    GlobalVar::settingPath = exePath.substr(0, lastSlash) + L"\\JarkViewer.db";

    PWSTR appDataPath = nullptr;
    std::wstring oldSettingPath;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &appDataPath))) {
        oldSettingPath = std::wstring(appDataPath) + L"\\JarkViewer.db";
        CoTaskMemFree(appDataPath);
        appDataPath = nullptr;
    }

    SettingParameter tmp;
    bool loaded = false;

    auto f = _wfopen(oldSettingPath.c_str(), L"rb");
    if (f) {
        auto readLen = fread(&tmp, 1, sizeof(SettingParameter), f);
        fclose(f);

        if (readLen == sizeof(SettingParameter) && !memcmp(GlobalVar::settingHeader.data(), tmp.header, GlobalVar::settingHeader.length())) {
            GlobalVar::settingParameter = tmp;
            loaded = true;
            DeleteFileW(oldSettingPath.c_str());
        }
    }

    if (!loaded) {
        f = _wfopen(GlobalVar::settingPath.c_str(), L"rb");
        if (f) {
            auto readLen = fread(&tmp, 1, sizeof(SettingParameter), f);
            fclose(f);

            if (readLen == sizeof(SettingParameter) && !memcmp(GlobalVar::settingHeader.data(), tmp.header, GlobalVar::settingHeader.length()))
                GlobalVar::settingParameter = tmp;
        }
    }

    // 计算恢复位置：优先放回上次使用的显示器（有记录且那块屏还接着），记录的位置
    // 不在这块屏上（换过屏、最大化时存的空矩形）就放到它的工作区中央；没有记录或
    // 屏幕已断开时按主屏兜底。副屏坐标允许为负，不能按主屏尺寸去夹。
    auto& setting = GlobalVar::settingParameter;
    if (setting.rect.right <= setting.rect.left || setting.rect.bottom <= setting.rect.top)
        setting.rect = { 0, 0, 800, 600 }; // 尺寸先保证非零（最大化时 rect 是空的）

    HMONITOR targetMonitor = nullptr;
    if (setting.monitorDevice[0]) {
        struct MonitorSearch {
            const wchar_t* name;
            HMONITOR monitor;
        };
        MonitorSearch search{ setting.monitorDevice, nullptr };
        ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto* found = reinterpret_cast<MonitorSearch*>(data);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(MONITORINFOEXW); // cbSize 在基类 MONITORINFO 里，不能走指定初始化器
            if (::GetMonitorInfoW(monitor, &info) && wcscmp(info.szDevice, found->name) == 0) {
                found->monitor = monitor;
                return FALSE;
            }
            return TRUE;
            }, reinterpret_cast<LPARAM>(&search));
        targetMonitor = search.monitor;
    }

    if (targetMonitor) {
        MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
        if (::GetMonitorInfoW(targetMonitor, &info)) {
            RECT overlap{};
            if (!::IntersectRect(&overlap, &setting.rect, &info.rcWork)) {
                const int workWidth = info.rcWork.right - info.rcWork.left;
                const int workHeight = info.rcWork.bottom - info.rcWork.top;
                const int width = (std::min)(static_cast<int>(setting.rect.right - setting.rect.left), workWidth);
                const int height = (std::min)(static_cast<int>(setting.rect.bottom - setting.rect.top), workHeight);
                setting.rect = {
                    info.rcWork.left + (workWidth - width) / 2,
                    info.rcWork.top + (workHeight - height) / 2,
                    info.rcWork.left + (workWidth + width) / 2,
                    info.rcWork.top + (workHeight + height) / 2 };
            }
        }
    }
    else if (setting.showCmd == SW_NORMAL) {
        int screenWidth = (::GetSystemMetrics(SM_CXFULLSCREEN));
        int screenHeight = (::GetSystemMetrics(SM_CYFULLSCREEN));

        if (setting.rect.left >= screenWidth || setting.rect.bottom >= screenHeight ||
            (setting.rect.right - setting.rect.left) >= screenWidth ||
            (setting.rect.bottom - setting.rect.top) >= screenHeight) {
            setting.rect = { screenWidth / 4, screenHeight / 4, screenWidth * 3 / 4, 100 + screenHeight * 3 / 4 };
        }
    }
}

void D3D11App::saveSettings() const {
    // 窗口几何是**两种模式共用**的一份记忆：看图窗口与播放器是同一个进程里二选一的顶层
    // 窗口，谁退出谁写（`Initialize` 里两边也读同一份 rect/showCmd）。于是"上次看电影
    // 调出来的窗口大小"会原样继承给下次看图，反过来也一样。**别再按模式分开记**：
    // 分开记就等于每次换模式都跳回默认尺寸。
    //
    // 唯一的例外是"退出时正处在全屏"（播放器的 F/F11/双击画面、看图的幻灯片）：
    // 那时窗口被临时改成了无边框满屏，把它当成几何记忆存下来，下次启动会变成一个
    // 满屏带标题栏的怪窗口；而**跳过不写**又会丢掉用户进全屏之前的调整（实测：调好
    // 尺寸 → F11 → 退出 → 下次开的是启动时那个尺寸）。所以用 jarkUtils 在**进全屏那一刻**
    // 记下的窗口状态——用户在全屏里挪不动窗口，那份就是"用户最后看到的窗口化样子"。
    WINDOWPLACEMENT wp{ .length = sizeof(WINDOWPLACEMENT) };
    const bool fromPreFullScreen = jarkUtils::IsFullScreen() &&
        jarkUtils::GetPreFullScreenPlacement(wp);
    if (!fromPreFullScreen)
        (void)GetWindowPlacement(m_hWnd, &wp);

    if (wp.showCmd == SW_NORMAL) {
        GlobalVar::settingParameter.showCmd = SW_NORMAL;
        GlobalVar::settingParameter.rect = wp.rcNormalPosition;
    }
    else {
        GlobalVar::settingParameter.showCmd = SW_MAXIMIZE;
        GlobalVar::settingParameter.rect = {};
    }

    // 记住窗口所在显示器（最大化也记）：下次启动优先回到这块屏。
    // 全屏时窗口铺满的正是它所在的那块屏，MonitorFromWindow 给的还是同一块
    if (HMONITOR monitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(MONITORINFOEXW);
        if (::GetMonitorInfoW(monitor, &info))
            wcscpy_s(GlobalVar::settingParameter.monitorDevice, info.szDevice);
    }

    memcpy(GlobalVar::settingParameter.header, GlobalVar::settingHeader.data(), GlobalVar::settingHeader.length());

    auto f = _wfopen(GlobalVar::settingPath.c_str(), L"wb");
    if (f) {
        fwrite(&GlobalVar::settingParameter, 1, sizeof(SettingParameter), f);
        fclose(f);
    }
}

HRESULT D3D11App::Initialize(HINSTANCE hInstance) {
    HRESULT hr = E_FAIL;
    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = D3D11App::WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = sizeof(void*);
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = L"D3D11WndClass";
    wcex.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCE(IDI_JARKVIEWER));
    RegisterClassExW(&wcex);

    // loadSettings() 已把 rect 归一为目标显示器上的合法矩形（含最大化时的占位尺寸）
    RECT window_rect = GlobalVar::settingParameter.rect;
    DWORD window_style = WS_OVERLAPPEDWINDOW;
    m_hWnd = CreateWindowExW(0, L"D3D11WndClass", m_wndCaption.c_str(), window_style,
        window_rect.left, window_rect.top, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
        0, 0, hInstance, this);
    hr = m_hWnd ? S_OK : E_FAIL;

    if (SUCCEEDED(hr)) {
        refreshUiScale();
        OnWindowCreated(); // 窗口句柄已就绪：让业务层先把耗时工作派出去，与下面建资源并行
        CreateDeviceResources();

        jark::ui::UiHost::instance().init(m_hWnd, m_pD3DDevice, m_pD3DDeviceContext, m_pSwapChain);

        BOOL themeMode = GlobalVar::isCurrentUIDarkMode;
        DwmSetWindowAttribute(m_hWnd, 20, &themeMode, sizeof(BOOL));
        DragAcceptFiles(m_hWnd, TRUE);
        ShowWindow(m_hWnd, GlobalVar::settingParameter.showCmd == SW_NORMAL ? SW_NORMAL : SW_MAXIMIZE);
        UpdateWindow(m_hWnd);
    }
    return hr;
}

HRESULT D3D11App::CreateDeviceResources() {
    HRESULT hr = S_OK;

    // 创建 D3D11 设备
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_pD3DDevice,
        &m_featureLevel,
        &m_pD3DDeviceContext);

    // 获取 DXGI 工厂（通过设备链：Device → DXGIDevice → Adapter → Factory）
    IDXGIDevice* pDxgiDevice = nullptr;
    IDXGIAdapter* pDxgiAdapter = nullptr;
    IDXGIFactory* pDxgiFactory = nullptr;

    if (SUCCEEDED(hr))
        hr = m_pD3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDevice);
    if (SUCCEEDED(hr))
        hr = pDxgiDevice->GetAdapter(&pDxgiAdapter);
    if (SUCCEEDED(hr))
        hr = pDxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pDxgiFactory);

    // 创建交换链（Win7 兼容：使用 IDXGIFactory::CreateSwapChain + DXGI_SWAP_EFFECT_DISCARD）
    if (SUCCEEDED(hr)) {
        RECT rect = { 0 };
        GetClientRect(m_hWnd, &rect);

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferDesc.Width = rect.right - rect.left;
        swapChainDesc.BufferDesc.Height = rect.bottom - rect.top;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 0;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // 翻转模型（FLIP_DISCARD）：与 DWM 组合更稳，避免旧 DISCARD 模型下
        // 某些驱动/多显示器场景出现“Present 成功但窗口空白”。
        swapChainDesc.BufferCount = 2;
        swapChainDesc.OutputWindow = m_hWnd;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.Flags = 0;

        hr = pDxgiFactory->CreateSwapChain(m_pD3DDevice, &swapChainDesc, &m_pSwapChain);
    }

    SafeRelease(pDxgiDevice);
    SafeRelease(pDxgiAdapter);
    SafeRelease(pDxgiFactory);

    if (SUCCEEDED(hr))
        CreateWindowSizeDependentResources();

    return hr;
}

void D3D11App::CreateWindowSizeDependentResources() {
    if (!m_pD3DDevice || !m_pSwapChain)
        return;

    // 释放旧暂存纹理与后缓冲视图
    SafeRelease(m_pStagingTexture);
    SafeRelease(m_pBackBufferRTV);
    m_pD3DDeviceContext->Flush();

    RECT rect = { 0 };
    GetClientRect(m_hWnd, &rect);
    UINT width = rect.right - rect.left;
    UINT height = rect.bottom - rect.top;
    if (width == 0 || height == 0)
        return;

    // 重设交换链缓冲区
    HRESULT hr = m_pSwapChain->ResizeBuffers(
        0, // 保持创建时的缓冲区数量
        width,
        height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0);
    assert(hr == S_OK);

    // 创建 CPU 可写暂存纹理
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    texDesc.MiscFlags = 0;

    hr = m_pD3DDevice->CreateTexture2D(&texDesc, nullptr, &m_pStagingTexture);
    assert(hr == S_OK);

    // 后缓冲渲染目标：ImGui 的绘制命令需要一个绑定的 RTV
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer))) {
        m_pD3DDevice->CreateRenderTargetView(pBackBuffer, nullptr, &m_pBackBufferRTV);
        pBackBuffer->Release();
    }

    m_stagingWidth = width;
    m_stagingHeight = height;
    m_presentRequested = true; // 后缓冲刚重建，需要重新呈现
}

void D3D11App::PresentCanvas(const uint8_t* data, int width, int height, int stride) {
    if (!m_pStagingTexture || !m_pSwapChain || !m_pD3DDeviceContext)
        return;

    // 尺寸不匹配时重建
    if ((UINT)width != m_stagingWidth || (UINT)height != m_stagingHeight)
        CreateWindowSizeDependentResources();

    // Map 暂存纹理，写入 CPU 画布数据
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_pD3DDeviceContext->Map(m_pStagingTexture, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (SUCCEEDED(hr)) {
        const int rowBytes = width * 4;
        if ((int)mapped.RowPitch == stride) {
            memcpy(mapped.pData, data, (size_t)rowBytes * height);
        } else {
            const uint8_t* src = data;
            uint8_t* dst = (uint8_t*)mapped.pData;
            for (int y = 0; y < height; y++) {
                memcpy(dst, src, rowBytes);
                src += stride;
                dst += mapped.RowPitch;
            }
        }
        m_pD3DDeviceContext->Unmap(m_pStagingTexture, 0);
    }
}

// 后缓冲 = 最近一次上传的画布；随后叠加 ImGui 界面并 Present
void D3D11App::PresentFrame() {
    if (!m_pSwapChain || !m_pD3DDeviceContext)
        return;

    if (m_pStagingTexture) {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (SUCCEEDED(m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer))) {
            m_pD3DDeviceContext->CopyResource(pBackBuffer, m_pStagingTexture);
            pBackBuffer->Release();
        }
    }

    auto& ui = jark::ui::UiHost::instance();
    if (ui.ready()) {
        if (m_pBackBufferRTV)
            m_pD3DDeviceContext->OMSetRenderTargets(1, &m_pBackBufferRTV, nullptr);

        static const bool skipUi = ::getenv("JARKVIEWER_NO_UI") != nullptr;
        if (!skipUi) {
            ui.newFrame();
            DrawUi();
            ui.renderDrawData();
        }
    }

    const HRESULT presentResult = m_pSwapChain->Present(0, 0);
    if (FAILED(presentResult))
        JARK_LOG("Present 失败 hr=0x{:08X} 设备原因=0x{:08X}", static_cast<uint32_t>(presentResult),
            static_cast<uint32_t>(m_pD3DDevice ? m_pD3DDevice->GetDeviceRemovedReason() : 0));
}

void D3D11App::PresentUiOnly() {
    PresentFrame();
}

void D3D11App::DiscardDeviceResources() {
    SafeRelease(m_pStagingTexture);
    SafeRelease(m_pBackBufferRTV);
    SafeRelease(m_pSwapChain);
    SafeRelease(m_pD3DDevice);
    SafeRelease(m_pD3DDeviceContext);
}

void D3D11App::Run() {
    while (m_fRunning) {
        MSG msg;
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else {
            DrawScene();
        }
    }
}

void D3D11App::OnDestroy() {
    if (m_destroying)
        return; // DestroyWindow 会同步送回一条 WM_DESTROY：别再存一次设置（那时窗口正在拆）

    m_destroying = true;
    saveSettings();
    m_fRunning = FALSE;

    // ImGui 必须在**窗口还活着**的时候拆掉：后端 Shutdown 会把窗口过程换回去，
    // 此时句柄若已经被系统回收给了下一个窗口（同一个进程里换窗口时很常见），
    // 就会把新窗口的窗口过程改坏、它再也收不到任何消息。
    jark::ui::UiHost::instance().shutdown();

    // 窗口必须**真的**销毁：退出走的是 PostMessage(WM_DESTROY)，那只调到这里，
    // 窗口本身还活着。对象随后就被析构，于是下一条鼠标/DPI/输入法消息打在这个窗口上
    // 就会调到已析构对象的纯虚函数（实测 `_purecall` → abort，退出码 0xC0000409）。
    // 进程直接退出时看不出来，一旦首尾相接（看图窗口 ↔ 视频播放器窗口交换）就必然撞上。
    // 真正的 WM_DESTROY（DestroyWindow 触发）走到这里时 DestroyWindow 会直接失败返回，
    // 不会重入成无限递归。
    if (IsWindow(m_hWnd))
        DestroyWindow(m_hWnd);
}


LRESULT D3D11App::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // ImGui 先记录输入状态（键盘/鼠标/IME/DPI 都由它维护）；它声明"已处理"的消息直接返回，
    // 不要再落到下面的 DefWindowProc —— WM_IME_COMPOSITION 被处理两遍的话，
    // 输入法上屏的中文会重复一遍（输入"安装"落进编辑框变成"安装安装"）
    auto* inputApp = reinterpret_cast<D3D11App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const bool mouseRelease = message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
    // backend 的 ReleaseCapture 会同步重入 WM_CAPTURECHANGED，这不是取消本次正常抬起。
    if (inputApp && mouseRelease)
        inputApp->m_processingMouseRelease = true;
    const bool handled = jark::ui::UiHost::processMessage(hwnd, message, wParam, lParam);
    if (inputApp && mouseRelease)
        inputApp->m_processingMouseRelease = false;
    if (handled)
        return S_OK;

    switch (message) {
    case WM_CREATE: {
        LPCREATESTRUCT pcs = (LPCREATESTRUCT)lParam;
        D3D11App* pApp = (D3D11App*)pcs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pApp);
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        const D3D11App* pApp = (const D3D11App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        const float scale = pApp ? pApp->uiScale() : 1.0f;
        mmi->ptMinTrackSize.x = (LONG)std::lround(400 * scale);
        mmi->ptMinTrackSize.y = (LONG)std::lround(300 * scale);
        return S_OK;
    }
    case WM_CONTEXTMENU: {
        if (inputApp && !inputApp->showsContextMenu())
            return S_OK;

        if (lParam == -1) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int x = (rc.right - rc.left) / 2;
            int y = (rc.bottom - rc.top) / 2;
            ShowContextMenu(hwnd, x, y);
        }
        else {
            ShowContextMenu(hwnd, LOWORD(lParam), HIWORD(lParam));
        }
        return S_OK;
    }
    }

    static TRACKMOUSEEVENT tme = {
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = TME_LEAVE,
    };

    D3D11App* pApp = reinterpret_cast<D3D11App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!pApp)
        return DefWindowProcW(hwnd, message, wParam, lParam);

    if (mouseRelease && pApp->OnMouseRelease(message, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam))
        return S_OK;
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE ||
        (message == WM_CAPTURECHANGED && !pApp->m_processingMouseRelease && reinterpret_cast<HWND>(lParam) != hwnd)) {
        pApp->OnPointerCancel();
    }

    // 鼠标/键盘先给界面（ImGui 需要时就不给画布，例如光标落在界面控件上）。
    // 窗口可见性在这里实时取：窗口刚被关掉时，ImGui 的捕获状态还停在上一帧，不能据此拦截。
    const bool windowVisible = pApp->hasVisibleWindows();
    const bool uiWantsMouse = jark::ui::UiHost::instance().mouseCaptured(windowVisible);
    const bool uiWantsKeyboard = jark::ui::UiHost::instance().keyboardCaptured(windowVisible);

    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (uiWantsMouse)
            return S_OK;
        pApp->OnMouseDown(message, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
        return S_OK;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_XBUTTONUP:
        if (uiWantsMouse)
            return S_OK;
        pApp->OnMouseUp(message, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
        return S_OK;

    case WM_MOUSEMOVE:
        if (!tme.hwndTrack) {
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        if (uiWantsMouse)
            return S_OK;
        pApp->OnMouseMove(message, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return S_OK;

    case WM_MOUSELEAVE:
        tme.hwndTrack = NULL;
        pApp->OnMouseLeave();
        break;

    case WM_MOUSEWHEEL:
        if (uiWantsMouse)
            return S_OK;
        pApp->OnMouseWheel(GET_KEYSTATE_WPARAM(wParam), GET_WHEEL_DELTA_WPARAM(wParam), GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return S_OK;

    case WM_KEYDOWN:
        if (uiWantsKeyboard)
            return S_OK;
        pApp->OnKeyDown(wParam);
        return S_OK;

    case WM_KEYUP:
        if (uiWantsKeyboard)
            return S_OK;
        pApp->OnKeyUp(wParam);
        return S_OK;

    case WM_DROPFILES:
        pApp->OnDropFiles(wParam);
        break;

    case WM_COMMAND:
        pApp->OnContextMenuCommand(wParam);
        break;

    case WM_SIZE:
        pApp->refreshUiScale(); // 跨显示器拖动时 WM_SIZE 先到，保证 OnResize 里画的是新缩放
        pApp->OnResize(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_PAINT:
        // 系统要求重绘（刚显示/被遮挡后恢复）：重新呈现一帧后交给 DefWindowProc 校验区域
        ValidateRect(hwnd, nullptr);
        pApp->markPresentRequested();
        return 0;

    case WM_DPICHANGED:
    case WM_DPICHANGED_AFTERPARENT:
        pApp->refreshUiScale();
        pApp->OnDpiChanged();
        pApp->markPresentRequested();
        break;

    case WM_SETTINGCHANGE:
        GlobalVar::isSystemDarkMode = jarkUtils::getSystemDarkMode();
        GlobalVar::isCurrentUIDarkMode = GlobalVar::settingParameter.UI_Mode == 0 ? GlobalVar::isSystemDarkMode : (GlobalVar::settingParameter.UI_Mode == 2);
        GlobalVar::isNeedUpdateTheme = true;
        break;

    case WM_DESTROY:
    {
        pApp->OnRequestExitOtherWindows();
        pApp->OnDestroy();
        PostQuitMessage(0);
        return S_OK;
    }
    break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}


HMENU D3D11App::CreateContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    MENUINFO mi = { sizeof(MENUINFO) };
    mi.fMask = MIM_STYLE | MIM_APPLYTOSUBMENUS;
    mi.dwStyle = MNS_NOCHECK;
    SetMenuInfo(hMenu, &mi);

    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openNewImage, getUIStringW(35));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::copyImageInfo, getUIStringW(25));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::copyImagePath, getUIStringW(26));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::copyImageData, getUIStringW(27));

    // 导出视频：实况照片/视频文件才有内嵌视频，其余情况置灰（不弹"没有视频"的框）。
    // 菜单文案走**宽表**的 60（两张表的 ID 各自独立，别拿窄表的编号来查）。
    const auto* app = reinterpret_cast<const D3D11App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const UINT exportFlags = MF_STRING | ((app && app->hasExportableVideo()) ? 0u : MF_GRAYED);
    AppendMenuW(hMenu, exportFlags, (UINT_PTR)ContextMenu::exportVideo, getUIStringW(60));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::toggleExifDisplay, getUIStringW(28));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openContainerFloder, getUIStringW(29));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::renameImage, getUIStringW(50));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::copyToTarget, getUIStringW(52));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::moveToTarget, getUIStringW(53));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::chooseTargetDir, getUIStringW(54));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openWithEditor, getUIStringW(57));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::chooseEditor, getUIStringW(58));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::deleteImage, getUIStringW(30));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openFileProperties, getUIStringW(36));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::printImage, getUIStringW(31));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::editImage, getUIStringW(47));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::slideshow, getUIStringW(48));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::batchProcess, getUIStringW(44));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::toggleFullScreen, getUIStringW(38));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openSetting, getUIStringW(32));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::openHelp, getUIStringW(37));
    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::aboutSoftware, getUIStringW(33));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(hMenu, MF_STRING, (UINT_PTR)ContextMenu::exitSoftware, getUIStringW(34));

    return hMenu;
}

void D3D11App::ShowContextMenu(HWND hwnd, int x, int y) {
    HMENU hMenu = CreateContextMenu(hwnd);
    POINT pt = { x, y };
    ClientToScreen(hwnd, &pt);

    PopupMenuThemeScope popupMenuThemeScope(GlobalVar::isCurrentUIDarkMode);
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    DWORD cmd = TrackPopupMenuEx(hMenu, flags, pt.x, pt.y, hwnd, NULL);

    if (cmd)
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);

    DestroyMenu(hMenu);
}
