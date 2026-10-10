# AGENTS.md

本文件为 JarkViewer 项目提供开发约束、构建、测试、调试和交付的说明。

## 项目概览

JarkViewer 是 Windows 10/11 x64 原生图片查看器，使用 C++23、Win32、Direct3D 11 和 OpenCV 构建。它以单可执行文件方式发布，重点支持大量静态图、动图、RAW、LivePhoto/MotionPhoto、EXIF 信息显示、打印/编辑/标注、批量处理与文件关联；视频与音频由**同一个进程里的独立播放器窗口**打开（`wWinMain` 按 `jark::isPlayerFile()` 在两者之间二选一构造，见「简易播放器」一节）。

**文档分工**：`README.md` / `README_EN.md` 只面向使用者（功能、操作、格式清单、常见问题），不写开发相关内容；开发者要的一切——构建、测试、调试、模块实现、踩坑记录——都写在本文件里。两边唯一的重叠是**格式清单**（用户要看，代码的不变量检查也要拿它交叉核对），中英两份必须逐项一致，`tools/check_source_invariants.ps1` 的 4/5/6/6b/6c 项盯着这件事。

**项目按 AI 协作方式开发**（一句话需求 → 小步改动 → 可复现验证 → 提交），所以本文件的重点不是"介绍代码"，而是把**踩过的坑与验证手段写成可执行的约束**。三条硬要求：

1. **每次改动都要留下一条能重跑的验证命令**（自检 / 回归脚本 / 不变量检查），"我看着没问题"不算；
2. **新增的约束要配反向验证**——不变量检查加一项，`tools/verify_source_invariant_checks.ps1` 就加一条能把它绊倒的破坏；
3. **注释与文档和代码同一轮内同步**：历史上英文 README 落后中文一整轮、注释里留着已经被替换掉的设计（"音符徽章"）都发生过，改动落地时顺手把跟随它的说法一起改掉。

具体流程与逐条经验见「开发流程与验证经验」一节。

## 常用命令

**PowerShell 一律用 7（`pwsh`），不要用系统自带的 Windows PowerShell 5.1**：仓库里的 `.ps1` 都在开头带一段守卫，5.1 下会直接打印"请改用 pwsh / 怎么装"的提示并退出（守卫能生效的前提是脚本带 UTF-8 BOM，见「环境与工具坑」）。编译、构建走仓库根目录的 `buildRelease.ps1`：

```powershell
# Release x64 构建
pwsh ./buildRelease.ps1

# 运行已构建程序
./x64/Release/JarkViewer.exe
./x64/Release/JarkViewer.exe "D:/path/to/image.png"

# 无界面解码自检（无需人眼，验证解码路由/EXIF/动图）
./x64/Release/JarkViewer.exe --probe <文件或通配展开的路径...> [--full] [--out 报告.txt]

# 注意：程序是 GUI 子系统，stdout 在脚本/管道里拿不到，自检结果要 `--out 报告.txt` 才看得见；
# 不写 --out 时会落到工作目录的 decode-probe.txt（已 gitignore）。跑自检一律显式给 --out。

# 播放器自检：视频的暂停/精确 seek/单帧/播完停在最后一帧/拖动三条不变式；
# 纯音频文件走同一入口的无帧断言
./x64/Release/JarkViewer.exe --probe --video-test <文件...> --out 报告.txt

# 播放流畅度（交付率/交付间隔/时钟倍速与最大停滞，对照媒体时长）
./x64/Release/JarkViewer.exe --probe --playback-test <文件...> --out 报告.txt

# 音频链路（音量 0 提交音频，只看播放时钟是否按采样率推进，不发声）
./x64/Release/JarkViewer.exe --probe --audio-test <音频> --out 报告.txt

# Release 下临时打开日志（写入 %TEMP%\JarkViewer.log；Debug 默认开启控制台日志）
$env:JARKVIEWER_LOG=1; ./x64/Release/JarkViewer.exe --log "D:/path/to/image.png"

# 截取程序窗口做视觉验证（不截取桌面其它内容；-Keys 可注入按键，如 "{UP 15}" 放大）
pwsh tools/capture_window.ps1 -Exe x64/Release/JarkViewer.exe -Argument "img.svg" -Out shot.png

# 生成测试语料（需要 Python + Pillow/numpy，可选 ffmpeg）
python tools/gen_testdata.py <输出目录>

# 标注逻辑自检（合成底图 + 像素断言，不需要人眼）
./x64/Release/JarkViewer.exe --probe --annotate [--annotate-out 输出目录] [图片]

# 色彩管理自检（源/目标同为 sRGB 时恒等跳过、变换生效、四通道 alpha 不动、大图并行与串行逐字节一致，
# 以及 Display P3 → sRGB 的已知落点：超色域原色被剪裁回自身、次级饱和红也剪成同一个红）
./x64/Release/JarkViewer.exe --probe --color-test

# 缩放平滑插值自检（重采样块与逐帧路径在四种旋转/两种通道下逐像素对齐、缩小时面积平均更平、
# 1:1 不做重采样、余量复用边界，以及超过 SHRT_MAX 的超宽位图放大不再触发 OpenCV 的 remap 断言）
./x64/Release/JarkViewer.exe --probe --resample-test

# 图像缓存自检（纯合成、不需要语料：预算淘汰 / LRU 顺序 / 外部持有的条目踢了也不释放 /
# 单条超预算不空转 / 最少留 2 张 / 预算 = 物理内存的 50%）
./x64/Release/JarkViewer.exe --probe --cache-test

# 图像缓存的实机回归：连翻 N 张，工作集必须收敛在预算内、翻回上一张不重新解码
# （语料与程序都放在 %TEMP% 的独立副本里，不会动到用户自己的设置与缓存）
pwsh tools/test_cache_budget.ps1 -Exe x64/Release/JarkViewer.exe -Image <一张中等大小的图> -Copies 12

# 实时频谱自检（纯合成，不需要语料：频段映射/电平标定/直流阻断/衰减单调/"按播放位置取帧"）
./x64/Release/JarkViewer.exe --probe --spectrum-test

# SVG 自检（light-dark()/var() 折叠、半透明区域反预乘为直通 alpha、按可视区域光栅化的区域/旋转语义，
# 以及 lunasvg 的能力边界：filter 被忽略但图元仍绘制、textPath 不渲染）
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

# 播放器暂停态改窗口尺寸后画面还在不在（回归：曾整窗全黑）
pwsh tools/test_player_resize.ps1 -Exe x64/Release/JarkViewer.exe -Video <文件> -OutDirectory <截图目录>

# 窗口几何记忆在两种模式间互继承（含"退出时正全屏/最大化"两个例外；在 %TEMP% 的独立
# exe 副本上跑，设置文件写在 exe 旁边，不会动到用户自己的 JarkViewer.db）
pwsh tools/test_placement_memory.ps1 -Exe x64/Release/JarkViewer.exe -Media <视频> -Image <图片>

# 播放器放大画面有没有经过滤波（黑白棋盘格视频 + 中间调断言；回归：曾用最近邻放大，颗粒感很重）
pwsh tools/test_player_scaling.ps1 -Exe x64/Release/JarkViewer.exe -Video <棋盘格视频> -OutDirectory <截图目录>

# 列出某进程的可见窗口（自动化测试定位窗口用）
pwsh tools/list_windows.ps1 -ProcessId <pid>

# 编辑窗口的文字工具实测（Ctrl+E → 文字工具 → 点锚点 → 点侧栏文字框 → 投递 Unicode WM_CHAR）
pwsh tools/test_editor_text.ps1 -Exe x64/Release/JarkViewer.exe -Image <图片> -Out <截图>

# 命令行 --lang 的语言覆盖回归（改了语言能生效 + 临时语言不落盘；跑在 %TEMP% 的独立副本上）
pwsh tools/test_language_override.ps1 -Exe x64/Release/JarkViewer.exe

# 图标码位探针：临时把主界面换成码位浏览器，截一张图确认字形存在（--revert 还原）
python tools/gen_icon_probe.py

# 抓间歇性崩溃：cdb 附着 + 落 dump（时序竞态请改用进程内 JARKVIEWER_CRASH_DUMP，见「修改注意事项」）
pwsh tools/catch_crash.ps1 -Exe x64/Release/JarkViewer.exe -AppArgs="--probe --video-test a.mp4 --out r.txt" -Dump "$env:TEMP\crash.dmp"

# 重建第三方静态库（OpenCV 源码自建、zlib-ng 替换；详见「构建前提」）
pwsh tools/build-opencv.ps1 -Install
pwsh tools/build-zlib-ng.ps1 -Install

# 源码不变量检查（字符串表 // N 编号、格式清单与 README 交叉核对、PSD 顺序、图像缓存析构、
# 菜单加速键、版本号一致、两个 VS 工程的文件列表齐不齐；只读源码、秒级出结果，提交前可单独跑）
pwsh tools/check_source_invariants.ps1

# 重排 VS 工程文件列表与筛选器目录树（新增/移动/删除源文件后跑一次；--check 只报告不写）
python -I tools/gen_vs_project_files.py
# 反向验证上面这套检查：逐项制造错误确认真能抓到，跑完按原字节还原（新增检查项要同步加破坏）
pwsh tools/verify_source_invariant_checks.ps1
```

每次修改后至少保证 `buildRelease.ps1` 能干净编译通过；解码相关改动应先用 `--probe` 跑一遍 `tools/gen_testdata.py` 生成的语料（包含错扩展名、无扩展名、损坏文件、EXIF 方向、动图、视频等），再做人工冒烟（静态图加载、动图播放、EXIF 显示、打印预览和导出流程）。

## 开发流程与验证经验

这一节是几轮实际开发攒下来的：照着做，改动才落得住。

### 一轮改动的闭环

1. **先定位**：仓库有 CodeGraph 索引（`.codegraph/`），问"这块怎么工作 / 在哪"先用 `codegraph_explore`（一次调用给出符号源码、调用链与影响面），比埋头 grep + 逐个读文件快得多。
2. **改代码**：贴着相邻文件的风格写；注释写"为什么"，尤其是反直觉的约束和曾经踩过的坑（本文件里那些"别这么写"都来自真实回归）。
3. **编译**：`pwsh ./buildRelease.ps1` 必须干净通过。
4. **跑对应的自检**（见下表），拿到数字，而不是"看着对"。
5. **给新约束留痕**：不变量检查加一项 → 反向验证加一条破坏；界面语义能写成断言的写成断言，写不成的用截图脚本量像素。
6. **同步文档**：README / AGENTS / 注释与代码同一轮内改完（见「项目概览」那三条硬要求）。
7. **提交**：中文一句话概括 + 要点列表，写清"改了什么、为什么、怎么验证的"——`git log` 里已有的提交就是这个格式。

### 改哪一块，跑哪个自检

| 改动范围 | 必跑 |
| --- | --- |
| 解码路由 / 格式嗅探 / EXIF | `--probe <gen_testdata.py 的语料>`（含错扩展名、无扩展名、损坏文件） |
| 画布几何 / 旋转 / 缩放平滑 | `--probe --navigation-test`、`--probe --resample-test` |
| 图像缓存 / 内存占用 | `--probe --cache-test`、`tools/test_cache_budget.ps1` |
| 色彩管理 | `--probe --color-test` |
| SVG 与预处理 | `--probe --svg-test` |
| 文件列表 / 排序 / 重命名 | `--probe --sort-test` |
| 缩略图链路 | `--probe --thumbnail-test`、`--probe --shell-thumbnail` |
| `MediaPlayer` / `MediaDecoder` / `VideoPlayback` | `--probe --video-test`（含纯音频语料）、`--probe --playback-test`、`--probe --audio-test` |
| 频谱分析 | `--probe --spectrum-test` |
| 播放器画面 / 条带 / 音频画面 | `tools/test_player_resize.ps1`、`tools/test_player_scaling.ps1` |
| 窗口几何与保存 | `tools/test_placement_memory.ps1` |
| 主界面导航浮层 | `--probe --navigation-test`、`tools/test_navigation.ps1` |
| 编辑与标注 | `--probe --annotate`、`tools/test_editor_text.ps1` |
| 命令行参数 / 语言 / 设置读写 | `tools/test_language_override.ps1` |
| 字符串表 / 格式清单 / 工程文件列表 | `tools/check_source_invariants.ps1` + `tools/verify_source_invariant_checks.ps1` |

多块一起动的改动就把这一列全跑一遍；提交前的底线永远是两条：`buildRelease.ps1` 干净、`check_source_invariants.ps1` 全绿。

### 静态检查必须"真能抓到错"

