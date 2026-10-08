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

# 色彩管理自检（源/目标同为 sRGB 时恒等跳过、变换生效、四通道 alpha 不动、大图并行与串行逐字节一致）
./x64/Release/JarkViewer.exe --probe --color-test

# SVG 自检（light-dark()/var() 折叠、半透明区域反预乘为直通 alpha、按可视区域光栅化的区域/旋转语义）
./x64/Release/JarkViewer.exe --probe --svg-test

# 文件列表排序自检（名称自然序 / 修改时间 / 文件大小，以及当前图片下标跟随重排）
./x64/Release/JarkViewer.exe --probe --sort-test

# Exif UserComment 编码自检（AI 生图提示词的 UNICODE/ASCII/无前缀 × UTF-16 大小端/BOM/中文）
./x64/Release/JarkViewer.exe --probe --exif-test

# 主界面导航自检（鸟瞰几何/定位/输入归属断言）与缩略图缓存自检（1000 项 LRU/双进程并发/清理 epoch/Shell 失败后的本地解码兜底）
./x64/Release/JarkViewer.exe --probe --navigation-test
./x64/Release/JarkViewer.exe --probe --thumbnail-test [--out-dir 临时目录]

# 系统缩略图链路自检（经 Shell 取一张缩略图存 PNG，验证处理器/方向/透明度）
./x64/Release/JarkViewer.exe --probe --shell-thumbnail <图片> [--out-dir 输出目录]

# 主界面导航实机交互（鸟瞰拖动/拖出客户区释放/悬停缩略图带/点击换图/滚轮隔离）
pwsh tools/test_navigation.ps1 -Exe x64/Release/JarkViewer.exe -Image <图片> -OutDirectory <截图目录> [-CheckSettings]

# 列出某进程的可见窗口（自动化测试定位窗口用）
pwsh tools/list_windows.ps1 -ProcessId <pid>

