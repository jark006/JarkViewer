<#
.SYNOPSIS
    用源码构建 JarkViewer 需要的 OpenCV 静态库（vcpkg 里没有它）。

.DESCRIPTION
    README「静态库」一节说明：工程里的第三方静态库除 OpenCV 外都来自 vcpkg，
    只有 OpenCV 是源码自建，并且带 2 处源码修改。这个脚本把整件事串起来：
    拉源码 → 打补丁（tools/opencv-jarkviewer.patch）→ CMake 配置 → 编译 → 安装。

    关键配置（4.13.0 那版是怎么配的，这里就怎么配，逐项对齐）：
      * 静态库、/MT（BUILD_WITH_STATIC_CRT=ON）、只出 Release；
      * world + opencv_contrib + nonfree；
      * 自带 3rdparty：jpeg/png/tiff/webp/openjpeg/openexr/zlib；
      * IPP / IPP IW / ITT 一律关闭（见下）；
      * Windows 后端：DirectX(D3D9/10/11)、DirectShow、MSMF；OpenCL 打开；
      * 关掉：dnn/objdetect/datasets（连带 aruco、face、text、wechat_qrcode 等不参与编译）、
        CUDA、OpenGL、Vulkan、GDAL、GDCM、Jasper、AVIF、JPEG XL、FFmpeg、GStreamer；
      * CPU_BASELINE=SSE3、CPU_DISPATCH=SSE4_1;SSE4_2;AVX;FP16;AVX2;AVX512_SKX
        （不抬高基线，其余按运行时派发）。

    为什么关掉 IPP（Intel Integrated Performance Primitives）：
      * 体积：IPP 的静态 blob 一项就占 exe 约 25 MiB（ippicvmt.lib 单独 67 MB）。
      * 覆盖：4.14 里 IPP 早就不管看图软件的主路径了——resize / warpAffine / cvtColor /
        imdecode / imencode 的源码里一个 ippi* 调用都没有（ippiResize、ippiWarpAffine、
        ippiColorToGray 全为 0 处），imgcodecs 整个模块也是 0 处。Mat::convertTo 的 IPP
        调用被上游注释掉了（/* [TODO] Recover IPP calls */）。
      * 实测：用同一份 opencv_world 编基准、以 OPENCV_IPP=disabled 对拍 21 项操作
        （4000x3000、12 线程），显示主路径全部无差异；关掉 IPP 反而更快的有多项
        （copyTo 2.4 倍、moments 1.9 倍、morphologyEx 1.8 倍）；只有 bilateralFilter
        与 Sobel 是 IPP 真的更快，而本工程一次都没调用。
      * 另外：ippicv 是 Intel 的闭源预编译二进制（构建时从 opencv_3rdparty 下载），
        关掉之后整套 OpenCV 只依赖一个官方源码 tarball，构建可复现。
      改回开启只需把下面三行改回 ON 并重新构建，但先看一遍上面这段实测数据。

    用 -Install 时把产物复制进工程：静态库进 JarkViewer/libopencv/，
    头文件整目录替换 JarkViewer/include/opencv2/。

    注意：本脚本要在 pwsh（PowerShell 7）下运行；Windows PowerShell 5.1 读无 BOM
    的 .ps1 会按 ANSI 解码，中文注释会被解成乱码。

.EXAMPLE
    pwsh tools/build-opencv.ps1                       # 只构建，产物留在构建目录
    pwsh tools/build-opencv.ps1 -Install              # 构建并替换工程里的库与头文件
    pwsh tools/build-opencv.ps1 -Version 4.15.0 -Install
#>
param(
    [string]$Version = "4.14.0",
    [string]$WorkDir = "D:\workSpace\vs",
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appRoot = Join-Path $repoRoot "JarkViewer"
$patch = Join-Path $PSScriptRoot "opencv-jarkviewer.patch"

$srcDir = Join-Path $WorkDir "opencv-$Version"
$contribDir = Join-Path $WorkDir "opencv_contrib-$Version"
$buildDir = Join-Path $WorkDir ("opencv-" + $Version + "_build")
$installDir = Join-Path $buildDir "install"

$cmake = "C:\Program Files\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) { throw "找不到 cmake.exe，请安装 CMake（或改脚本里的路径）" }
}

