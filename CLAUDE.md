# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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

# 标注逻辑自检（合成底图 + 像素断言，不需要人眼）
./x64/Release/JarkViewer.exe --probe --annotate [--annotate-out 输出目录] [图片]

# 列出某进程的可见窗口（自动化测试定位窗口用）
pwsh tools/list_windows.ps1 -ProcessId <pid>
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
- 界面改动可用 `tools/capture_window.ps1` 做视觉验证：`-Keys "{F1}"` 注入按键（窗口都在主窗口内，不再需要 `-Window` 选择）、`-Keys2 "{ESC}i"` + `-Keys2DelayMs` 送第二批按键（开窗、点击、再按键这类时序）、`-Hover "x,y"` 悬停、`-Drag "x1,y1,x2,y2[;...]"` 拖动或点击（物理客户区坐标）、`-RightClick "x,y"` + `-MenuKeys "b{ENTER}"` 右键菜单（菜单是独立弹窗，要配 `-Screen` 才截得到）。ImGui 的窗口默认居中于主窗口。
- 视频相关改动除 `--probe` 外，可用 `--probe --audio-test <文件>` 验证音频链路：它以音量 0 提交音频并观察播放时钟是否按采样率推进（不发出声音）。
- `JarkViewer/src/DecodeProbe.cpp` 提供无界面解码自检（`--probe`），用于在没有窗口的情况下验证解码路由与 EXIF 处理。
- `JarkViewer/include/BatchProcessor.h` 与 `src/BatchProcessor.cpp` 是批量处理逻辑（转换/缩放/旋转翻转/重命名/删除到回收站）：解码走工程内解码器（HEIC/AVIF/RAW 等也能参与转换），编码用 OpenCV；不依赖窗口，可用命令行 `--probe --batch <文件...> [--out-dir 目录] [--to 格式] [--max-edge N] [--rotate 90|180|270] [--flip-h|--flip-v] [--gray] [--invert] [--rename 前缀] [--overwrite]` 直接验证。
- `JarkViewer/include/BatchWindow.h` 是批量处理窗口（Ctrl+B 或右键菜单打开，处理当前目录的图片列表），ImGui 界面，处理在工作线程执行、界面轮询进度。
- `JarkViewer/include/ImageAdjust.h` 与 `src/ImageAdjust.cpp` 存放打印/编辑与批量共用的图像调整（亮度对比度、黑白/黑白文档/黑白抖动、反相、BGRA→白底 BGR），原先内嵌在 Printer.h 中。
- `JarkViewer/include/ImageAnnotator.h` 与 `src/ImageAnnotator.cpp` 是标注模型与渲染（矩形/椭圆/箭头/直线/画笔/马赛克/文字），含撤销重做与裁剪，纯逻辑不依赖窗口；`--probe --annotate [--annotate-out 目录]` 用合成底图跑 28 项像素断言自检。`JarkViewer/include/EditorWindow.h` 是编辑与标注窗口（主窗口 Ctrl+E 或右键菜单打开）：画布**固定按适应窗口显示、不提供缩放/平移**（拖动绘制、裁剪框选，屏幕坐标与图像像素一一对应，**点击必须落在图像上才算**，画布空白处不产生标注），透明图会先合成棋盘格（`rebuildTexture()` 里对 CV_8UC4 做，格色取主题的 `BLACK_GRID`/`WHITE_GRID`）并画图像边框，否则看不出可编辑范围；右侧工具栏提供工具/颜色/线宽/字号/填充/撤销重做/旋转翻转反相/应用裁剪/另存为/复制到剪贴板/覆盖原文件（**png 保留透明通道**，jpg 透明区域铺白底，见 `jark::encodeAnnotatedImage`）；覆盖保存后置 `GlobalVar::isNeedReloadImageCache` 让主窗口重载。
- 界面全部由 **Dear ImGui**（`JarkViewer/vendor/imgui`，Win32 + DX11 后端，随工程静态编译）绘制，
  宿主模块是 `JarkViewer/include/UiHost.h` 与 `src/UiHost.cpp`：创建上下文/后端、深浅两套主题
  （跟随 `GlobalVar::isCurrentUIDarkMode`）、按窗口 DPI 缩放字号与样式、系统字体（Segoe UI +
  微软雅黑 + 图标字体合并，1.92+ 动态字形加载）、以及把 `cv::Mat` 上传成 `ImTextureID` 的纹理池。
  业务侧只需实现 `D3D11App::DrawUi()` 提交界面，主循环在 `PresentFrame()` 里完成
  “画布贴后缓冲 → ImGui 一帧 → Present”。