# 源码不变量检查（字符串表 // N 编号、格式清单与 README 交叉核对、PSD 顺序、LRU 析构、
# 菜单加速键、版本号一致；只读源码、秒级出结果，提交前可单独跑）
pwsh tools/check_source_invariants.ps1
# 反向验证上面这套检查：逐项制造错误确认真能抓到，跑完按原字节还原（新增检查项要同步加破坏）
pwsh tools/verify_source_invariant_checks.ps1
```

每次修改后至少保证 `buildRelease.ps1` 能干净编译通过；解码相关改动应先用 `--probe` 跑一遍 `tools/gen_testdata.py` 生成的语料（包含错扩展名、无扩展名、损坏文件、EXIF 方向、动图、视频等），再做人工冒烟（静态图加载、动图播放、EXIF 显示、打印预览和导出流程）。

## 构建前提

- 项目文件是 `JarkViewer/JarkViewer.vcxproj`，工具集为 `v145`，语言标准为 C++23，目标平台为 x64；需要安装支持 v145 工具集的 Visual Studio/Build Tools。
- `JarkViewer.vcxproj` 中 `VcpkgEnabled=false`，默认使用仓库内的静态库目录：`JarkViewer/lib*`、`JarkViewer/ffmpeg`、`JarkViewer/include`。
- README 说明第三方静态库需从 release 的 `static_lib` 包准备；如果改为 vcpkg，需要在项目属性中启用并补齐依赖。
- `JarkViewer/libopencv/zlib.lib` 已换成 **zlib-ng 的 compat 构建**（大 PNG 解压约快 25%）。compat 模式不改符号名，OpenCV 是最终链接时才解析 inflate，所以是原地替换、不用重建 OpenCV；但 **include 下的 `zlib.h`/`zconf.h`/`zlib_name_mangling.h` 必须与 .lib 是同一来源**。换机器或重建静态库环境时跑一次 `pwsh tools/build-zlib-ng.ps1 -Install`（原件备份在同目录 `zlib-1.3.1.lib`；头文件回退用 git checkout）。
- Release 输出程序位于 `x64/Release/JarkViewer.exe`，中间文件位于 `JarkViewer/x64/<Configuration>`。

## 高层架构

- `JarkViewer/src/main.cpp` 定义 `JarkViewerApp` 和 `wWinMain`。入口初始化 Exiv2 BMFF、禁用 IME、初始化 COM，然后创建窗口、解析命令行图片路径并进入主循环。
- `JarkViewer/include/D3D11App.h` 与 `JarkViewer/src/D3D11App.cpp` 提供 Win32 窗口、消息分发、Direct3D 11 设备/交换链和 `PresentCanvas()`。业务层通过继承并实现鼠标、键盘、拖放、右键菜单和绘制回调；窗口句柄刚创建、D3D 设备尚未建时另有一次 `OnWindowCreated()` 回调——主窗口在这里就把首图解码派发出去（只派发不等待），和随后几十毫秒的设备/交换链创建并行。等待与收尾仍走 `initOpenFile`：它靠 `startupFilePrepared_` 标志跳过重扫（重扫里的 `imgDB.clear()` 会把在途解码作废），命令行 `--lang` 也因此要提前到 `InitWindow` 之前应用（窗口一就绪就会扫目录、放占位并派发解码）。
- `JarkViewer/include/ImageDatabase.h` 与 `JarkViewer/src/ImageDatabase.cpp` 负责图片加载、格式分派、EXIF 处理和 LRU 缓存。核心路径是 `ImageDatabase::loader()` → `myLoader()` → **按文件头（魔数）嗅探格式后再分派**：`FormatSniffer` 判定真实格式 → `decodeByFormat()` 调用 JXL/WP2/AVIF/HEIF/RAW/SVG/PSD/OpenCV/WIC/FFmpeg 等解码器 → 统一转为 OpenCV `cv::Mat`；嗅探失败或解码失败时再用扩展名路由兜底，最后才是 OpenCV/WIC 通用兜底。EXIF 后处理统一由 `applyExifInfo()` 按 `ExifPolicy`（None/SimpleOnly/Full/FullWithOrientation）完成，不再散落在各格式分支里。
- `JarkViewer/include/FormatSniffer.h` 与 `JarkViewer/src/FormatSniffer.cpp` 是纯文件头嗅探模块（不依赖任何第三方库）：扩展名与文件头冲突时以文件头为准，但 RAW/视频/LIVP/LEP/TGA 等扩展名携带文件头无法表达的信息（`isExtensionAuthoritative()`）时优先按扩展名路由。`JarkThumbnailProvider` 里的同名模块与其同源，后续计划合并为两个工程共用的模块。
- 界面改动可用 `tools/capture_window.ps1` 做视觉验证：`-Keys "{F1}"` 注入按键（窗口都在主窗口内，不再需要 `-Window` 选择）、`-Keys2 "{ESC}i"` + `-Keys2DelayMs` 送第二批按键（开窗、点击、再按键这类时序）、`-Hover "x,y"` 悬停、`-Drag "x1,y1,x2,y2[;...]"` 拖动或点击（物理客户区坐标）、`-RightClick "x,y"` + `-MenuKeys "b{ENTER}"` 右键菜单（菜单是独立弹窗，要配 `-Screen` 才截得到）、`-DragHold` 拖到最后一个点**不松手**再截图（验证"拖动中"才有的画面，如马赛克/裁剪的拖框）。ImGui 的窗口默认居中于主窗口。编辑窗口的文字工具另有 `tools/test_editor_text.ps1`：选文字工具 → 点锚点 → 点侧栏文字框 → 投递 Unicode `WM_CHAR`（等价于输入法上屏后的字符），一张图同时验证锚点光标、文字预览和中文能进输入框。主界面导航另有 `tools/test_navigation.ps1`：动态按 DPI 换算客户区坐标，依次验证鸟瞰拖动、拖出客户区释放、悬停展开缩略图带、点击直接换图、滚轮只滚条带不穿透、移出后隐藏、设置窗口往返；加 `-CheckSettings` 时还会点「清理缓存」「显示鸟瞰图」并重启核对设置文件字节（该分支要求 `-Exe` 指向 `%TEMP%` 下的独立副本，避免动到用户的设置与缓存）。
- 视频相关改动除 `--probe` 外，可用 `--probe --audio-test <文件>` 验证音频链路：它以音量 0 提交音频并观察播放时钟是否按采样率推进（不发出声音）。
- **播放流畅度自检 `--probe --playback-test <文件...>`**：音量 0 起一个真实 `MediaPlayer`，像主循环那样定时取帧，把"卡不卡"变成数字——交付率、交付间隔中位/最大、**播放时钟倍速与最大停滞**、总用时（对照媒体时长）。关键不变式是**时钟按 1 倍速推进**：音频时钟一停，取帧判定就不再满足，画面跟着停——"画面卡 + 声音一卡一卡"是同一个故障的两个表现。卡顿这类问题靠眼睛看不出是解码、队列还是时钟，改动 `MediaPlayer`/`AudioOutput` 后拿它跑一遍实况照片与视频（`D:\Downloads\test\livp` 整个目录是一个好语料）。
- `JarkViewer/src/DecodeProbe.cpp` 提供无界面解码自检（`--probe`），用于在没有窗口的情况下验证解码路由与 EXIF 处理。
- `JarkViewer/include/BatchProcessor.h` 与 `src/BatchProcessor.cpp` 是批量处理逻辑（转换格式/缩放/旋转翻转/重命名，**没有删除任务**——删文件走主窗口的「删除到回收站」）：解码走工程内解码器（HEIC/AVIF/RAW 等也能参与转换），编码用 OpenCV；不依赖窗口，可用命令行 `--probe --batch <文件...> [--out-dir 目录] [--to 格式] [--quality N] [--rotate 90|180|270] [--flip-h|--flip-v] [--gray] [--invert] [--rename 前缀] [--overwrite]` 直接验证。
  缩放任务的规则集中在纯函数 `scaledSize()`（界面预览与批处理共用）：按百分比 / 按宽度 / 按高度（后两者保持宽高比）/ 自定义宽高（可拉伸变形）/ 限制长边（只缩不放），插值方式 `ScaleAlgorithm` 的默认 `Auto` 是**缩小用 INTER_AREA、放大用 Lanczos**（用 INTER_AREA 放大会退化成最近邻）；命令行对应 `--scale-percent N | --scale-width N | --scale-height N | --scale-size 宽x高 | --max-edge N` 与 `--scale-algo 0..5`，不给 `--to` 时**保持原格式**（缩放任务里的 `outputExtension` 为空即表示保持原格式）。
- `JarkViewer/include/BatchWindow.h` 是批量处理窗口（Ctrl+B 或右键菜单打开，处理当前目录的图片列表），ImGui 界面，处理在工作线程执行、界面轮询进度。缩放面板的「输出分辨率预览」取第一个选中文件（没有选中就用列表第一张）的原始尺寸，**取尺寸要整图解码，必须走常驻工作线程**（`ensurePreviewThread()` + `previewPath_/previewSource_` 缓存），在界面线程解码会把窗口卡住；批处理跑完（可能就地改写了源文件）要清 `previewPath_` 让预览重取。
- `JarkViewer/include/ImageAdjust.h` 与 `src/ImageAdjust.cpp` 存放打印/编辑与批量共用的图像调整（亮度对比度、黑白/黑白文档/黑白抖动、反相、BGRA→白底 BGR），原先内嵌在 Printer.h 中。
- `JarkViewer/include/ImageAnnotator.h` 与 `src/ImageAnnotator.cpp` 是标注模型与渲染（矩形/椭圆/箭头/直线/画笔/马赛克/文字），含撤销重做与裁剪，纯逻辑不依赖窗口；`--probe --annotate [--annotate-out 目录]` 用合成底图跑 28 项像素断言自检。`JarkViewer/include/EditorWindow.h` 是编辑与标注窗口（主窗口 Ctrl+E 或右键菜单打开）：画布**固定按适应窗口显示、不提供缩放/平移**（拖动绘制、裁剪框选，屏幕坐标与图像像素一一对应，**点击必须落在图像上才算**，画布空白处不产生标注），透明图会先合成棋盘格（`rebuildTexture()` 里对 CV_8UC4 做，格色取主题的 `BLACK_GRID`/`WHITE_GRID`）并画图像边框，否则看不出可编辑范围（**纹理重建要放在 `drawCanvas()` 里、算出 `fitScale_` 之后**：棋盘格宽度是按"约 16 屏幕像素"换算成图内像素的，用上一帧/初始的比例重建会让格子尺寸在首次绘制后跳一下）；马赛克拖框期间画裁剪那种白色框示意，但**不压暗**框外（整幅变暗体验差），少了压暗没有对比，所以白框底下垫一圈黑边；右侧工具栏提供工具/颜色/线宽/字号/填充/撤销重做/旋转翻转反相/应用裁剪/另存为/复制到剪贴板/覆盖原文件（文字工具是**点一下锚定**，不是拖动，所以画面上的光标与文字预览不能放在受 `drawingShape_` 约束的 `drawActiveShape()` 里，否则点完毫无反馈；预览用 `drawTextCaret()` + `AddText`，深浅双色细线保证压在任何底图上都看得见）（**png 保留透明通道**，jpg 透明区域铺白底，见 `jark::encodeAnnotatedImage`）；覆盖保存后置 `GlobalVar::isNeedReloadImageCache` 让主窗口重载。侧栏（右侧功能区）要保持**不出现滚动条**：控件标签一律写在控件**左边**（ImGui 的 `SliderInt` 标签画在控件后面，占满宽度的滑块会把标签顶出可视区，看起来就是"要横向滚动才看得到线宽/字号"，见 `drawSidebarSlider()`）；色块按可用宽度均分而不是写死尺寸（选中态是色块外圈一道强调色描边 `ImGuiCol_CheckMark`：外扩 2.5px、与色块间留背景空隙，hover 用半透明同色半圈；**不要**用 PushStyleVar 改 FrameBorderSize、再按"点击后"的 `colorIndex_` 收尾——点击会在循环中途改写它，Push/Pop 失配会触发 ImGui 错误恢复（`ConfigErrorRecoveryEnableTooltip` 默认开启），给整个侧栏子窗口画一帧纯红 (255,0,0) 边框，看起来像闪一下红框）；状态提示（已保存/已复制）用 `AddRectFilled`+`AddText` 画成浮层，**不要**用 `Text`/`Spacing` 占布局，否则内容一满就闪出滚动条。默认窗口高度 760 是按"侧栏内容刚好放下"定的，加内容时要么一起调高度要么压缩间距。
- 界面全部由 **Dear ImGui**（`JarkViewer/vendor/imgui`，Win32 + DX11 后端，随工程静态编译）绘制，
  宿主模块是 `JarkViewer/include/UiHost.h` 与 `src/UiHost.cpp`：创建上下文/后端、深浅两套主题
  （跟随 `GlobalVar::isCurrentUIDarkMode`）、按窗口 DPI 缩放字号与样式、系统字体（Segoe UI +
  微软雅黑 + 图标字体合并，1.92+ 动态字形加载）、以及把 `cv::Mat` 上传成 `ImTextureID` 的纹理池。
  界面里的符号图标统一写在 `UiHost.h` 的 `jark::ui::icon`（`Glyph<码位>`）：**优先用普通几何
  符号**（□ ○ 〰 ▣ ↗ ╱ ▦，外形接近即可，观感统一），只有确实没有合适几何符号的按钮
  （撤销/重做/打印/设置…）才用 **Segoe Fluent Icons / Segoe MDL2 Assets** 的私用区码位。
  **加新图标必须先确认字形存在**——用 `python tools/gen_icon_probe.py` 临时把 `DrawUi` 换成
  码位浏览器，截一张图看（`--revert` 还原）；码位猜错会画成豆腐块。
  业务侧只需实现 `D3D11App::DrawUi()` 提交界面，主循环在 `PresentFrame()` 里完成
  “画布贴后缓冲 → ImGui 一帧 → Present”。
- 设置窗口的帮助页把文案（`kStrHelpBody`，一行行「按键：说明」、行内用两个全角空格分组）切成
  **2 列**表格排版：列数不能再多，单元格宽度按窗口默认宽度算，3 列以上"窗口左右边缘：上一张 /
  下一张"这类长条目会被裁掉；关于页顶部的软件图标是 `IDB_PNG_ABOUT_ICON`
  （`file/aboutIcon.png`，从旧版设置页贴图 `settingRes.png` 里抠出来的透明底图标，纹理槽 4），
  深浅主题通用，不需要两套图。常规页最上面的六个勾选项按 **3 行 2 列**排布（ImGui 没有等宽列：
  第二列起点 = 第一列最长项的实际文本宽 + 勾选框宽 + 列间距，随语言自适应；增减勾选项时
  记得同步这个列宽推导）。
- 各窗口都是 ImGui 窗口（不再是独立窗口线程）：`SettingWindow.h`（常规/文件关联/帮助/关于）、
  `PrintWindow.h`（打印预览与打印）、`BatchWindow.h`（批量处理）、`EditorWindow.h`（编辑与标注）、
  `RenameWindow.h`（重命名当前图片：Ctrl+R 或右键菜单，只编辑文件名主体、扩展名保持原样；
  校验空名/非法字符/结尾点空格/保留设备名/过长/同名，通过后把新路径交回主窗口执行
  `applyRename()`——改盘 + 列表自然重排 + 缩略图失效 + 缓存作废重装）。窗口一律经
  `anyWindowVisible()/closeTopWindow()` 登记输入与 Esc 行为，新增窗口时别漏。
  右键菜单在「打开所在位置」之后依次是 重命名 / 复制到目标文件夹 / 移动到目标文件夹 /
  选择目标文件夹：目标记在 `SettingParameter::copyTargetDir`（单目标，首次使用弹
  `jarkUtils::SelectFolder`），目标不存在自动创建（含多层）、重名按资源管理器习惯让到
  `名 (2).ext` 绝不覆盖、跨盘移动退化为复制+删除；移动成功与删除同样从列表摘掉当前项。
  加菜单项注意加速键不要与既有项重复（`&C` 已被「复制图像数据」占用）。
  紧随其后是 用外部编辑器打开 / 选择外部编辑器：程序路径记在 `SettingParameter::externalEditor`，
  图片路径**整体加引号**交给 ShellExecute（带空格的路径是常态），失败按返回值提示。
  **vendor/imgui 有本地改动**：标题栏关闭按钮改成了系统标题栏按钮的样式（贴右上角、铺满标题栏高度、
  宽 = 1.5 倍高、悬停/按下铺系统红底 `#C42B1C` 与白色 ✕），改动在 `imgui.cpp` 的
  `RenderWindowTitleBarContents()`、`imgui_widgets.cpp` 的 `CloseButton()`（多一个 `size` 参数，
  默认 0 即原来的 FontSize 见方，Dock 页签栏仍走原行为）与 `imgui_internal.h` 的声明处，
  三处都有 `[JarkViewer 本地修改]` 注释；**升级 imgui 后要把这三处补回去**。
  窗口只允许从**右下角的抓手**改尺寸：`UiHost` 里设了 `io.ConfigWindowsResizeFromEdges = false`
  （ImGui 自带开关，关掉后上下左右四条边框与左下角抓手都不能拖，只剩右下角那一个），**别删这一行**，
  否则缩放后的窗口边框到处都能拖、很容易误触。
  每个窗口都用 `ImGui::SetNextWindowSizeConstraints()` 设了**最小尺寸**，别写小到把控件藏起来；
  尺寸（`SetNextWindowSize` 与 Min/Max 约束）都要先过 `jark::ui::fitWindowSizeToMainViewport()`
  收敛到主视口——多视口模式下主视口**装不下**的窗口会被 ImGui 分离成独立 OS 窗口：圆角外露黑底
  （绘制面是不透明的）、窗口跑到主窗口外、还会出现在任务栏。编辑器工具栏在窄窗口下自动换行；
  底部有固定内容（说明文字 + 按钮行）的页面要按实际高度给子区域留白（设置页的文件关联页就是这样
  算的），否则整页会多出一条窗口滚动条。
  主窗口的悬停按钮/动图播放条用 `ImGui::GetForegroundDrawList()` 贴 `file/mainRes.png`
  雪碧图（200x200，切片见 `main.cpp` 的 `OverlayIcons`，按 `uiScale()` 拉伸绘制，
  换图标只改这张图或切片表），EXIF 面板仍用前景列表排版文字；命中区域仍是原来的 `cursorPos` 逻辑。
  **叠加层必须经 `JarkViewerApp::uiPos()` 换算坐标**：多视口模式下主视口原点是“客户区左上角在
  屏幕上的位置”（`ImGui::GetMainViewport()->Pos`），直接按客户区坐标绘制会整体偏移，
  动图播放条会有一半被顶到客户区上边。切片 UV 要**内缩半个像素**，否则放大绘制时边缘会
  掺进相邻格（下面的播放条是亮的，会在打印/设置图标底部拉出一条亮线）。
