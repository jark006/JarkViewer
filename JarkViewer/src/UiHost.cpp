#include "UiHost.h"

#include "jarkUtils.h"

#include <d3d11.h>
#include <filesystem>
#include <format>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

// 后端头文件里这行声明被 #if 0 包着（避免它去包含 windows.h），需要自己前置声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace jark::ui {
namespace {

    uint32_t accentColor() {
        // 与主题里的选中色保持一致
        return GlobalVar::currentTheme.CHECK;
    }

    void colorFromHex(ImVec4& target, uint32_t argb, float alphaOverride = -1.0f) {
        target.x = ((argb >> 16) & 0xFF) / 255.0f;
        target.y = ((argb >> 8) & 0xFF) / 255.0f;
        target.z = (argb & 0xFF) / 255.0f;
        target.w = alphaOverride >= 0.0f ? alphaOverride : ((argb >> 24) & 0xFF) / 255.0f;
    }

    ImVec4 hex(uint32_t argb, float alphaOverride = -1.0f) {
        ImVec4 value;
        colorFromHex(value, argb, alphaOverride);
        return value;
    }

    ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    }

} // namespace

UiHost& UiHost::instance() {
    static UiHost host;
    return host;
}

std::string UiHost::systemFontPath(const wchar_t* fileName) {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};

    std::filesystem::path path(buffer);
    path /= L"Fonts";
    path /= fileName;
    return jarkUtils::wstringToUtf8(path.wstring());
}

bool UiHost::init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    if (initialized_)
        return true;
    if (!hwnd || !device || !context || !swapChain)
        return false;

    hwnd_ = hwnd;
    device_ = device;
    context_ = context;
    swapChain_ = swapChain;

    // 窗口的输入法上下文先摘下来存着：默认不挂 IME（中文输入法会吃掉 p/c 这类单键快捷键），
    // 等 ImGui 里有文本框获得焦点再挂回去（ImGui 会把组字/候选窗口摆到文本光标处）。
    // 注意别用 ImmDisableIME()——它是线程级且没有反向接口，之后再也接不回输入法。
    imeContext_ = ::ImmAssociateContext(hwnd_, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;   // 界面布局不落盘（窗口位置由各窗口自己管）
    io.ConfigViewportsNoAutoMerge = false;
    io.ConfigViewportsNoTaskBarIcon = false;
    // 只允许从右下角那个抓手改窗口尺寸：关掉后四边边框不再能拖，也不再有左下角抓手
    // （ImGui 原本是「边框 + 两个下角」都能拖，边框在缩放后的窗口上很容易误触）
    io.ConfigWindowsResizeFromEdges = false;

    if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX11_Init(device, context)) {
        ImGui::DestroyContext();
        hwnd_ = nullptr;
        return false;
    }

    initialized_ = true;

    const UINT dpi = ::GetDpiForWindow(hwnd);
    refreshScale(dpi >= 48 ? static_cast<float>(dpi) / 96.0f : 1.0f, true);

    JARK_LOG("ImGui {} 初始化完成，缩放 {:.2f}", ImGui::GetVersion(), scale_);
    return true;
}

