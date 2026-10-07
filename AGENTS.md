# AGENTS.md

This file provides guidance to Codex and other coding agents when working with code in this repository.

## 项目概览

JarkViewer 是 Windows 10/11 x64 原生图片查看器，使用 C++23、Win32、Direct3D 11 和 OpenCV 构建。它以单可执行文件方式发布，重点支持大量静态图、动图、RAW、LivePhoto/MotionPhoto、EXIF 信息显示、打印/简单编辑和文件关联。

## 常用命令

本项目优先使用 PowerShell 执行仓库根目录下的 `buildRelease.ps1` 脚本进行编译、构建：

```powershell
# Release x64 构建
pwsh ./buildRelease.ps1

# 运行已构建程序
./x64/Release/JarkViewer.exe
./x64/Release/JarkViewer.exe "D:/path/to/image.png"

# 无界面解码自检（无需人眼，验证解码路由/EXIF/动图）
./x64/Release/JarkViewer.exe --probe <文件或通配展开的路径...> [--full] [--out 报告.txt]

# Release 下临时打开日志（写入 %TEMP%\JarkViewer.log；Debug 默认开启控制台日志）
$env:JARKVIEWER_LOG=1; ./x64/Release/JarkViewer.exe --log "D:/path/to/image.png"

# 截取程序窗口做视觉验证（不截取桌面其它内容；-Keys 可注入按键，如 "{UP 15}" 放大）
pwsh tools/capture_window.ps1 -Exe x64/Release/JarkViewer.exe -Argument "img.svg" -Out shot.png

# 生成测试语料（需要 Python + Pillow/numpy，可选 ffmpeg）
python tools/gen_testdata.py <输出目录>
```

每次修改后至少保证 `buildRelease.ps1` 能干净编译通过；解码相关改动应先用 `--probe` 跑一遍 `tools/gen_testdata.py` 生成的语料（包含错扩展名、无扩展名、损坏文件、EXIF 方向、动图、视频等），再做人工冒烟（静态图加载、动图播放、EXIF 显示、打印预览和导出流程）。

## 构建前提

- 项目文件是 `JarkViewer/JarkViewer.vcxproj`，工具集为 `v145`，语言标准为 C++23，目标平台为 x64；需要安装支持 v145 工具集的 Visual Studio/Build Tools。
- `JarkViewer.vcxproj` 中 `VcpkgEnabled=false`，默认使用仓库内的静态库目录：`JarkViewer/lib*`、`JarkViewer/ffmpeg`、`JarkViewer/include`。
- README 说明第三方静态库需从 release 的 `static_lib` 包准备；如果改为 vcpkg，需要在项目属性中启用并补齐依赖。
- Release 输出程序位于 `x64/Release/JarkViewer.exe`，中间文件位于 `JarkViewer/x64/<Configuration>`。

## 高层架构