- `D3D11App::WndProc` 开头必须 `if (UiHost::processMessage(...)) return S_OK;` —— **按返回值提前返回**。
  后端返回非 0 表示这条消息它已经处理完（`WM_IME_COMPOSITION` 带 `GCS_RESULTSTR`、`WM_SETCURSOR`），
  再落到 `DefWindowProc` 会把输入法上屏的结果生成两遍：中文变成双份（输入"安装"落进编辑框变成
  "安装安装"），英文走 `WM_CHAR` 不受影响——所以这个 bug 只在中文输入时出现。
- `D3D11App` 另有两个**先于 ImGui 捕获判定**的输入钩子（新增导航浮层时加入）：`OnMouseRelease`
  在 `uiWantsMouse` 之前调用，让"已在浮层上按下"的手势即使拖出客户区、或此时 ImGui 想接管，
  也能收到对应的抬起（返回 true 表示这条抬起已被消费）；`OnPointerCancel` 在
  `WM_KILLFOCUS`/`WM_CANCELMODE`/异常 `WM_CAPTURECHANGED` 时清理画布与浮层的拖动状态。
  坑：ImGui 后端在正常抬起时会先 `ReleaseCapture()`，同步重入的 `WM_CAPTURECHANGED` 不是取消，
  WndProc 用 `m_processingMouseRelease` 区分。鼠标坐标一律用有符号的 `GET_X_LPARAM/GET_Y_LPARAM`
  （拖动允许出客户区，负坐标被 `LOWORD/HIWORD` 折成大正数会飞掉）；`WM_MOUSEWHEEL` 的 lParam 是
  **屏幕坐标**，`OnMouseWheel` 里先 `ScreenToClient`。
