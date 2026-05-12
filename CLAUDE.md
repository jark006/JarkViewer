# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

JarkViewer 是 Windows 10/11 x64 原生图片查看器，使用 C++23、Win32、Direct3D 11 和 OpenCV 构建。它以单可执行文件方式发布，重点支持大量静态图、动图、RAW、LivePhoto/MotionPhoto、EXIF 信息显示、打印/简单编辑和文件关联。

## 常用命令

在 Visual Studio Developer Command Prompt、Developer PowerShell，或已加载 MSBuild 环境的终端中执行：

```powershell
# Release x64 构建
MSBuild.exe JarkViewer.slnx /m /p:Configuration=Release /p:Platform=x64

# Debug x64 构建
MSBuild.exe JarkViewer.slnx /m /p:Configuration=Debug /p:Platform=x64

# 只构建 vcxproj
MSBuild.exe JarkViewer/JarkViewer.vcxproj /m /p:Configuration=Release /p:Platform=x64

# 清理 Release x64
MSBuild.exe JarkViewer.slnx /t:Clean /p:Configuration=Release /p:Platform=x64

# 运行已构建程序
./x64/Release/JarkViewer.exe
./x64/Release/JarkViewer.exe "D:/path/to/image.png"
```

### 在 Bash 环境下的 MSBuild 调用区别

如果在 Bash 环境下，参数前缀要使用`-`，而不是`/`，不然参数会被当作路径，导致报错：“MSBUILD : error MSB1008: 只能指定一个项目”。而且路径分隔符要使用正斜杆 `/`。

```bash
MSBuild.exe JarkViewer/JarkViewer.vcxproj -m -p:Configuration=Release -p:Platform=x64
```

## 普通终端环境需要使用 MSBuild.exe 的绝对路径
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"

当前仓库没有检测到自动化测试、lint 或格式化配置。每次修改后至少保证 `Release|x64` 能干净编译；行为变更需手动冒烟验证静态图加载、动图播放、EXIF 显示、打印预览和导出流程。

## 构建前提

- 项目文件是 `JarkViewer/JarkViewer.vcxproj`，工具集为 `v145`，语言标准为 C++23，目标平台为 x64；需要安装支持 v145 工具集的 Visual Studio/Build Tools。
- `JarkViewer.vcxproj` 中 `VcpkgEnabled=false`，默认使用仓库内的静态库目录：`JarkViewer/lib*`、`JarkViewer/ffmpeg`、`JarkViewer/include`。
- README 说明第三方静态库需从 release 的 `static_lib` 包准备；如果改为 vcpkg，需要在项目属性中启用并补齐依赖。
- Release 输出程序位于 `x64/Release/JarkViewer.exe`，中间文件位于 `JarkViewer/x64/<Configuration>`。

## 高层架构

- `JarkViewer/src/main.cpp` 定义 `JarkViewerApp` 和 `wWinMain`。入口初始化 Exiv2 BMFF、禁用 IME、初始化 COM，然后创建窗口、解析命令行图片路径并进入主循环。
- `JarkViewer/include/D3D11App.h` 与 `JarkViewer/src/D3D11App.cpp` 提供 Win32 窗口、消息分发、Direct3D 11 设备/交换链和 `PresentCanvas()`。业务层通过继承并实现鼠标、键盘、拖放、右键菜单和绘制回调。
- `JarkViewer/include/ImageDatabase.h` 与 `JarkViewer/src/ImageDatabase.cpp` 负责图片加载、格式分派、EXIF 处理和 LRU 缓存。核心路径是 `ImageDatabase::loader()` → `myLoader()` → 按扩展名调用 JXL/WP2/AVIF/HEIF/RAW/SVG/PSD/OpenCV/WIC/FFmpeg 等解码器 → 统一转为 OpenCV `cv::Mat`。
- `JarkViewer/include/jarkUtils.h` 与 `JarkViewer/src/jarkUtils.cpp` 集中放置 Win32/OpenCV 工具、主题/设置全局状态、剪贴板、全屏、资源读取、文件操作和日志。
- `JarkViewer/include/Printer.h` 和 `JarkViewer/include/Setting.h` 是基于 OpenCV 窗口绘制的打印与设置界面；主窗口会在打开其中一个时请求另一个退出，因为这些 OpenCV 窗口暂时不能共存。
- `JarkViewer/src/TextDrawer.cpp`、`stringRes.cpp`、`exifParse.cpp`、`videoDecoder.cpp`、`blpDecoder.cpp` 分别支撑文字绘制、多语言字符串、元数据解析、视频帧解码和 BLP 解码。

## 代码约定

- 源码使用 UTF-8 和 C++23；现有代码主要采用 4 空格缩进。
- 类型名多用 `PascalCase`，函数、方法和局部变量多用 `camelCase`；新增代码优先贴合相邻文件风格。
- 提交信息惯例是简短中文描述，例如“优化PSD解码”“更新版本号”。

## 运行时数据流

1. `wWinMain` 读取命令行路径并调用 `JarkViewerApp::initOpenFile()`。
2. `initOpenFile()` 扫描同目录下所有受支持图片扩展，按 Windows 自然排序建立 `imgFileList`。
3. 当前图片通过 `ImageDatabase::getSafePtr()` 进入缓存；切换图片时会预取相邻图片。
4. 鼠标、键盘、滚轮、拖放和菜单事件转成 `ActionENUM` 放入 `OperateQueue`。
5. `JarkViewerApp::DrawScene()` 消费操作队列，更新缩放、平移、旋转、帧索引、EXIF 显示、打印/设置窗口等状态。
6. 当前帧绘制到 CPU 端 `cv::Mat mainCanvas`，最后通过 `D3D11App::PresentCanvas()` 上传到 D3D11 纹理并显示。

## 修改注意事项

- `SettingParameter` 按固定 4096 字节设置文件持久化；不要随意调整成员顺序、大小或删除保留字段，否则会破坏旧设置兼容性。
- 新增图片格式时，同时检查 `ImageDatabase::supportExt` / `supportRaw`、加载分派逻辑、EXIF/方向处理、设置页文件关联列表和 README 格式列表。
- UI 文本来自 `stringRes`，设置/帮助/关于和打印按钮大量使用资源图切片；改文案或布局时要同步检查中文、英文、浅色、深色资源。
- README 记录的 OpenCV 预编译库带有源码改动：移除 `imgcodecs` 分辨率限制，并将 HighGUI Win32 窗口光标从 `IDC_CROSS` 改为 `IDC_ARROW`；替换或重建 OpenCV 时要保留这些行为。
- 不要提交 `.vcxproj.user`、`.vs/` 或机器相关的本地库路径。
- 主窗口渲染路径以 OpenCV `cv::Mat` 作为 CPU 画布，再交给 Direct3D 显示；避免在高频绘制路径中引入阻塞 I/O 或昂贵同步操作。
- Debug 构建会分配控制台并启用 `JARK_LOG`；Release 下日志宏为空。
