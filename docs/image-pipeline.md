# 图像链路

[文档导航](README.md) · [架构导航](architecture.md) · [验证指南](testing.md)

## 解码与失败语义

入口为 `ImageDatabase::loader()` → `myLoader()` → `FormatSniffer` / `decodeByFormat()`。通常按文件头识别真实格式，失败后用扩展名路由，再走 OpenCV / WIC 通用兜底。RAW、视频、LIVP、LEP、TGA 等扩展名包含额外语义，由 `isExtensionAuthoritative()` 决定优先级，不能简单改成所有格式都只信魔数。

- PSD 保持 psd_sdk 优先、stb 兜底；反过来会让部分 16 位 RLE 图静默全透明。
- EXIF 后处理统一由 `applyExifInfo()` 按 `ExifPolicy` 执行，避免在各格式分支重复旋转/补元数据。
- 解码入口兜住 C++ 异常并返回失败结果，不能让异常越过后台线程入口终止进程。短文件名和无扩展名也能按内容解码。
- 解码失败保持 `primaryFrame` 为空，以 `PlaceholderKind` 区分 UnsupportedFormat / DecodeFailed / FileMissing；不能把提示图当作成功解码的像素交给批处理。
- `InfoScreen` 在界面层按语言、主题、DPI 和尺寸生成主页/失败画面；悬停仅改变内容时不重置视图。主页按钮命中使用与绘制一致的布局和逆变换。

新增格式需同时核对 `supportExt` / `supportRaw`、`FormatSniffer` 的嗅探与扩展名映射、解码分支、EXIF/方向、设置页关联列表和两份 README。查看器与缩略图工程有同源的嗅探/SVG 预处理实现，改动时检查两边，不能只修一份。

## 渐进加载与动图

当前图经 `requestCurrentImage()` 非阻塞查缓存，未命中时由主循环 `updatePendingLoad()` 轮询，完成后 `adoptCurrentImage()` 接管。新加“载入当前图”路径也走该入口，不直接用阻塞的 `getSafePtr()`。

启动/主页可先显示缩略预览，切图等待时保留上一张；`pendingLoad_` 期间持续出帧以更新加载提示，超时才走现有等待兜底。判断缓存命中必须在 move `loadedAsset` 之前记录，否则会错误禁掉切图动画。

动图剩余帧时间用微秒累计，欠帧时循环推进并保留余量；起播、暂停恢复、切图经 `animClockArmed` 重置计时起点。整毫秒截断或每帧清余量会累积成慢放，加载/暂停耗时也不能算进首帧。

## 图像缓存

`ImageAssetCache.h` 是 `ImageDatabase` 的基类，键为路径、值为 `ImageAsset`，带后台预读。预算约束的是缓存估算值，不是进程工作集的绝对硬上限：

| 规则 | 原因 |
| --- | --- |
| 条数上限默认 10，字节预算为物理内存的 50%，最低 512 MiB | 小图控制条目，大图控制像素内存；主页占位也占一条 |
| 最少保留 2 条，即使暂时超预算 | 保留常用的当前/上一张，避免来回翻页反复解码 |
| LRU 淘汰跳过外部仍持有的 `shared_ptr` 条目 | 踢掉也不能释放内存，只会损失命中 |
| 一圈都不能回收时停止 | 不空转，不强制破坏外部持有者 |
| 当前图大于预算一半时跳过相邻预取 | 避免大图预取导致内存再翻倍 |

`ImageAsset::memoryBytes()` 按像素 `data` 指针去重，避免 `primaryFrame` 与 `frames[0]` 的浅拷贝重复计数；lunasvg 文档内存不在该估算中。`eraseLocked()` 统一记录条数/字节两条路径的淘汰，`stats()` 与 EXIF 面板用于诊断。