- 主界面导航浮层 `JarkViewer/include|src/NavigationOverlay.{h,cpp}`：右下角**鸟瞰图**（显示旋转后的
  整图与当前可见区域框；框上拖动平移、点其它位置定位过去，几何用 `zoomCur/slideCur` 而不是动画
  目标值——小图从当前已加载的 `currentSourceImage()` 缩小，绝不读文件）+ 底部**悬停缩略图带**
  （鼠标进入**整块预览带区域**即展开——触发区就是展开后的面板矩形，**但鸟瞰面板区域豁免**：
  悬停面板不会展开预览带、也不会把面板顶上去；展开/换图时当前图片严格位于控件水平正中，
  两侧图片不足就留空且留空位置不可点；滚轮/左右按钮翻页，点击排队 `jumpToImage` 直接换图；
  带体与文件名浮签是半透明底——`ImGuiCol_PopupBg` × 0.82，悬停展开时透出后面的图像，
  鸟瞰面板保持不透明）。它**不加入
  `anyWindowVisible()`**，而是自己画在 `GetForegroundDrawList()` 上、自己做客户区命中
  （`OnMouseDown/Move/Wheel` 顶部优先处理，命中即拦截旧边缘按钮/画布拖动逻辑，二级窗口打开或
  图片切换时 `cancel()`）。动作走 `OperateQueue` 新增的 `jumpToImage`/`navigateImage` 在
  `DrawScene()` 消费：`navigateImage` 是**绝对位置**（队列里按“同 generation 覆盖”合并，不能走
  `slide` 的位移相加），两者都带 `Action::generation`（`navigationImageVersion`/`directoryVersion`），
  换图/旋转/改尺寸后旧坐标作废。手势归属由 `ownedButtons_` 管理：浮层上按下后，移动/抬起即使出了
  控件矩形也由浮层收尾（对应的抬起不能让给 `uiWantsMouse`）；失焦/失捕获时 `swallowedButtons_`
  保证残余抬起仍被吞掉。`switchToFile(index, direction)` 的 **direction==0 就是“直接切图”**
  （不准备滑动动画、不按上一张预取），点当前缩略图直接忽略；切图前必须 `stopMediaPlayback()`，
  否则同一次绘制还会取到旧视频帧。鸟瞰面板**右上角有 ✕ 收起按钮**（常态灰 ✕、悬停垫按钮底色；
  按下即返回 `Event::closeNavigator`，主窗口把它写进 `hideNavigator`——与设置页勾选同一字段，
  随退出统一写盘；`closeRect()` 供自检观察）：点击只收起鸟瞰，不影响预览带，也不进入面板拖动。
  鸟瞰开关是 `SettingParameter::hideNavigator`（**原 `reserve2`
  原位复用**，保持 4096 字节布局；旧设置默认 false=显示），别再加新字段。