不变量检查最危险的失效方式是**正则写歪了照样 PASS**。所以每加一项检查，就同步在 `tools/verify_source_invariant_checks.ps1` 里加一条"故意制造这个错误"的破坏用例（它跑完按原字节还原，不用 `git checkout`）；这个脚本自己也会报"检查没抓到破坏"。破坏条数用 `$script:breakCount` 动态算，别写死——写死的那个数字已经错过一次（写"13 项"时实际 17 项）。

### 界面语义用截图 + 像素断言

肉眼看着"应该对"的东西（条带颜色、放大有没有滤波、暂停时改窗口尺寸会不会黑屏）一律脚本化：`tools/capture_window.ps1`（`-Keys` 注入按键、`-Drag/-DragHold` 拖动、`-Hover/-RightClick/-MenuKeys/-Screen`）驱动窗口截图，再量像素。写这类脚本的实测经验：

- **参数里的路径不要带空格、也不要用长绝对路径**：`-Argument` 是拼接成一条命令行再用 `Start-Process -ArgumentList` 递出去的，带空格的路径会被截断（程序表现成"打开失败"，看截图会误判成程序坏了），长绝对路径还会让参数绑定直接报错。要打开某个文件做截图验证时，把它复制到工作目录里用短相对名（`tools/test_language_override.ps1` 就是这么干的）。
- 坐标是**物理客户区坐标**且随 DPI 变；写死坐标的脚本要注明"布局改了就得重调"（`test_editor_text.ps1` 就是）。
- 窗口枚举与前台化放在 C# 辅助类里（`MainWindow` / `ForceForeground`，AttachThreadInput + BringWindowToTop）比 PowerShell 的 `SetForegroundWindow` 可靠，F11 这类要求前台的操作才不会静默失败。
- 会碰用户设置/缓存的脚本必须在 `%TEMP%` 的**独立 exe 副本**上跑（设置文件写在 exe 旁边），别动用户的 `JarkViewer.db`。

### 调试手段速查

- **日志**：Release 下 `--log` 或 `JARKVIEWER_LOG=1` 写 `%TEMP%\JarkViewer.log`；新增诊断点直接写 `JARK_LOG(...)`，`isLogEnabled()` 为假时参数不求值（零开销，可以放心留在代码里）。
- **启动慢**：`JARKVIEWER_STARTUP_TRACE=<文件>` 记录各阶段相对起点的毫秒数（解码派发 / 设备与 UI 就绪 / 首帧绘制），新增关键阶段顺手补一个 `startupTraceMark()`。
- **崩溃**：必崩的用 `tools/catch_crash.ps1`（cdb 附着后落 dump）；**时序竞态不能挂调试器**（attach 会把窗口关掉，实测 1/5 的频率一挂就基本不复现），改用 `JARKVIEWER_CRASH_DUMP=<文件>` 让进程自己落盘，再看 `cdb.exe -z <dump> -c ".ecxr;r;kb 16;q"`（`.ecxr` 必需，默认线程不是出事的那个）。想更快撞上就几路并行跑。
- **界面**：`tools/capture_window.ps1` 截图，配 `-Keys` 注入按键、`-DragHold` 看"拖动中"才有的画面（马赛克/裁剪框）。
- **媒体卡顿**：`--probe --playback-test` 把"卡不卡"变成交付率、时钟倍速与最大停滞。
- **翻页慢 / 内存占用**：按 `TAB` 或中键打开 EXIF 面板，顶部两行就是缓存状况（条数/字节/上限、解码次数、淘汰、预取跳过）。用户报"翻页慢"时先看它：`解码`次数远超翻过的张数＝缓存一直在被淘汰后重解码；`预取跳过`在涨＝当前图太大、按设计不预读下一张（那种图的等待是解码本身）。要细一点就开 `--log`，淘汰/放弃回收/跳过预取都会打行，`--probe --cache-test` 还能在无界面下量一遍预算逻辑。

### 环境与工具坑

- 控制台是 GBK，中文/韩文输出会乱码：看自检报告一律 `--out` 写文件再读（`python -I` 读文件），别用终端输出判断内容。
- 写 Python 脚本用 `-I`（隔离模式）；Windows 路径用 `os.environ['TEMP']` 拼接，别在 heredoc 里直接写 `C:\...`（反斜杠会被当转义）。
- **PowerShell 只用 7（`pwsh`）**：`tools/*.ps1` 与 `buildRelease.ps1` 开头都有一段"5.1 拒绝守卫"，跑在不支持的环境会打印安装/改用 pwsh 的提示再退出。守卫能跑起来的前提是脚本**带 UTF-8 BOM**——5.1 把无 BOM 的 `.ps1` 按 ANSI 解码，会在执行到守卫之前就抛语法错误（`tools/check_source_invariants.ps1` 就踩过：中文注释被解成乱码，报的是"表达式或语句中的 '.' 后缺少表达式"）。**新增或改写 `.ps1` 后确认首三字节是 `EF BB BF`**（用 `python -c "open(f,'rb').read(3)"` 或 `head -c 3 | xxd -p` 查），否则守卫形同虚设。另外：`[Parameter(Mandatory)]` 的脚本在 5.1 下不给参数会先报"缺少参数"，给了参数才会看到守卫提示。
- **别在 MSBuild 编译进行中改源文件，也别在编译时跑反向验证**：文件被编译进程占着，Edit 会 `EPERM` 失败；`verify_source_invariant_checks.ps1` 是"原地破坏 → 跑检查 → 按原字节还原"，还原那一步撞上占用会失败（脚本现在会重试并逐个还原、失败就报出来，但最省事的做法就是等编译结束再跑）。
- **写"等构建结束"的脚本要盯构建进程本身，别盯 `MSBuild.exe` 进程数**：MSBuild 的节点复用工作进程（`/nodemode:1 /nodeReuse:true`）空闲时会长期驻留（十几分钟甚至更久），拿"MSBuild 进程数归零"当停止条件会**永远等下去**（实测卡了 20 多分钟）。用 `Start-Process ... -PassThru` 拿进程对象、循环里看 `$p.HasExited`；要顺手清干净可以给 MSBuild 传 `/nodeReuse:false`。
- 语料：实机大语料在 `D:\Downloads\test`（**只读**，别往里写）；自己造的语料放 `%TEMP%`。文件名带空格时给程序传参要用 null 分隔的写法。

## 构建前提

- 项目文件是 `JarkViewer/JarkViewer.vcxproj`，工具集为 `v145`，语言标准为 C++23，目标平台为 x64；需要安装支持 v145 工具集的 Visual Studio/Build Tools。
- **工程文件列表与筛选器目录树是生成出来的**：`tools/gen_vs_project_files.py` 按磁盘上的文件重排
  `ClInclude/ClCompile/ResourceCompile/Image` 条目并重写 `.vcxproj.filters`（筛选器 GUID 用名字派生、
  稳定可重复）。自有代码按模块挂在 `App / Ui / Image / Media / Metadata / Core` 下，第三方挂在
  `ThirdParty\<库>\…`（保留库内子目录），资源在 `Resources`。**第三方只列头文件**（`.lib` 是预编译好的，
  源码不参与本工程编译；`imgui` 例外，它是参与编译的 vendored 源码）；平台相关头文件
  （CUDA/DRM/VAAPI/va_intel/Vulkan/OpenCL/Android JNI/mediacodec/videotoolbox/vdpau/qsv，脚本里的
  `EXCLUDED`）不进工程，免得 IntelliSense 满屏解析错误——它们仍可由包含路径使用。
  `tools/check_source_invariants.ps1` 的第 11 项检查盯着这件事（两个工程都查：自有 `src/*.cpp`、
  `include/*.h` 全部在列、工程条目都指向存在的文件、`.vcxproj` 与 `.filters` 一一对应），
  反向验证在 `tools/verify_source_invariant_checks.ps1` 里。加文件后忘了跑生成器时，跑一次即修正。