- `JarkViewer/src/main.cpp` 定义 `JarkViewerApp` 和 `wWinMain`。入口初始化 Exiv2 BMFF、禁用 IME、初始化 COM，然后创建窗口、解析命令行图片路径并进入主循环。
- `JarkViewer/include/D3D11App.h` 与 `JarkViewer/src/D3D11App.cpp` 提供 Win32 窗口、消息分发、Direct3D 11 设备/交换链和 `PresentCanvas()`。业务层通过继承并实现鼠标、键盘、拖放、右键菜单和绘制回调。
- `JarkViewer/include/ImageDatabase.h` 与 `JarkViewer/src/ImageDatabase.cpp` 负责图片加载、格式分派、EXIF 处理和 LRU 缓存。核心路径是 `ImageDatabase::loader()` → `myLoader()` → **按文件头（魔数）嗅探格式后再分派**：`FormatSniffer` 判定真实格式 → `decodeByFormat()` 调用 JXL/WP2/AVIF/HEIF/RAW/SVG/PSD/OpenCV/WIC/FFmpeg 等解码器 → 统一转为 OpenCV `cv::Mat`；嗅探失败或解码失败时再用扩展名路由兜底，最后才是 OpenCV/WIC 通用兜底。EXIF 后处理统一由 `applyExifInfo()` 按 `ExifPolicy`（None/SimpleOnly/Full/FullWithOrientation）完成，不再散落在各格式分支里。
- `JarkViewer/include/FormatSniffer.h` 与 `JarkViewer/src/FormatSniffer.cpp` 是纯文件头嗅探模块（不依赖任何第三方库）：扩展名与文件头冲突时以文件头为准，但 RAW/视频/LIVP/LEP/TGA 等扩展名携带文件头无法表达的信息（`isExtensionAuthoritative()`）时优先按扩展名路由。`JarkThumbnailProvider` 里的同名模块与其同源，后续计划合并为两个工程共用的模块。
- `JarkViewer/src/DecodeProbe.cpp` 提供无界面解码自检（`--probe`），用于在没有窗口的情况下验证解码路由与 EXIF 处理。
- `JarkViewer/include/VectorImage.h` 与 `JarkViewer/src/VectorImage.cpp` 负责矢量图（SVG）的按需光栅化：`ImageAsset::vectorSource` 持有 lunasvg 文档与文档尺寸（intrinsic），位图分辨率随缩放变化（`vectorTargetEdge()` + `refreshVectorRaster()`，滞后阈值 1.25 避免缩放动画中反复渲染，长边上限 4096）。主窗口在画面稳定后（`DrawScene` 空闲分支）调用 `refreshVectorRasterIfNeeded()` 升级分辨率。
- `JarkViewer/include/jarkUtils.h` 与 `JarkViewer/src/jarkUtils.cpp` 集中放置 Win32/OpenCV 工具、主题/设置全局状态、剪贴板、全屏、资源读取、文件操作和日志。
- `JarkViewer/include/Printer.h` 和 `JarkViewer/include/Setting.h` 是打印与设置界面，均继承自轻量基类 `JarkViewer/include/MatWindow.h`。`MatWindow` 用纯 Win32 API（`RegisterClassExW` + `CreateWindowExW` + 自己的 `wndProc` 与消息循环）创建独立窗口，子类把 UI 绘制到 `cv::Mat m_uiCanvas` 上，最后通过 GDI `StretchDIBits` 把 BGRA Mat 贴到窗口 DC，这里 OpenCV 只用作画布像素操作（`cv::rectangle`、`cv::cvtColor` 等）。
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
- 新增图片格式时，同时检查 `ImageDatabase::supportExt` / `supportRaw`、`FormatSniffer`（文件头嗅探与扩展名映射）、`decodeByFormat()` 分支、EXIF/方向处理、设置页文件关联列表和 README 格式列表。
- `buildRelease.ps1` 必须保持 ASCII-only，且不能用 `ProcessStartInfo.ArgumentList`（Windows PowerShell 5.1 不支持，会静默丢掉全部参数并退化成默认 Debug 构建）。
- `JarkViewerApp::drawCanvas()` 有两条必须同时成立的规则：**几何尺寸用名义尺寸**（`curPar.width/height`，矢量图 100% 时屏幕上应有的尺寸），**采样密度用位图分辨率**（`srcScaleX/srcScaleY = 位图尺寸 / 名义尺寸`）。矢量图的位图分辨率会随缩放变化，任何"用 `srcImg.cols/rows` 当几何尺寸"或"用 `zoomInvert` 直接换算位图坐标"的写法都会让画面尺寸/位置错乱。
- Release 构建默认不打印日志，排障时用 `--log` 或 `JARKVIEWER_LOG=1`（写入 `%TEMP%\JarkViewer.log`）；新增诊断日志直接写 `JARK_LOG(...)` 即可，`isLogEnabled()` 为假时不会计算参数。
- UI 文本来自 `stringRes`，设置/帮助/关于和打印按钮大量使用资源图切片；改文案或布局时要同步检查中文、英文、浅色、深色资源。
- README 记录的 OpenCV 预编译库带有源码改动：移除 `imgcodecs` 分辨率限制，并将 HighGUI Win32 窗口光标从 `IDC_CROSS` 改为 `IDC_ARROW`；替换或重建 OpenCV 时要保留这些行为。
- 不要提交 `.vcxproj.user`、`.vs/` 或机器相关的本地库路径。
- 主窗口渲染路径以 OpenCV `cv::Mat` 作为 CPU 画布，再交给 Direct3D 显示；避免在高频绘制路径中引入阻塞 I/O 或昂贵同步操作。
- Debug 构建会分配控制台并启用 `JARK_LOG`；Release 下日志宏为空。