- `JarkViewer/include|src/ThumbnailService.{h,cpp}` 是缩略图服务（`jark` 命名空间，单例）：
  取图顺序是**内存缓存 → `JarkViewer.thumbnail` 持久缓存 → `IThumbnailCache`(`WTS_INCACHEONLY`)
  → Shell 正常提取（`WTS_EXTRACT`）→ 工程内解码器兜底**——Shell 提取默认走系统 surrogate，
  **禁止 `WTS_EXTRACTINPROC`、禁止直接加载 provider DLL**；Shell 拿不到缩略图时（无处理器、
  未安装 JarkThumbnailProvider.dll 等）由专属解码线程用**私有 `ImageDatabase` 实例**调
  `myLoader` 生成（缩到长边 ≤256 的 8UC4，深度转换复用 `ImageDatabase::convertMatToCV_8U`，
  与查看器显示同一套语义；成功后照常持久化，失败才落占位、仅内存记忆），因此预览带不依赖
  任何 Shell 处理器注册，也不碰应用的图像缓存与窗口状态。缓存文件固定在**实际设置文件
  （`GlobalVar::settingPath`）同目录**、固定名 `JarkViewer.thumbnail`（不是 `JarkViewer.db`
  的扩展名）：64 字节文件头 + **1000 个定长索引槽** + 追加数据区（规范化路径 + ≤256px PNG，
  显式序列化、CRC 校验、损坏只丢单条）；LRU 按“实际展示”更新（预取不刷热），跨进程用命名
  互斥锁提交、`generation/epoch` 保证在途旧结果不覆盖清理后的缓存；原图只查文件属性
  （大小/修改时间），覆盖保存等已知变化要 `invalidate()`。三个常驻 worker（缓存 I/O、Shell COM、
  本地解码兜底，后两者各自 `CoInitializeEx`，解码线程 `THREAD_PRIORITY_BELOW_NORMAL` 不抢前台
  解码）：慢提取/慢解码不阻塞缓存查询/清理，退出有界等待，同步调用**不能**强制中断、
  线程只持有自己的状态。
  GPU 侧：鸟瞰用纹理槽 5、缩略图带用 16..79，`UiHost::releaseTexture(slot)` 是新增的单槽释放
  （清理缩略图**不能**调全局 `releaseTextures()`，会连其它窗口的纹理一起失效）；后台只产 CPU 位图，
  上传/释放在界面线程。设置页常规页有「显示鸟瞰图」与「清理缩略图缓存」
  （`ThumbnailService::stats()/clear()`，`stats().clearVersion` 变化时浮层释放自己的纹理槽）。
- 输入法（IME）不能靠 `ImmDisableIME()` 禁用：它是**线程级且没有反向接口**，一旦调用，
  界面里的所有 ImGui 文本框就永远打不了中文。改为 `UiHost` 在窗口上挂/摘 IME 上下文
  （`ImmAssociateContext`）：默认把上下文摘下来（中文输入法会吃掉 p/c 这类单键快捷键），
  `io.WantTextInput` 为真（有文本框在编辑）时才挂回去；组字与候选窗口的位置由 ImGui 的
  `Platform_SetImeDataFn` 默认实现按文本光标摆放，不用自己算。
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
- `JarkViewer/include/Localization.h` 与 `src/Localization.cpp` 管界面语言（简体中文/繁體中文/English/日本語/한국어/Русский）：`UIStringTable[stringID][语言]` 与 `UIStringTableWide[stringID][语言]` 两张表（前者供画布文字、后者供 Win32 API；同 ID 文案不同是历史遗留，新增文案请追加到表尾）。`getUIString()` 按当前语言取用并在缺失时回退到英文、简体中文；`getUIStringW()` 由 UTF-8 转换而来。帮助/关于页与主页/解码失败画面都改为按语言的文字排版（前两者是 ImGui，后者由 `InfoScreen` 画到画布），不再有按语言分套的资源图；`prefersChineseResources()` 现在只决定 **EXIF 标签文案**用中文还是英文（简繁→中文，其余→英文）。命令行 `--lang 0..5` 可临时指定语言。新增语言时：`Language` 枚举、两张表每一行的新列（顺序对齐、数量断言）、`languageFromSystem()`、`languageDisplayName()`、设置页语言单选项，缺一不可。
- 图像内文字渲染（标注文字等）在 `JarkViewer/include/TextRenderer.h` 与 `src/TextRenderer.cpp`：
  用 stb_truetype 按**真实字形度量**（进退宽度/字距/bearing）绘制 UTF-8 文本，支持多行与按宽度折行，
  字形位图按 (字号, 码位) 缓存。字体全部取系统字体（微软雅黑/等线/黑体/宋体…），工程不再内嵌 ttf。
  界面文字由 ImGui 负责，不要再往 TextRenderer 里加界面相关职责。字体文件数据按路径**进程内共享**
  （`sharedFontData()`）：多个 TextRenderer 实例（EXIF 面板、InfoScreen）不会各读一份几十 MB 的 ttf，
  各实例只保留自己的 stbtt_fontinfo 与字形缓存；它仍是**界面线程专用**。
