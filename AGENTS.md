# JarkViewer 协作约定

JarkViewer 是 Windows 10/11 x64 原生看图软件，使用 C++23、Win32、Direct3D 11、OpenCV 与 Dear ImGui。视频/音频由同进程中的独立播放器窗口处理。优先使用简体中文沟通。

本文件只保留日常工作规则；具体操作与实现细节见 [开发文档导航](docs/README.md)，按任务查阅。

## 工作方式

1. 先看工作区状态，保留用户已有改动。仓库有 `.codegraph/` 时，理解或定位代码先用 `codegraph_explore`，或 `codegraph explore "符号或问题"`；没有索引就跳过，不自行建立索引。文档和未索引配置可直接搜索、读取。
2. 贴合相邻代码风格，小步修改；源码使用 UTF-8、C++23、通常为 4 空格缩进。注释解释原因，尤其是生命周期、兼容性和回归风险。
3. 为行为改动保留可重跑的验证命令。新增源码不变量检查时，同步在 `tools/verify_source_invariant_checks.ps1` 增加能触发失败的破坏用例。
4. 代码、注释和相关文档同轮更新；用户可见变化同步中英文 README。交付说明写清改动、实际验证结果与未验证项；提交时沿用中文简短标题并说明原因和验证。

## 命令与验证

在仓库根目录使用 PowerShell 7（`pwsh`），Python 脚本使用 `python -I`。

```powershell
pwsh ./buildRelease.ps1
pwsh tools/check_source_invariants.ps1
```

- 修改 C++、资源、工程、依赖或构建逻辑：Release 构建与源码不变量检查必须通过，再按 [验证指南](docs/testing.md) 运行受影响模块的自检/回归。
- 仅修改文档或注释：检查差异、链接和命令是否有效，并运行源码不变量检查；无需重编译或运行整套 GUI/媒体回归。
- 修改不变量检查：构建结束后运行 `pwsh tools/verify_source_invariant_checks.ps1`。它会原地破坏再还原文件，不能与构建或其它源码编辑同时运行。
- 新增、移动或删除查看器源码后运行 `python -I tools/gen_vs_project_files.py`，并检查两个工程的文件列表。
- 无界面自检显式指定 `--out` 报告文件；GUI 子系统输出不宜依赖终端，退出码和报告都要检查。完整命令及语料要求见验证指南。

## 必须守住的边界

- **保护用户数据**：真实语料 `D:/Downloads/test` 只读；生成语料、报告和导出放独立 TEMP 目录。会读写设置/缓存的界面测试在 TEMP 的独立 exe 副本运行；GUI 脚本串行执行。
- **保护文件写入**：编码/保存/导出使用 `jarkUtils::writeFileAtomically`，失败保留原文件；批处理覆盖不能误伤同批次其它输入。
- **保持设置兼容**：`SettingParameter` 固定 4096 字节，不随意改变布局；临时覆盖在读盘后应用。看图与播放器共用窗口几何记忆。
- **保持线程与窗口生命周期**：后台任务有明确停止/回收路径；UI 线程不阻塞解码。涉及缓存、批处理或窗口交接，先读 [架构导航](docs/architecture.md) 的生命周期边界。
- **同步格式与文案**：新增格式同时核对嗅探/解码、EXIF/方向、关联列表和两份 README；字符串窄/宽表是两套独立 ID，新文案追加到相应表尾。
- **保持构建可复现**：`.ps1` 保留 UTF-8 BOM 与 PowerShell 7 守卫；构建期间不改源码。不提交 `.vcxproj.user`、`.vs/`、本机库路径、设置、缓存或测试产物。

## 按任务阅读

| 任务 | 文档 |
| --- | --- |
| 首次构建、静态库、工程配置 | [构建与依赖](docs/build.md) |
| 回归选择、探针、真实语料、GUI 验证 | [验证指南](docs/testing.md) |
| 崩溃、启动慢、日志、硬解排查 | [调试与排障](docs/debugging.md) |
| 模块定位、入口、窗口交接、设置 | [架构导航](docs/architecture.md) |
| 解码、缓存、几何、SVG、ICC、EXIF | [图像链路](docs/image-pipeline.md) |
| 播放器、音频、seek、D3D11VA、频谱 | [媒体播放](docs/media-playback.md) |
| ImGui、IME、多语言、导航、打印、编辑 | [界面与交互](docs/ui.md) |

历史审计仅在追溯时阅读，入口在开发文档导航。不要把测试流水账和完整实现说明重新堆回本文件。