void UiHost::shutdown() {
    if (!initialized_)
        return;

    releaseTextures();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

void UiHost::refreshScale(float scale, bool forceReload) {
    const float clamped = scale > 0.05f ? scale : 1.0f;
    const bool scaleChanged = std::abs(clamped - scale_) > 0.001f;
    if (!scaleChanged && !forceReload)
        return;

    scale_ = clamped;
    dark_ = GlobalVar::isCurrentUIDarkMode;

    reloadFonts();
    applyTheme();
}

void UiHost::reloadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // 基准字号 16（96DPI 下的逻辑字号），高 DPI 下按比例放大
    const float size = 16.0f * scale_;

    ImFontConfig baseConfig;
    baseConfig.OversampleH = 2;
    baseConfig.OversampleV = 1;

    ImFont* font = nullptr;

    // 拉丁字形用 Segoe UI，缺字形时回退到下面合并进来的字体
    const std::string latin = systemFontPath(L"segoeui.ttf");
    if (!latin.empty())
        font = io.Fonts->AddFontFromFileTTF(latin.c_str(), size, &baseConfig);

    if (!font) {
        // 没有 Segoe UI 时的兜底：其它常见系统字体
        for (const wchar_t* fallback : { L"segoeui.ttf", L"Deng.ttf", L"simhei.ttf" }) {
            const std::string path = systemFontPath(fallback);
            if (!path.empty() && std::filesystem::exists(path)) {
                font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &baseConfig);
                if (font)
                    break;
            }
        }
    }

    if (!font) {
        io.Fonts->AddFontDefault();
        io.FontDefault = io.Fonts->Fonts[0];
        return;
    }

    // 中文（微软雅黑，含简繁与日文汉字）
    ImFontConfig cjkConfig;
    cjkConfig.MergeMode = true;
    cjkConfig.OversampleH = 2;
    cjkConfig.OversampleV = 1;
    cjkConfig.PixelSnapH = true;
    const std::string cjk = systemFontPath(L"msyh.ttc");
    if (!cjk.empty())
        io.Fonts->AddFontFromFileTTF(cjk.c_str(), size, &cjkConfig);

    // 韩文回退
    const std::string korean = systemFontPath(L"malgun.ttf");
    if (!korean.empty())
        io.Fonts->AddFontFromFileTTF(korean.c_str(), size, &cjkConfig);

    // 图标（占位用的符号字体）
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.OversampleH = 1;
    iconConfig.OversampleV = 1;
    iconConfig.PixelSnapH = true;
    for (const wchar_t* iconFont : { L"SegoeIcons.ttf", L"segmdl2.ttf" }) {
        const std::string path = systemFontPath(iconFont);
        if (!path.empty() && std::filesystem::exists(path)) {
            io.Fonts->AddFontFromFileTTF(path.c_str(), size, &iconConfig);
            break;
        }
    }

    io.FontDefault = font;
}