- `JarkViewer/include/MediaDecoder.h` / `MediaPlayer.h` / `AudioOutput.h`（对应 `src/*.cpp`）组成媒体播放链路：`MediaDecoder` 在内存数据上做解复用+解码，按出现顺序产出视频帧或音频批（音频统一重采样为 48kHz 立体声 16 位）；`AudioOutput` 用 XAudio2 输出并提供已播放样本数作为主时钟；`MediaPlayer` 以音频时钟驱动视频帧、一次播完。
  **视频与音频必须各占一个解码器实例 + 一个线程**（`MediaPlayer` 里因此有两个 `MediaDecoder`，`MediaDecoder::StreamFilter` 让每个实例只解自己那一路，另一路只解复用不解码）。起因是一个会自锁的恶性循环：视频队列只有 6 帧（一帧几 MB，必须有上限）要靠"播放端取帧"背压，音频却必须永远跑在播放位置之前。两条路抢同一个线程时，视频队列一满，整个循环就卡在等取帧上，音频提交被迫一起停下——视频队满 → 音频提交掉到 1 倍速以下 → 声卡饿死、播放时钟停住 → 播放端按时钟判"还没到点"不再取帧 → 队列永远满，卡顿就此锁死（实测 3 秒的视频播了 9 秒、中间冻 1.15 秒）。拆开后视频线程只受队列上限约束、音频线程只受声卡队列约束，互不牵连。**别再把它们合回一个线程**，那怕只是为了"省一个解码器"。
  **音频提交有两个硬约束**：① XAudio2 对单个 source voice 最多排 64 个缓冲区（`XAUDIO2_MAX_QUEUED_BUFFERS`），超了 `SubmitSourceBuffer` 直接失败——按样本数限流拦不住（21ms 的批 × 64 就到顶），必须按**缓冲区个数**限流（`AudioOutput::kMaxQueuedBuffers`），并且**提交失败要重试、不能丢**（丢一批就是声音里一个 20ms 空洞 = "一卡一卡"）；② 音轨比视频短时（实况照片常见）要**补静音到媒体总时长**，否则音频队列一空时钟就停，视频最后几帧永远等不到"到点"、`hasFinished()` 永远为假（画面停在末尾、实况照片回不到静态图、播放状态卡死）。
  **视频尺寸分两种，别用错**：`MediaInfo::width/height` 是**编码尺寸**（容器/码流里的原始尺寸，未旋转），`displayWidth()/displayHeight()` 是**帧实际交出去时的尺寸**（按 `rotationDegrees` 换过轴）。解码器会按 display matrix 把帧旋转（手机竖拍视频的编码尺寸是横的），所以**给播放端的尺寸必须用 display\***——`MediaPlayer::getVideoSize()` 就是栽在这里：它原先返回编码尺寸，实况照片播放时主窗口拿它当名义尺寸（`applyViewForSize`），竖帧被塞进横框里拉伸。另外帧的长边超过 `kMaxVideoEdge` 时会**降采样**（4K 实况视频只出 1920×1080 的帧），所以"帧尺寸 == 名义尺寸"**不是**不变式，**宽高比一致**才是；`--probe --full` 对每个视频/实况照片都会打印 `显示 WxH (编码 WxH) rot=R` 与 `首帧 WxH … 宽高比一致`，不一致时会显式报 `!! 宽高比不一致`。实况照片（livp / MotionPhoto）与视频文件都走这条路：静态图/首帧作 `ImageAsset::primaryFrame`，视频字节放在 `ImageAsset::videoSource`，由主窗口自动播放一次后回到静态图（不再预解码成帧序列，避免上百 MB 内存）。空格键在实况照片上是播放开关：播放中切回静态图（并把 `playedAsset` 标记为已播放，防自动续播）、静态图时从头播放（主动操作，出声）。