if (-not (Test-Path -LiteralPath $patch)) { throw "找不到补丁 $patch" }

# 1. 源码
# 判存在用的标记不同：opencv 根目录有 CMakeLists.txt，opencv_contrib 只有 modules/
foreach ($item in @(@{ Dir = $srcDir; Repo = "https://github.com/opencv/opencv.git"; Marker = "CMakeLists.txt" },
                    @{ Dir = $contribDir; Repo = "https://github.com/opencv/opencv_contrib.git"; Marker = "modules" })) {
    if (Test-Path -LiteralPath (Join-Path $item.Dir $item.Marker)) { continue }
    Write-Host "克隆 $($item.Repo) (tag $Version) ..." -ForegroundColor Cyan
    & git clone --depth 1 --branch $Version $item.Repo $item.Dir
    if ($LASTEXITCODE -ne 0) { throw "克隆失败：$($item.Repo)" }
}

# 2. 补丁（幂等：已经打过就跳过）
$loadsave = Join-Path $srcDir "modules\imgcodecs\src\loadsave.cpp"
if ((Get-Content -LiteralPath $loadsave -Raw) -match "CV_IO_MAX_IMAGE_PIXELS = LLONG_MAX") {
    Write-Host "补丁已应用，跳过"
}
else {
    Write-Host "应用 $patch ..." -ForegroundColor Cyan
    Push-Location $srcDir
    try {
        & git apply -p1 --verbose $patch
        if ($LASTEXITCODE -ne 0) { throw "补丁没有干净应用（源码版本对不上？）" }
    }
    finally { Pop-Location }
}

# 3. 配置
#    下载缓存放在源码目录的 .cache 下（ade、xfeatures2d 的测试数据），
#    换版本时若缓存里的版本对不上，CMake 会自己重新下载。
$options = @(
    "-DCMAKE_INSTALL_PREFIX=$installDir",
    "-DCMAKE_CONFIGURATION_TYPES=Release",
    "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_WITH_STATIC_CRT=ON",
    "-DBUILD_opencv_world=ON",
    "-DOPENCV_EXTRA_MODULES_PATH=$contribDir/modules",
    "-DOPENCV_ENABLE_NONFREE=ON",
    "-DOPENCV_ENABLE_ALLOCATOR_STATS=ON", "-DOPENCV_ENABLE_ATOMIC_LONG_LONG=ON",
    "-DOPENCV_ENABLE_MEMALIGN=ON",
    "-DOPENCV_GENERATE_SETUPVARS=ON", "-DOPENCV_MSVC_PARALLEL=ON",
    "-DOPENCV_DOWNLOAD_PATH=$srcDir/.cache",

    "-DWITH_IPP=OFF", "-DBUILD_IPP_IW=OFF",
    "-DWITH_ITT=OFF", "-DBUILD_ITT=OFF",
    "-DWITH_DIRECTX=ON", "-DWITH_DIRECTML=OFF",
    "-DWITH_MSMF=ON", "-DWITH_DSHOW=ON", "-DWITH_OBSENSOR=OFF",
    "-DWITH_OPENCL=ON",

    "-DWITH_OPENEXR=ON", "-DBUILD_OPENEXR=ON",
    "-DWITH_OPENJPEG=ON", "-DBUILD_OPENJPEG=ON",
    "-DWITH_JPEG=ON", "-DBUILD_JPEG=ON",
    "-DWITH_PNG=ON", "-DBUILD_PNG=ON",
    "-DWITH_TIFF=ON", "-DBUILD_TIFF=ON",
    "-DWITH_WEBP=ON", "-DBUILD_WEBP=ON",
    "-DWITH_ZLIB=ON", "-DBUILD_ZLIB=ON",
    "-DWITH_JASPER=OFF", "-DBUILD_JASPER=OFF",

    "-DWITH_FFMPEG=OFF", "-DWITH_GSTREAMER=OFF",
    "-DWITH_OPENGL=OFF", "-DWITH_VULKAN=OFF", "-DWITH_GDAL=OFF", "-DWITH_GDCM=OFF",
    "-DWITH_EIGEN=OFF", "-DWITH_LAPACK=OFF", "-DWITH_TBB=OFF", "-DWITH_OPENMP=OFF",
    "-DWITH_JPEGXL=OFF", "-DWITH_AVIF=OFF", "-DWITH_OPENVX=OFF",

    "-DBUILD_opencv_dnn=OFF", "-DBUILD_opencv_objdetect=OFF", "-DBUILD_opencv_datasets=OFF",
    "-DBUILD_opencv_dnn_objdetect=OFF", "-DBUILD_opencv_dnn_superres=OFF", "-DBUILD_opencv_ts=OFF",
    "-DBUILD_opencv_apps=OFF", "-DBUILD_opencv_python3=OFF", "-DBUILD_JAVA=OFF",
    "-DBUILD_TESTS=OFF", "-DBUILD_PERF_TESTS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_DOCS=OFF",

    "-DCPU_BASELINE=SSE3",
    "-DCPU_DISPATCH=SSE4_1;SSE4_2;AVX;FP16;AVX2;AVX512_SKX",
    "-DENABLE_PIC=ON",

    "-S", $srcDir, "-B", $buildDir
)

