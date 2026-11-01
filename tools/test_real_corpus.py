"""逐文件测试真实图片语料，原文件只读，程序/设置/报告在独立 TEMP 目录。

python -I tools/test_real_corpus.py D:/Downloads/test --workers 2
报告包含每个文件的解码结果、耗时、退出码，以及测试前后的原文件 SHA-256。
"""

import argparse
from collections import Counter
import concurrent.futures
import hashlib
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
NO_CONSOLE = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def summarize(root):
    """合并独立运行的解码/播放/窗口结果，最后再次核对原图字节与修改时间。"""
    rows = json.loads((root / "results.json").read_text(encoding="utf-8"))
    media = json.loads((root / "playback-results.json").read_text(encoding="utf-8")) if (root / "playback-results.json").exists() else []
    ui = json.loads((root / "ui/results.json").read_text(encoding="utf-8")) if (root / "ui/results.json").exists() else []
    ui_map = {row["index"]: row for row in ui}
    media_map = {}
    for row in media:
        media_map.setdefault(row["file"], []).append(row)
    changed = []
    for row in rows:
        path = Path(row["path"])
        row["sha256_after"] = file_hash(path) if path.exists() else None
        row["source_unchanged"] = row["sha256_after"] == row["sha256_before"] and path.stat().st_mtime_ns == row["mtime_ns"]
        if not row["source_unchanged"]:
            changed.append(row["relative"])
    summary = {"files": len(rows), "decode_passed": sum(row["status"] == "pass" for row in rows),
               "formats": len({row["extension"] for row in rows}), "multi_frame": sum(row.get("frames", 0) > 1 for row in rows),
               "media_files": len(media_map), "playback_checks": len(media), "playback_passed": sum(row["ok"] for row in media),
               "ui_cases": len(ui), "ui_passed": sum(row["ok"] for row in ui), "changed_sources": changed,
               "slowest": [{key: row[key] for key in ("relative", "seconds", "report")} for row in
                           sorted(rows, key=lambda row: row["seconds"], reverse=True)[:10]]}
    for name in ("export-results.json", "interaction-results.json"):
        if (root / name).exists():
            summary[name.removesuffix(".json")] = json.loads((root / name).read_text(encoding="utf-8"))
    (root / "final-summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    (root / "results.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    body = []
    for row in rows:
        shots = ui_map.get(row["index"])
        links = f'<a href="{row["report"].replace(chr(92), "/")}">解码/EXIF</a>'
        if shots:
            prefix = shots["screenshots"].replace(chr(92), "/")
            for name, label in (("01-image.png", "原图"), ("02-next-frame.png", "下一帧"),
                                ("04-info.png", "信息面板"), ("05-rotate-zoom.png", "旋转/缩放")):
                if (root / prefix / name).exists():
                    links += f' · <a href="{prefix}/{name}">{label}</a>'
        for item in media_map.get(row["relative"], []):
            links += f' · <a href="{item["report"].replace(chr(92), "/")}">{item["mode"]}</a>'
        body.append(f'<tr><td>{row["index"]}</td><td>{html.escape(row["relative"])}</td><td>{row["status"]}</td>'
                    f'<td>{row["seconds"]:.3f}</td><td>{row.get("width", "")}×{row.get("height", "")}</td>'
                    f'<td>{row.get("frames", "")}</td><td>{links}</td></tr>')
    page = ('<!doctype html><meta charset="utf-8"><title>JarkViewer 真实图片实测报告</title>'
            '<style>body{font:15px system-ui;margin:24px}td,th{padding:6px 12px;text-align:left}'
            'table{border-collapse:collapse}tr{border-bottom:1px solid #ddd}input{padding:8px;width:460px}</style>'
            '<h1>JarkViewer 真实图片实测</h1>'
            f'<p>解码 {summary["decode_passed"]}/{len(rows)}，{summary["formats"]} 种扩展名，'
            f'多帧 {summary["multi_frame"]}，媒体测试 {summary["playback_passed"]}/{len(media)}，'
            f'窗口冒烟 {summary["ui_passed"]}/{len(ui)}，原文件变化 {len(changed)}。</p>'
            '<p><a href="final-summary.json">完整汇总</a> · <a href="metadata.json">程序哈希/运行配置</a> · '
            '<a href="exports.txt">批量导出结果</a> · <a href="navigation.log">导航交互</a> · '
            '<a href="editor.log">中文文字工具</a></p>'
            '<p>计时包含诊断输出的 I/O：当前 WP2 静态库带逐符号 trace，写文件会增加耗时；'
            '可用 test_probe_redirection.py 对照 NUL、管道、文件与实际窗口。界面截图只覆盖代表样本；'
            '自动解码通过不代表每张图的视觉效果均与其它渲染器逐像素一致。</p>'
            '<input id="filter" placeholder="按目录、文件名或格式筛选" aria-label="筛选文件">'
            '<table><thead><tr><th>#</th><th>文件</th><th>结果</th><th>秒</th><th>尺寸</th><th>帧</th><th>证据</th>'
            '</tr></thead><tbody>' + "".join(body) + '</tbody></table>'
            '<script>document.getElementById("filter").addEventListener("input",e=>{const q=e.target.value.toLowerCase();'
            'document.querySelectorAll("tbody tr").forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(q));});</script>')
    (root / "report.html").write_text(page, encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items() if key != "slowest"}, ensure_ascii=False), flush=True)
    exports = summary.get("export-results", {})
    export_failed = bool(exports) and (exports.get("exit") != 0 or exports.get("inputs") != exports.get("outputs") or bool(exports.get("broken")))
    interaction_failed = any(not check.get("ok", False) for check in summary.get("interaction-results", {}).values())
    return int(bool(changed) or summary["decode_passed"] != len(rows) or summary["playback_passed"] != len(media) or
               summary["ui_passed"] != len(ui) or export_failed or interaction_failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, nargs="?")
    parser.add_argument("--summarize", type=Path, help="合并已有的解码/播放/界面报告，并再次核对原图 SHA-256")
    parser.add_argument("--exe", type=Path, default=REPO / "x64/Release/JarkViewer.exe")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--out", type=Path, help="新的输出目录；默认创建独立 TEMP 目录")
    args = parser.parse_args()
    if args.summarize:
        return summarize(args.summarize.resolve(strict=True))
    if not args.corpus:
        parser.error("请指定语料目录，或使用 --summarize <报告目录>")
    corpus = args.corpus.resolve(strict=True)
    root = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="JarkViewer-real-"))
    if root == corpus or corpus in root.parents:
        parser.error("输出目录必须位于只读语料目录之外")
    if args.out:
        root.mkdir(parents=True, exist_ok=False)
    (root / "app").mkdir()
    (root / "decode").mkdir()
    exe = root / "app/JarkViewer.exe"
    shutil.copy2(args.exe.resolve(strict=True), exe)
    print(f"Reports: {root}", flush=True)

    inventory = []
    for path in sorted(corpus.rglob("*")):
        if path.is_file():
            stat = path.stat()
            inventory.append({"path": str(path), "relative": path.relative_to(corpus).as_posix(),
                              "bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns,
                              "extension": path.suffix.lower(), "sha256_before": file_hash(path)})
    (root / "inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding="utf-8")
    (root / "metadata.json").write_text(json.dumps({"source": str(corpus), "executable": str(args.exe.resolve()),
        "exe_sha256": file_hash(exe), "workers": args.workers, "timeout": args.timeout,
        "files": len(inventory), "bytes": sum(row["bytes"] for row in inventory)}, indent=2), encoding="utf-8")

    def decode(index, item):
        report = root / "decode" / f"{index:04}.txt"
        started = time.monotonic()
        row = dict(item, index=index, report=str(report.relative_to(root)))
        with (root / "decode" / f"{index:04}.log").open("wb") as log:
            try:
                process = subprocess.run([str(exe), "--probe", "--full", item["path"], "--out", str(report)],
                                         cwd=root, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=args.timeout, creationflags=NO_CONSOLE)
                text = report.read_text(encoding="utf-8-sig") if report.exists() else ""
                row["exit"] = process.returncode
                header = next((line for line in text.splitlines() if line.startswith("[")), "")
                row["header"] = header
                match = re.search(r"(\d+)x(\d+) frames=(\d+) ori=(\d+) (\d+)ms", header)
                if match:
                    row.update(zip(("width", "height", "frames", "orientation", "decode_ms"), map(int, match.groups())))
                row["media"] = "| media: video=true" in text
                row["status"] = "pass" if process.returncode == 0 and "1 ok, 0 failed, 1 total" in text else "fail"
                row["aspect_error"] = "!! 宽高比不一致" in text
                if row["aspect_error"]:
                    row["status"] = "fail"
            except subprocess.TimeoutExpired:
                row.update(status="timeout", exit=None)
            except OSError as error:
                row.update(status="error", exit=None, error=str(error))
        row["seconds"] = round(time.monotonic() - started, 3)
        return row

    results = []

    def write_results():
        ordered = sorted(results, key=lambda row: row["index"])
        (root / "results.json").write_text(json.dumps(ordered, ensure_ascii=False, indent=2), encoding="utf-8")
        rows = []
        for row in ordered:
            rows.append(f'<tr class="{row["status"]}"><td>{row["index"]}</td><td>{html.escape(row["relative"])}</td>'
                        f'<td>{row["status"]}</td><td>{row["seconds"]:.3f}</td><td>{row.get("width", "")}×'
                        f'{row.get("height", "")}</td><td>{row.get("frames", "")}</td><td>'
                        f'<a href="{row["report"]}">report</a></td></tr>')
        page = ('<!doctype html><meta charset="utf-8"><title>JarkViewer 真实语料测试</title>'
                '<style>body{font:15px system-ui;margin:24px}td,th{padding:5px 12px;text-align:left}'
                'table{border-collapse:collapse}tr{border-bottom:1px solid #ddd}.fail,.error,.timeout{background:#ffdddd}</style>'
                f'<h1>真实语料测试：{len(ordered)}/{len(inventory)}</h1><p>{html.escape(str(corpus))}</p>'
                '<table><thead><tr><th>#</th><th>文件</th><th>结果</th><th>秒</th><th>尺寸</th><th>帧</th><th>报告</th>'
                '</tr></thead><tbody>' + "".join(rows) + '</tbody></table>')
        (root / "report.html").write_text(page, encoding="utf-8")

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.workers, 4))) as executor:
        futures = [executor.submit(decode, index, item) for index, item in enumerate(inventory)]
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            results.append(row)
            write_results()
            if row["status"] != "pass" or len(results) % 10 == 0 or len(results) == len(inventory):
                print(f'{len(results)}/{len(inventory)} {row["status"]} {row["seconds"]:.2f}s {row["relative"]}', flush=True)

    changed = []
    for row in results:
        path = Path(row["path"])
        row["sha256_after"] = file_hash(path)
        row["source_unchanged"] = row["sha256_before"] == row["sha256_after"] and path.stat().st_mtime_ns == row["mtime_ns"]
        if not row["source_unchanged"]:
            changed.append(row["relative"])
    write_results()
    summary = {"files": len(inventory), "statuses": dict(Counter(row["status"] for row in results)),
               "extensions": dict(Counter(row["extension"] for row in results)),
               "changed_sources": changed, "slowest": sorted(results, key=lambda row: row["seconds"], reverse=True)[:10]}
    (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"files": len(inventory), "statuses": summary["statuses"], "changed_sources": changed},
                     ensure_ascii=True), flush=True)
    return int(bool(changed) or any(row["status"] != "pass" for row in results))


if __name__ == "__main__":
    raise SystemExit(main())