- `JarkViewer/include/CanvasRenderer.h` 与 `JarkViewer/src/CanvasRenderer.cpp` 是从 `JarkViewerApp` 抽出的画布绘制模块：按 `ViewState`（名义尺寸 / 定点缩放 / 平移 / 旋转）把图像绘制到 BGRA 画布，含透明区域棋盘格与图像边框。`JarkViewerApp::drawCanvas()` 只是把 `curPar` 转成 `ViewState` 后调用它。`imageGeometry()` 是主画布与鸟瞰**共用**的纯几何（旋转后名义尺寸、显示矩形、归一化可见区域——`slide + round((canvas-rendered)/2)` 的定位公式只有这一处），`navigationSlide()` 把归一化图像点换算成目标 slide（某轴完整可见时保持居中、不改动原有“允许留白”的拖图夹取）；`--probe --navigation-test` 用合成断言覆盖旋转/平移/端点夹取与浮层输入归属。`ViewState::border=false` 表示不画图像边框（主页/解码失败是界面画面，不是照片）。
- 主页与解码失败画面由 `JarkViewer/include|src/InfoScreen.{h,cpp}` **按当前语言、主题、DPI 实时绘制**（旧的 `home.png`/`tips.png`、`getHomeMat()/getErrorTipsMat()` 和 ColorManager 里"识别内置提示图"的像素启发式都已删除）：`ImageAsset::placeholder`（`PlaceholderKind`：Home/UnsupportedFormat/DecodeFailed/FileMissing）与 `placeholderDetail` 由 `myLoader` 在失败时填写——文件头与扩展名都识别不出→UnsupportedFormat，能识别但解码失败→DecodeFailed，打不开/不存在→FileMissing（各解码器的失败分支保持"空帧 + format=None"交给 myLoader 继续路由，不再往 primaryFrame 塞提示图）。**失败时 primaryFrame 保持为空**：`--probe` 据此判定失败并输出 `placeholder=<原因>`，批量处理也会如实报"无法解码"（以前提示图会被当成解码成功的图参与批处理）。界面层用 `JarkViewerApp::updatePlaceholderImage()` 按 `jark::infoScreenStamp(尺寸, DPI缩放, 语言, 主题, 按钮交互)` 决定重绘（结果写进 `placeholderStamp`；返回 0/1/2 = 无变化/仅内容变/尺寸也变，只有尺寸变才 `curPar.Init()`，悬停反馈不会重置缩放）：`initOpenFile`/`switchToFile`/重载/删除都在 `curPar.Init()` 前调用一次，`DrawScene()` 开头再兜一次，窗口缩放/换主题/换语言自动重绘。支持格式清单直接读 `ImageDatabase::supportExt/supportRaw/videoExt`，永远与实际解码能力一致。主页是图标 + 名称/版本 + **「打开图片」按钮**（`homeButtonRect` 与绘制共用同一布局；主窗口把客户区坐标按缩放/平移/旋转逆变换回画布像素做命中，悬停/按下有底色反馈，点击等同 Ctrl+O）。文案是窄表 156~165。
- `JarkViewer/include/VectorImage.h` 与 `JarkViewer/src/VectorImage.cpp` 负责矢量图（SVG）的按需光栅化：`ImageAsset::vectorSource` 持有 lunasvg 文档与文档尺寸（intrinsic），位图分辨率随缩放变化（`vectorTargetEdge()` + `refreshVectorRaster()`，滞后阈值 1.25 避免缩放动画中反复渲染，长边上限 4096）。主窗口在画面稳定后（`DrawScene` 空闲分支）调用 `refreshVectorRasterIfNeeded()` 升级分辨率。lunasvg 的位图是 **ARGB32 预乘**（内存 B,G,R,A），`renderVectorImage()` 一律**手工反预乘**成直通 alpha（直接调 `convertToRGBA()` 会换成 R,G,B,A 字节序，画布按 B 读第一个字节会红蓝互换）。`<text>` 要先 `jark::ensureVectorFonts()` 注册系统字体（lunasvg 没有内置字体，不注册就什么都画不出来）。
  `JarkViewer/include/SVGPreprocessor.h`（还在 `JarkThumbnailProvider` 里有一份同源的）在解码前做三件 lunasvg 做不到的事，顺序不能换：① `<switch>` 选择——`foreignObject` 与 `requiredFeatures` 里的 Extensibility 一律判为**不支持**，否则 draw.io 导出的画布会选中画不出来的 foreignObject、同时把后面等价的 `<text>` 兜底删掉（表现是方框连线都在、文字一个字都没有，见 issue #33/#51）；② 收集 `--x: value`（`:root` 规则表与内联 style 都覆盖）；③ 把 `var(--x[, fallback])` / `light-dark(a, b)` 折叠成字面量——lunasvg 不认识 CSS Color 5 的这些函数，颜色值判为无效时会把**整个图元丢掉不画**（draw.io 图整幅空白）。`light-dark` 取**亮色分支**：位图会进 LRU 缓存，随主题变化的颜色没有意义，亮色分支在深浅主题下都保持可读。
  **放大超过 4096 上限后按可视区域出高清块**（`VECTOR_DETAIL_MAX_EDGE` / `VectorImage::detailFrame`）：全幅位图（鸟瞰、缩略图、打印/批处理仍用它）已经榨不出细节，这时用 `renderVectorImageRegion()` 只光栅化当前可视区域+25% 余量（`VectorImage.cpp` 把"文档→旋转后名义空间"的仿射系数写进 `Document::render(bitmap, matrix)` 的矩阵里一次完成，所以位图直接就是旋转后的名义空间，取样不必再套旋转）。`CanvasRenderer` 侧只多了 `ViewState::sourceLeft/Top/Width/Height`（归一化区域）与 `sourcePreRotated`：采样原点按区域左上角平移、采样密度按"位图像素 / 该区域的名义像素"算，元素级循环一行没改。**复用判断只看可视区域（不含余量）**，否则余量会被逐帧的微小移动吃掉、每帧重光栅化；可视区域跑出旧块外的那一帧退回全幅位图（糊但不缺块），稳定后自动重出。切图时释放上一张的高清块（`lastDetailVector_`），避免 LRU 里每张 SVG 各攒几十 MB。
