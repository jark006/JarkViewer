#pragma once

// 无界面解码自检：`JarkViewer.exe --probe <文件...> [--out <报告路径>]`
//
// 用途：在没有窗口/人眼的情况下验证解码路由（文件头嗅探、扩展名兜底、EXIF 处理）是否正确。
// 结果同时输出到控制台与报告文件（默认 decode-probe.txt，UTF-8）。

#include <string>
#include <vector>

namespace jark {

// argv 为完整命令行参数（argv[0] 为可执行文件路径）；返回进程退出码
int runDecodeProbe(const std::vector<std::wstring>& argv);

} // namespace jark
