# 架构导航

[文档导航](README.md)

JarkViewer 是 Windows 10/11 x64 原生应用，使用 C++23、Win32、Direct3D 11、OpenCV 与 Dear ImGui。主程序静态链接；可选的 `JarkThumbnailProvider.dll` 提供资源管理器缩略图。

## 入口与主路径

`JarkViewer/src/main.cpp` 的 `wWinMain` 初始化 COM / Exiv2，解析命令行；`--probe` 进入 `DecodeProbe`，正常启动按 `jark::isPlayerFile()` 二选一构造查看器或播放器窗口。两种窗口在同一个进程里，通过 `takeHandoffPath()` 交接文件、销毁当前窗口并重新构造另一种窗口。

| 场景 | 主路径 | 详细说明 |
| --- | --- | --- |
| 看图 | 目录列表 → 异步 `ImageAssetCache` → `ImageDatabase` → `ImageAsset` → CPU 画布 → D3D11 | [图像链路](image-pipeline.md) |
| 独立音视频 | `MappedFileReader` → `VideoPlayback` → `MediaPlayer` → 两路解码 / XAudio2 → 适应窗口绘制 | [媒体播放](media-playback.md) |
| 实况照片 | 图片解码得到静态图和 `VideoSource` → 查看器播放一次 → 回到静态图 | [图像链路](image-pipeline.md#实况照片) |
| ImGui 工具窗口 | `DrawUi()` → `UiHost` → Win32/DX11 后端，与主窗口共用界面线程 | [界面与交互](ui.md) |
| 无界面验证 | `--probe` → `DecodeProbe.cpp` 中的对应自检 | [验证指南](testing.md) |

查看器输入转换为 `ActionENUM` 放入 `OperateQueue`，`DrawScene()` 消费操作并更新视图。`CanvasRenderer` 绘制 CPU 端 `cv::Mat`，`D3D11App::PresentCanvas()` 上传纹理，`PresentFrame()` 合成 ImGui 后呈现。

## 模块定位

自有实现主要在 `JarkViewer/include` 与 `JarkViewer/src`，VS 筛选器按模块组织。定位代码先使用现有 CodeGraph 索引，再查下面的入口。

| 职责 | 入口文件或符号 |
| --- | --- |
| 看图状态、目录列表、切图、操作队列 | `main.cpp` / `JarkViewerApp` |
| Win32 消息、D3D 设备、窗口几何、设置读写 | `D3D11App.{h,cpp}` |
| ImGui、DPI、主题、IME、纹理池 | `UiHost.{h,cpp}` |
| 格式嗅探、解码、EXIF、缓存 | `FormatSniffer`、`ImageDatabase`、`ImageAssetCache.h` |
| 画布、重采样、SVG、ICC | `CanvasRenderer`、`ImageResampler`、`VectorImage`、`SVGPreprocessor`、`ColorManager` |
| 鸟瞰、预览带、缩略图缓存 | `NavigationOverlay`、`ThumbnailService` |
| 媒体链路 | `VideoPlayerApp`、`VideoPlayback`、`MediaPlayer`、`MediaDecoder`、`AudioOutput`、`AudioSpectrumAnalyzer` |
| 打印、编辑、批处理 | `PrintWindow`、`EditorWindow`、`BatchWindow`、`BatchProcessor`、`ImageAdjust`、`ImageAnnotator` |
| 设置、重命名、关联 | `SettingWindow`、`RenameWindow`、`FileAssociationManager`、`ThumbnailRegistrar` |
| 多语言、占位画面、图内文字、元数据 | `Localization`、`stringRes`、`InfoScreen`、`TextRenderer`、`exifParse`、`AiPrompt` |
| Win32/文件/剪贴板/日志工具、公共状态 | `jarkUtils.{h,cpp}` |
| Shell 缩略图处理器 | `JarkThumbnailProvider` 工程 |

## 生命周期边界

- 首图在 `OnWindowCreated()` 中提前派发，让后台解码与 D3D 初始化重叠；`initOpenFile()` 用 `startupFilePrepared_` 跳过重复扫描。重扫会 `imgDB.clear()`，使在途加载失效。
- UI 线程负责窗口状态和 GPU 上传/释放，后台工作只交付数据；不要在绘制、命中测试或尺寸预览里同步解码大图。
- `ImageDatabase` 析构须在派生类成员销毁前停止缓存预读线程。停止标记在 `preload_mutex` 内设置，再 notify / join，避免检查 predicate 与入睡之间漏通知。
- 批处理和尺寸预览使用可回收的 `jthread`，查看器析构先 `BatchWindow::shutdown()`。TLS 中的 `ImageDatabase(false)` 不启动预读线程，避免在 Windows loader lock 下析构时 join 子线程而死锁。
- 同进程换窗口前必须真正销毁旧 HWND：单独投递 `WM_DESTROY` 不会销毁窗口。`OnDestroy()` 防重入，并在窗口仍存活时先 `UiHost::shutdown()`，再 `DestroyWindow()`；否则后续消息可能调用已析构对象。
- `UiHost::init()` 接管新窗口前完整 shutdown / 重建；不能靠 HWND 或设备指针相等认定可复用，系统可能复用地址和句柄。

## 持久设置

`SettingParameter` 按固定 4096 字节整体持久化。保留成员偏移和旧字段占位，新增设置复用明确预留空间并验证旧设置兼容；不要随意重排、删除或扩张布局。

`loadSettings()` 整体赋值，命令行/环境变量临时覆盖要在读盘后应用。`--lang` 经 `pendingLanguageOverride` 传递，保存时恢复原语言，临时覆盖不落盘。

查看器与播放器共用 `rect` / `showCmd` / `monitorDevice`，窗口几何互相继承。全屏退出保存进入全屏前的 `WINDOWPLACEMENT`，包含最大化状态；不能保存无边框满屏矩形，也不能简单跳过保存而丢掉进全屏前的调整。