- 各窗口都是 ImGui 窗口（不再是独立窗口线程）：`SettingWindow.h`（常规/文件关联/帮助/关于）、
  `PrintWindow.h`（打印预览与打印）、`BatchWindow.h`（批量处理）、`EditorWindow.h`（编辑与标注）。
  每个窗口都用 `ImGui::SetNextWindowSizeConstraints()` 设了**最小尺寸**，别写小到把控件藏起来；
  底部有固定内容（说明文字 + 按钮行）的页面要按实际高度给子区域留白（设置页的文件关联页就是这样
  算的），否则整页会多出一条窗口滚动条。
  主窗口的悬停按钮/动图播放条用 `ImGui::GetForegroundDrawList()` 贴 `file/mainRes.png`
  雪碧图（200x200，切片见 `main.cpp` 的 `OverlayIcons`，按 `uiScale()` 拉伸绘制，
  换图标只改这张图或切片表），EXIF 面板仍用前景列表排版文字；命中区域仍是原来的 `cursorPos` 逻辑。
  **叠加层必须经 `JarkViewerApp::uiPos()` 换算坐标**：多视口模式下主视口原点是“客户区左上角在
  屏幕上的位置”（`ImGui::GetMainViewport()->Pos`），直接按客户区坐标绘制会整体偏移，
  动图播放条会有一半被顶到客户区上边。切片 UV 要**内缩半个像素**，否则放大绘制时边缘会
  掺进相邻格（下面的播放条是亮的，会在打印/设置图标底部拉出一条亮线）。
- 输入分发的硬性规则：**只有确实有界面窗口在显示时，ImGui 才能独占鼠标键盘**
  （`D3D11App::hasVisibleWindows()` → `UiHost::mouseCaptured()/keyboardCaptured()`，实时判断）。
  ImGui 在最后一个窗口关闭后不会复位 `WantCapture*`（导航窗口、活动控件等状态还在），
  只按它拦截会让主窗口再也收不到任何操作。同理，用 Ctrl 组合键打开的窗口会吃掉 CTRL 的
  释放消息，这些分支必须清 `ctrlIsPressing`；`ESC` 在有窗口时先关窗口（`closeTopWindow()`），
  窗口失焦时也能兜住，不会直接退出程序。另外，空闲分支要靠 `anyWindowVisible()` 出帧，
  否则刚打开的窗口要等鼠标动了才画出来；反过来窗口刚被关掉时，这一帧的画面里还有它，
  `DrawUi()` 要 `markPresentRequested()` 补一帧把它擦掉，否则屏幕停在旧画面上，
  看起来就是“点了关闭按钮卡住”（要等鼠标动或系统重绘才恢复）。同一条规则适用于换图：
  `switchToFile()` 结束前要 `operateQueue.push({refresh})`，否则幻灯片在画面稳定时换图，
  主循环还走空闲分支，屏幕会停在上一张（播放到动图之后“不再换图”就是这样来的）。
- 幻灯片播放（'P' 键或右键菜单）会切到窗口全屏（`jarkUtils::SetFullScreen`，退出时还原；
  进来之前本来就全屏的话不还原），按 ESC 停止播放。
- 打印窗口的预览必须**基于 sourceImage_ 的副本**做调整（`refreshPreviewIfNeeded()` 里小图也要
  clone）：`applyImageAdjustments()` 是就地修改的，图小于预览上限时浅拷贝会连着原图一起改，
  预览会叠加前一次的效果、另存和打印也跟着错。`adjustBrightnessContrast()` 里对比度>100 时
  低灰度会算出负值，`pow(负底数)` 是 NaN（整片变黑），必须先夹到 0~255。调整参数在
  `close()` 里用 `rememberParameters()` 记进 `settingParameter`（关窗口就算数，不必先打印）。
- 主窗口（`D3D11App`/`JarkViewerApp`）是 PerMonitorHighDPIAware：`D3D11App::uiScale()`/`dp()`
  给出所在显示器的缩放（`WM_DPICHANGED`/`WM_SIZE` 时刷新）；ImGui 侧由 `UiHost` 统一缩放。
- 交换链使用**翻转模型**（`DXGI_SWAP_EFFECT_FLIP_DISCARD` + 双缓冲）。旧的
  `DXGI_SWAP_EFFECT_DISCARD` + 单缓冲在本机会出现“Present 返回成功但窗口全白”，
  改回旧模型前请先复现验证；`WM_PAINT` 与尺寸变化会置 `m_presentRequested`，
  空闲分支据此补一次呈现。