- **并行编译**：`buildRelease.ps1` 传的 `/m` 只让 MSBuild 在**工程之间**并行（本解决方案只有看图 + 缩略图处理器两个工程，看图独占大头），**一个工程内部的 `.cpp` 是串行编译的**——文件级并行要编译器开关 **`/MP`**（工程属性里的"多处理器编译"）。两个工程的 Debug/Release 都设了 `<MultiProcessorCompilation>true</MultiProcessorCompilation>`，**别删**：掉了不会有任何报错，只是全量重编退回单核（本机 Ryzen 5 5600X / 6C12T 实测：8 个源文件关着 `max cl=1`、36s，开着 `max cl=9`、14s；全量重编开着 38s）。`tools/check_source_invariants.ps1` 第 13 项盯着它。并发实例数默认按逻辑核数（本机 12），每个实例都要解析 OpenCV/FFmpeg 的头，内存吃紧时可以在 `AdditionalOptions` 里补 `/MP6` 限一下（`MultiProcessorCompilation` 只开关、不带数目）。**做对照实验时注意**：命令行 `/p:MultiProcessorCompilation=false` 覆盖不了这个开关（工程里显式写的条目元数据优先），想验证"关掉会怎样"只能临时改 `.vcxproj`、量完再改回来——不然两次跑的都是开着的样子，并发序列一模一样，很容易得出"这开关没用"的错误结论。
- `JarkViewer.vcxproj` 中 `VcpkgEnabled=false`，默认使用仓库内的静态库目录：`JarkViewer/lib*`（含 `libffmpeg`、`libopencv`、`libjxl`、`libavif`、`libexiv2`、`libwebp2`）、`JarkViewer/include`。
- **第三方静态库从哪来**：仓库不自带 `.lib`（体积大、不进 git），开发前需从
  [releases/tag/static_lib](https://github.com/jark006/JarkViewer/releases/tag/static_lib) 下载对应版本的静态库包，
  按说明解压到 `JarkViewer/lib*` 与 `JarkViewer/include`。除 `OpenCV` 外都来自 vcpkg 的
  `x64-windows-static`（项目用 `Visual Studio 2026` 开发）。自己装一套的话（装了之后在项目属性里启用 vcpkg）：
  ```sh
  vcpkg install --triplet x64-windows-static ^
      x265 zlib libyuv minizip[core,bzip2] ^
      exiv2[core,bmff,png,xmp] libavif[core,aom,dav1d] libjxl libheif[core,hevc] ^
      libraw[core,dng-lossy,openmp] lunasvg directxtex ^
      "ffmpeg[all,amf,aom,ass,avcodec,avdevice,avfilter,avformat,bzip2,dav1d,fontconfig,freetype,fribidi,iconv,ilbc,lzma,modplug,mp3lame,nvcodec,opencl,opengl,openh264,openjpeg,openmpt,opus,qsv,sdl2,snappy,soxr,speex,srt,ssh,swresample,swscale,theora,vorbis,vpx,vulkan,webp,xml2,zlib]"
  ```
  `ffmpeg` 必须带这串特性：默认特性会少掉 H.264/HEVC/VP8/VP9/AV1/Opus/Vorbis 一大半编解码器，
  实况照片与手机视频就播不动了。**经典模式下 `vcpkg install` 不会升级已装过的包**（同名旧版本直接报
  "already installed" 跳过），升级要显式跑 `vcpkg upgrade --no-dry-run --triplet x64-windows-static`。
- ⚠️ **libheif / libde265 必须用修复版本**：`lib/heif.lib` 与 `lib/libde265.lib` 需来自 **libheif ≥ 1.22.0**、
  **libde265 ≥ 1.0.17**（当前 1.23.5 / 1.1.3）。旧版本带 **CVE-2026-32741**（libheif 解掩码图时按 `iloc`
  长度直接 `memcpy` 到按图像尺寸分配的缓冲区，堆溢出）与 **CVE-2026-33165**（libde265 在 SPS 变更后越界写
  2 字节），一个恶意文件就能触发。**只换头文件没有意义**——有漏洞的是预编译的 `.lib`，而且会造成头/库不一致。
- 源码只需要最新提交（历史提交里有大量占空间的冗余文件）：`git clone git@github.com:jark006/JarkViewer.git --depth=50`。
  想快速理解模块实现，可以看项目的 [DeepWiki](https://deepwiki.com/jark006/JarkViewer) 与
  [Zread](https://zread.ai/jark006/JarkViewer)（AI 生成的结构化说明，两份 README 的顶部徽章也是这两个入口）。
- **OpenCV 不在 vcpkg 里，是源码自建**：`pwsh tools/build-opencv.ps1 [-Version 4.14.0] -Install` 一步到位（拉源码 → 打 `tools/opencv-jarkviewer.patch` → CMake 配置 → 编译 → 安装），`-Install` 再把产物复制进 `JarkViewer/libopencv/` 与 `JarkViewer/include/opencv2/`。脚本里的 CMake 参数是从 4.13.0 那版的构建树逐项对齐来的：world + opencv_contrib + nonfree + 自带 3rdparty（jpeg/png/tiff/webp/openjpeg/openexr/zlib）、`/MT` 静态 CRT、只出 Release；**IPP / IPP-IW / ITT 已关闭**（原本那版是开的）——IPP 的静态 blob 一项就占 exe 约 25 MiB，而 4.14 里 IPP 早就不覆盖看图主路径了（`resize`/`warpAffine`/`cvtColor`/`imdecode` 的源码里没有一处 `ippi*` 调用，`Mat::convertTo` 的 IPP 调用被上游注释成 `[TODO] Recover IPP calls`）。实测方式是把工程那份 `opencv_world` 单独编个基准、用 `OPENCV_IPP=disabled` 对拍 21 项操作（4000x3000、12 线程）：显示主路径全部无差异，`copyTo`/`moments`/`morphologyEx` 关掉反而快 1.8~2.4 倍，只有 `bilateralFilter` 与 `Sobel` 是 IPP 真快（本工程一次都没调用）。判断「要不要开回来」先跑这个对拍，别凭印象；**关掉** dnn/objdetect/datasets（连带 aruco/face/text/wechat_qrcode 不参与编译）、CUDA、OpenGL、Vulkan、GDAL、GDCM、Jasper、AVIF、JPEG XL、OpenCV 自带的 FFmpeg/GStreamer（工程用的是自己那份 FFmpeg）。改这些参数会让产出与发布出去的 `static_lib` 包对不上，升级版本时只改 `-Version`。补丁内容：① `modules/imgcodecs/src/loadsave.cpp` 去掉图像分辨率限制（宽/高 `1<<20`、总像素 `1<<30` 三个硬上限）；② `modules/highgui/src/window_w32.cpp` 的窗口光标 `IDC_CROSS` → `IDC_ARROW`。两处代码在 4.13/4.14 里完全一致、行号相同。升级到 4.14 时新 OpenCL 头多出 `HAVE_OPENCL_D3D11_NV`，是上游版本差异，不影响本工程用到的功能（imgcodecs/imgproc/core）。
- `JarkViewer/libopencv/zlib.lib` 已换成 **zlib-ng 的 compat 构建**（大 PNG 解压约快 25%）。compat 模式不改符号名，OpenCV 是最终链接时才解析 inflate，所以是原地替换、不用重建 OpenCV；但 **include 下的 `zlib.h`/`zconf.h`/`zlib_name_mangling.h` 必须与 .lib 是同一来源**。换机器或重建静态库环境时跑一次 `pwsh tools/build-zlib-ng.ps1 -Install`（原件备份在同目录 `zlib-1.3.1.lib`；头文件回退用 git checkout）——**必须在 `build-opencv.ps1 -Install` 之后跑**，否则会被 OpenCV 自带的那份 zlib 覆盖回去。
- **升级静态库时会变的链接清单**（`ImageDatabase.h` 的 `#pragma comment(lib, …)` 与
  `JarkThumbnailProvider.vcxproj` 的 `AdditionalDependencies`）：升级到 ffmpeg 9.0.2 那一轮踩到的坑——
  QSV 改走 oneVPL 调度器，`MFXLoad`/`MFXCreateSession` 在 **`vpl.lib`** 里，且**不能同时链旧的 `libmfx.lib`**
  （两者都带 `mfx_function_table.cpp.obj`，会撞 LNK2005）；libssh 改用 Windows CNG，要补 **`ncrypt.lib`**
  （不再需要 OpenSSL）；ffmpeg 9 新拉进 **`SvtAv1Enc.lib`** 与 **`twolame.lib`**，而 `SvtAv1Enc` 又引用
  **`fastfeat`** 的 `fast9_detect_nonmax`；libheif 1.23.5 带 brotli 压缩，缩略图工程也要补 `brotlienc.lib`。
  abseil 20260107 合并掉了 6 个目标（`absl_string_view`/`absl_low_level_hash`/`absl_bad_any_cast_impl`/
  `absl_bad_optional_access`/`absl_bad_variant_access`/`absl_random_internal_pool_urbg`），对应 pragma 已删；
  minizip 1.3.2 的静态库改叫 **`minizips.lib`**（两个工程都已改）。另外工程里那批只有 pragma、源码零引用的
  库（`FreeImage`/`FreeImagePlus`/`pixman-1`/`freeglut`/`yasm` 与孤儿文件 `thorvg-0.lib`）已清掉，
  验证方式是删掉后仍能链接。
- **FFmpeg 静态库是「只解不编」的构建**：在 vcpkg 那套配置上只多一条 `--disable-encoders`，解码器（531）、解复用器（363）、解析器、外挂库一个没少，exe 里省下约 9.7 MiB。**`opus` 与 `adpcm_g722` 这两个编码器必须保留**——它们的 x86 asm 对象在 FFmpeg 源码里挂在 `CONFIG_*_ENCODER` 下，而解码路径用的是同一份：`opus/pvq.c` 调 `ff_celt_pvq_init_x86`（`x86/celt_pvq_init.o`）、`g722dsp.c` 调 `ff_g722dsp_init_x86`（`x86/g722dsp*.o`），全关掉会让 Opus / G.722 解码器各差几十个符号链接不上。重建时的环境与坑（都是实测踩出来的）：
  - 工具链取 vcpkg 自带的：MSYS2 的 bash+make 在 `D:\vcpkg\downloads\tools\msys2\<hash>\usr\bin`，nasm 在 `D:\vcpkg\downloads\tools\nasm\*`，`ar-lib` 在 `D:\vcpkg\installed\x64-windows\share\vcpkg-make\wrappers`（不放进 PATH 会报 `ar-lib: command not found`），pkg-config 用 `<msys2 根>\mingw64\bin\pkg-config.exe` 并设 `PKG_CONFIG_PATH=<vcpkg 的 installed 目录>\lib\pkgconfig`。
  - MSVC 的 bin 必须排在 `/usr/bin` **前面**：msys2 的 coreutils 里也有个 `link.exe`，会被先找到，然后 configure 的链接测试全挂。
  - **不要设 `MSYS2_ARG_CONV_EXCL=*`**：它关掉 msys2 的路径转换，`cl.exe` 拿到 `/d/vcpkg/.../x.c` 会把开头当成 `/D` 选项，报 D8043 未知选项。
  - configure 的路径参数要写成 `D:/...` 而不是 `/d/...`——configure 从 `$0` 推出 `SRC_PATH`，写成 POSIX 形式会把 `/d/...` 塞进 `config.mak`。
  - 改过 configure 之后必须 `make clean` 再 `make`：残留的旧 `.o` 里还引用着 `ff_*_encoder`，直接链会得到上百个 LNK2001。
  - 验证口径：`make` 完看 `config_components.h`，`CONFIG_.*_ENCODER` 应当只剩 `OPUS` / `ADPCM_G722`，`CONFIG_.*_DECODER` 仍是 531、`CONFIG_.*_DEMUXER` 仍是 363；再跑一遍 `--probe --playback-test D:\Downloads\test\livp`（126 项）确认实况视频与声音没受影响。
- Release 输出程序位于 `x64/Release/JarkViewer.exe`，中间文件位于 `JarkViewer/x64/<Configuration>`。

## 高层架构

- `JarkViewer/src/main.cpp` 定义 `JarkViewerApp` 和 `wWinMain`。入口初始化 Exiv2 BMFF、禁用 IME、初始化 COM，然后创建窗口、解析命令行图片路径并进入主循环。
- `JarkViewer/include/D3D11App.h` 与 `JarkViewer/src/D3D11App.cpp` 提供 Win32 窗口、消息分发、Direct3D 11 设备/交换链和 `PresentCanvas()`。业务层通过继承并实现鼠标、键盘、拖放、右键菜单和绘制回调；窗口句柄刚创建、D3D 设备尚未建时另有一次 `OnWindowCreated()` 回调——主窗口在这里就把首图解码派发出去（只派发不等待），和随后几十毫秒的设备/交换链创建并行。等待与收尾仍走 `initOpenFile`：它靠 `startupFilePrepared_` 标志跳过重扫（重扫里的 `imgDB.clear()` 会把在途解码作废），命令行 `--lang` 也因此要提前到 `InitWindow` 之前应用（窗口一就绪就会扫目录、放占位并派发解码）。
- `JarkViewer/include/ImageDatabase.h` 与 `JarkViewer/src/ImageDatabase.cpp` 负责图片加载、格式分派、EXIF 处理和图像缓存。核心路径是 `ImageDatabase::loader()` → `myLoader()` → **按文件头（魔数）嗅探格式后再分派**：`FormatSniffer` 判定真实格式 → `decodeByFormat()` 调用 JXL/WP2/AVIF/HEIF/RAW/SVG/PSD/OpenCV/WIC/FFmpeg 等解码器 → 统一转为 OpenCV `cv::Mat`；嗅探失败或解码失败时再用扩展名路由兜底，最后才是 OpenCV/WIC 通用兜底。EXIF 后处理统一由 `applyExifInfo()` 按 `ExifPolicy`（None/SimpleOnly/Full/FullWithOrientation）完成，不再散落在各格式分支里。
- `JarkViewer/include/ImageAssetCache.h` 是图像缓存（`ImageDatabase` 的基类；键 = 文件路径、值 = `ImageAsset`，带一个后台预读线程）。原先是通用模板 `LRU<keyType, valueType>`，但全项目只有看图这一处用，已按本项目**特化**（键/值定死，"占多少字节"直接问值自己，不再挂一层虚函数）。容量三个口径：
  - **条数上限 10 张**（`CAPACITY`，兜住"海量小图把索引撑爆"）与**字节上限 = 物理内存的 50%**（`ImageDatabase::defaultCacheBudgetBytes()`，下限 512MB，`GlobalMemoryStatusEx`）**先到先算**；
  - **条数下限 2 张**（`minEntries`）：翻页时"当前图 + 上一张"是最常见的一组，少到 1 张就退化成每翻一张都重解码；这是**硬下限**——预算连两张都装不下时也照样留住两条（宁可短暂超预算），别再把它"修"成"严格不超预算就只剩一张"；
  - **淘汰从最久未用那头开始，但跳过外部还持有引用的条目**（`use_count() > 1`）：踢了也不释放内存（shared_ptr 还在别处），只白丢一次命中；当前显示的图被 `curPar.imageAssetPtr` 持有，天然轮不到它。一圈都有人持有时直接停，不空转。
  为什么非要有字节这条：按条数留 4 张 43890x38875 的扫描件（每张解码后 6.36GB）就是 25GB，32GB 机器直接爆。**"这条占多少字节"问 `ImageAsset::memoryBytes()`**（定义在 `jarkUtils.cpp`）：同一块像素被多个成员共享时按 `data` 指针去重（实况/动图的 `primaryFrame` 与 `frames[0]` 是浅拷贝），lunasvg 的文档内存没法估、不计。条数上限里**主页占位图也占一格**，所以实际能留 9 张图。
  **配套两条**：① 当前图大到"两张装不进预算"时**不再预取邻图**（`JarkViewerApp::shouldPrefetchNeighbor()`，"当前图 × 2 > 预算"即跳过）——预算只管得住"可以不持有的"，当前图必须留，硬解下一张就是内存翻倍，而那种图的翻页本来就要重解码一两分钟；② 平滑重采样块的视图指纹里带 `CurImageParameter::sourceToken`（每次 `Init` 自增）：淘汰变积极之后会出现"旧图释放 → 新图落在同一地址 + 尺寸/画布/缩放/平移/旋转全相同"（同尺寸扫描件翻页时的缩放值就是同一个），只用位图裸指针撞键会复用上一张的重采样块。
  **看得见的运行状态**：`stats()` 给出 `entries/bytes/budget/capacity` 与四个计数（`decodes` 真解码了几次、`hits` 查中几次、`evictions` 被两个上限踢掉几条、`gaveUp` 因"剩下的都被外部持有"放弃回收几次）。看图窗口的 EXIF 面板顶部画两行（跟"色彩空间/质量"那两行同属"当前状态"，因此用面板既有的中/英双语而不是六语言表——那一块本来就只分中英）：`缓存: 10/10 张 · 3.18/16.0 GB`、`解码 12 · 淘汰 2 · 预取跳过 0`。**两条淘汰路径都要计数**：条数上限那条（`putInternal` 里）与字节预算那条（`trimLocked`）都得走 `eraseLocked()`，漏掉前者会出现"界面上淘汰 0、实际每翻一张都在踢"（第一批就是这么漏的）。预取跳过则记在 `JarkViewerApp::prefetchSkips_`（判定是纯函数 `ImageAssetCache::shouldPrefetchNeighbor()`，图省得测不到）。开 `--log` 时淘汰/放弃回收/跳过预取各打一行。
  自检：`--probe --cache-test`（33 项合成断言，含预算/条数/计数/预取边界；去掉字节回收会报 6 项 FAIL）+ `tools/test_cache_budget.ps1`（实机翻页看工作集曲线，实测 12 张 53421x1600（各 326MB）在第 9 张后收敛在 3405MB，面板上显示 `缓存 10/10 张 · 解码 12 · 淘汰 2`）。README（中/英）常见问题各加一条"内存会不会一直涨"，口径与这里保持一致。
- `JarkViewer/include/FormatSniffer.h` 与 `JarkViewer/src/FormatSniffer.cpp` 是纯文件头嗅探模块（不依赖任何第三方库）：扩展名与文件头冲突时以文件头为准，但 RAW/视频/LIVP/LEP/TGA 等扩展名携带文件头无法表达的信息（`isExtensionAuthoritative()`）时优先按扩展名路由。`JarkThumbnailProvider` 里的同名模块与其同源，后续计划合并为两个工程共用的模块。
- 界面改动可用 `tools/capture_window.ps1` 做视觉验证：`-Keys "{F1}"` 注入按键（窗口都在主窗口内，不再需要 `-Window` 选择）、`-Keys2 "{ESC}i"` + `-Keys2DelayMs` 送第二批按键（开窗、点击、再按键这类时序）、`-Hover "x,y"` 悬停、`-Drag "x1,y1,x2,y2[;...]"` 拖动或点击（物理客户区坐标）、`-RightClick "x,y"` + `-MenuKeys "b{ENTER}"` 右键菜单（菜单是独立弹窗，要配 `-Screen` 才截得到）、`-DragHold` 拖到最后一个点**不松手**再截图（验证"拖动中"才有的画面，如马赛克/裁剪的拖框）。ImGui 的窗口默认居中于主窗口。编辑窗口的文字工具另有 `tools/test_editor_text.ps1`：选文字工具 → 点锚点 → 点侧栏文字框 → 投递 Unicode `WM_CHAR`（等价于输入法上屏后的字符），一张图同时验证锚点光标、文字预览和中文能进输入框。主界面导航另有 `tools/test_navigation.ps1`：动态按 DPI 换算客户区坐标，依次验证鸟瞰拖动、拖出客户区释放、悬停展开缩略图带、点击直接换图、滚轮只滚条带不穿透、移出后隐藏、设置窗口往返；加 `-CheckSettings` 时还会点「清理缓存」「显示鸟瞰图」并重启核对设置文件字节（该分支要求 `-Exe` 指向 `%TEMP%` 下的独立副本，避免动到用户的设置与缓存）。
- 视频相关改动除 `--probe` 外，可用 `--probe --audio-test <文件>` 验证音频链路：它以音量 0 提交音频并观察播放时钟是否按采样率推进（不发出声音）。
- **播放流畅度自检 `--probe --playback-test <文件...>`**：音量 0 起一个真实 `MediaPlayer`，像主循环那样定时取帧，把"卡不卡"变成数字——交付率、交付间隔中位/最大、**播放时钟倍速与最大停滞**、总用时（对照媒体时长）。关键不变式是**时钟按 1 倍速推进**：音频时钟一停，取帧判定就不再满足，画面跟着停——"画面卡 + 声音一卡一卡"是同一个故障的两个表现。卡顿这类问题靠眼睛看不出是解码、队列还是时钟，改动 `MediaPlayer`/`AudioOutput` 后拿它跑一遍实况照片与视频（`D:\Downloads\test\livp` 整个目录是一个好语料）。

- **独立播放器自检 `--probe --video-test <文件...>`**：把界面上只能靠眼睛看的语义变成断言——暂停后时钟是否真停、暂停中精确 seek 的落点偏差、单帧前进/后退、**播完停在最后一帧**、结尾按播放从头重播、**seek 之后音频从目标附近出声**（落点断言 + "落点在目标之前时也要拉回目标"两条，见「简易播放器」一节）；以及 `VideoPlayback` 那份拖动状态机的三条不变式（拖动中时钟停在把手位置、**一下只交一帧关键帧预览**、松手精确落位后才恢复播放）。改动 `MediaPlayer`/`MediaDecoder`/`VideoPlayback` 后跑它，再跑 `--playback-test` 确认实况照片那条老链路没回归。**纯音频文件也喂给它**（mp3/flac/wav…）：那时没有视频轨，它改跑一套无帧断言（时钟 1 倍速、暂停真的停、精确 seek 的 `positionMs()` 落点、播完 `hasFinished()`、`seek(0)` 重播，以及 `VideoPlayback` 在纯音频上的开/拖/恢复），视频那几条帧断言自动跳过。它曾经有约 1/5 的间歇性崩溃，已定位为**音频提交缓冲的 use-after-free**并修掉（是 `AudioOutput` 的缓冲区释放时机，见「简易播放器」一节那条硬约束 ③）。这类"要反复开关句柄、几路一起忙才撞上"的竞态**不能挂调试器抓**（会掩盖），改用 `JARKVIEWER_CRASH_DUMP` 让进程自己落盘——用法与看现场的办法见下面「修改注意事项」里那条。
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
  `jark::ui::drawWrappedText()`（同文件）是**折行文本块**的唯一实现：测量/绘制/配合滚动的裁剪，
  看图窗口的 EXIF 面板与播放器的媒体信息面板共用；坐标是客户区坐标，`origin` 传主视口左上角
  （多视口模式下画在前景列表上要换算）。
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
  深浅主题通用，不需要两套图。常规页最上面的勾选项按 **2 列**排布（现在 9 项，末行只有左列一项。
  ImGui 没有等宽列：第二列起点 = 第一列（偶数下标项）最长文本的实际文本宽 + 勾选框宽 + 列间距，
  随语言自适应；增减勾选项时记得同步这个列宽推导）。「到最后一张时停住」「全屏时使用纯黑背景」
  都在这组里。
  **右键不再有"退出程序"这个可选项**（右键 = 只弹右键菜单）：设置页那行单选已删除，
  `SettingParameter` 里的旧字段原位改名成 `legacyRightClickAction`（删掉会改变结构体布局、
  把旧设置文件整体错读）。`ShowContextMenu(hwnd, x, y)` 收的是**客户区坐标**、内部自己
  `ClientToScreen`，所以 `WM_RBUTTONUP` 里那下 `PostMessageW(WM_CONTEXTMENU, MAKELPARAM(x, y))`
  **不要**再自己转屏幕坐标——转一次就成了双重换算，菜单整体偏出窗口偏移那么多。
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
  加菜单项注意加速键不要与既有项重复（`&C` 已被「复制图像数据」占用，「导出视频」用 `&V`），
  并且**宽表的 ID 与窄表是两套**：菜单文案走 `getUIStringW`，新加的行插在宽表表尾、按宽表当前
  行数编号（往窄表末尾加一行、却拿窄表编号去查宽表，会得到一个空字符串——菜单项在界面上变成
  一行看不出字的空白，不报错也不崩）。
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
  **鸟瞰面板同款半透明**（`background` 与 `stripBackground` 是同一个值；面板内边距会透出后面的
  图像，白图后面量到 (66,67,71)、黑图后面 (24,25,29)）。它**不加入
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
- **退出必须真的销毁窗口**（`D3D11App::OnDestroy()` 里补的 `DestroyWindow`）：退出走的是
  `PostMessageW(WM_DESTROY)`，那只调用到 `OnDestroy`，**窗口本身还活着**；对象随后就被析构，
  于是下一条鼠标/DPI/输入法消息打在这个窗口上就会调到已析构对象的纯虚函数上。实测是
  `_purecall` → `abort()`，退出码 `0xC0000409`（`HeapEnableTerminationOnCorruption` 之下表现为
  "安全 cookie/堆损坏"），而且**时间点取决于下一条消息什么时候来**（2~5 秒不等，看着像随机崩溃）。
  进程直接退出时看不出来；一旦首尾相接（看图窗口 ↔ 播放器窗口交换，见「简易播放器」一节）
  就必然撞上——**新加"同一个进程里换窗口"的玩法前，先确认这条**。`OnDestroy` 用 `m_destroying`
  保证只生效一次（`DestroyWindow` 会同步送回一条 WM_DESTROY），真正的 WM_DESTROY 走进去时
  `DestroyWindow` 会直接失败返回、不会递归。
  **配套的是 `UiHost` 每个窗口都要整套重建**：`UiHost::init()` 不能再"已初始化就提前返回"，
  必须先 `shutdown()` 再接管新的窗口/设备/交换链（`shutdown()` 也会把 `hwnd_/device_/context_/
  swapChain_/imeContext_` 置空）；而且**不能拿 hwnd/设备指针比较"是不是同一个窗口"**——句柄会被
  系统回收复用，看着一样其实已经是新窗口。顺序也有讲究：`OnDestroy()` 要在**窗口还活着时**先
  `UiHost::shutdown()`（后端 Shutdown 会把窗口过程换回去，句柄已被回收的话就改坏了新窗口的
  窗口过程），再 `DestroyWindow`。漏了这一步的症状很隐蔽：**画面由应用自己画的部分（视频帧）
  一切正常、自己的鼠标命中也照常工作，只有 ImGui 画的东西（浮层、音量提示、条带）永远不出现，
  输入法/键盘也失灵**——因为 ImGui 还绑在那个已经没了的窗口与已释放的交换链上，画到了空处。
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
  进来之前本来就全屏的话不还原），按 ESC 停止播放。**播放期间画面上只留图片本身**：
  `hidesOverlayUi()`（就是 `slideshowActive`）让鸟瞰图/底部预览带/EXIF 面板/悬停按钮/动图播放条/
  实况角标/**「加载中」浮标**全部不画，对应的鼠标命中（`NavigationOverlay` 走 `sync(blocked=true)`、
  面板滚轮、角标悬停重播）也一起停，免得"看不见的控件"还在吃鼠标；左右边缘点击换图仍然有效。
  加浮标那条是后补的：预读只预取"列表里的下一张"（`switchToFile` 的 `nextIndex`），
  幻灯片设成**随机**时下一张是随机挑的、预读永远取错图 → 每次切换都现场解码 → `pendingLoad_`
  为真 → 浮标闪一下（"加载中 0.0s"）。顺序播放时预读命中，一般不闪。
  ESC 停止后原样恢复（不动任何设置项）。`toggleSlideshow()` 里要补 `markPresentRequested()`：
  本来就在全屏时不会走 `WM_SIZE`，画面稳定分支也不会自己出帧，少了它浮层要等下一次换图才消失/回来。
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
- `JarkViewer/include/Localization.h` 与 `src/Localization.cpp` 管界面语言（简体中文/繁體中文/English/日本語/한국어/Русский）：`UIStringTable[stringID][语言]` 与 `UIStringTableWide[stringID][语言]` 两张表（前者供画布文字、后者供 Win32 API；同 ID 文案不同是历史遗留，新增文案请追加到表尾）。`getUIString()` 按当前语言取用并在缺失时回退到英文、简体中文；`getUIStringW()` 由 UTF-8 转换而来。帮助/关于页与主页/解码失败画面都改为按语言的文字排版（前两者是 ImGui，后者由 `InfoScreen` 画到画布），不再有按语言分套的资源图；`prefersChineseResources()` 现在只决定 **EXIF 标签文案**用中文还是英文（简繁→中文，其余→英文）。命令行 `--lang 0..5` 可临时指定语言，**但要在设置文件读盘之后再盖**：`loadSettings()` 是整体赋值（`settingParameter = tmp`），窗口对象构造时会把窗口创建之前设好的分量冲掉——命令行参数走 `GlobalVar::pendingLanguageOverride`，读完盘应用、退出写盘时换回文件里的原值（临时语言不落盘）；回归 `tools/test_language_override.ps1`（撤掉那几行就会报 FAIL）。新增语言时：`Language` 枚举、两张表每一行的新列（顺序对齐、数量断言）、`languageFromSystem()`、`languageDisplayName()`、设置页语言单选项，缺一不可。
- 图像内文字渲染（标注文字等）在 `JarkViewer/include/TextRenderer.h` 与 `src/TextRenderer.cpp`：
  用 stb_truetype 按**真实字形度量**（进退宽度/字距/bearing）绘制 UTF-8 文本，支持多行与按宽度折行，
  字形位图按 (字号, 码位) 缓存。字体全部取系统字体（微软雅黑/等线/黑体/宋体…），工程不再内嵌 ttf。
  界面文字由 ImGui 负责，不要再往 TextRenderer 里加界面相关职责。字体文件数据按路径**进程内共享**
  （`sharedFontData()`）：多个 TextRenderer 实例（EXIF 面板、InfoScreen）不会各读一份几十 MB 的 ttf，
  各实例只保留自己的 stbtt_fontinfo 与字形缓存；它仍是**界面线程专用**。
- `JarkViewer/include/MediaDecoder.h` / `MediaPlayer.h` / `AudioOutput.h` / `AudioSpectrumAnalyzer.h`（对应 `src/*.cpp`）组成媒体播放链路（最后一个只服务于纯音频画面上的实时频谱）：`MediaDecoder` 在内存数据上做解复用+解码，按出现顺序产出视频帧或音频批（音频统一重采样为 48kHz 立体声 16 位）；`AudioOutput` 用 XAudio2 输出并提供已播放样本数作为主时钟；`MediaPlayer` 以音频时钟驱动视频帧、一次播完。
  **视频与音频必须各占一个解码器实例 + 一个线程**（`MediaPlayer` 里因此有两个 `MediaDecoder`，`MediaDecoder::StreamFilter` 让每个实例只解自己那一路，另一路只解复用不解码）。起因是一个会自锁的恶性循环：视频队列只有 6 帧（一帧几 MB，必须有上限）要靠"播放端取帧"背压，音频却必须永远跑在播放位置之前。两条路抢同一个线程时，视频队列一满，整个循环就卡在等取帧上，音频提交被迫一起停下——视频队满 → 音频提交掉到 1 倍速以下 → 声卡饿死、播放时钟停住 → 播放端按时钟判"还没到点"不再取帧 → 队列永远满，卡顿就此锁死（实测 3 秒的视频播了 9 秒、中间冻 1.15 秒）。拆开后视频线程只受队列上限约束、音频线程只受声卡队列约束，互不牵连。**别再把它们合回一个线程**，那怕只是为了"省一个解码器"。
  **音频提交有三个硬约束**：① XAudio2 对单个 source voice 最多排 64 个缓冲区（`XAUDIO2_MAX_QUEUED_BUFFERS`），超了 `SubmitSourceBuffer` 直接失败——按样本数限流拦不住（21ms 的批 × 64 就到顶），必须按**缓冲区个数**限流（`AudioOutput::kMaxQueuedBuffers`），并且**提交失败要重试、不能丢**（丢一批就是声音里一个 20ms 空洞 = "一卡一卡"）；② 音轨比视频短时（实况照片常见）要**补静音到媒体总时长**，否则音频队列一空时钟就停，视频最后几帧永远等不到"到点"、`hasFinished()` 永远为假（画面停在末尾、实况照片回不到静态图、播放状态卡死）；③ **提交出去的那块内存只有 `OnBufferEnd` 到过（或引擎整个销毁）才能释放**——XAudio2 是照着 `pAudioData` 直接取样的，不拷贝。`Stop(0)` + `FlushSourceBuffers()` **不能**当作"用完了"：正在播的那一块不会被冲掉，它的数据此刻仍在被读，这时释放就崩在音频线程里（`ucrtbase!memcpy ← CSWVoice::Process`、`MatrixMixFromInt16DiagonalAvx`）。所以 `flush()/stop()` 只把缓冲区挪进 `retiredBuffers`（不再计入队列长度，但内存留着），`releaseAllBuffers()` 放在 `IXAudio2::Release()` 之后当兜底。seek 会走 flush，所以"带音轨 + 有 seek"才会撞上，`--playback-test` 从不 seek 就一次都不复现（这处 2026-10-10 修过一次，回归口径：`--video-test` 带音轨语料 4 路并行 48 次零崩溃、无音轨语料 40 次零崩溃）。
  **视频尺寸分两种，别用错**：`MediaInfo::width/height` 是**编码尺寸**（容器/码流里的原始尺寸，未旋转），`displayWidth()/displayHeight()` 是**帧实际交出去时的尺寸**（按 `rotationDegrees` 换过轴）。解码器会按 display matrix 把帧旋转（手机竖拍视频的编码尺寸是横的），所以**给播放端的尺寸必须用 display\***——`MediaPlayer::getVideoSize()` 就是栽在这里：它原先返回编码尺寸，实况照片播放时主窗口拿它当名义尺寸（`applyViewForSize`），竖帧被塞进横框里拉伸。另外帧的长边超过 `kMaxVideoEdge` 时会**降采样**（4K 实况视频只出 1920×1080 的帧），所以"帧尺寸 == 名义尺寸"**不是**不变式，**宽高比一致**才是；`--probe --full` 对每个视频/实况照片都会打印 `显示 WxH (编码 WxH) rot=R` 与 `首帧 WxH … 宽高比一致`，不一致时会显式报 `!! 宽高比不一致`。实况照片（livp / MotionPhoto）与视频文件都走这条路：静态图/首帧作 `ImageAsset::primaryFrame`，视频字节放在 `ImageAsset::videoSource`，由主窗口自动播放一次后回到静态图（不再预解码成帧序列，避免上百 MB 内存）。空格键在实况照片上是播放开关：播放中切回静态图（并把 `playedAsset` 标记为已播放，防自动续播）、静态图时从头播放（主动操作，出声）。
- **简易播放器**（`VideoPlayerApp` + `VideoPlayback` + `MappedFileReader`）是**独立于看图的一个顶层窗口**，视频与音频共用它（音频没有视频帧，界面改画音频画面）：`wWinMain` 按入参在「看图窗口」与它之间**二选一构造**，另一个对象根本不构造，所以播放器里不可能出现"按 P 进幻灯片""按 Q 旋转图片"这类串味；它不经过 `ImageDatabase`/缩略图/导航浮层/EXIF/设置窗口，只复用 `CanvasRenderer` 画帧、`UiHost` 画条带。
  - **判定与入口**：**`jark::isPlayerFile()`（= `isVideoFile() || isAudioFile()`，`FormatSniffer` 的扩展名表）是唯一判定**，命令行/拖放/Ctrl+O 三处都走它——视频归视频端、音频也归播放器，**各写一半就会同一个文件在不同入口下进不同窗口**。`webm` 归视频端；视频与音频都**不在** `supportExt` 里（否则看图列表里会出现"点开却跳到播放器"的项，规则就不唯一了）。`videoExt`（`ImageDatabase`）与视频那张表必须一致、README 的「视频」「音频」两行也必须与表一致，`tools/check_source_invariants.ps1` 第 6/6b 项盯着（音频是两处：表 == README，且音频扩展名不得混进 `supportExt`/`videoExt`）；中文那份 README 由 4/5/6/6b 查，英文那份的四张清单由 6c 查（英文 README 曾落后中文一整轮——加了播放器却没同步）。运行中"按内容换窗口"由 `wWinMain` 的接力循环实现：当前窗口退出时留下 `takeHandoffPath()`，循环据此构造另一个窗口。ESC / Ctrl+W 退出程序（单向门，没有"切回看图"的路）。
  - **音频（没有视频轨）这条路**：`MediaPlayer::start()` 允许"只有音频"——没有视频轨就不建视频线程、`videoDecodeFinished` 直接为真，时长/单帧时长取现有那一路的 `info()`（音频的 `frameRate` 是 0，单帧时长保持默认 33ms，只用来算 seek 上界与时钟余量）。凡是"有没有媒体"的守卫都用 `videoDecoder || audioDecoder`，**不能只看视频那一侧**：`stop()` 照旧写法会直接返回、音频线程继续跑（换片/析构时声音停不下来），`pause()/resume()/seek()` 同理。`hasFinished()` 对纯音频用 `mediaDurationMs` 当结尾（音频线程按 `silenceUntilMs()` 把静音补到时长，队列排空时时钟正好压在结尾），`seek()` 的上界也不再留一帧余量。`applyAudioSeek()` 在**没有声卡**时要补 `rebaseClockForSystemClock()`——纯音频没有视频线程兜底，不补这一下 seek 之后时钟还按墙上时间从老位置跑（"只有一个时钟写者"这条不变式照旧：有视频线程时仍由视频侧写）。`VideoPlayback::hasVideo()` 为假时：`stepFrame()` 退化成 ±`kNudgeMs`（暂停态 A/← 与 D/→ 在音频上仍有意义，界面不用为它另写分支），`endScrub()` 不做"等落点帧"（音频没有落点帧可言，白等 4 秒才恢复播放）。
  - **媒体信息面板**（`I` / `Tab` 切换，与看图窗口的 EXIF 面板同一个键）：样式照 EXIF 面板——左侧四分之一宽的圆角面板（`BG_DEEP × 0.82`）、文本按宽度折行、滚轮只滚面板内容不穿透、贴右缘画滚动条。折行与滚动那套算法**只有一份**：`jark::ui::drawWrappedText(drawList, origin, …)`（`UiHost.cpp`，看图窗口的那份本地实现已改成委托调用），只有排版差异（播放器面板底部给条带留出高度——条带是唯一的操作入口，压住文字就没法边看信息边拖进度；看图那份是整幅高度）。内容由 `jark::mediaInfoText(path, info)`（`MediaDecoder.cpp`，纯函数、`--probe` 拿它做断言）产出：路径/大小/时长/格式/总码率 + `视频: h264 1280x720 30.00fps`、`音频: aac 48000Hz 2ch 126 kb/s`（流参数用 FFmpeg 短名与单位，语言无关，不占多语言表；标签复用 39 路径 / 40 大小 / 164 视频 / 190 音频 + 191 时长 / 192 格式 / 193 总码率）。`MediaInfo` 为此补了 `formatName/videoCodec/audioCodec/bitRate/audioBitRate`，在 `MediaDecoder::open()` 里填；播放器拿的是**一次性 Both 解码器**读出来的信息（正在播的两个实例各只解一路）。
  - **音频画面**：`PlaceholderKind::Audio` + `InfoScreen::renderAudio()` 画**静态底板**（主题标签底色的圆角面板 + 标题「音频」+ 文件名 + 路径），面板里那列**跟着声音实时跳的频谱**由 `VideoPlayerApp::drawAudioSpectrum()` 每帧叠上去（`jark::audioSpectrumRect()` 与 renderAudio 共用同一套布局；底板仍是带指纹的缓存位图，只有条是逐帧画的——整幅 InfoScreen 含文字排版每帧重画没有必要）。
  - **频谱分析**在 `JarkViewer/include|src/AudioSpectrumAnalyzer.{h,cpp}`（Media 模块，纯计算）：解码线程喂已解码的 PCM（与提交给声卡的是同一批样本，所以与音量无关、也不依赖声卡回环），下混单声道 → 一阶高通去直流 → 10.7ms 一跳的加窗 FFT（1024 窗、Hann）→ 聚合成 28 段对数频段（40Hz~16kHz，段宽约 1.24 倍）→ 快起慢落（每帧衰减 0.045，满格约 0.24 秒落完）→ 电平按 -72dB~-6dB 映射到 0~1（**不做 AGC**：AGC 会让静音也满格跳，而且自检没法钉住数值）。两条硬约束：
    - **每一帧必须带媒体时间戳**：音频线程永远跑在播放位置前面（队列提前约 2 秒），画"最新算出来的那一帧"会让频谱比听到的声音早出一两秒（音乐上很明显）。所以帧按 ptsMs 进 512 帧的环形缓冲，界面用 `MediaPlayer::positionMs()` 去取"pts ≤ 播放位置里最新的一帧"（没有更早的帧就退回最早那帧，宁可旧一点也别空着闪）。
    - **进 FFT 前必须去直流**：很多素材带几十 LSB 的直流偏置（实测那批测试音就有 ≈-50dBFS），它的能量全落在最低那一两个 bin 上，而最低几段又都挤在同一个 bin 上——不滤掉的话最低三根条会永远亮着、像坏了。同理，**相邻段不许共用同一个 bin**（两端都四舍五入会让同一个 bin 属于前后两段，最强段还偏半档：440Hz 曾落到第 10 段、而含 440Hz 的是第 11 段）。
    - 这两条加上电平标定/衰减单调，`--probe --spectrum-test` 全钉着（17 项，纯合成不需要语料；把直流阻断绕过、相位改成"取最新一帧"都会让它报 FAIL——直流那条反向验证过）。seek 会让 `applyAudioSeek()` 调 `reset()` 作废旧位置的帧；只有纯音频画面才 `setSpectrumEnabled(true)`，视频/实况那条路不白算 FFT。播放器在没有视频帧时画它：`VideoPlayerApp::updatePlaceholder()` 一个缓存位图管"打开失败"与"纯音频"两种静态画面，`startFile()` 里必须**无条件**把 `placeholder_` 清掉、`placeholderStamp_` 置 0——`infoScreenStamp()` 的指纹里没有文件名（尺寸/DPI/语言/主题），不清就会拿上一片的文件名重画。
  - **看图那条路遇到音频**：`myLoader` 认出 `FileFormat::Audio` 时按"不支持的格式"报告（它由播放器播，不是图片损坏），`--probe` 另外打一行 media 统计（`video=false` + 音频轨信息）。`JarkViewer/src/FormatSniffer.cpp` 里判 Audio 的**只有无歧义的魔数**（`fLaC`/`OggS`/`ID3`/`RIFF`+`WAVE`/`FORM`+`AIFF`/`MAC `/`wvpk`/`TTA1`/`DSD `/`#!AMR`）；`mp4/mkv/ogg` 这类容器照旧判 `Video`——它们可能有视频也可能没有，"有没有视频轨"由播放器实际探测，按扩展名/魔数硬判成音频反而会与入口判定打架。
  - **数据来源是整文件内存映射**（`MappedFileReader`，原在 `ImageDatabase.cpp` 的匿名命名空间里，提到公共头共用）：不再把视频读进内存、**没有 256 MiB 上限**（`ImageDatabase` 侧那条老上限与 `VideoSource` 的整份拷贝只留给实况照片/`--probe`/批处理/缩略图，没动）。映射**必须活得比 `MediaPlayer` 久**（`MediaDecoder::open()` 只持 span 指针、不拷贝数据），`VideoPlayback::close()` 里先 stop 再 reset 映射就是这个原因。
  - **窗口几何与看图窗口共用一份记忆**（`SettingParameter::rect`/`showCmd`/`monitorDevice`）：两个窗口是同一个进程里二选一的顶层窗口，谁退出谁在 `D3D11App::saveSettings()` 里写、谁启动谁在 `Initialize()` 里读（`loadSettings()` 在构造函数里跑，换窗口的接力循环里第二个窗口会重新读盘）。所以**"上次看视频调出来的窗口尺寸会继承给下次看图，反之亦然"**是设计目标，不是 bug；别再按模式分开记。
    唯一的例外是**退出时正处在全屏**（播放器的 F/F11/双击画面、看图的幻灯片）：窗口那时被临时改成了无边框满屏，直接存下来会让下次启动变成"满屏带标题栏"的怪窗口；而"全屏时干脆不写"又会丢掉用户进全屏**之前**做的调整（实测：调好尺寸 → F11 → 退出 → 下次开的还是启动时那个尺寸）。所以 `jarkUtils::SetFullScreen(hwnd, true)` 在进全屏那一刻额外记一份 `WINDOWPLACEMENT`，`saveSettings()` 在 `IsFullScreen()` 时改用它（`GetPreFullScreenPlacement`）——用户在全屏里挪不动窗口，那份就是"最后看到的窗口化样子"，最大化状态也一并保住。
    回归：`pwsh tools/test_placement_memory.ps1 -Exe x64/Release/JarkViewer.exe -Media <视频> -Image <图片>`——五项断言：播放器调成 A 尺寸退出 → 看图必须开在 A；看图调成 B 退出 → 播放器必须开在 B；退出时正全屏 → 下次必须回到进全屏前的窗口化尺寸（而不是 2560x1440 那块屏）；最大化退出 → 下次也最大化。它在 `%TEMP%` 的**独立 exe 副本**上跑（设置文件写在 exe 旁边），不会动用户的 `JarkViewer.db`。
  - **键位**：空格播放/暂停（**播完停在最后一帧**并转入暂停态，再按空格从头重播）；A/D 与 ←/→ 播放中 ±5 秒、**暂停中单帧进退**（纯音频没有帧，这一档退化成 ±5 秒）；W/S 与 ↑/↓ 音量 ±5%（初始 50%、**不落盘**）；滚轮也是音量（光标在信息面板里时改为滚面板）；I/Tab 媒体信息面板；Home/End 开头/结尾；F/F11/双击画布全屏；Ctrl+O 换片（选到图片则交接给看图窗口）；Ctrl+W/ESC 退出；**其余键一律吞掉**（播放器里不存在"顺势落到看图那套分支"这回事）。**鼠标单击画面也是播放/暂停**（按下与抬起都落在画面上才算，避免"按在条带上、抬在画面上"被误判）；双击全屏与它是两条路——两次单击各切一次、正好抵消，代价只是中间一下很短的停顿，换来单击的零延迟。
  - **拖动进度条**：按下即暂停并 `SetCapture`（拖到条带外、拖出客户区也跟随，`OnPointerCancel` 收尾）；拖动中每 ~60 ms 只发一次**关键帧预览** seek（`SeekMode::KeyFrame`），被节流挡下的最新目标由 `takeFrame()` 到期补发（用户停手后那一下不能丢）；松手发**精确** seek（`SeekMode::Exact`）。三条不变式，`--probe --video-test` 盯着：①拖动中时钟停在把手位置；②**一下只交一帧预览**（`MediaPlayer` 的 `previewHold`：关键帧交出后就停下等下一次 seek。少了它，解码线程会顺着关键帧把"还没到目标"的帧一帧帧送出来——它们的 PTS 都早于已重设到目标的时钟、全都算到点，画面看起来像在追赶）；③松手后**等精确落位再恢复播放**，落位判定用"已交出的帧 PTS ≥ 目标−一帧半"，**不能**只看"落位在途"标志（那个标志由解码线程置位，很可能在本帧取帧之前就被消费掉了）。
  - **seek 的实现**：命令按序号发布（`seekTargetMs` + `seekSerial`，两个解码线程各自比对序号、只取最新目标 = 在途旧请求自动作废、不做累积），两个解码器各自 `av_seek_frame`（`MediaDecoder::seek` 里 `AVSEEK_FLAG_BACKWARD` 退到不晚于目标的关键帧再冲解码器状态）；**精确落点靠丢帧**：视频丢"整帧都落在目标之前"的帧、音频裁掉目标之前的样本；丢帧过滤**在缩放/拷贝之前**也做一遍（`MediaDecoder::setSkipBeforeMs`）——从关键帧向前解码要解上百帧，每帧 `sws_scale` + `clone` 就是几毫秒，那是落点耗时的大头（实测 1080p 长 GOP 1.5s → 0.15s、4K 1.5s → 0.4s）。**时钟基线**（`clockBaseMs` + 声卡已播样本数，没有声卡时是墙上时间累计）在 seek 时重设，且**只有一个写者**：有声卡时由音频线程写、没有时由视频线程写，别两边都改。
  - **seek 必须按"本实例负责的那条流"来**（`MediaDecoder::seek` 拿 `videoStreamIndex`/`audioStreamIndex`，并换算到该流的时间基；只有两条流都没有时才退回 `stream_index = -1`）。别用默认流：视频+音频的 MP4 默认流是视频流，音频实例跟着跳到**视频关键帧**上（实测 seek 18000ms 落到 16619ms），于是 seek 之后先送出一段目标之前的旧音频——听感正是"半秒到几秒的杂音 / 像在快放"，之后才接上正确位置。音频流每个包都是关键帧，自己 seek 只差一帧（实测偏差 5~11ms）。
  - **"早于目标的样本一律不出声"由解码器侧统一兜住**（`setSkipBeforeMs` 对音频同样生效）：音频**先 resample 再裁**（swr 的内部历史要连续喂着走，跳过输入帧会在接缝处留下爆音），裁过的批把 `ptsMs` 一起往前推，整批在目标之前就整批丢掉、下一批接着裁。播放端**不再自己裁**：以前那里只在 seek 后裁第一块，落点远在目标之前时第二块起就被当成正常音频送出去了（同一个"杂音"的另一半原因）。`--video-test` 用两条断言钉着：音频实例的落点要在目标 100ms 内；落点在目标之前时（拿同时解两路流的实例刻意制造），交出的第一批音频也要被拉回目标附近。
  - **两个解码线程在 EOF 之后不许退出**（改成等下一次 seek）：播放器要停在最后一帧等用户拖回中间，线程一退出 seek 就没人执行了。同理 `hasFinished()` 的时钟上限要**留一帧余量**（最后一帧的时间戳常常略超容器时长，卡死在时长上会让"时钟 ≥ 最后一帧"永不成立——实况照片会卡在播放状态、播完停不下来）。
  - 暂停是 `AudioOutput::pause()` 的 `Stop(0)`（**保留位置与已排队缓冲**，别丢队列重来，否则暂停再继续会有静音空洞且反复暂停会累积偏移）；seek 才 `flush()` 丢队列。`AudioOutput::create` 失败时回退系统时钟、静音播放，暂停/seek 照常可用。
  - **条带**：底部 50 逻辑像素（与看图里动图播放条同高）、半透明底 `ImGuiCol_PopupBg × 0.82`、画在 ImGui 前景绘制列表上并经 `uiPos()` 换算；左侧 50×50 正方形播放/暂停按钮（半透明底只在按钮这一块露出来），图标**手绘**（`AddTriangleFilled` / 两个 `AddRectFilled`，不用字体也不用雪碧图——前者要确认字形存在，后者是连条带背景一起烘焙的、裁不出透明按钮）；**按钮右侧整块就是进度条**：占满条带高度、无内边距无圆角、无把手，两个色调——已播 = 主题强调色（`ImGuiCol_CheckMark`，深色主题下是深蓝），未播 = 它往白色拉 0.55 的浅色调（"左深右浅"在两套主题下都成立：强调色本来就是"与背景对比的那一档"，再往白拉只会更浅）。**两层各铺一层、都半透明（`kTrackAlpha = 0.45`，同一个 α）**，画面会明显透出来（和条带底同一套叠色语义；实测反解出的层色就是 (0,106,164) / (140,188,214)，两点色差 (140,82,50) 与 α 完全吻合）。**别写成"整条先铺浅色、再把深色盖上"**：两层半透明在已播段里互相透色，α 一低两块就糊成一种颜色（实测已播/未播在画面上的亮度差只有 15，现在是 50），所以已播只铺左边那段、未播只铺右边那段。时间 `mm:ss / mm:ss` **水平+垂直居中压在条上**（整块都是进度条，只能叠上去；底下垫一层 `IM_COL32(0,0,0,140)` 深色底 + 白字，才在深/浅两种底色上都看得清——**别**改用主题文字色，浅色主题下会变成深字压浅条）。**显隐分两种**：视频是**纯悬停触发、瞬时显隐**（鼠标进条带即显示、移开立即隐藏，唯一例外是按住拖动期间不隐藏——拖到条带外、拖出客户区也跟随），隐藏时不参与命中测试；**纯音频画面（`showsAudioScreen()`）下条带常驻**——整幅画面就剩它一个能看、能操作的东西，藏起来等于把进度与时间也藏了。音量提示是手绘小喇叭 + 百分数字（1.2 秒后自己擦掉，**不新增多语言文案**）。播放器用 ImGui **只画条带、不做输入捕获**（`hasVisibleWindows()` 返回 false，命中与拖动都在消息处理里自己做）。
  - **画面缩放只有"适应窗口"一档，且必须先重采样到屏幕尺寸再画**（`updateFitView()` + `drawFitFrame()`）：播放器没有缩放/平移操作，倍率固定 `scale = min(窗口宽/帧宽, 窗口高/帧高)`；逐帧采样路径（`drawCanvasImpl`）为速度只用最近邻，**放大时一个源像素被铺成一整块方块**，帧分辨率明显低于画面区域时颗粒感一眼可见。看图窗口靠"画面静止后平滑重采样"（`ImageResampler`）补这一步，播放器没有、也不可能每帧做一遍，所以 `drawFitFrame()` 自己先把帧 `cv::resize` 到 `fitSize_`（`imageGeometry` 算出的绘制矩形尺寸：缩小 `INTER_AREA`、放大 `INTER_LINEAR`，`INTER_AREA` **只能**用于缩小），再用"名义尺寸 = 位图尺寸、zoom = zoomBase"的 `fitView_` 交给采样路径——采样密度恰好 1:1，滤波只做一次，采样端退化成逐像素拷贝；1:1 时（`fitSize_ == frame_.size()`）不重采样，直接画 `frame_`。放大**不用** CUBIC/Lanczos4 是实测定的：1920x1080 → 2500x1406 一帧 INTER_LINEAR 1.9ms（整帧 10.5ms）、INTER_CUBIC 6.7ms（15.1ms），而 60fps 只有 16.7ms。回归：`tools/test_player_scaling.ps1`（黑白棋盘格视频放大后必须出现中间调灰阶；把这一步去掉就退回 2 个灰阶、0% 中间调，反向验证过）。
  - **尺寸/DPI 变化后必须立刻重画并重传画布**：`OnResize()` 里的 `CreateWindowSizeDependentResources()` 会把**上传画布用的暂存纹理整块重建**（内容是空的），而 `DrawScene()` 只在"画布变了"时才重画重传——`updateFitView()` 已经把 `viewWinWidth_/viewWinHeight_` 记成新值，那条判定再也不会触发。播放中下一帧会顺手补上，**暂停时没有"下一帧"，于是整个窗口全黑**（要等恢复播放、或再改一次尺寸才回来）。所以 `OnResize()` 在 `updateFitView()` 之后要直接 `drawImageToCanvas + PresentCanvas` 一次（与看图窗口 `OnResize` 里 `drawCanvas` 同一套路），别只 `markPresentRequested()`。回归：`tools/test_player_resize.ps1`（暂停后先缩后放，量画面平均亮度；把那一下去掉会掉到 0，实测反向验证过）。
  - **打开失败**沿用 `InfoScreen` 占位（文件打不开 = `FileMissing`、解不出视频流 = `DecodeFailed`），**不画条带**，标题栏仍显示文件名。
  - 自检：`--probe --video-test <文件...>`（`MediaPlayer` 的暂停/seek/单帧/播完停在最后一帧/seek(0) 重播，`VideoPlayback` 的拖动三条不变式；纯音频文件跑同一份入口的无帧断言——音频没有帧时间戳，落点对着 `positionMs()` 量，容差 120ms；seek 是异步的，**先 pump 一段再采样基准**，别拿"刚发完命令"的位置当起点，视频那段也踩过同一个坑）。造语料用 ffmpeg 生成到临时目录：>256 MiB 大文件、4K、无音轨、竖拍带 display matrix、长 GOP（`-g 250`，专门量落点耗时）、截断损坏文件；音频用 `-f lavfi -i sine=frequency=440:duration=8` 加 `-c:a libmp3lame|flac|aac|alac|libvorbis|libopus|wmav2|wavpack|pcm_s16le|…` 造一圈（ape/mpc/tta/tak/dsf 这几种没有编码器，只能靠"解复用器+解码器都在"核对）。
- `JarkViewer/include/CanvasRenderer.h` 与 `JarkViewer/src/CanvasRenderer.cpp` 是从 `JarkViewerApp` 抽出的画布绘制模块：按 `ViewState`（名义尺寸 / 定点缩放 / 平移 / 旋转）把图像绘制到 BGRA 画布，含透明区域棋盘格与图像边框。`JarkViewerApp::drawCanvas()` 只是把 `curPar` 转成 `ViewState` 后调用它。`imageGeometry()` 是主画布与鸟瞰**共用**的纯几何（旋转后名义尺寸、显示矩形、归一化可见区域——`slide + round((canvas-rendered)/2)` 的定位公式只有这一处），`navigationSlide()` 把归一化图像点换算成目标 slide（某轴完整可见时保持居中、不改动原有“允许留白”的拖图夹取）；`--probe --navigation-test` 用合成断言覆盖旋转/平移/端点夹取与浮层输入归属。`ViewState::border=false` 表示不画图像边框（主页/解码失败是界面画面，不是照片）。
- 主页与解码失败画面由 `JarkViewer/include|src/InfoScreen.{h,cpp}` **按当前语言、主题、DPI 实时绘制**（旧的 `home.png`/`tips.png`、`getHomeMat()/getErrorTipsMat()` 和 ColorManager 里"识别内置提示图"的像素启发式都已删除）：`ImageAsset::placeholder`（`PlaceholderKind`：Home/UnsupportedFormat/DecodeFailed/FileMissing）与 `placeholderDetail` 由 `myLoader` 在失败时填写——文件头与扩展名都识别不出→UnsupportedFormat，能识别但解码失败→DecodeFailed，打不开/不存在→FileMissing（各解码器的失败分支保持"空帧 + format=None"交给 myLoader 继续路由，不再往 primaryFrame 塞提示图）。**失败时 primaryFrame 保持为空**：`--probe` 据此判定失败并输出 `placeholder=<原因>`，批量处理也会如实报"无法解码"（以前提示图会被当成解码成功的图参与批处理）。界面层用 `JarkViewerApp::updatePlaceholderImage()` 按 `jark::infoScreenStamp(尺寸, DPI缩放, 语言, 主题, 按钮交互)` 决定重绘（结果写进 `placeholderStamp`；返回 0/1/2 = 无变化/仅内容变/尺寸也变，只有尺寸变才 `curPar.Init()`，悬停反馈不会重置缩放）：`initOpenFile`/`switchToFile`/重载/删除都在 `curPar.Init()` 前调用一次，`DrawScene()` 开头再兜一次，窗口缩放/换主题/换语言自动重绘。支持格式清单直接读 `ImageDatabase::supportExt/supportRaw/videoExt`，永远与实际解码能力一致。主页是图标 + 名称/版本 + **「打开图片」按钮**（`homeButtonRect` 与绘制共用同一布局；主窗口把客户区坐标按缩放/平移/旋转逆变换回画布像素做命中，悬停/按下有底色反馈，点击等同 Ctrl+O）。文案是窄表 156~165。
- **画面静止后的平滑重采样**：`JarkViewer/include|src/ImageResampler.{h,cpp}` + `main.cpp` 的
  `activeSmoothBlock()`/`SmoothBlock` 缓存。逐帧采样路径（`CanvasRenderer`）为速度只用最近邻
  （缩小时 2×2 近似），放大看文字有锯齿、缩小看细密纹理有摩尔纹；视图**停稳**后
  （`zoomCur == zoomTarget && slideCur == slideTarget`）把可视区域+25% 余量重采样成一块位图
  （放大 `warpAffine` + `INTER_LANCZOS4`、缩小裁出源矩形后 `INTER_AREA`，再按 rotation 转成
  预旋转块），交给 `CanvasRenderer` 采样——块原点与区域按 `llround(regionX*scale)` 回填，
  采样密度算出来恰好 1:1（`srcScale == 缩放系数`），与矢量图的高清块走同一条 `sourceLeft/
  Top/Width/Height + sourcePreRotated` 通道。约定与坑：
  - **旋转方向必须与采样端一致**：`rotation 1 = 逆时针 90°`（`Q` 键那档），名义空间归一化点
    映到源位图是 `rot0: (nx,ny) / rot1: (1-ny,nx) / rot2: (1-nx,1-ny) / rot3: (ny,1-nx)`；
    方向写反不会崩，只是画面整体镜像/转向，`--probe --resample-test` 用有方向性的合成图对拍钉着。
  - 只在**静止**、且不是动图/实况播放/矢量图（矢量自己按可视区域出块）时启用；键里带源位图
    指针+尺寸+类型、画布尺寸、zoom/slide/rotation，任一变化就重算。一次重算约 12~20ms（2K 画面），
    只发生在停稳那一下。
  - **放大分支必须先把源裁到"这次用得到的那块"再 `warpAffine`**：`cv::warpAffine` 的非最近邻插值
    内部走 `cv::remap`，而 `remap` 断言 `src`/`dst` 两个方向都 `< SHRT_MAX(32767)`
    （`imgwarp.cpp`，`hal::warpAffine` → `WarpAffineInvoker` → `remap`）。整幅喂进去时，宽或高
    超过 32767 的图（长卷轴、大扫描件）在**放大到 100% 以上**就会抛 `cv::Exception`：界面线程没人接，
    进程直接退出——用户看到的正是"打开没问题（缩小时走 `cv::resize`，不受这条约束）、放大到一百多个
    百分点闪退"。裁出来的块只有画布大小，顺带省掉整幅的边界换算；裁剪边要多留 8 像素，否则贴着裁剪边
    采样时 `BORDER_REPLICATE` 会拿错边。`--probe --resample-test` 的超宽位图用例（33000×40，2.0x）
    钉着这件事：去掉裁剪，或让裁剪原点错 1 个源像素，它都报 FAIL（后者靠"离图像原点偶数个画布像素的
    位置必须与逐帧路径逐字节相同"——2.00x 时这些位置正好落在源像素中心，Lanczos 核在整数处取 1）。
  - 两条分支都包在 `try/catch (cv::Exception)` 里：重采样只是"停稳后看得更清楚"的增强，
    OpenCV 的内部尺寸限制不该把整个程序带走，失败就记日志并退回逐帧最近邻（`valid=false`）。
  - 设置页常规页「缩放平滑插值」（`SettingParameter::disableZoomSmoothing`，**取反命名**：
    旧设置该字节为 0 即默认开启；占 `blackFullscreenBackground` 之后的原对齐填充字节，
    不改变 4096 布局）关掉即退回全最近邻。
- `JarkViewer/include/VectorImage.h` 与 `JarkViewer/src/VectorImage.cpp` 负责矢量图（SVG）的按需光栅化：`ImageAsset::vectorSource` 持有 lunasvg 文档与文档尺寸（intrinsic），位图分辨率随缩放变化（`vectorTargetEdge()` + `refreshVectorRaster()`，滞后阈值 1.25 避免缩放动画中反复渲染，长边上限 4096）。主窗口在画面稳定后（`DrawScene` 空闲分支）调用 `refreshVectorRasterIfNeeded()` 升级分辨率。lunasvg 的位图是 **ARGB32 预乘**（内存 B,G,R,A），`renderVectorImage()` 一律**手工反预乘**成直通 alpha（直接调 `convertToRGBA()` 会换成 R,G,B,A 字节序，画布按 B 读第一个字节会红蓝互换）。`<text>` 要先 `jark::ensureVectorFonts()` 注册系统字体（lunasvg 没有内置字体，不注册就什么都画不出来）。
  `JarkViewer/include/SVGPreprocessor.h`（还在 `JarkThumbnailProvider` 里有一份同源的）在解码前做三件 lunasvg 做不到的事，顺序不能换：① `<switch>` 选择——`foreignObject` 与 `requiredFeatures` 里的 Extensibility 一律判为**不支持**，否则 draw.io 导出的画布会选中画不出来的 foreignObject、同时把后面等价的 `<text>` 兜底删掉（表现是方框连线都在、文字一个字都没有，见 issue #33/#51）；② 收集 `--x: value`（`:root` 规则表与内联 style 都覆盖）；③ 把 `var(--x[, fallback])` / `light-dark(a, b)` 折叠成字面量——lunasvg 不认识 CSS Color 5 的这些函数，颜色值判为无效时会把**整个图元丢掉不画**（draw.io 图整幅空白）。`light-dark` 取**亮色分支**：位图会进图像缓存，随主题变化的颜色没有意义，亮色分支在深浅主题下都保持可读。
  **放大超过 4096 上限后按可视区域出高清块**（`VECTOR_DETAIL_MAX_EDGE` / `VectorImage::detailFrame`）：全幅位图（鸟瞰、缩略图、打印/批处理仍用它）已经榨不出细节，这时用 `renderVectorImageRegion()` 只光栅化当前可视区域+25% 余量（`VectorImage.cpp` 把"文档→旋转后名义空间"的仿射系数写进 `Document::render(bitmap, matrix)` 的矩阵里一次完成，所以位图直接就是旋转后的名义空间，取样不必再套旋转）。`CanvasRenderer` 侧只多了 `ViewState::sourceLeft/Top/Width/Height`（归一化区域）与 `sourcePreRotated`：采样原点按区域左上角平移、采样密度按"位图像素 / 该区域的名义像素"算，元素级循环一行没改。**复用判断只看可视区域（不含余量）**，否则余量会被逐帧的微小移动吃掉、每帧重光栅化；可视区域跑出旧块外的那一帧退回全幅位图（糊但不缺块），稳定后自动重出。切图时释放上一张的高清块（`lastDetailVector_`），避免图像缓存里每张 SVG 各攒几十 MB。
  **lunasvg 的能力边界**（判断"要不要换渲染库"先看这几条，实测语料 `car.svg`/`13.svg`/`AA_5.svg`/draw.io 图都能出图）：支持 `mask(含渐变遮罩)/pattern/marker/clipPath/嵌套 svg/各类渐变/<text>`；**不支持 SVG filter**（`feGaussianBlur`/`feDropShadow` 等一律忽略——图元照画、只是没有滤镜效果）与 **`textPath`**（整段不渲染）。`--probe --svg-test` 里两条断言盯着这个边界：filter 用例断言"图元仍被绘制"（比"没模糊"严重得多的是整块消失），textPath 断言"不渲染任何文字"（真补上支持时这条会失败，提醒改文档）。换库（resvg/ThorVG）需要新做一套 MSVC x64 静态库、且静态库包还没发布，除非出现"整幅画不出来"的真实报告，否则维持现状 + 定向补 `SVGPreprocessor`。
- `JarkViewer/include/ColorManager.h` 与 `src/ColorManager.cpp` 是色彩管理（lcms2）：把解码出的像素从**图像内嵌 profile** 转到**目标 profile**。有两条路，目标不同**不能混**：
  - 查看器/编辑/打印走 `ImageDatabase::loader()` → `applyToImageAsset()`：目标取**当前显示器 ICC**（`GetICMProfileW`，进程内缓存），显示器没设 profile 就退回 sRGB（内置）。
  - 批量转换走 `BatchProcessor::loadImage()`：目标固定 **sRGB**——落盘的文件带不上 profile（OpenCV 写不了 ICC），只留 P3/AdobeRGB 的原始数值却去掉标签，等于把颜色悄悄改了（这条以前压根没做色彩管理，P3 图转出来在别的软件里会偏色）。**不能**取显示器 profile：结果是要给别人看的文件，跟转换时这台机器接什么显示器无关。
  内嵌 profile 的来源按格式分：JXL/HEIF/实况在 `myLoader` 各自的分支里填 `iccProfile`，其余格式（JPEG/PNG/WebP/TIFF…）由 `ImageDatabase::readIccProfile()` 用 Exiv2 从文件补读，两条路都调它。
  两个 profile 都缺或逐字节相同时变换是恒等的，直接**跳过**（一亿像素白跑一趟要两三百毫秒，这步紧跟在解码之后、顶在出图时间上）；`applyToMat` 就地变换、`cmsFLAGS_COPY_ALPHA` 保证 alpha 不动，大图按行并行（与串行逐字节一致，`--probe --color-test` 钉着）；开日志会打一行 `色彩管理: 源 → 目标 (WxH Nch)`，"颜色不对"的报告先看这一行。
  **超色域颜色转窄色域会被剪裁，看着像串色但不是 bug**：`D:\Downloads\test\P3\Webkit-logo-P3.jxl` 整幅只有 Display P3 的两种超饱和红（255,0,0 与 242,0,0），转 sRGB 后都被剪到 (255,0,0) —— 图案消失、只剩纯红；这时关掉色彩管理看到的"隐约图案"才是假的（那是把 P3 数值直接当 sRGB 读的未管理画面）。`--color-test` 里有这组已知值断言（P3 三原色/中灰/次级饱和红 → sRGB 落点，与 lcms2 参考实现一致），改色彩链路后跑它。
  缩略图那条路（`ThumbnailService` 的本地解码兜底）**有意不做色彩管理**：它是"大概预览"，不要求准确，何况解码线程按约定不读显示器状态、按固定 sRGB 转又会在宽色域显示器上与主画面不一致。别再往上加 ICC（要加就得连持久缓存键一起改，否则切换开关会留旧图）。
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

- `SettingParameter` 按固定 4096 字节设置文件持久化；不要随意调整成员顺序、大小或删除保留字段，否则会破坏旧设置兼容性。**读盘是整体赋值**（`settingParameter = tmp`，`loadSettings()`）：任何"在窗口构造之前"写进 `settingParameter` 的东西都会被文件覆盖——命令行/环境变量之类的临时覆盖要在读盘之后再应用（`--lang` 就是这么修的），否则表现为"参数完全没生效"。
- 新增图片格式时，同时检查 `ImageDatabase::supportExt` / `supportRaw`、`FormatSniffer`（文件头嗅探与扩展名映射）、`decodeByFormat()` 分支、EXIF/方向处理、设置页文件关联列表和 README 格式列表。
- `buildRelease.ps1`：**PowerShell 7 专用**（脚本自带 5.1 拒绝守卫，别把守卫删了），构建参数走 `ProcessStartInfo.Arguments` 字符串而不是 `ArgumentList`——少一处平台差异，命令行也一眼看得全。历史上用 5.1 跑 `ArgumentList.Add()` 会静默丢掉全部参数，MSBuild 退化成默认 Debug 构建。
- `JarkViewerApp::drawCanvas()` 有两条必须同时成立的规则：**几何尺寸用名义尺寸**（`curPar.width/height`，矢量图 100% 时屏幕上应有的尺寸），**采样密度用位图分辨率**（`srcScaleX/srcScaleY = 位图尺寸 / 名义尺寸`）。矢量图的位图分辨率会随缩放变化，任何"用 `srcImg.cols/rows` 当几何尺寸"或"用 `zoomInvert` 直接换算位图坐标"的写法都会让画面尺寸/位置错乱。
- Release 构建默认不打印日志，排障时用 `--log` 或 `JARKVIEWER_LOG=1`（写入 `%TEMP%\JarkViewer.log`）；新增诊断日志直接写 `JARK_LOG(...)` 即可，`isLogEnabled()` 为假时不会计算参数。
- 查"启动/打开一张图为什么慢"用 `JARKVIEWER_STARTUP_TRACE=<文件路径>` 环境变量（`jarkUtils::startupTraceMark()`）：记录 begin / 窗口就绪并派发解码 / 设备与 UI 就绪 / 首帧就绪 / 首帧绘制各阶段相对起点的毫秒数；没设环境变量时零开销。新增关键阶段时在对应位置补一个 `startupTraceMark()`。
- 崩溃只在"反复开关句柄 / 多线程一起忙"时才出现（时序竞态）时，**不能挂调试器抓**：挂 cdb（attach 或直接启动）会把竞态窗口关掉，实测 1/5 的频率一 attach 就基本不复现（`tools/catch_crash.ps1` 对这类问题因此指望不上，它只适合必崩的那种）。改成让进程自己落盘：`--probe` 认环境变量 `JARKVIEWER_CRASH_DUMP=<文件路径>`，在 `SetUnhandledExceptionFilter` 里用 `MiniDumpWriteDump(MiniDumpWithFullMemory)` 写完整内存 dump（约 200~300MB）——时序不变，照样崩。想更快撞上就几路并行跑（12 核机器上 4 路并行比单开容易撞得多）。
  ```bash
  JARKVIEWER_CRASH_DUMP='C:\Temp\crash.dmp' JarkViewer.exe --probe --video-test a.mp4 b.mp4 --out r.txt
  cdb.exe -z crash.dmp -c ".ecxr;r;kb 16;q"         # 崩溃线程与栈；.ecxr 必需，只看默认线程会看错栈
  cdb.exe -z crash.dmp -c "!address <出事的地址>;q"  # 那块内存现在是什么区域
  ```
  系统模块（ntdll/kernel32/ucrtbase/XAudio2）自带符号，`srv*` 够用；要自己的符号就得留 PDB——`-p:` 全局属性**覆盖不了** vcxproj 的 ItemDefinitionGroup 元数据，只能直接把 `DebugInformationFormat` 改成 `ProgramDatabase`、`GenerateDebugInformation` 改成 `true` 再 Rebuild（改完记得还原）。这台机器不是管理员，写不了 WER 的 `LocalDumps` 注册表项、也没有 `CrashDumps` 目录，所以进程内 dump 是唯一顺手的路子。
- UI 文本来自 `stringRes`：**两张表同名不同文案**——`UIStringTable[stringID][语言]` 供 ImGui/画布，`UIStringTableWide[stringID][语言]` 供 Win32（窗口标题、菜单、消息框），写 `getUIStringW(id)` 时一定要核对**宽表**里那个 ID 是什么（历史上出过把消息框正文写成"关于 (&A)"菜单项的事故）。`getUIStringW` 返回 `UIStringWide`（自带缓冲区、可隐式转 `const wchar_t*`）：旧实现共用同一个 thread_local 缓冲，`MessageBoxW(h, getUIStringW(a), getUIStringW(b))` 后一次转换会冲掉前一次的指针内容（标题乱码）。需要指针活过当前语句时（如 `BROWSEINFO.lpszTitle`）用 `.str()` 存一份 `std::wstring`；丢给 `std::format`/`wstring_view` 参数时要显式 `.c_str()`。**新增文案追加到对应表尾**，并在使用处写成具名常量（各窗口文件里已有 `kStr*` 常量块）。改动后再跑一次 `--probe --lang-test`，它会打印窄表与宽表各若干条文案用于确认 ID 没有错位。
- 发布包里的 OpenCV 预编译库带有源码改动：移除 `imgcodecs` 分辨率限制，并将 HighGUI Win32 窗口光标从 `IDC_CROSS` 改为 `IDC_ARROW`；替换或重建 OpenCV 时要保留这些行为（补丁与参数见「构建前提」）。
- 不要提交 `.vcxproj.user`、`.vs/` 或机器相关的本地库路径。
- **渐进加载**：当前图的解码不阻塞主循环——`requestCurrentImage()` 用 `ImageAssetCache::tryGetPtr` 非阻塞查缓存，未命中就挂起，主循环每帧 `updatePendingLoad()` 轮询，就绪后 `adoptCurrentImage()` 收尾。等待期间：启动/主页场景用主页画面垫底（`allowPreviewSwap_`），缩略图（ThumbnailService，持久缓存命中时毫秒级）先到就先顶上当模糊预览；切图场景保留旧图停留（相邻图通常已被预取，点翻页零等待）。客户区左上角画「加载中 X.Xs」浮标（逐帧跳秒——画面稳定分支要按 `pendingLoad_` 持续出帧），超过 60 秒退回一次阻塞等待兜底。**新增"载入当前图"路径时一律用 `requestCurrentImage()`**，不要再直接调 `getSafePtr`（会退回"翻页等解码"）。
- **动图播放计时**（`DrawScene` 的动画块）：帧推进的剩余时间 `delayRemain` 按**微秒**累计并跨帧保留（欠帧时 `while (delayRemain <= 0)` 循环推进、推进后把超出的部分留给下一帧），不要退回"整毫秒截断"或"每帧重置余量"——主循环每帧 10~16ms，零头被截掉/丢弃会逐帧累积成慢放（100ms 的帧实测会播成 103~109ms）；起播、暂停恢复、切图后要经 `animClockArmed` 重新对齐计时起点，否则加载或暂停的耗时会被算进第一帧（首帧长时间不动）。
- **实况照片的视频有三种拿法，缺一不可**（`ImageDatabase::loadMotionPhoto` / `loadLivp`，顺序即优先级）：
  1. 按「文件大小 - `Item:Length`/`MicroVideoOffset`」反推起点，再在期望起点附近用 MP4 首个 `ftyp` 盒校正真实起点（`locateMotionPhotoVideoStart()`）——尾部视频不一定紧贴文件末尾（DJI 等导出带尾块），起点偏几十字节会让采样偏移整体错位，表现为"能识别出视频轨但解码全是乱码"。
  2. 同目录同名侧车视频（苹果/VIVO 的 `.mov`/`.mp4`）。
  3. **尾部找 MP4**（`locateTrailerMp4Start()`）：XMP 的长度字段缺失或明显不当的 Samsung "versionless / mpv2 trailer" 型（实测 `Item:Length` 写成 `68`，而尾部实挂 2.4MB 可解码 MP4；另一型只有 `MotionPhoto_Data` 标记 + `ftyp mp42`，文件以 `SEFT` 收尾）。规则：正文里必须**有实况照片标记**（`MotionPhoto_Data` / `Item:Semantic="MotionPhoto"` / `GCamera:MotionPhoto="1"`，否则普通 HEIC 的盒结构本身就长得像 MP4，会把图像数据当视频切出去）、候选 `ftyp` 盒的品牌不能是 HEIF/AVIF 系、且从它按盒长走链必须**见过 `moov`**、主体走到文件尾（容许末尾 ≤4KB 厂商尾块）。**没有这条兜底时这两类实况照片会静默退化成静态图**（`--probe --playback-test` 报"跳过：没有可播放的媒体流"，看上去像"这张本来就没视频"）。
- **导出实况视频**（右键菜单「导出视频」，`JarkViewerApp::exportCurrentVideo()`）：`VideoSource::data` 本来就是完整容器（livp 解包的 `.mov`、Android 尾部切片、Samsung 尾部 MP4、或整个视频文件），落盘即用，不做转码；`VideoSource::extension` 记着原扩展名（livp 的 zip 条目名、视频文件的扩展名），另存对话框按它给默认名与过滤器。菜单项由 `D3D11App::hasExportableVideo()` 决定是否置灰（基类返回 false，主窗口按当前资源里有没有视频覆盖）。语料实测：8 个 livp/实况文件导出后 ffprobe + 全解码无一报错。
- 主窗口渲染路径以 OpenCV `cv::Mat` 作为 CPU 画布，再交给 Direct3D 显示；避免在高频绘制路径中引入阻塞 I/O 或昂贵同步操作。
- Debug 构建会分配控制台并启用 `JARK_LOG`；Release 下日志宏为空。
