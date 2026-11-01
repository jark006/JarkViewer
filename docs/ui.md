# 界面与交互

[文档导航](README.md) · [架构导航](architecture.md) · [验证指南](testing.md)

## ImGui 宿主、主题与布局

`UiHost` 统一管理 Dear ImGui 的 Win32/DX11 后端、主题、字体、DPI、IME 与纹理池。业务实现 `D3D11App::DrawUi()`；设置、打印、批处理、编辑、重命名均在主窗口内绘制，与主窗口共用界面线程。

- 尺寸经 `uiScale()` / `dp()` 或 UiHost 缩放，工具窗口的默认/最小尺寸经 `fitWindowSizeToMainViewport()` 收敛。多视口模式下超出主视口可能分离为额外 OS 窗口。
- 保留 `io.ConfigWindowsResizeFromEdges=false`，仅右下角抓手改尺寸。新增控件要检查最小窗口下的可达性，底部固定按钮/说明按真实高度预留。
- 浅色控件使用白填充与灰边框；每次应用主题都重设 `FrameBorderSize`，防止切回深色后残留。回归 `test_theme_controls.py`。
- 前景绘制列表使用屏幕空间，客户区坐标先经 `uiPos()` 加主视口原点。`mainRes.png` 雪碧图切片 UV 内缩半像素，避免放大后串入邻格。
- 折行文本统一使用 `jark::ui::drawWrappedText()`，查看器 EXIF 与播放器信息面板共用，不再各维护一份测量/裁剪/滚动算法。
- 图标统一定义在 `UiHost.h` 的 `jark::ui::icon`，优先普通几何字形；使用 Fluent/MDL2 私用码位前用图标探针确认存在。
- 同窗口控件标签须有独立 ImGui ID，必要时附 `##名字`。样式 Push/Pop 不依赖循环中会被点击修改的状态，避免失配触发错误恢复闪红框。

vendored ImGui 有 `[JarkViewer 本地修改]`：`imgui.cpp::RenderWindowTitleBarContents()`、`imgui_widgets.cpp::CloseButton()` 和 `imgui_internal.h` 的声明，实现系统风格标题栏关闭按钮。升级 ImGui 时检查并迁移这三处，Dock 页签的默认关闭行为保持原语义。

## 输入、IME 与出帧

- `WndProc` 根据 `UiHost::processMessage()` 返回值提前返回，已处理的 `WM_IME_COMPOSITION` 不能再交 `DefWindowProc`，否则中文上屏重复。
- 不调用不可逆的线程级 `ImmDisableIME()`。UiHost 平时摘下 IME 上下文，`io.WantTextInput` 时挂回；候选窗位置由后端按文本光标设置。
- 只有确实显示工具窗口时才按 ImGui 的 `WantCapture*` 独占输入；最后一个窗口关闭后该标志可能残留。新工具窗登记 `anyWindowVisible()` / `closeTopWindow()`，Esc 先关顶层工具窗。
- Ctrl 快捷键打开工具窗时清理自己的 Ctrl 状态，避免释放消息被工具窗接走后卡住。
- `OnMouseRelease` 在 UI 捕获判断之前收尾已有手势，`OnPointerCancel` 处理失焦/取消/异常失捕获。正常抬起时后端 `ReleaseCapture()` 会同步重入，`m_processingMouseRelease` 区分这种情况。
- 客户区负坐标使用有符号 `GET_X_LPARAM` / `GET_Y_LPARAM`；滚轮 lParam 是屏幕坐标，先 `ScreenToClient`。`ShowContextMenu()` 接收客户区坐标并自行转换，不重复 `ClientToScreen`。
- 空闲分支也为显示中的工具窗出帧；关闭工具窗后补一次 Present 擦除旧画面。切图排入 refresh，切换幻灯片显隐也主动请求出帧。

交换链使用 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 双缓冲，尺寸变化/WM_PAINT 请求补呈现。改回旧模型前需定向验证，曾出现 Present 成功但窗口全白。