- `JarkViewer/include/jarkUtils.h` 与 `JarkViewer/src/jarkUtils.cpp` 集中放置 Win32/OpenCV 工具、主题/设置全局状态、剪贴板、全屏、资源读取、文件操作和日志。
- `JarkViewer/src/TextRenderer.cpp`、`stringRes.cpp`、`exifParse.cpp`、`videoDecoder.cpp`、`blpDecoder.cpp` 分别支撑图像文字渲染、多语言字符串、元数据解析、视频帧解码和 BLP 解码。EXIF 数值的**摄影写法**（曝光时间 `1/60 s`、光圈 `f/2.8`、焦距 `89.9 mm`、曝光补偿 `+1/3 EV`、ISO、方向翻词）由 `exifParse.cpp` 的 `formatExifValue()` 负责：按**标签号**判定（不依赖落在哪个 IFD）、只翻认得的标签，认不得的返回空串退回通用显示（厂商私有标签照规范翻会翻出错的词）；曝光时间的分母按值重算，不能照搬 EXIF 里存的分母（尼康把 1/60 存成 10/600）。
  **Exif UserComment 的编码不能靠猜固定端序**：AI 生图工具（A1111/ComfyUI/Fooocus…）把提示词、参数甚至整份 ComfyUI 工作流 JSON 塞进这个标签，正文是 8 字节字符集码（`UNICODE\0` / `ASCII\0\0\0` / `JIS\0\0\0\0\0` / 8 个 0 = 未指定）之后跟正文，**UNICODE 的 UTF-16 不强制带 BOM**，大小端都有；而且 Exiv2 回吐的字节序不一定等于文件 TIFF 头（实测 `II` 文件也可能拿到小端）。早先按 `bigEndian` 硬解，ASCII 提示词会被整段解成"低字节恒为 0"的汉字（`hyperdetailed` → `栀礀瀀攀爀`）。现在 `utf16ToUtf8()` 先认 BOM，没有 BOM 就两种端序都解、用 `textPlausibility()` 打分（可打印 ASCII 加分，控制字符与 `(c & 0xFF) == 0` 的高位字符扣分）挑更像话的那个，难分伯仲时才用调用者给的文件字节序；无前缀正文若中段出现 0x00（按单字节读明显坏掉）再按 UTF-16 补解，首尾的 0 一律去掉。文件字节序经 `getExifDetail` → `exifDataToString(path, exifData, image->byteOrder())` 传进来。`--probe --exif-test` 用现造的最小 JPEG（SOI+APP1+EOI）覆盖各种编码组合。

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
- 查"启动/打开一张图为什么慢"用 `JARKVIEWER_STARTUP_TRACE=<文件路径>` 环境变量（`jarkUtils::startupTraceMark()`）：记录 begin / 窗口就绪并派发解码 / 设备与 UI 就绪 / 首帧就绪 / 首帧绘制各阶段相对起点的毫秒数；没设环境变量时零开销。新增关键阶段时在对应位置补一个 `startupTraceMark()`。
- UI 文本来自 `stringRes`：**两张表同名不同文案**——`UIStringTable[stringID][语言]` 供 ImGui/画布，`UIStringTableWide[stringID][语言]` 供 Win32（窗口标题、菜单、消息框），写 `getUIStringW(id)` 时一定要核对**宽表**里那个 ID 是什么（历史上出过把消息框正文写成"关于 (&A)"菜单项的事故）。`getUIStringW` 返回 `UIStringWide`（自带缓冲区、可隐式转 `const wchar_t*`）：旧实现共用同一个 thread_local 缓冲，`MessageBoxW(h, getUIStringW(a), getUIStringW(b))` 后一次转换会冲掉前一次的指针内容（标题乱码）。需要指针活过当前语句时（如 `BROWSEINFO.lpszTitle`）用 `.str()` 存一份 `std::wstring`；丢给 `std::format`/`wstring_view` 参数时要显式 `.c_str()`。**新增文案追加到对应表尾**，并在使用处写成具名常量（各窗口文件里已有 `kStr*` 常量块）。改动后再跑一次 `--probe --lang-test`，它会打印窄表与宽表各若干条文案用于确认 ID 没有错位。
- README 记录的 OpenCV 预编译库带有源码改动：移除 `imgcodecs` 分辨率限制，并将 HighGUI Win32 窗口光标从 `IDC_CROSS` 改为 `IDC_ARROW`；替换或重建 OpenCV 时要保留这些行为。
- 不要提交 `.vcxproj.user`、`.vs/` 或机器相关的本地库路径。
- **渐进加载**：当前图的解码不阻塞主循环——`requestCurrentImage()` 用 `LRU::tryGetPtr` 非阻塞查缓存，未命中就挂起，主循环每帧 `updatePendingLoad()` 轮询，就绪后 `adoptCurrentImage()` 收尾。等待期间：启动/主页场景用主页画面垫底（`allowPreviewSwap_`），缩略图（ThumbnailService，持久缓存命中时毫秒级）先到就先顶上当模糊预览；切图场景保留旧图停留（相邻图通常已被预取，点翻页零等待）。客户区左上角画「加载中 X.Xs」浮标（逐帧跳秒——画面稳定分支要按 `pendingLoad_` 持续出帧），超过 60 秒退回一次阻塞等待兜底。**新增"载入当前图"路径时一律用 `requestCurrentImage()`**，不要再直接调 `getSafePtr`（会退回"翻页等解码"）。
- **动图播放计时**（`DrawScene` 的动画块）：帧推进的剩余时间 `delayRemain` 按**微秒**累计并跨帧保留（欠帧时 `while (delayRemain <= 0)` 循环推进、推进后把超出的部分留给下一帧），不要退回"整毫秒截断"或"每帧重置余量"——主循环每帧 10~16ms，零头被截掉/丢弃会逐帧累积成慢放（100ms 的帧实测会播成 103~109ms）；起播、暂停恢复、切图后要经 `animClockArmed` 重新对齐计时起点，否则加载或暂停的耗时会被算进第一帧（首帧长时间不动）。
- **实况照片的尾部视频**不一定紧贴文件末尾（部分厂商导出在视频数据后还有尾块）：按「文件大小 - Item:Length/MicroVideoOffset」推出起点后，要在期望起点附近按 MP4 首个 `ftyp` 盒校正真实起点（`locateMotionPhotoVideoStart()`，找不到则保持原行为），否则起点偏移几十字节会让 MP4 采样偏移整体错位，表现为"能识别出视频轨但解码全是乱码"。
- 主窗口渲染路径以 OpenCV `cv::Mat` 作为 CPU 画布，再交给 Direct3D 显示；避免在高频绘制路径中引入阻塞 I/O 或昂贵同步操作。
- Debug 构建会分配控制台并启用 `JARK_LOG`；Release 下日志宏为空。