停止预读与派生类析构顺序见 [生命周期边界](architecture.md#生命周期边界)。回归使用 `--cache-test` 与实机 `test_cache_budget.ps1`，不能仅看缓存条数断言进程内存已收敛。

## 画布几何与平滑重采样

`CanvasRenderer` 根据 `ViewState` 绘制 BGRA CPU 画布；`imageGeometry()` 是主画布与鸟瞰共享的旋转/定位/可见区域计算，`navigationSlide()` 负责归一化图像点到平移量的换算。

两类尺寸必须分开：几何使用名义尺寸 `curPar.width/height`，采样使用实际位图分辨率与 `srcScaleX/Y`。SVG 位图会随倍率改变，不能用 `srcImg.cols/rows` 替代几何尺寸，也不能仅用 `zoomInvert` 直接算位图坐标。

静止的普通位图由 `ImageResampler` 将可视区域加 25% 余量生成平滑块：放大用 Lanczos4，缩小用 `INTER_AREA`；动画、实况播放和 SVG 不走此路径。

- 块走 `sourceLeft/Top/Width/Height` 与 `sourcePreRotated` 通道，按屏幕对齐以 1:1 采样。旋转 1 是逆时针 90°，须与原采样路径一致。
- 缓存键包含 `sourceToken`、源图信息与完整视图；只用位图裸指针会因释放后地址复用而误用上一张的块。
- 放大前先裁出所需源区域，边缘保留采样余量，再 `warpAffine`。OpenCV 非最近邻仿射内部的 remap 对尺寸有 `< SHRT_MAX` 限制，整幅超宽图直接输入会抛异常。
- 捕获 `cv::Exception`，失败退回普通采样，不因增强画质功能退出程序。`--resample-test` 覆盖 33000×40 的超宽图与逐像素对齐。
- `disableZoomSmoothing` 是兼容旧设置的取反字段，0 表示开启，不能按字段名直觉改变默认值。

## SVG

`VectorImage` 持有 lunasvg 文档，按缩放动态光栅化；全幅长边上限 4096，稳定后刷新，超过能力时按可视区域生成高清块。全幅仍用于鸟瞰、缩略图、打印与批处理。

- lunasvg 输出为预乘 ARGB32（内存 B,G,R,A），手动反预乘成直通 alpha；直接 `convertToRGBA()` 会交换红蓝字节。`<text>` 渲染前注册系统字体。
- 预处理顺序保持：处理 `<switch>`（不支持 foreignObject / Extensibility）→ 收集 CSS 变量 → 折叠 `var()` / `light-dark()`。选亮色分支使缓存结果不随 UI 主题变化，保留 draw.io 的 `<text>` 兜底。
- 高清块直接位于旋转后的名义空间，采样端不再重复旋转。复用只检查当前可视区域是否被旧块覆盖，不把新增余量也纳入判断；越界那一帧先退回全幅，停稳后刷新，切图释放上一张的高清块。
- 当前 lunasvg 支持 mask、pattern、marker、clipPath、嵌套 SVG、渐变和普通 text；忽略 filter 效果但保留图元，textPath 不渲染。`--svg-test` 记录这些能力边界；若升级补齐支持，应同步调整断言和文档，而不是保留“必须不支持”的规则。

## 色彩管理

`ColorManager` 使用 lcms2，源 ICC 从解码分支或 Exiv2 补读。各路径目标不同：

| 路径 | 目标 |
| --- | --- |
| 查看器 / 编辑 / 打印，经 `ImageDatabase::loader()` | 当前显示器 ICC，缺失时 sRGB |
| 批量输出，经 `BatchProcessor::loadImage()` | 固定 sRGB，结果不取决于当前显示器 |
| `ThumbnailService` 本地兜底 | 目前不做 ICC，后台不读显示器状态 |

批量输出的 OpenCV 编码链路不写入 ICC，若丢掉标签却保留 P3/AdobeRGB 数值会改变颜色，故必须先转 sRGB。缩略图若新增 ICC，需连持久缓存键/失效策略一起设计，不能只在解码后加一次转换。

恒等变换直接跳过；就地转换保留 alpha（`cmsFLAGS_COPY_ALPHA`），大图并行与串行须逐字节一致。Display P3 的超饱和颜色转 sRGB 可能剪裁为同一个红色；用 `--color-test` 已知值判断，不能因图案消失就绕过色彩管理。

## 实况照片

`loadMotionPhoto` / `loadLivp` 将静态图放在 `primaryFrame`，完整媒体容器放在 `videoSource`，播放时由 `MediaPlayer` 解码一次，不预解成所有视频帧。

视频提取按以下途径查找，不能缺少无可靠 XMP 长度时的兜底：

1. 按 `Item:Length` / `MicroVideoOffset` 反推尾部起点，在附近用 MP4 `ftyp` 盒校正；厂商可能另带尾块。
2. 同目录同名的 `.mov` / `.mp4` 侧车文件。
3. 带明确实况标记的文件中查尾部 MP4：品牌排除 HEIF/AVIF，盒链必须见到 `moov` 并延伸至文件尾（容许小厂商尾块）。普通 HEIC 本身也有 `ftyp`，不能据此误切视频。

导出视频使用 `VideoSource::data` 的完整容器字节和原扩展名，原子写出，不转码。实况播放一次后回静态；空格在播放中切回静态并标记已播放，在静态时主动从头播放。显示尺寸/编码尺寸与降采样语义见 [媒体播放](media-playback.md#解码与硬件加速)。

## EXIF 与图内文字

`exifParse::formatExifValue()` 按标签号处理已知摄影格式，未知标签退回通用显示，不把厂商私有值强行翻译；曝光时间分母按数值计算，不能照搬原分数的分母。

UserComment 的字符集前缀与正文分别处理。UTF-16 先认 BOM，无 BOM 时尝试两种端序并以文本可信度评分，文件端序只作平局参考；Exiv2 返回的字节序不一定与 TIFF 头一致。无前缀且中间含零字节的正文也需考虑 UTF-16。回归为 `--exif-test`。

`TextRenderer` 用系统字体与 stb_truetype 绘制图像中的文字，按真实字形度量排版并缓存字形；字体数据按路径在进程内共享，实例仍为界面线程专用。ImGui 负责 UI 文字，不把其职责重新塞进 TextRenderer。