void UiHost::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

    const bool dark = dark_;
    if (dark)
        ImGui::StyleColorsDark();
    else
        ImGui::StyleColorsLight();

    // 几何尺寸（逻辑像素，最后统一乘缩放）
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    style.WindowPadding = { 14.0f, 12.0f };
    style.FramePadding = { 10.0f, 6.0f };
    style.CellPadding = { 8.0f, 5.0f };
    style.ItemSpacing = { 10.0f, 8.0f };
    style.ItemInnerSpacing = { 8.0f, 6.0f };
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;

    style.WindowTitleAlign = { 0.0f, 0.5f };
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = { 0.0f, 6.0f };

    ImVec4* colors = style.Colors;
    const uint32_t accent = accentColor();

    if (dark) {
        const ImVec4 bg = hex(0xFF1E1F24);
        const ImVec4 bgDeep = hex(0xFF17181C);
        const ImVec4 bgLight = hex(0xFF2A2C33);

        colors[ImGuiCol_WindowBg] = bg;
        colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_PopupBg] = mix(bg, bgDeep, 0.4f);
        colors[ImGuiCol_MenuBarBg] = bgDeep;

        colors[ImGuiCol_Border] = hex(0xFF3A3D45);
        colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

        colors[ImGuiCol_FrameBg] = bgLight;
        colors[ImGuiCol_FrameBgHovered] = mix(bgLight, hex(accent), 0.25f);
        colors[ImGuiCol_FrameBgActive] = mix(bgLight, hex(accent), 0.45f);

        colors[ImGuiCol_TitleBg] = bgDeep;
        colors[ImGuiCol_TitleBgActive] = bgDeep;
        colors[ImGuiCol_TitleBgCollapsed] = bgDeep;

        colors[ImGuiCol_Button] = bgLight;
        colors[ImGuiCol_ButtonHovered] = mix(bgLight, hex(accent), 0.40f);
        colors[ImGuiCol_ButtonActive] = mix(bgLight, hex(accent), 0.65f);

        colors[ImGuiCol_Header] = mix(bgLight, hex(accent), 0.35f);
        colors[ImGuiCol_HeaderHovered] = mix(bgLight, hex(accent), 0.50f);
        colors[ImGuiCol_HeaderActive] = mix(bgLight, hex(accent), 0.70f);

        colors[ImGuiCol_Tab] = bgDeep;
        colors[ImGuiCol_TabHovered] = mix(bgDeep, hex(accent), 0.45f);
        colors[ImGuiCol_TabSelected] = mix(bgDeep, hex(accent), 0.30f);
        colors[ImGuiCol_TabDimmed] = bgDeep;
        colors[ImGuiCol_TabDimmedSelected] = mix(bgDeep, hex(accent), 0.15f);

        colors[ImGuiCol_Text] = hex(0xFFE6E7EA);
        colors[ImGuiCol_TextDisabled] = hex(0xFF8A8D96);
        colors[ImGuiCol_Separator] = hex(0xFF3A3D45);

        colors[ImGuiCol_SliderGrab] = hex(accent);
        colors[ImGuiCol_SliderGrabActive] = mix(hex(accent), ImVec4(1, 1, 1, 1), 0.2f);
        colors[ImGuiCol_CheckMark] = hex(accent);

        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab] = hex(0xFF44474F);
        colors[ImGuiCol_ScrollbarGrabHovered] = hex(0xFF55595F);
        colors[ImGuiCol_ScrollbarGrabActive] = hex(0xFF6A6E76);

        colors[ImGuiCol_TableHeaderBg] = bgDeep;
        colors[ImGuiCol_TableBorderStrong] = hex(0xFF3A3D45);
        colors[ImGuiCol_TableBorderLight] = hex(0xFF2C2F35);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);

        colors[ImGuiCol_NavCursor] = hex(accent);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    }
    else {
        const ImVec4 bg = hex(0xFFF6F7F9);
        const ImVec4 bgDeep = hex(0xFFEDEFF3);
        const ImVec4 bgLight = hex(0xFFFFFFFF);

        colors[ImGuiCol_WindowBg] = bg;
        colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_PopupBg] = bgLight;
        colors[ImGuiCol_MenuBarBg] = bgDeep;

        colors[ImGuiCol_Border] = hex(0xFFD3D7DE);
        colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

        colors[ImGuiCol_FrameBg] = bgLight;
        colors[ImGuiCol_FrameBgHovered] = mix(bgLight, hex(accent), 0.15f);
        colors[ImGuiCol_FrameBgActive] = mix(bgLight, hex(accent), 0.25f);

        colors[ImGuiCol_TitleBg] = bgDeep;
        colors[ImGuiCol_TitleBgActive] = bgDeep;
        colors[ImGuiCol_TitleBgCollapsed] = bgDeep;

        colors[ImGuiCol_Button] = bgDeep;
        colors[ImGuiCol_ButtonHovered] = mix(bgDeep, hex(accent), 0.30f);
        colors[ImGuiCol_ButtonActive] = mix(bgDeep, hex(accent), 0.55f);

        colors[ImGuiCol_Header] = mix(bgDeep, hex(accent), 0.30f);
        colors[ImGuiCol_HeaderHovered] = mix(bgDeep, hex(accent), 0.45f);
        colors[ImGuiCol_HeaderActive] = mix(bgDeep, hex(accent), 0.65f);

        colors[ImGuiCol_Tab] = bgDeep;
        colors[ImGuiCol_TabHovered] = mix(bgDeep, hex(accent), 0.35f);
        colors[ImGuiCol_TabSelected] = bgLight;
        colors[ImGuiCol_TabDimmed] = bgDeep;
        colors[ImGuiCol_TabDimmedSelected] = mix(bgDeep, ImVec4(1, 1, 1, 1), 0.5f);

        colors[ImGuiCol_Text] = hex(0xFF1F2126);
        colors[ImGuiCol_TextDisabled] = hex(0xFF868A93);
        colors[ImGuiCol_Separator] = hex(0xFFD3D7DE);

        colors[ImGuiCol_SliderGrab] = hex(accent);
        colors[ImGuiCol_SliderGrabActive] = mix(hex(accent), ImVec4(0, 0, 0, 1), 0.15f);
        colors[ImGuiCol_CheckMark] = hex(accent);

        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab] = hex(0xFFC4C8D0);
        colors[ImGuiCol_ScrollbarGrabHovered] = hex(0xFFB0B5BF);
        colors[ImGuiCol_ScrollbarGrabActive] = hex(0xFF9AA0AB);

        colors[ImGuiCol_TableHeaderBg] = bgDeep;
        colors[ImGuiCol_TableBorderStrong] = hex(0xFFD3D7DE);
        colors[ImGuiCol_TableBorderLight] = hex(0xFFE4E7EC);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0.02f);

        colors[ImGuiCol_NavCursor] = hex(accent);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
    }

    applyStyleSizes();
}

void UiHost::applyStyleSizes() {
    ImGui::GetStyle().ScaleAllSizes(scale_);
}

