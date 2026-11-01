"""按真实语料解码报告测试其中所有实况/视频的完整播放，并验证独立视频 seek 与硬解。

python -I tools/test_real_corpus_playback.py <test_real_corpus.py 的报告目录>
所有媒体依次播放、静音，避免并发争用声卡导致流畅度测试误报。
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path)
    args = parser.parse_args()
    root = args.reports.resolve(strict=True)
    records = json.loads((root / "results.json").read_text(encoding="utf-8"))
    selected = [row for row in records if row["status"] == "pass" and
                "| media: video=true" in (root / row["report"]).read_text(encoding="utf-8-sig")]
    destination = root / "playback"
    destination.mkdir(exist_ok=True)
    results = []
    for row in selected:
        modes = ["playback"]
        # seek/hw 入口针对独立媒体，实况字节的提取由 playback 入口负责。
        if row["extension"] in {".mov", ".mp4", ".mkv", ".avi", ".webm"}:
            modes += ["video", "hw"]
        for mode in modes:
            report = destination / f'{row["index"]:04}-{mode}.txt'
            start = time.monotonic()
            result = {"file": row["relative"], "mode": mode, "report": str(report.relative_to(root)), "ok": False}
            try:
                process = subprocess.run([str(root / "app/JarkViewer.exe"), "--probe", f"--{mode}-test",
                                          row["path"], "--out", str(report)], cwd=root,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
                text = report.read_text(encoding="utf-8-sig") if report.exists() else ""
                summary = next((line for line in reversed(text.splitlines()) if line.startswith("---- ")), "")
                result.update(exit=process.returncode, summary=summary, assertions=text.count("[ok]"),
                              failures=[line for line in text.splitlines() if "[FAIL]" in line])
                result["ok"] = process.returncode == 0 and "0 failed, 0 skipped" in summary and "FAIL" not in text
                match = re.search(r"交付 (\d+)/(\d+) 帧 \((\d+)%\).*?最大 ([\d.]+)ms.*?时钟 ([\d.]+) 倍速", text)
                if match:
                    result.update(delivered=int(match[1]), expected=int(match[2]), percent=int(match[3]),
                                  max_gap_ms=float(match[4]), clock_ratio=float(match[5]))
            except (subprocess.TimeoutExpired, OSError) as error:
                result["error"] = str(error)
            result["seconds"] = round(time.monotonic() - start, 3)
            results.append(result)
            (root / "playback-results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f'{len(results)} {"PASS" if result["ok"] else "FAIL"} {mode} {row["relative"]}', flush=True)
    print(f'{len(selected)} media files; {sum(row["ok"] for row in results)}/{len(results)} checks passed', flush=True)
    return int(not results or any(not row["ok"] for row in results))


if __name__ == "__main__":
    raise SystemExit(main())
