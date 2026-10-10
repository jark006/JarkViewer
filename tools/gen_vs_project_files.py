#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""按磁盘上的文件重排 JarkViewer 的 VS 工程文件列表与筛选器目录树。

做三件事（都在 JarkViewer/ 下）：
  1. JarkViewer.vcxproj 的 ClInclude / ClCompile / ResourceCompile / Image 条目按
     「自有代码 → 第三方 → 资源」重新生成，新增/移动/删除文件后跑一次即可；
  2. JarkViewer.vcxproj.filters 按模块目录树重新生成（App / Ui / Image / Media /
     Metadata / Core / ThirdParty\<库> / Resources），筛选器 GUID 用名字派生，稳定可重复；
  3. 顺带报告：自有源文件没归类的、第三方里被排除的（平台相关头文件，IntelliSense 解不了）。

第三方只列**头文件**（.lib 是预编译好的，源码不参与本工程编译），并按库分目录；
平台相关头文件（CUDA/DRM/VAAPI/Vulkan/OpenCL/Android JNI/mediacodec/videotoolbox/vdpau/qsv）
列在 EXCLUDED 里不进工程，避免 IntelliSense 报一堆解析错误。

用法（仓库根目录）：python -I tools/gen_vs_project_files.py [--check]
加 --check 只报告差异、不写文件（供 CI/自检用）。
"""

import argparse
import os
import re
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_DIR = os.path.join(ROOT, "JarkViewer")
VCXPROJ = os.path.join(PROJECT_DIR, "JarkViewer.vcxproj")
FILTERS = os.path.join(PROJECT_DIR, "JarkViewer.vcxproj.filters")

# 自有代码的模块划分：文件名（不含扩展名）-> 筛选器
OWN_GROUPS = {
    # App 是程序宿主与顶层窗口（视频播放器窗口整轮接管，和看图窗口互不构造）
    "App": {"main", "D3D11App", "UiHost", "InfoScreen", "DecodeProbe", "VideoPlayerApp"},
    "Ui": {"NavigationOverlay", "SettingWindow", "PrintWindow", "BatchWindow",
           "EditorWindow", "RenameWindow"},
    "Image": {"ImageDatabase", "FormatSniffer", "VectorImage", "SVGPreprocessor",
              "CanvasRenderer", "ImageResampler", "TextRenderer", "ImageAnnotator",
              "ImageAdjust", "ColorManager", "blpDecoder", "videoDecoder"},
    "Media": {"MediaDecoder", "MediaPlayer", "AudioOutput", "VideoPlayback",
              "AudioSpectrumAnalyzer"},
    "Metadata": {"exifParse", "AiPrompt"},
    "Core": {"jarkUtils", "Localization", "stringRes", "LRU", "ThumbnailService",
             "FileAssociationManager", "FileAssociationNaming", "ThumbnailRegistrar",
             "ThumbnailProviderGuids", "framework", "targetver", "BatchProcessor",
             "MappedFileReader"},
    "ThirdParty\\tinyxml2": {"tinyxml2"},
}
OWN_BY_STEM = {stem: group for group, stems in OWN_GROUPS.items() for stem in stems}

# 第三方库目录（include 下）-> 筛选器名
THIRD_PARTY_DIRS = {
    "aom": "aom", "avif": "libavif", "dav1d": "dav1d", "exiv2": "exiv2",
    "ffmpeg": "ffmpeg", "jxl": "libjxl", "libde265": "libde265",
    "libheif": "libheif", "libraw": "libraw", "libwebp2": "libwebp2",
    "libyuv": "libyuv", "minizip": "minizip", "opencv2": "opencv2",
    "psdsdk": "psdsdk",
}

# 平台相关/外来头文件：本机 Windows 环境解不了，不进工程（仍可由包含路径使用）
EXCLUDED = (
    "cuda", "drm", "vaapi", "va_intel", "vulkan", "opencl", "mediacodec",
    "videotoolbox", "vdpau", "jni.h", "qsv", "metal", "hwcontext_d3d1",
)

# 资源
RESOURCE_FILES = ("JarkViewer.rc",)
IMAGE_FILES = ("JarkViewer.ico", "small.ico", "file\\mainRes.png", "file\\aboutIcon.png")


def rel(path):
    return os.path.relpath(path, PROJECT_DIR).replace("/", "\\")


def classify(relative):
    """返回 (筛选器, 条目类型)；条目类型 None 表示不收录。"""
    lower = relative.lower()
    if lower in (x.lower() for x in RESOURCE_FILES):
        return "Resources", "ResourceCompile"
    if lower in (x.lower() for x in IMAGE_FILES):
        return "Resources", "Image"

    parts = relative.split("\\")
    name = parts[-1]

    # 自有源码/头文件
    if len(parts) == 2 and parts[0] in ("src", "include"):
        stem = os.path.splitext(name)[0]
        if stem in OWN_BY_STEM:
            return OWN_BY_STEM[stem], "ClInclude" if lower.endswith(".h") else "ClCompile"

    # vendored imgui（参与编译的第三方源码）
    if parts[0] == "vendor":
        sub = parts[1:-1]
        return "\\".join(["ThirdParty"] + [s for s in sub]), \
            "ClInclude" if lower.endswith((".h", ".hpp")) else "ClCompile"

    if parts[0] == "include":
        if any(x in lower for x in EXCLUDED):
            return None, None
        if len(parts) == 2:  # 单文件头库（stb/qoi/lunasvg/lcms2/zlib/x265/…）
            return "ThirdParty\\single-header", "ClInclude"
        library = THIRD_PARTY_DIRS.get(parts[1])
        if library:
            sub = parts[2:-1]
            return "\\".join(["ThirdParty", library] + sub), "ClInclude"

    # 其它（如 include\targetver.h 这类未使用的样板）
    return None, None


def collect():
    items = {}      # 类型 -> [相对路径]
    unclassified = []
    excluded = []
    for base, dirs, files in os.walk(PROJECT_DIR):
        dirs[:] = [d for d in dirs if d not in (".vs", "x64")]
        for name in files:
            if not name.lower().endswith((".cpp", ".c", ".h", ".hpp", ".rc", ".ico", ".png")):
                continue
            relative = rel(os.path.join(base, name))
            if relative.lower().startswith("x64\\") or relative.lower().startswith(".vs\\"):
                continue
            group, kind = classify(relative)
            if kind is None:
                if relative.startswith(("src\\", "include\\")) or relative.startswith("vendor\\"):
                    (excluded if any(x in relative.lower() for x in EXCLUDED) else unclassified).append(relative)
                continue
            items.setdefault(kind, []).append(relative)
    for kind in items:
        items[kind].sort(key=lambda p: (group_order(p)[0], group_order(p)[1], p.lower()))
    return items, unclassified, excluded


def group_order(relative):
    """排序键：先按筛选器归属的模块顺序，再按文件名。"""
    group, _kind = classify(relative)
    order = ["App", "Ui", "Image", "Media", "Metadata", "Core", "ThirdParty", "Resources"]
    head = group.split("\\")[0] if group else "zzz"
    try:
        index = order.index(head)
    except ValueError:
        index = len(order)
    return (index, relative.lower())


def build_item_groups(items):
    lines = []
    for kind in ("ClCompile", "ClInclude", "ResourceCompile", "Image"):
        entries = items.get(kind, [])
        if not entries:
            continue
        lines.append("  <ItemGroup>")
        for entry in entries:
            lines.append('    <%s Include="%s" />' % (kind, entry))
        lines.append("  </ItemGroup>")
    return "\n".join(lines) + "\n"


def build_filters(items):
    # 目录树：把所有条目的筛选器路径展开成节点
    nodes = set()
    for kind, entries in items.items():
        for entry in entries:
            group, _ = classify(entry)
            if not group:
                continue
            parts = group.split("\\")
            for i in range(len(parts)):
                nodes.add("\\".join(parts[:i + 1]))

    def unique_id(name):
        return "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "jarkviewer/" + name)).upper()

    out = ['<?xml version="1.0" encoding="utf-8"?>',
           '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
           "  <ItemGroup>"]
    for name in sorted(nodes, key=lambda s: (s.count("\\"), s)):
        out.append('    <Filter Include="%s">' % name)
        out.append("      <UniqueIdentifier>%s</UniqueIdentifier>" % unique_id(name))
        out.append("    </Filter>")
    out.append("  </ItemGroup>")

    for kind in ("ClCompile", "ClInclude", "ResourceCompile", "Image"):
        entries = items.get(kind, [])
        if not entries:
            continue
        out.append("  <ItemGroup>")
        for entry in entries:
            group, _ = classify(entry)
            out.append('    <%s Include="%s">' % (kind, entry))
            out.append("      <Filter>%s</Filter>" % (group or "Other"))
            out.append("    </%s>" % kind)
        out.append("  </ItemGroup>")

    out.append("</Project>")
    return "\n".join(out) + "\n"


def rewrite_vcxproj(items):
    text = open(VCXPROJ, encoding="utf-8").read()
    text = re.sub(r"\n  <ItemGroup>\n(?:    <(?:ClCompile|ClInclude|ResourceCompile|Image) [^\n]*\n)*  </ItemGroup>",
                  "\n", text)
    text = re.sub(r"\n  <ItemGroup>\n  </ItemGroup>", "", text)
    marker = '  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />'
    if marker not in text:
        raise SystemExit("找不到 Import 行，vcxproj 结构变了，需手工调整脚本")
    text = text.replace(marker, build_item_groups(items) + "\n" + marker)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="只报告差异，不写文件")
    args = parser.parse_args()

    items, unclassified, excluded = collect()
    groups = build_item_groups(items)
    filters = build_filters(items)
    vcxproj = rewrite_vcxproj(items)

    counts = {k: len(v) for k, v in items.items()}
    print("条目：", ", ".join("%s=%d" % kv for kv in sorted(counts.items())))
    if unclassified:
        print("未归类（脚本的 OWN_GROUPS 需要补上）：")
        for entry in unclassified:
            print("   ", entry)
    if excluded:
        print("按平台排除（不进工程，仍可用）：", len(excluded), "个")

    current_vcxproj = open(VCXPROJ, encoding="utf-8").read()
    current_filters = open(FILTERS, encoding="utf-8").read() if os.path.exists(FILTERS) else ""
    changed = (current_vcxproj != vcxproj) or (current_filters != filters)
    print("需要更新" if changed else "已是最新")

    if args.check:
        return 1 if changed else 0
    if changed:
        open(VCXPROJ, "w", encoding="utf-8", newline="\n").write(vcxproj)
        open(FILTERS, "w", encoding="utf-8", newline="\n").write(filters)
        print("已写入 %s / %s" % (os.path.basename(VCXPROJ), os.path.basename(FILTERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
