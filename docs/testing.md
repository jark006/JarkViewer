# 验证指南

[文档导航](README.md) · [构建与依赖](build.md) · [调试与排障](debugging.md)

## 按改动范围选择

代码、资源、工程和构建变更的底线是 Release 构建与源码不变量检查通过，再运行下表对应项。纯文档/注释变更检查链接、命令与差异，并跑源码不变量检查即可。不要把旧报告中的断言总数当成本轮结果。

| 改动 | 对应验证 |
| --- | --- |
| 解码路由、格式、EXIF、方向 | 生成语料的 `--probe --full`；EXIF 编码另跑 `--exif-test`；实开相关格式确认原图接管 |
| 几何、旋转、缩放 | `--navigation-test`、`--resample-test` |
| WASD 平移与反向设置 | `test_wasd_pan.py` |
| 图像缓存、预读、内存 | `--cache-test`、`test_cache_budget.ps1` |
| ICC、SVG、文件排序 | 分别跑 `--color-test`、`--svg-test`、`--sort-test` |
| 缩略图服务、Shell 链路 | `--thumbnail-test`、`--shell-thumbnail`、导航 GUI 回归 |
| MediaPlayer / MediaDecoder / VideoPlayback / AudioOutput | `--video-test`（含纯音频）、`--playback-test`、`--audio-test`、`--hw-test` |
| 音频频谱 | `--spectrum-test`，实开音频确认按播放位置显示 |
| 播放器绘制、条带 | `test_player_resize.ps1`、`test_player_scaling.ps1`，硬解相关变更也用硬解语料验证 |
| 窗口几何、全屏、交接 | `test_placement_memory.ps1` |
| 导航浮层 | `--navigation-test`、`test_navigation.ps1` |
| 编辑与标注 | `--annotate`、`test_editor_text.ps1` |
| 批处理、文件写入、任务生命周期 | `--batch-test`、`test_batch_lifecycle.py`；涉及颜色/深度另跑 `--color-test` |
| 打印、抖动、预览滑块 | `--print-test`、`test_print_preview.py` |
| 主题、帮助页 | 分别跑 `test_theme_controls.py`、`test_help_page.py` |
| 命令行语言覆盖、多语言 | `test_language_override.ps1`、`--lang-test`，文案改动检查对应界面 |
| 字符串表、格式清单、工程列表、不变量检查 | `check_source_invariants.ps1`；修改检查逻辑时再跑 `verify_source_invariant_checks.ps1` |
| 探针控制台/重定向、WP2 计时 | `test_probe_redirection.py` |

## 隔离与结果判定

- PowerShell 用 `pwsh`，Python 脚本用 `python -I`。Python 回归通常需要 Pillow / numpy，媒体语料生成需要命令行 ffmpeg / ffprobe；这与应用内的静态 FFmpeg 是两回事。
- 生成语料、设置、缩略图缓存、导出、截图与报告使用独立 TEMP 目录。真实语料 `D:/Downloads/test` 只读；测试前后核对原文件哈希和修改时间。
- 会触碰设置/缓存的 GUI 验证必须使用独立 exe 副本。部分专用脚本自行复制，通用 `capture_window.ps1` 和导航等脚本不会替你隔离，调用前确认。`test_navigation.ps1 -CheckSettings` 明确要求 TEMP 副本。
- GUI 脚本串行执行，会操作鼠标键盘；运行时避免同时操作测试窗口。修改布局后同步坐标、DPI 换算与就绪条件。
- 成功截图不代表语义通过。优先像素、状态和文件字节断言；看图窗口须等标题出现原图尺寸再截图，不能把 256px 渐进预览算作原图。播放器标题只有文件名，须单独判断就绪。
- 检查退出码和报告中的失败/跳过原因；缺少设备或语料是未验证，不能写成通过。媒体性能要对照时长和真实窗口表现，不能只看一个解码耗时。

## 构建与源码检查

```powershell
pwsh ./buildRelease.ps1
pwsh tools/check_source_invariants.ps1
python -I tools/gen_vs_project_files.py --check
```

新增不变量检查时配一条故意制造错误的反向用例，并在构建结束后单独运行：

```powershell
pwsh tools/verify_source_invariant_checks.ps1
```

反向验证会原地改写再按原字节还原源码，不与编译或其它编辑并行；运行后确认工作区没有残留。用脚本计算实际用例数，不硬编码总数。

## 无界面探针