## 设置与多语言

设置布局与覆盖顺序见 [持久设置](architecture.md#持久设置)。兼容字段的语义包括：

| 字段 | 兼容约定 |
| --- | --- |
| `legacyRightClickAction` | 旧右键动作字段保留占位；右键当前只打开菜单 |
| `hideNavigator` | 原 `reserve2` 原位复用，旧值 0 表示显示 |
| `disableZoomSmoothing` / `disableVideoHardwareDecode` | 取反存储，旧值 0 默认开启 |
| `reverseWasdPan` | 使用原 reserve 的首个 uint32（偏移 1212），reserve 后移至 1216；关联清单仍在 3296，总长 4096。读到大于 1 的值回落为 0 |

WASD 反向只改变键盘平移符号，不影响鼠标拖动；步幅和按旋转后尺寸夹取的规则保留。回归 `test_wasd_pan.py`。

`UIStringTable`（ImGui/画布）与 `UIStringTableWide`（Win32 标题/菜单/消息框）有独立 ID，同编号不保证同文案。新文案追加到相应表尾，使用处以具名 `kStr*` 常量引用；不得在中间插入导致旧 ID 错位。

`getUIStringW()` 返回自带缓冲的 `UIStringWide`。需要跨语句保留文本时用 `.str()` 存 `std::wstring`；传给 `std::format` / `wstring_view` 等场合按接口显式 `.c_str()`。不能重新改成共用 thread_local 转换缓冲，否则同语句多次调用互相覆盖。

六种语言的枚举、系统识别、显示名、设置选项和两张表的每列必须一致。帮助页以 `\t` 分隔“操作/输入”，`|` 分隔替代输入，不能按自然语言空格或标点拆列；增改快捷键时核对查看器与播放器各自的输入处理。回归 `--lang-test`、`test_language_override.ps1`、`test_help_page.py`。

## 导航与缩略图

`NavigationOverlay` 提供鸟瞰与底部悬停预览带，自己命中、自己管理手势，不加入工具窗的 `anyWindowVisible()`。

- 鸟瞰使用当前已加载图缩小，不做文件 I/O；几何用 `zoomCur/slideCur` 和共享 `imageGeometry()`。
- 预览带展开区域与触发区域一致，鸟瞰区域豁免；当前图严格水平居中，两侧不足留空且不可点。滚轮/按钮/点击命中后拦截画布操作。
- `navigateImage` 是绝对位置，同 generation 覆盖合并，不按 slide 增量相加；动作带图像/目录版本，换图、旋转或改尺寸使旧坐标失效。
- 浮层按下后的移动/抬起由它收尾，出控件也不转交画布；失焦后吞掉残余抬起。开工具窗或切图 cancel 手势。
- 直接点缩略图使用 `direction=0`，不准备滑动动画；切换前停止旧媒体。鸟瞰的关闭按钮只改变 `hideNavigator`，不影响预览带。
- 幻灯片期间隐藏导航、EXIF、按钮、播放条、角标和加载提示，同时禁用对应命中；左右边缘换图保留。停止后恢复，临时显隐不落盘。

`ThumbnailService` 取图顺序为内存 → `JarkViewer.thumbnail` → Shell 仅查缓存 → Shell 正常提取 → 私有 `ImageDatabase` 本地解码。Shell 经系统 surrogate，禁止 `WTS_EXTRACTINPROC` 和直接加载 provider DLL。

持久缓存放在实际设置文件同目录，使用显式序列化、CRC、1000 槽索引与追加数据区；损坏只丢单条。按路径及原文件属性识别，已知覆盖/重命名调用 invalidate。LRU 按实际展示刷新，预取不刷热；命名互斥锁协调跨进程提交，generation/epoch 阻止清理前的旧任务回写。

缓存 I/O、Shell COM、本地解码分别由常驻 worker 承担，慢提取不阻塞缓存查询/清理；本地解码线程不碰查看器图像缓存/窗口状态。同步 Shell 调用不能强制中断，退出使用有界等待且 worker 只持有自身状态。

后台只产 CPU 位图，GPU 纹理在界面线程上传/释放。清缩略图仅释放浮层自己的槽（鸟瞰 5，预览带 16..79），不能用全局 `releaseTextures()` 使其它窗口纹理失效。回归 `--thumbnail-test`、`--shell-thumbnail` 和导航 GUI 测试。

## 编辑、批处理与保存

编码/编辑保存、打印预览另存、批处理和实况视频导出统一使用 `jarkUtils::writeFileAtomically`：在目标同目录独占临时文件，写完、刷盘、关闭成功后才替换。编码/写盘/锁定失败不能先截断原文件或报告成功。

`BatchProcessor` 解码复用工程内路由、编码用 OpenCV。覆盖输出不能覆盖同批其它输入，冲突加序号；批量重命名遇到其它输入则跳过，前缀不能携带目录。显式就地操作与普通覆盖分开处理，命令行任务失败返回 1。

16 位/浮点输入先 `convertMatToCV_8U()` 再 ICC 与白底合成，不能按 8 位解释高位深 alpha。缩放规则集中在 `scaledSize()`：默认缩小 AREA、放大 Lanczos；未指定输出扩展名时保持原格式。用 `--batch-test` 验证文件字节与已知像素。

批处理尺寸预览需要解码，走常驻后台线程；任务可能改写源文件，完成后清预览路径缓存。关闭重开活跃批处理只恢复显示，不清 worker 正在写的结果；停止/析构顺序见 [生命周期边界](architecture.md#生命周期边界)。

`ImageAnnotator` 是独立标注模型，`EditorWindow` 负责交互。编辑画布固定适应窗口，命中先按比例映射到图像，空白处不创建标注。透明棋盘纹理在本帧 `fitScale_` 算出后重建；文字工具点锚定，光标/预览不能依赖拖动状态。PNG 保留 alpha，JPEG 铺白；覆盖后通知主窗口作废图像缓存。

编辑侧栏标签放控件左边，色块按可用宽度分配，保存/复制提示画浮层，不额外挤占布局导致滚动条闪现。工具栏窄窗口换行；增加侧栏内容时同步检查高度。回归 `--annotate`、`test_editor_text.ps1`。

重命名由主窗口完成改盘、重排、缩略图失效和缓存重载；复制/移动重名让到 `名 (2).ext`，不覆盖，跨盘移动先复制成功再删除源文件。新增菜单加速键不可与现有项冲突，Win32 文案使用宽表；外部编辑器的图片参数整体加引号并检查 ShellExecute 结果。

## 打印预览

`ImageAdjust` 为打印、编辑和批量共用的就地图像调整。打印预览基于 `sourceImage_` 的副本，即使小图不用缩放也 clone，不能让预览反复修改源图。对比度计算在 pow 前夹值，避免负底数产生 NaN；关闭窗口即记住参数，不要求先打印。

- `toBitmap()` 的负 `biHeight` 表示从上到下，按源 `ptr(y)` 正序复制；24bpp 行距按 4 字节对齐，支持非连续 ROI。
- 黑白抖动先缩到实际 framebuffer 物理像素尺寸，再调整/抖动并 1:1 最近邻显示，随后恢复线性采样。原点与居中偏移也对齐物理像素，resize 作废预览缓存。
- 打印按打印机分辨率处理，另存按源尺寸处理，不能把显示预览拿去替代两者。
- 亮度/对比度悬停滚轮每格 ±1、范围 0–200。两个滑块分别累积高精度余量，离开/拖动/编辑时清余量，边界不积压反向响应；滚轮归属阻止窗口同时滚动。

回归 `--print-test` 与 `test_print_preview.py`（滚轮可加 `--wheel-only`）。自动验证覆盖预览与保存链路，不向真实打印机提交作业。
