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
#include <imm.h>        // HIMC（输入法上下文）
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
        constexpr Glyph<0x25AC> kRect;          // ▬ 矩形（标注工具；空心长方形 25AD ▭ 字体里没有）
        constexpr Glyph<0x25CB> kEllipse;       // ○ 椭圆（标注工具）
        constexpr Glyph<0x3030> kPen;           // 〰 画笔（标注工具）
        constexpr Glyph<0x25A3> kCrop;          // ▣ 裁剪（标注工具）

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

    // 按宽度折行画一段文本（CJK 逐字断行即可；拉丁文尽量在空格处断开）。
    // draw=false 只测量总高（返回占用高度，不画）；draw=true 从 top 起画，
    // clipTop/clipBottom 之外的行跳过（y 仍推进），配合滚动使用。
    // left/right/top 等都是**客户区坐标**，origin 传主视口左上角（多视口模式下
    // 画在前景列表上要换算，见各窗口里的 uiPos）；color 是已转好的 ImU32。
    // 看图窗口的 EXIF 面板与播放器的媒体信息面板共用这一份（别各写一套折行）。
    float drawWrappedText(ImDrawList* drawList, ImVec2 origin, float left, float top, float right,
        float clipTop, float clipBottom, const std::string& text, ImU32 color, bool draw);

    // 把窗口期望尺寸收敛到"主视口装得下"的范围（含少量余量）。
    // 多视口模式下，ImGui 只在主视口矩形**完全包含**窗口矩形时才把它并回主窗口绘制，
    // 装不下的窗口会被分离成独立 OS 窗口：圆角/边框不再跟随主题（角外露黑底）、还会
    // 跑到主窗口外面去。各工具窗口的 SetNextWindowSize/SetNextWindowSizeConstraints
    // 必须用本函数钳一下，否则主窗口比窗口小一点时整个交互形态就变了。
    ImVec2 fitWindowSizeToMainViewport(const ImVec2& desired);

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
        void releaseTexture(int slot);
        void releaseTextures();

        // ImGui 是否要独占鼠标/键盘。windowVisible 由调用方传入“是否有界面窗口在显示”
        // （见 D3D11App::hasVisibleWindows()）：ImGui 在最后一个窗口关闭后不会自己复位
        // WantCapture*，只看它会永久吞掉主窗口的输入。
        bool mouseCaptured(bool windowVisible) const;
        bool keyboardCaptured(bool windowVisible) const;

        // Win32 消息先过这里，让 ImGui 记录输入状态。
        // 返回 true 表示后端已经处理完这条消息，调用方**必须直接返回**、不要再走
        // DefWindowProc：WM_IME_COMPOSITION 落在 DefWindowProc 上会让输入法上屏的结果
        // 再生成一遍（中文会变成双份），WM_SETCURSOR 也是它自己设光标的。
        static bool processMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    private:
        UiHost() = default;
        ~UiHost() = default;
        UiHost(const UiHost&) = delete;
        UiHost& operator=(const UiHost&) = delete;

        void refreshScale(float scale, bool forceReload);
        void reloadFonts();
        void applyTheme();
        void applyStyleSizes();
        void updateImeAssociation(bool wantTextInput);
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

        // 输入法：窗口的 IME 上下文在 init 时摘下来存着，只有 ImGui 里文本框获得焦点
        // 时才挂回去（见 updateImeAssociation），这样中文能输入、又不会让输入法吃掉快捷键
        HIMC imeContext_ = nullptr;
        bool imeAttached_ = false;
    };

} // namespace jark::ui
