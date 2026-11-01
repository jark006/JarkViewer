"""核对 --probe 尊重 stdout/stderr 重定向，并对照真实窗口的原图接管时间。

python -I tools/test_probe_redirection.py --image D:/Downloads/test/wp2/AndroidWallpaper.wp2
使用 TEMP 内独立 exe/设置；原图只读。适用于会大量输出诊断的第三方解码器。
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import os


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path(__file__).resolve().parent.parent / "x64/Release/JarkViewer.exe")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    image = args.image.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-probe-stdio-"))
    exe = root / "JarkViewer.exe"
    shutil.copy2(args.exe.resolve(strict=True), exe)
    original_hash = digest(image)
    results = []
    expected_size = None
    print(f"Reports: {root}", flush=True)

    def record(row):
        results.append(row)
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f'{"PASS" if row["ok"] else "FAIL"} {row["mode"]}: {row.get("seconds", 0):.3f}s '
              f'decode={row.get("decode_ms", "-")}ms stderr={row.get("stderr_bytes", "-")}B', flush=True)

    for mode in ("nul", "pipe", "file"):
        report = root / f"{mode}.txt"
        command = [str(exe), "--probe", str(image), "--out", str(report)]
        started = time.monotonic()
        row = {"mode": mode, "ok": False}
        try:
            if mode == "nul":
                process = subprocess.run(command, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         timeout=args.timeout, creationflags=subprocess.CREATE_NO_WINDOW)
            elif mode == "pipe":
                process = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         timeout=args.timeout, creationflags=subprocess.CREATE_NO_WINDOW)
                row["stdout_bytes"] = len(process.stdout)
                row["stderr_bytes"] = len(process.stderr)
                row["stderr_prefix"] = process.stderr[:500].decode("utf-8", errors="replace")
                (root / "pipe.stdout").write_bytes(process.stdout)
            else:
                with (root / "file.stdout").open("wb") as output:
                    process = subprocess.run(command, cwd=root, stdout=output, stderr=subprocess.DEVNULL,
                                             timeout=args.timeout, creationflags=subprocess.CREATE_NO_WINDOW)
                row["stdout_bytes"] = (root / "file.stdout").stat().st_size
            text = report.read_text(encoding="utf-8-sig") if report.exists() else ""
            row.update(exit=process.returncode, ok=process.returncode == 0 and "1 ok, 0 failed, 1 total" in text)
            if mode != "nul":
                captured = process.stdout if mode == "pipe" else (root / "file.stdout").read_bytes()
                row["stdout_preserved"] = b"1 ok, 0 failed, 1 total" in captured
                row["ok"] &= row["stdout_preserved"]
            elapsed = re.search(r"ori=\d+ (\d+)ms", text)
            if elapsed:
                row["decode_ms"] = int(elapsed[1])
            dimensions = re.search(r" (\d+)x(\d+) frames=", text)
            if dimensions:
                expected_size = f" {dimensions[1]}x{dimensions[2]} "
        except (subprocess.TimeoutExpired, OSError) as error:
            row["error"] = str(error)
        row["seconds"] = round(time.monotonic() - started, 3)
        record(row)

    # 先看完整原图接管 trace；小图可能在 initOpenFile 前已解码完、没有 adopted 标记，
    # 此时核对原图尺寸标题，不能把启动主页/缩略图首帧当作原图。
    trace = root / "gui-trace.txt"
    started = time.monotonic()
    process = subprocess.Popen([str(exe), str(image)], cwd=root,
                               env=dict(os.environ, JARKVIEWER_STARTUP_TRACE=str(trace)),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    row = {"mode": "gui-original-image", "ok": False}
    user = ct.windll.user32
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
    user.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def ready_title():
        titles = []

        @callback_type
        def visit(hwnd, _):
            owner = wt.DWORD()
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            if owner.value == process.pid:
                title = ct.create_unicode_buffer(2048)
                user.GetWindowTextW(hwnd, title, len(title))
                if expected_size and expected_size in title.value:
                    titles.append(title.value)
            return True

        user.EnumWindows(visit, 0)
        return titles[0] if titles else None
    try:
        while time.monotonic() - started < args.timeout:
            if process.poll() is not None:
                raise RuntimeError(f"window exited early: {process.returncode}")
            text = trace.read_text(encoding="utf-8", errors="replace") if trace.exists() else ""
            adopted = re.search(r"image adopted \+(\d+) ms", text)
            if adopted:
                row.update(ok=True, adopted_ms=int(adopted[1]))
                break
            if original_title := ready_title():
                row.update(ok=True, title=original_title)
                break
            time.sleep(0.02)
        row["seconds"] = round(time.monotonic() - started, 3)
    finally:
        @callback_type
        def close(hwnd, _):
            owner = wt.DWORD()
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            name = ct.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, name, len(name))
            if owner.value == process.pid and name.value == "D3D11WndClass":
                user.PostMessageW(hwnd, 0x10, 0, 0)
            return True

        user.EnumWindows(close, 0)
        try:
            row["exit"] = process.wait(timeout=args.timeout)
            row["ok"] &= row["exit"] == 0
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            row.update(ok=False, error="window close timed out")
    record(row)
    unchanged = digest(image) == original_hash
    metadata = {"image": str(image), "image_sha256": original_hash, "source_unchanged": unchanged,
                "exe_sha256": digest(exe), "timeout": args.timeout}
    (root / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f'{sum(row["ok"] for row in results)}/{len(results)} passed; source unchanged={unchanged}', flush=True)
    return int(not unchanged or any(not row["ok"] for row in results))


if __name__ == "__main__":
    raise SystemExit(main())