- `JarkViewer/include/Localization.h` 与 `src/Localization.cpp` 管界面语言（简体中文/繁體中文/English/日本語/한국어）：`UIStringTable[stringID][语言]` 与 `UIStringTableWide[stringID][语言]` 两张表（前者供画布文字、后者供 Win32 API；同 ID 文案不同是历史遗留，新增文案请追加到表尾）。`getUIString()` 按当前语言取用并在缺失时回退到英文、简体中文；`getUIStringW()` 由 UTF-8 转换而来。帮助/关于/提示/首页等资源图只有中英两套，`prefersChineseResources()` 决定用哪套（简繁用中文图，其余语言用英文图）。命令行 `--lang 0..4` 可临时指定语言。
- 图像内文字渲染（标注文字等）在 `JarkViewer/include/TextRenderer.h` 与 `src/TextRenderer.cpp`：
  用 stb_truetype 按**真实字形度量**（进退宽度/字距/bearing）绘制 UTF-8 文本，支持多行与按宽度折行，
  字形位图按 (字号, 码位) 缓存。字体全部取系统字体（微软雅黑/等线/黑体/宋体…），工程不再内嵌 ttf。
  界面文字由 ImGui 负责，不要再往 TextRenderer 里加界面相关职责。
- `JarkViewer/include/MediaDecoder.h` / `MediaPlayer.h` / `AudioOutput.h`（对应 `src/*.cpp`）组成媒体播放链路：`MediaDecoder` 在内存数据上做解复用+解码，按出现顺序产出视频帧或音频批（音频统一重采样为 48kHz 立体声 16 位）；`AudioOutput` 用 XAudio2 输出并提供已播放样本数作为主时钟；`MediaPlayer` 以音频时钟驱动视频帧、一次播完。实况照片（livp / MotionPhoto）与视频文件都走这条路：静态图/首帧作 `ImageAsset::primaryFrame`，视频字节放在 `ImageAsset::videoSource`，由主窗口自动播放一次后回到静态图（不再预解码成帧序列，避免上百 MB 内存）。
- `JarkViewer/include/CanvasRenderer.h` 与 `JarkViewer/src/CanvasRenderer.cpp` 是从 `JarkViewerApp` 抽出的画布绘制模块：按 `ViewState`（名义尺寸 / 定点缩放 / 平移 / 旋转）把图像绘制到 BGRA 画布，含透明区域棋盘格与图像边框。`JarkViewerApp::drawCanvas()` 只是把 `curPar` 转成 `ViewState` 后调用它。
- `JarkViewer/include/VectorImage.h` 与 `JarkViewer/src/VectorImage.cpp` 负责矢量图（SVG）的按需光栅化：`ImageAsset::vectorSource` 持有 lunasvg 文档与文档尺寸（intrinsic），位图分辨率随缩放变化（`vectorTargetEdge()` + `refreshVectorRaster()`，滞后阈值 1.25 避免缩放动画中反复渲染，长边上限 4096）。主窗口在画面稳定后（`DrawScene` 空闲分支）调用 `refreshVectorRasterIfNeeded()` 升级分辨率。
- `JarkViewer/include/jarkUtils.h` 与 `JarkViewer/src/jarkUtils.cpp` 集中放置 Win32/OpenCV 工具、主题/设置全局状态、剪贴板、全屏、资源读取、文件操作和日志。
- `JarkViewer/src/TextRenderer.cpp`、`stringRes.cpp`、`exifParse.cpp`、`videoDecoder.cpp`、`blpDecoder.cpp` 分别支撑图像文字渲染、多语言字符串、元数据解析、视频帧解码和 BLP 解码。

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
- UI 文本来自 `stringRes`：**两张表同名不同文案**——`UIStringTable[stringID][语言]` 供 ImGui/画布，`UIStringTableWide[stringID][语言]` 供 Win32（窗口标题、菜单、消息框），写 `getUIStringW(id)` 时一定要核对**宽表**里那个 ID 是什么（历史上出过把消息框正文写成"关于 (&A)"菜单项的事故）。`getUIStringW` 返回 `UIStringWide`（自带缓冲区、可隐式转 `const wchar_t*`）：旧实现共用同一个 thread_local 缓冲，`MessageBoxW(h, getUIStringW(a), getUIStringW(b))` 后一次转换会冲掉前一次的指针内容（标题乱码）。需要指针活过当前语句时（如 `BROWSEINFO.lpszTitle`）用 `.str()` 存一份 `std::wstring`；丢给 `std::format`/`wstring_view` 参数时要显式 `.c_str()`。**新增文案追加到对应表尾**，并在使用处写成具名常量（各窗口文件里已有 `kStr*` 常量块）。改动后再跑一次 `--probe --lang-test`，它会打印窄表与宽表各若干条文案用于确认 ID 没有错位。
- README 记录的 OpenCV 预编译库带有源码改动：移除 `imgcodecs` 分辨率限制，并将 HighGUI Win32 窗口光标从 `IDC_CROSS` 改为 `IDC_ARROW`；替换或重建 OpenCV 时要保留这些行为。
- 不要提交 `.vcxproj.user`、`.vs/` 或机器相关的本地库路径。
- 主窗口渲染路径以 OpenCV `cv::Mat` 作为 CPU 画布，再交给 Direct3D 显示；避免在高频绘制路径中引入阻塞 I/O 或昂贵同步操作。
- Debug 构建会分配控制台并启用 `JARK_LOG`；Release 下日志宏为空。
