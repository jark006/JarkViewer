#pragma once

// 主页与解码失败提示的画面：按当前语言、主题、DPI 实时绘制到画布 Mat。
//
// 取代过去的 home.png / tips.png 资源图（只有中英两套、深浅色写死、支持格式列表容易过期）：
// 文案全部来自 UIStringTable（五种语言），配色取自 ThemeColor，支持列表直接读
// ImageDatabase 的扩展名集合，永远与实际能力一致。

#include <cstdint>
#include <string>

// 先完整包含 opencv（与 CanvasRenderer.h 一致）：core.hpp 单包含时，
// 后续 windows.h 的 min/max 宏会破坏 opencv 模板头的解析
#include <opencv2/opencv.hpp>

#include "jarkUtils.h"

namespace jark {

// 内容指纹：尺寸、DPI 缩放、界面语言、深浅主题任一变化都应重新绘制
uint64_t infoScreenStamp(cv::Size size, float scale);

// 绘制主页/解码失败占位画面（CV_8UC4，尺寸为画布的物理像素，scale 为界面 DPI 缩放）。
// detail 为补充信息（通常是文件路径），主页可传空。
cv::Mat renderInfoScreen(PlaceholderKind kind, const std::wstring& detail, cv::Size size, float scale);

// 供 --probe 输出使用的短名（home / unsupported / decode-failed / missing）
const char* placeholderName(PlaceholderKind kind);

} // namespace jark
