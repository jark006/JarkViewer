#pragma once

// ImGui 宿主：把 Dear ImGui（Win32 + Direct3D 11 后端）接到主窗口的交换链上。
//
// 业务代码只需要三件事：
//   1) 在 DrawUi() 里用 ImGui:: 提交界面（窗口、控件）；
//   2) 需要显示图像时用 textureFromImage() 拿到纹理；
//   3) 主循环每帧调用 D3D11App::PresentFrame()（内部完成 ImGui 的一帧与 Present）。
//
// 主题、字体、DPI 缩放都在这里统一处理：高 DPI 下按比例放大字号与控件尺寸，
// 深浅色跟随 GlobalVar::isCurrentUIDarkMode。

#include <cstdint>
#include <string>
#include <unordered_map>

#include <windows.h>
#include <opencv2/opencv.hpp>

#include "imgui.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace jark::ui {

    // 编译期把 Unicode 码位编码成 UTF-8 字面量（占位图标用系统字体自带的符号）
    template <uint32_t Code>
    struct Glyph {
        char text[5]{};

        constexpr Glyph() {
            if constexpr (Code < 0x80) {
                text[0] = static_cast<char>(Code);
            }
            else if constexpr (Code < 0x800) {
                text[0] = static_cast<char>(0xC0 | (Code >> 6));
                text[1] = static_cast<char>(0x80 | (Code & 0x3F));
            }
            else if constexpr (Code < 0x10000) {
                text[0] = static_cast<char>(0xE0 | (Code >> 12));
                text[1] = static_cast<char>(0x80 | ((Code >> 6) & 0x3F));
                text[2] = static_cast<char>(0x80 | (Code & 0x3F));
            }
            else {
                text[0] = static_cast<char>(0xF0 | (Code >> 18));
                text[1] = static_cast<char>(0x80 | ((Code >> 12) & 0x3F));
                text[2] = static_cast<char>(0x80 | ((Code >> 6) & 0x3F));
                text[3] = static_cast<char>(0x80 | (Code & 0x3F));
            }
        }

        constexpr const char* c_str() const { return text; }
        constexpr operator const char* () const { return text; }
    };

    // 占位图标：先用 Segoe UI / Segoe Fluent Icons 自带的符号，
    // 后续换成专门绘制的图标时只需要改这里的码位。
    namespace icon {
        constexpr Glyph<0x25C0> kPrev;          // ◀ 上一张
        constexpr Glyph<0x25B6> kNext;          // ▶ 下一张
        constexpr Glyph<0x23F8> kPause;         // ⏸ 暂停
        constexpr Glyph<0x23F5> kPlay;          // ⏵ 继续
        constexpr Glyph<0x2197> kArrow;         // ↗ 箭头
        constexpr Glyph<0x2571> kLine;          // ╱ 直线
        constexpr Glyph<0x25A6> kMosaic;        // ▦ 马赛克
        constexpr Glyph<0x2715> kClose;         // ✕ 关闭

        // 以下取自 Segoe Fluent Icons / Segoe MDL2 Assets（已确认字形存在）
        constexpr Glyph<0xE7A7> kUndo;          // 撤销 / 逆时针
        constexpr Glyph<0xE7A6> kRedo;          // 重做 / 顺时针
        constexpr Glyph<0xE749> kPrint;         // 打印
        constexpr Glyph<0xE74E> kSave;          // 保存
        constexpr Glyph<0xE8C8> kCopy;          // 复制
        constexpr Glyph<0xE713> kSetting;       // 设置
        constexpr Glyph<0xE74D> kDelete;        // 删除
        constexpr Glyph<0xE8A3> kZoomIn;        // 放大
        constexpr Glyph<0xE71F> kZoomOut;       // 缩小
        constexpr Glyph<0xE8B7> kFolder;        // 文件夹
        constexpr Glyph<0xE8E5> kOpenFile;      // 打开
        constexpr Glyph<0xE91B> kPhoto;         // 图片
    }

    class UiHost {
    public:
        static UiHost& instance();

        bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain);
        void shutdown();
        bool ready() const { return hwnd_ != nullptr && device_ != nullptr; }

        float scale() const { return scale_; }
        HWND window() const { return hwnd_; }

        // 当前是否有界面在显示（没有界面时主循环可以完全跳过 ImGui 的一帧）
        bool uiVisible() const { return uiVisible_; }
        void setUiVisible(bool visible) { uiVisible_ = visible; }

        // —— 每帧 ——
        void newFrame();          // 开始一帧（自动处理 DPI/主题变化）
        void renderDrawData();    // 提交绘制命令（不 Present）

        // 把 OpenCV 图像上传成纹理（同一个 slot 复用同一张纹理）
        ImTextureID textureFromImage(const cv::Mat& image, int slot);
        void releaseTextures();

        bool mouseCaptured() const;
        bool keyboardCaptured() const;

        // Win32 消息先过这里，让 ImGui 记录输入状态
        static void processMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    private:
        UiHost() = default;
        ~UiHost() = default;
        UiHost(const UiHost&) = delete;
        UiHost& operator=(const UiHost&) = delete;

        void refreshScale(float scale, bool forceReload);
        void reloadFonts();
        void applyTheme();
        void applyStyleSizes();
        static std::string systemFontPath(const wchar_t* fileName);

        struct TextureEntry {
            ID3D11Texture2D* texture = nullptr;
            ID3D11ShaderResourceView* view = nullptr;
            int width = 0;
            int height = 0;
        };

        HWND hwnd_ = nullptr;
        ID3D11Device* device_ = nullptr;
        ID3D11DeviceContext* context_ = nullptr;
        IDXGISwapChain* swapChain_ = nullptr;

        float scale_ = 1.0f;
        bool dark_ = true;
        bool initialized_ = false;
        bool uiVisible_ = false;
        std::unordered_map<int, TextureEntry> textures_;
    };

} // namespace jark::ui
