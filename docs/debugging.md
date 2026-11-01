# 调试与排障

[文档导航](README.md) · [验证指南](testing.md)

## 日志与启动分段

Release 默认关闭日志，用 `--log` 或 `JARKVIEWER_LOG=1` 临时开启，文件在 `%TEMP%/JarkViewer.log`；Debug 默认开启控制台日志。新增诊断使用 `JARK_LOG(...)`，关闭时参数不求值。

```powershell
./x64/Release/JarkViewer.exe --log <图片>
$env:JARKVIEWER_STARTUP_TRACE = "$env:TEMP/jv-startup.txt"
./x64/Release/JarkViewer.exe <图片>
```

`startupTraceMark()` 记录 begin、解码派发、设备/UI 就绪、首帧就绪与绘制的相对毫秒数；新增关键阶段可补标记。测试后移除自己设置的环境变量，避免后续计时混入旧配置。

## 窗口截图与输入

`capture_window.ps1` 默认只截目标窗口；使用前把 exe 和测试图片放进独立 TEMP 目录，避免改变用户设置。`-Argument` 是一整条命令行字符串，内部使用 `Start-Process -ArgumentList`；优先用当前工作目录下的短相对文件名，带空格时必须正确引用，路径相对于启动工作目录解析。

```powershell
pwsh tools/capture_window.ps1 -Exe <TEMP副本.exe> -Argument "img.svg" -Keys "{UP 15}" -Out <截图.png>
pwsh tools/list_windows.ps1 -ProcessId <pid>
```

| 参数 | 用法 |
| --- | --- |
| `-Keys` / `-Keys2` | 第一批与第二批按键，后者配 `-Keys2DelayMs` |
| `-Hover "x,y"` | 物理客户区坐标，悬停 |
| `-Drag "x1,y1,x2,y2[;...]"` | 物理客户区坐标；起终点相同表示点击 |
| `-DragHold` | 最后一段拖动保持按下，截图后释放，适合裁剪/马赛克拖框 |
| `-Click` / `-RightClick` | 按实际窗口宽度 / `-LogicWidth` 缩放的逻辑客户区坐标；与 Hover/Drag 单位不同 |
| `-MenuKeys` | 驱动 Win32 右键菜单，可配 `-Screen` 截取屏幕上的实际显示 |
| `-Window` | 脚本仍支持选择窗口；常规 ImGui 工具窗就在主窗口内，使用默认 `main` |

这些坐标不能混用。先定位真实客户区和 DPI，再判定动作是否命中；脚本采用窗口枚举与 `ForceForeground` 提高按键可靠性。布局变化要同步专项回归，详见 [GUI 验证](testing.md#专项-gui-命令)。

图标字形不确定时，用 `python -I tools/gen_icon_probe.py` 临时生成码位浏览器并截图确认；结束后 `python -I tools/gen_icon_probe.py --revert`，确认源码已还原。

## 崩溃与内存 dump

稳定复现的问题可用 cdb 附着：

```powershell
pwsh tools/catch_crash.ps1 -Exe x64/Release/JarkViewer.exe -AppArgs="--probe --video-test a.mp4 --out r.txt" -Dump "$env:TEMP/crash.dmp"
```

附着本身可能掩盖时序竞态；间歇性开关/seek 崩溃优先让探针进程自行落 dump，再离线分析：

```powershell
$env:JARKVIEWER_CRASH_DUMP = "$env:TEMP/jv-crash.dmp"
& ./x64/Release/JarkViewer.exe --probe --video-test <视频> --out "$env:TEMP/video-test.txt" | Out-Null
cdb.exe -z "$env:TEMP/jv-crash.dmp" -c ".ecxr;r;kb 16;q"
Remove-Item Env:JARKVIEWER_CRASH_DUMP
```

该环境变量由 `--probe` 路径注册异常处理，生成完整内存 dump。并发复现时每个进程使用独立 dump/报告路径。`.ecxr` 必需，否则默认线程可能不是崩溃线程。

系统模块可用符号服务器；本工程源码定位需要匹配 PDB。临时启用 `DebugInformationFormat=ProgramDatabase` 与 `GenerateDebugInformation=true` 后重新构建，保留对应 exe/PDB；工程中的条目元数据未必能被命令行 `/p:` 覆盖。诊断完成后恢复临时工程改动。

## 硬件解码

先看日志 `media opened` 的 `解码=`：是 `硬解(d3d11va)` 还是软件。不能只凭显卡存在认定正在硬解；AV1 当前优先选到 libdav1d，会走软件。用户可在设置中取消“视频硬件解码”排查驱动问题。

`--probe --hw-test <视频>` 将硬解优先与强制软解逐帧对照，验证帧数、PTS、尺寸与像素容差。回读故障的开局回退路径平时难覆盖，定向验证可：

1. 在干净可还原的改动范围内，临时将 `MediaDecoder::readNext` 的 `av_hwframe_transfer_data(...)` 结果改为 `AVERROR(ENOSYS)`。
2. 构建，跑带旋转与普通 H.264 的 `--hw-test`，确认报告实际回退且与软解帧序列一致。
3. 按原字节还原临时改动，重新构建正常 Release，检查差异。

硬解共用 D3D11 设备的并发问题可在独立复制的实况语料目录里连翻，核对硬解实例、回退和 dump。文件参数相对工作目录解析，不能在仓库根目录用只有语料目录才存在的短文件名。

## 翻页、颜色与播放

- **翻页慢 / 内存**：按 Tab 或中键查看 EXIF 面板的缓存条数/字节/预算、解码、淘汰与预取跳过。解码次数持续超出浏览数量说明在反复淘汰；预取跳过增长说明当前图过大。进一步开日志并跑 `--cache-test` / `test_cache_budget.ps1`。
- **颜色异常**：核对“源 ICC → 目标 ICC”日志。查看器目标是显示器，批量输出是 sRGB；P3 超色域颜色转 sRGB 时可能剪裁到相同颜色，图案因此消失并不一定是错误。用 `--color-test` 的已知落点区分，详见 [色彩管理](image-pipeline.md#色彩管理)。
- **音画卡顿**：跑 `--playback-test`，同时看交付率、时钟倍速和最大停滞；音频饿死会让主时钟和视频一起停住。seek 问题还要跑 `--video-test`，音频队列问题跑 `--audio-test`，仅自然播放覆盖不到 seek 的缓冲生命周期。
- **暂停后缩放黑屏**：检查尺寸变化后是否重画并重新上传画布，只有 Present 请求无法补回被重建的空纹理，跑 `test_player_resize.ps1`。

## WP2 解码耗时与重定向

某些预编译 `webp2.lib` 带 `WP2_TRACE`，一张小图也可能写出数十 MB 的逐符号 stderr。过去把这类输出阻塞误判为解码慢，已撤回，具体对照见 [历史记录](history/2026-11-audit.md#wp2-计时纠正)。

`runDecodeProbe` 保留继承的 stderr，只在 stdout 未重定向时连接控制台；不得强制把 stderr 重开为 `CONOUT$`。改应用头文件里的宏不能关闭已编入 `.lib` 的 trace，换库时应在构建库本身时关闭。

```powershell
python -I tools/test_probe_redirection.py --image <带trace的WP2图片>
```

计时需要对照 stdout/stderr 到 NUL、文件、管道的差异，并等真实窗口完成原图接管；不要依据单次 `--probe` 时间给某个格式加特殊跳过。
