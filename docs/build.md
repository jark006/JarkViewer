# 构建与依赖

[文档导航](README.md) · [验证指南](testing.md)

## Release 构建

需要 Windows x64、支持 MSVC v145 的 Visual Studio / Build Tools、Windows SDK 和 PowerShell 7。项目使用 C++23 与静态 CRT（`/MT`），解决方案为 `JarkViewer.slnx`，包含查看器与 `JarkThumbnailProvider` 两个工程。

```powershell
pwsh ./buildRelease.ps1
./x64/Release/JarkViewer.exe
```

`buildRelease.ps1` 当前写明了 Visual Studio 18 Community 的 MSBuild 路径。换机器时先检查该路径；找不到工具链不能算构建通过。Release 主程序在 `x64/Release/JarkViewer.exe`，查看器中间文件在 `JarkViewer/x64/<Configuration>`。

构建期间不要改源码，也不要运行会原地修改文件的反向验证。等待自己启动的构建进程退出；不要等全部 `MSBuild.exe` 消失，节点复用进程可能长期驻留。

## 第三方静态库

`.lib` 不随 Git 分发。先从 [static_lib Release](https://github.com/jark006/JarkViewer/releases/tag/static_lib) 下载匹配的库包，按包内说明放入 `JarkViewer/lib*` 与 `JarkViewer/include`。两个工程默认 `VcpkgEnabled=false`，不会自动为本机恢复依赖。头文件和静态库必须来自相同版本，不能只换头文件。

除定制构建外，依赖基于 vcpkg 的 `x64-windows-static`。需要自己准备时，可在 PowerShell 中运行：

```powershell
$packages = @(
    'x265', 'zlib', 'libyuv', 'minizip[core,bzip2]',
    'exiv2[core,bmff,png,xmp]', 'libavif[core,aom,dav1d]', 'libjxl',
    'libheif[core,hevc]', 'libraw[core,dng-lossy,openmp]', 'lunasvg', 'directxtex',
    'ffmpeg[all,amf,aom,ass,avcodec,avdevice,avfilter,avformat,bzip2,dav1d,fontconfig,freetype,fribidi,iconv,ilbc,lzma,modplug,mp3lame,nvcodec,opencl,opengl,openh264,openjpeg,openmpt,opus,qsv,sdl2,snappy,soxr,speex,srt,ssh,swresample,swscale,theora,vorbis,vpx,vulkan,webp,xml2,zlib]'
)
vcpkg install --triplet x64-windows-static @packages
```

这只是准备依赖，仍需将产物按工程的库目录与链接配置接入。经典模式的 `vcpkg install` 不会升级已安装包；升级使用 `vcpkg upgrade --no-dry-run --triplet x64-windows-static`，随后重新核对链接依赖与解码能力。不要提交机器专属的绝对库路径。

HEIF 相关库至少保留此前修复版本：libheif ≥ 1.22.0、libde265 ≥ 1.0.17。升级时还需核对上游后续修复；这些最低版本不代表长期安全保证。

## 定制库的重建入口

优先使用仓库脚本；默认版本、工具路径、CMake/configure 参数以脚本为准。`-Install` 会替换工程使用的库，操作前检查工作区改动。

```powershell
pwsh tools/build-opencv.ps1 -Install
# 必须在 OpenCV 安装之后执行，否则会被它自带的 zlib 覆盖
pwsh tools/build-zlib-ng.ps1 -Install
pwsh tools/build-ffmpeg-noenc.ps1 -Install
```

### OpenCV 与 zlib-ng

入口是 [build-opencv.ps1](../tools/build-opencv.ps1) 和 [opencv-jarkviewer.patch](../tools/opencv-jarkviewer.patch)。保留以下项目行为：

- 静态 world + contrib + nonfree、Release、`/MT`；项目使用独立 FFmpeg，OpenCV 自带 FFmpeg/GStreamer 关闭。
- 补丁移除 imgcodecs 的宽、高、总像素硬上限，并将 Win32 HighGUI 光标由 `IDC_CROSS` 改为 `IDC_ARROW`。升级后确认补丁仍成功应用。
- IPP / IPP-IW / ITT 当前关闭，以减小静态产物；不要依据旧版本印象改变性能配置，应针对当前实际调用做基准。
- 可用 `-Version`、`-WorkDir` 指定版本和构建目录；其它裁剪参数的理由见脚本，不在文档复制整份配置。

[build-zlib-ng.ps1](../tools/build-zlib-ng.ps1) 用 compat 模式替换 `libopencv/zlib.lib`，不需重建 OpenCV。`zlib.h`、`zconf.h`、`zlib_name_mangling.h` 必须一起匹配，库备份与回退时也要配套恢复。

### FFmpeg

[build-ffmpeg-noenc.ps1](../tools/build-ffmpeg-noenc.ps1) 基于 vcpkg 已打补丁的源码构建，支持 `-VcpkgRoot`、`-Msys2Root`、`-WorkDir`。先在对应 vcpkg 环境安装 FFmpeg，让 `buildtrees/ffmpeg/src` 留下补丁源码；脚本使用 MSVC、MSYS2 bash/make、nasm、pkg-config 与 `ar-lib`。

当前构建以解码为主，关闭编码器但保留 `opus` / `adpcm_g722`：它们的解码路径共用挂在编码开关下的 x86 asm 对象，全部禁用会链接失败。保留解复用器、解码器和 D3D11VA 支持；用 `--probe --hw-test` 验证实际硬解，不能仅凭头文件存在断言可用。

修改脚本时注意：MSVC 的 bin 必须优先于 MSYS2 的同名 `link.exe`；不要禁用 MSYS2 路径转换；configure 路径用 `D:/...`；更改配置后清理旧对象再构建。这些处理已由脚本集中维护，无需手动复刻一套环境。

### 升级后的链接核对

查看器在 `ImageDatabase.h` 使用 `#pragma comment(lib, ...)`，缩略图工程使用 `AdditionalDependencies`，两边都要核对。曾发生过的版本变化可作为排错线索：

| 依赖变化 | 链接影响 |
| --- | --- |
| FFmpeg QSV 使用 oneVPL | `vpl.lib`，不要同时链带重复对象的旧 `libmfx.lib` |
| libssh 使用 Windows CNG | `ncrypt.lib` |
| FFmpeg 引入 SVT-AV1 / TwoLAME | `SvtAv1Enc.lib`、`twolame.lib`，前者还依赖 fastfeat |
| libheif 使用 Brotli | 缩略图工程也需要 `brotlienc.lib` |
| minizip 静态库改名 | 两个工程统一使用对应版本的 `minizips.lib` |
| Abseil 合并目标 | 删除已不存在的旧库名，按实际符号依赖更新 |

这些是排错记录，不是所有未来版本都必须照搬的库清单。

## 工程文件与编译并行

新增、移动或删除查看器源码后运行：

```powershell
python -I tools/gen_vs_project_files.py
python -I tools/gen_vs_project_files.py --check
pwsh tools/check_source_invariants.ps1
```

生成器维护 `JarkViewer.vcxproj` 和 `.filters` 的文件列表及模块分组。新增模块可能需要更新生成器的分类；缩略图工程的条目另行维护，源码不变量检查会检查两个工程。第三方库通常只列头文件，vendored ImGui 源码参与编译，平台无关性差的外部头由生成器排除。

`/m` 只控制工程之间的并行；同一工程内部依赖每个 Debug/Release 配置的 `MultiProcessorCompilation=true`（`/MP`）。不要删除该配置。MSBuild 的 `/p:MultiProcessorCompilation=false` 不能可靠覆盖工程中的条目元数据，做对照实验须确认实际编译命令。

`.ps1` 必须保留 PowerShell 7 守卫和 UTF-8 BOM；无 BOM 时 Windows PowerShell 5.1 可能在执行守卫前就因中文解码失败。不要提交 `.vcxproj.user`、`.vs/`、构建产物或本机配置。