void UiHost::newFrame() {
    if (!initialized_)
        return;

    const UINT dpi = ::GetDpiForWindow(hwnd_);
    refreshScale(dpi >= 48 ? static_cast<float>(dpi) / 96.0f : 1.0f, false);

    // 主题切换（设置窗口改深浅色后立即生效）
    if (dark_ != GlobalVar::isCurrentUIDarkMode) {
        dark_ = GlobalVar::isCurrentUIDarkMode;
        applyTheme();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // io.WantTextInput 在 ImGui::NewFrame() 里刷新，所以放在它后面
    updateImeAssociation(ImGui::GetIO().WantTextInput);
}

// 输入法跟着"有没有文本框在编辑"走：编辑时才把 IME 挂回窗口，其余时间窗口没有 IME，
// 单键快捷键（J/K/P/E…）不会被输入法吃掉。组字与候选窗口的位置由 ImGui 的
// Platform_SetImeDataFn 默认实现设置（它按文本框光标位置摆），这里只管挂/摘。
void UiHost::updateImeAssociation(bool wantTextInput) {
    if (!hwnd_ || wantTextInput == imeAttached_)
        return;

    imeAttached_ = wantTextInput;
    if (wantTextInput) {
        ::ImmAssociateContext(hwnd_, imeContext_);
        JARK_LOG("输入法：挂回窗口（有文本框在编辑）");
    }
    else {
        if (HIMC context = ::ImmAssociateContext(hwnd_, nullptr))
            imeContext_ = context; // 记下窗口的上下文，下次编辑时再挂回去
        JARK_LOG("输入法：从窗口摘下（不干扰快捷键）");
    }
}

void UiHost::renderDrawData() {
    if (!initialized_)
        return;

    ImGui::Render();

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

bool UiHost::mouseCaptured(bool windowVisible) const {
    return initialized_ && windowVisible && ImGui::GetIO().WantCaptureMouse;
}

bool UiHost::keyboardCaptured(bool windowVisible) const {
    return initialized_ && windowVisible && ImGui::GetIO().WantCaptureKeyboard;
}

bool UiHost::processMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!instance().initialized_)
        return false;

    // 让 ImGui 记录输入状态；是否消费由调用方按 WantCapture* 决定。
    // 后端返回非 0 = 它已经处理完（WM_IME_COMPOSITION 带 GCS_RESULTSTR、WM_SETCURSOR 等），
    // 调用方要直接返回，别让消息再落到 DefWindowProc（否则中文上屏会重复一遍）
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) != 0;
}

ImTextureID UiHost::textureFromImage(const cv::Mat& image, int slot) {
    if (!device_ || !context_ || image.empty())
        return 0;

    cv::Mat bgra;
    if (image.type() == CV_8UC4)
        bgra = image;
    else if (image.type() == CV_8UC3)
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    else if (image.type() == CV_8UC1)
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    else
        return 0;

    if (bgra.step != static_cast<size_t>(bgra.cols) * 4) {
        cv::Mat packed;
        bgra.copyTo(packed);
        bgra = packed;
    }

    TextureEntry& entry = textures_[slot];
    if (!entry.texture || entry.width != bgra.cols || entry.height != bgra.rows) {
        if (entry.view) {
            entry.view->Release();
            entry.view = nullptr;
        }
        if (entry.texture) {
            entry.texture->Release();
            entry.texture = nullptr;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(bgra.cols);
        desc.Height = static_cast<UINT>(bgra.rows);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &entry.texture))) {
            JARK_LOG("创建纹理失败 {}x{}", bgra.cols, bgra.rows);
            return 0;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = desc.Format;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;

        if (FAILED(device_->CreateShaderResourceView(entry.texture, &viewDesc, &entry.view))) {
            entry.texture->Release();
            entry.texture = nullptr;
            return 0;
        }

        entry.width = bgra.cols;
        entry.height = bgra.rows;
    }

    context_->UpdateSubresource(entry.texture, 0, nullptr, bgra.data,
        static_cast<UINT>(bgra.step), 0);

    return reinterpret_cast<ImTextureID>(entry.view);
}

void UiHost::releaseTextures() {
    for (auto& [slot, entry] : textures_) {
        (void)slot;
        if (entry.view) {
            entry.view->Release();
            entry.view = nullptr;
        }
        if (entry.texture) {
            entry.texture->Release();
            entry.texture = nullptr;
        }
    }
    textures_.clear();
}

} // namespace jark::ui