Write-Host "=== 配置 OpenCV $Version ===" -ForegroundColor Cyan
& $cmake "-G" "Visual Studio 18 2026" "-A" "x64" @options
if ($LASTEXITCODE -ne 0) { throw "配置失败" }

Write-Host "=== 编译 ===" -ForegroundColor Cyan
& $cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

Write-Host "=== 安装 ===" -ForegroundColor Cyan
& $cmake --build $buildDir --config Release --target INSTALL
if ($LASTEXITCODE -ne 0) { throw "安装失败" }

$staticLib = Join-Path $installDir "x64\vc18\staticlib"
$worldLib = Get-ChildItem -LiteralPath $staticLib -Filter "opencv_world*.lib" | Select-Object -First 1
if (-not $worldLib) { throw "没有生成 opencv_world*.lib" }
Write-Host ""
Write-Host ("产物 {0}  {1:N0} MB" -f $worldLib.Name, ($worldLib.Length / 1MB))

if (-not $Install) {
    Write-Host "未安装到工程。加 -Install 可替换 JarkViewer/libopencv 与 include/opencv2。" -ForegroundColor Yellow
    return
}

# 4. 复制进工程。只搬工程真正链接的那些（与 ImageDatabase.h 的 #pragma 清单对应）：
#    world 本体 + 它自带的 3rdparty。ade / libprotobuf / opencv_img_hash 用不到，不搬；
#    ipphal / ippicvmt / ippiw / ittnotify 随 WITH_IPP / WITH_ITT 关闭而不再产生。
$libNames = @("IlmImf", "libjpeg-turbo",
    "libopenjp2", "libpng", "libtiff", "libwebp", "zlib", $worldLib.BaseName)
$targetLibDir = Join-Path $appRoot "libopencv"
foreach ($name in $libNames) {
    $from = Join-Path $staticLib "$name.lib"
    if (-not (Test-Path -LiteralPath $from)) { throw "缺库文件 $name.lib" }
    Copy-Item -LiteralPath $from -Destination (Join-Path $targetLibDir "$name.lib") -Force
    Write-Host "  库 $name.lib"
}
# 清掉上一版的 opencv_world<旧版本号>.lib，留着只会让链接器找到两套符号
Get-ChildItem -LiteralPath $targetLibDir -Filter "opencv_world*.lib" |
    Where-Object { $_.Name -ne "$($worldLib.BaseName).lib" } |
    ForEach-Object {
        Write-Host "  删除旧库 $($_.Name)" -ForegroundColor Yellow
        Remove-Item -LiteralPath $_.FullName
    }

$targetInclude = Join-Path $appRoot "include\opencv2"
if (Test-Path -LiteralPath $targetInclude) { Remove-Item -Recurse -Force -LiteralPath $targetInclude }
Copy-Item -Recurse -LiteralPath (Join-Path $installDir "include\opencv2") -Destination $targetInclude
Write-Host "  头文件 include\opencv2\"

Write-Host ""
Write-Host "装好了。接着做：" -ForegroundColor Green
Write-Host "  1. 确认 ImageDatabase.h 里的 opencv_world<版本号>.lib 与 $($worldLib.BaseName).lib 一致"
Write-Host "  2. pwsh tools/build-zlib-ng.ps1 -Install    # 把 libopencv/zlib.lib 换成 zlib-ng 的 compat 构建"
Write-Host "  3. pwsh ./buildRelease.ps1"
