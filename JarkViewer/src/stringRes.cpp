#include "jarkUtils.h"

// 界面字符串表最大条目数
constexpr uint32_t STRING_MAX_NUM = 1024;

// 界面字符串表，暂时仅支持中文和英文
// 条目所在行号减10即为 stringID
// 因为使用索引是硬编码，所以不要随意在中间增减条目，会打乱索引，只能在后面追加条目
const char* const UIStringTable[STRING_MAX_NUM][2] = {
    {"NULL", "NULL"},
    {"设置", "Settings"},
    {"常规", "General"},
    {"文件关联", "Association"},
    {"帮助", "Help"},
    {"关于", "About"},
    {"常见格式", "Common Formats"},  
    {"选择常用", "Select Common"},
    {"全选", "Select All"},
    {"全不选", "Clear All"},
    {"立即关联", "Apply"}, // 10
    {"本软件原生绿色单文件，请把软件放置到合适位置再关联文件格式，若软件位置变化则需重新关联。\n若不再使用本软件，请点击【全不选】再点击【立即关联】即可移除所有关联关系。", "This software is a portable single file.  Please place the software in an appropriate location before associating file formats.\nIf you no longer to use this software,  please click \"Clear All\" and then click \"Apply\" to remove all associations." },
    {"旋转动画", "Rotate Animation"},
    {"缩放动画", "Zoom Animation"},
    {"优化渲染 (拖动图像时会微微失真)", "Optimized rendering (slight distortion when dragging)"},
    {"删除前提示", "Confirm Before Delete"},
    {"切换动画模式", "Switch Animation Mode"},
    {"幻灯片顺序", "Slideshow Order"},
    {"幻灯片间隔(秒)", "Slideshow Interval (seconds)"},
    {"编译时间 UTC+8", "[Build time UTC+8]"},
    {"切图动画", "SwitchAnim"},  // 20
    {"无动画", "Off"},
    {"上下滑动", "Vertical"},
    {"左右滑动", "Horizontal"},
    {"主题", "Theme"},
    {"跟随系统", "System"},
    {"浅色", "Light"},
    {"深色", "Dark"},
    {"语言", "Language"},
    {"打印", "Print"},
    {"中文", "中文"},  // 30
    {"English", "English"},
    {"使用Ctrl+O或拖入图像文件打开", "Use Ctrl+O or drag the image file to open."},
    {"图像格式不支持", "Image format not supported"},
    {"JarkViewer看图", "JarkViewer"},
    {"错误", "Error"},
    {"鼠标右键", "RightClick"},
    {"菜单", "Menu"},
    {"退出程序", "Exit"},
    {"路径", "Path"},
    {"大小", "FileSize"},  // 40
    {"分辨率", "Resolution"},
    {"【按 C 键复制图像全部信息】", "[Press C to copy all image information]"},
    {"\n正提示词: ", "\nPositive prompt: "},
    {"\n\n反提示词: ", "\n\nNegative prompt: "},
    {"\n\n参数: Steps:", "\n\nParameter: Steps:"},
    {"\n\nAI生图提示词:\n", "\n\nAI-generated image prompt:\n"},
    {"北纬 N", "North Latitude"},
    {"南纬 S", "South Latitude"},
    {"东经 E", "East Longitude"},
    {"西经 W", "West Longitude"},  // 50
    {"子图数量", "Number of subImage"},
    {"\n\nAI生图提示词 ComfyUI工作流.json\n", "\n\nAI-generated image prompt ComfyUI_workflow.json\n"},
    {"\n方向: ", "\nExif.Image.Orientation: "},
    {"XX", "XX"},
    {"XX", "XX"},
    {"XX", "XX"},
    {"XX", "XX"},
    {"XX", "XX"},

};


const wchar_t* const UIStringTableW[STRING_MAX_NUM][2] = {
    {L"NULL", L"NULL"},
    {L"JarkViewer看图", L"JarkViewer"},
    {L"文件关联设置成功！", L"Association successful!"},
    {L"文件关联设置失败！", L"Association failed!"},
    {L"保存当前帧到图像文件", L"Save the current frame to an image file"},
    {L"是否要将此动图或实况图视频的全部帧批量保存到png图片文件？\n\n帧数：", L"Batch save each frames of this animated image to PNG image files? \n\nFrames:"},  // 5
    {L"保存每一帧到原图所在文件夹", L"Save each frame to the folder containing the current image"},
    {L"确定要将以下文件移至回收站吗？", L"Move the following files to the recycle bin?"},
    {L"删除失败，错误码", L"Deletion failed, error code"},
    {L"逐帧浏览", L"Frame by frame"},
    {L"逆时针旋转90°", L"Rotate 90° counterclockwise"}, // 10
    {L"顺时针旋转90°", L"Rotate 90° clockwise"},
    {L"旋转180°", L"Rotate 180°"},
    {L"窗口创建失败！", L"Window creation failed!"},
    {L"错误", L"Error"},
    {L"提示", L"Tips"}, // 15
    {L"图像分辨率太大，将缩放到", L"Resolution is too high, it will be scaled down to"},
    {L"图像为空，无法复制到剪贴板", L"The image is empty and cannot be copied to the clipboard."},
    {L"图像通道: ", L"Channel: "},
    {L"不支持的图像格式", L"Unsupported image formats"},
    {L"图像格式转换失败", L"Image format conversion failed"}, // 20
    {L"无法打开剪贴板", L"Unable to open clipboard"},
    {L"清空剪贴板失败", L"Clearing clipboard failed"},
    {L"保存到图像文件", L"Save to image file"},
    {L"若不再使用本软件，请关闭软件后再手动删除配置文件:\n", L"If no longer in use this program, please close the program and then manually delete the configuration files:  "},
    {L"复制EXIF信息 (&E)", L"Copy &EXIF info"},  // 25
    {L"复制文件路径 (&P)", L"Copy file &path"},
    {L"复制图像数据 (&C)", L"&Copy image data"},
    {L"显示EXIF信息 (&I)", L"Show EXIF &info"},
    {L"打开所在位置 (&L)", L"Open file &location"},
    {L"删除到回收站 (&D)", L"Move to recycle bin"},  // 30
    {L"打印 (&P)", L"&Print"},
    {L"设置 (&S)", L"&Settings"},
    {L"关于 (&A)", L"&About"},
    {L"退出 (&X)", L"E&xit"},
    {L"打开新图像 (&O)", L"&Open new image"},  // 35
    {L"文件属性 (&A)", L"File properties"},
    {L"帮助 (&H)", L"&Help"},
    {L"全屏 (&F)", L"&FullScreen"},
    {L"设置", L"Settings"},
    {L"打印", L"Print"},  // 40
    {L"XX", L"XX"},
    {L"XX", L"XX"},

};

// 获取字符串
const char* const getUIString(const uint32_t stringidx) {
    if (stringidx >= STRING_MAX_NUM)
        return "NULL";
    return UIStringTable[stringidx][GlobalVar::settingParameter.UI_LANG];
}

const wchar_t* const getUIStringW(const uint32_t stringidx) {
    if (stringidx >= STRING_MAX_NUM)
        return L"NULL";
    return UIStringTableW[stringidx][GlobalVar::settingParameter.UI_LANG];
}