入口是 `JarkViewer.exe --probe`。显式传 `--out`，否则报告默认落在工作目录的 `decode-probe.txt`。GUI 子系统输出不适合依赖终端；在交互式 PowerShell 中管道可确保等待进程退出，报告写完后再读取：

```powershell
& ./x64/Release/JarkViewer.exe --probe --cache-test --out "$env:TEMP/cache-test.txt" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'cache-test failed' }
Get-Content -LiteralPath "$env:TEMP/cache-test.txt" -Encoding utf8
```

终端编码导致乱码时用 `python -I` 按 UTF-8 读报告。探针应保留继承的 stderr 重定向，不强行重开到控制台，原因见 [WP2 排障](debugging.md#wp2-解码耗时与重定向)。

下面是同一入口的选项；均追加 `--out <报告.txt>`，每项使用独立报告：

| 选项 | 覆盖内容 / 所需输入 |
| --- | --- |
| `<文件...> --full` | 解码、EXIF、全部动图帧；支持多路径，通配符先由 shell 展开 |
| `--annotate --annotate-out <目录>` | 合成底图和标注像素断言，无需图片 |
| `--color-test` | ICC 恒等跳过、alpha 保留、串并行一致、P3 已知落点 |
| `--resample-test` | 四种旋转、两种通道、平滑块复用、超宽位图边界 |
| `--cache-test` | 字节预算、LRU、外部持有、至少两条、预取边界 |
| `--spectrum-test` | 频段、电平、直流过滤、衰减、按播放位置取帧 |
| `--svg-test` | CSS 折叠、直通 alpha、区域/旋转、filter/textPath 能力边界 |
| `--sort-test` | 自然序、时间/大小排序及当前图片下标 |
| `--exif-test` | UserComment 字符集前缀、UTF-16 端序/BOM/中文 |
| `--print-test` | GDI 方向、颜色、行距及抖动布局；不提交打印作业 |
| `--navigation-test` | 鸟瞰几何、定位和手势归属 |
| `--thumbnail-test --out-dir <目录>` | 持久缓存、LRU、并发、清理 epoch 与本地解码兜底 |
| `--shell-thumbnail <图片> --out-dir <目录>` | Shell 缩略图提取、方向和透明度 |
| `--batch-test` | 原子保存、同批输入保护、失败保留、16 位透明图转换 |
| `--lang-test` | 窄/宽字符串表抽样，辅助检查 ID 与文案 |
| `--video-test <媒体...>` | 暂停、双向 seek 首帧、单帧、EOF、重播、拖动；纯音频走无帧断言 |
| `--playback-test <媒体...>` | 交付率、间隔、时钟倍速、最大停滞，对照媒体时长 |
| `--audio-test <音频>` | 音量 0 的真实 XAudio2 提交、时钟、背压、flush、暂停 |
| `--hw-test <视频...>` | 硬解优先与强制软解逐帧对照：帧数/PTS/尺寸和像素容差 |

### 生成与使用语料

```powershell
$corpus = Join-Path $env:TEMP ('jv-corpus-' + [Guid]::NewGuid().ToString('N'))
python -I tools/gen_testdata.py $corpus
$inputs = @((Get-ChildItem -LiteralPath $corpus -File -Recurse).FullName)
& ./x64/Release/JarkViewer.exe --probe @inputs --full --out "$corpus/decode.txt" | Out-Null
$probeExit = $LASTEXITCODE
Get-Content -LiteralPath "$corpus/decode.txt" -Encoding utf8
```

生成集包含故意损坏、错扩展名、无扩展名和方向样本，不能要求每个文件解码成功；需要按预期核对失败原因。要自动汇总预期结果，使用下面的审计入口。

PSD/PSB 专项回归（Pillow / numpy）：生成不同位深、颜色模式、透明/专色通道和压缩组合，直接加载本轮缩略图 DLL 检查像素，同时验证探针、批量导出与坏输入拒绝。无兼容性合成图的生成样本另覆盖层叠顺序、隐藏、负坐标、越界裁剪、opacity/fill opacity、直通 alpha 和 16 位 `Lr16`；真实纯白合成图必须保持纯白。指定真实样本目录时，合成图按内嵌 ICC 与 Pillow 的独立解码结果比对，无合成图的样本使用同名 PNG 参考，并核对原文件哈希和修改时间；PSB 只在内存重包装合成图供 Pillow 读取。全部输出放在命令打印的 TEMP 目录。

```powershell
python -I tools/test_psd_decode.py --real-corpus D:/Downloads/test/psd
```

媒体改动还应覆盖带/无音轨、短音轨、纯音频、竖拍旋转、长 GOP、4K、大于 256 MiB 文件及截断输入。`--video-test` 的 seek 是异步的，先 pump 再采样基准；短片恢复断言的下界按媒体时长夹取，不能把自然播完当失败。

## 专项 GUI 命令

下列占位路径要替换；通用脚本的 `-Exe` 应指向事先放在 TEMP 的独立副本。

```powershell
pwsh tools/test_cache_budget.ps1 -Exe x64/Release/JarkViewer.exe -Image <中等图片> -Copies 12
pwsh tools/test_navigation.ps1 -Exe <TEMP副本.exe> -Image <图片> -OutDirectory <截图目录> -CheckSettings
pwsh tools/test_player_resize.ps1 -Exe <TEMP副本.exe> -Video <视频> -OutDirectory <截图目录>
pwsh tools/test_player_scaling.ps1 -Exe <TEMP副本.exe> -Video <棋盘格视频> -OutDirectory <截图目录>
pwsh tools/test_placement_memory.ps1 -Exe x64/Release/JarkViewer.exe -Media <视频> -Image <图片>
pwsh tools/test_editor_text.ps1 -Exe <TEMP副本.exe> -Image <图片> -Out <截图.png>
pwsh tools/test_language_override.ps1 -Exe x64/Release/JarkViewer.exe
python -I tools/test_batch_lifecycle.py --exe x64/Release/JarkViewer.exe
python -I tools/test_print_preview.py --exe x64/Release/JarkViewer.exe
python -I tools/test_help_page.py --exe x64/Release/JarkViewer.exe
python -I tools/test_theme_controls.py --exe x64/Release/JarkViewer.exe
python -I tools/test_wasd_pan.py --exe x64/Release/JarkViewer.exe
```

`test_print_preview.py --wheel-only` 可定向验证滑块滚轮。导航语料要有足够同目录图片（建议 20 张），缩略图带两侧可能留空，应点击当前图相邻的实际项目。文字回归必须同时断言输入变化和画布预览，不能只保存截图；Unicode `WM_CHAR` 只覆盖上屏结果，候选窗组字还需真实输入法检查。

硬解缩放回归使用 H.264 4:2:0 棋盘格，`yuv444p` 无损片通常只能走软解。可生成：

```powershell
ffmpeg -f lavfi -i "nullsrc=s=320x240:d=4:r=10,format=yuv420p,geq=lum='if(eq(mod(floor(X/8)+floor(Y/8),2),0),0,255)':cb=128:cr=128" -c:v libx264 -crf 1 -pix_fmt yuv420p <棋盘格.mp4>
```

开启日志核对 `解码=硬解(d3d11va)`，再跑播放器 resize/scaling 两条回归。硬解运行期回退可用 [故障注入步骤](debugging.md#硬件解码) 单独覆盖。

## 综合回归

[`test_refactor_audit.py`](../tools/test_refactor_audit.py) 先复制 exe，将程序、设置、语料和报告放在独立 TEMP 目录，保存退出码、耗时、断言、失败与跳过原因。`--gui` 追加界面测试，`--real-corpus` 只抽样，不等同于全目录实测。

```powershell
python -I tools/test_refactor_audit.py --gui --stress-rounds 8
python -I tools/test_refactor_audit.py --gui --cases "batch*,color,annotate" --stress-rounds 0
python -I tools/test_refactor_audit.py --real-corpus D:/Downloads/test
```

## 全量真实语料

第一步打印报告目录，填入后续命令；界面流程串行运行。程序、输出与缓存独立，原文件始终只读。

```powershell
python -I tools/test_real_corpus.py D:/Downloads/test --workers 2
python -I tools/test_real_corpus_playback.py <报告目录>
python -I tools/test_real_corpus_ui.py <报告目录>
python -I tools/test_real_corpus_ui.py <报告目录> --export-only
python -I tools/test_real_corpus.py --summarize <报告目录>
```

`report.html` 提供逐文件结果与截图链接；`inventory.json`、`results.json`、`metadata.json` 保存哈希、程序版本与运行配置。最终汇总会再次核对原图。全量解码成功不等于与其它渲染器逐像素一致；SVG 能力边界仍以当前库为准。
