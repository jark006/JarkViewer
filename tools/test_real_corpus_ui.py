"""读取 test_real_corpus.py 的报告，按格式/目录挑选真实图片进行窗口冒烟。

python -I tools/test_real_corpus_ui.py <报告目录>
只操作临时 exe 的窗口和设置，原图片只读。截图只抓程序客户区。
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from PIL import Image, ImageChops, ImageDraw, ImageStat


def export_images(root, records):
    samples = json.loads((root / "ui/results.json").read_text(encoding="utf-8"))
    indices = {row["index"] for row in samples if row["ok"]}
    files = [row["path"] for row in records if row["index"] in indices and row["extension"] != ".mov"]
    output = Path(tempfile.mkdtemp(prefix="exports-", dir=root))
    report = root / "exports.txt"
    started = time.monotonic()
    process = subprocess.run([str(root / "app/JarkViewer.exe"), "--probe", "--batch", *files,
                              "--out-dir", str(output), "--to", "png", "--out", str(report)], cwd=root,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    images = list(output.glob("*.png"))
    broken = []
    for path in images:
        try:
            with Image.open(path) as image:
                image.verify()
        except Exception as error:
            broken.append({"file": str(path), "error": str(error)})
    result = {"inputs": len(files), "outputs": len(images), "exit": process.returncode, "broken": broken,
              "output_directory": str(output.relative_to(root)), "seconds": round(time.monotonic() - started, 3)}
    (root / "export-results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
    return int(process.returncode != 0 or not files or len(files) != len(images) or bool(broken))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path)
    parser.add_argument("--only", help="只复测逗号分隔的报告 index，结果合并回原报告")
    parser.add_argument("--export-only", action="store_true", help="将已通过的窗口样本另存为 PNG 并完整性校验，原图不写入")
    args = parser.parse_args()
    root = args.reports.resolve(strict=True)
    rows = json.loads((root / "results.json").read_text(encoding="utf-8"))
    if args.export_only:
        return export_images(root, rows)
    selected = {}
    for row in rows:
        if row["status"] != "pass":
            continue
        # 每种扩展名与每个目录各覆盖至少一张，再补齐下列特定语义的样本。
        selected.setdefault(row["extension"], row)
        selected.setdefault(row["relative"].split("/")[0], row)
    # 另外补齐 RAW/PSD、透明、高位深、超长中文/emoji 文件名与不同实况结构。
    for row in rows:
        if row["extension"] in {".dng", ".cr3", ".psd", ".mov", ".livp"} or \
                any(word in row["relative"] for word in ("Galaxy", "Apple iPhone 15.HEIC", "😈", "car.svg", "AA_5.svg")):
            selected[row["relative"]] = row
    selected = sorted({row["index"]: row for row in selected.values()}.values(), key=lambda row: row["index"])
    if args.only:
        indices = set(map(int, args.only.split(",")))
        selected = [row for row in selected if row["index"] in indices]
    destination = root / "ui"
    destination.mkdir(exist_ok=True)
    app = destination / "app"
    app.mkdir(exist_ok=True)
    exe = app / "JarkViewer.exe"
    shutil.copy2(root / "app/JarkViewer.exe", exe)
    user, gdi, kernel = ct.windll.user32, ct.windll.gdi32, ct.windll.kernel32
    user.SetProcessDPIAware()
    user.GetForegroundWindow.restype = wt.HWND
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
    user.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
    user.GetWindowRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
    user.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ct.c_int]
    user.ShowWindow.argtypes = [wt.HWND, ct.c_int]
    user.MoveWindow.argtypes = [wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.BOOL]
    user.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    user.GetDC.argtypes = [wt.HWND]
    user.GetDC.restype = wt.HDC
    user.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
    user.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
    gdi.CreateCompatibleDC.argtypes = [wt.HDC]
    gdi.CreateCompatibleDC.restype = wt.HDC
    gdi.CreateCompatibleBitmap.argtypes = [wt.HDC, ct.c_int, ct.c_int]
    gdi.CreateCompatibleBitmap.restype = wt.HBITMAP
    gdi.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
    gdi.SelectObject.restype = wt.HGDIOBJ
    gdi.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ct.c_void_p, ct.c_void_p, wt.UINT]
    gdi.DeleteObject.argtypes = [wt.HGDIOBJ]
    gdi.DeleteDC.argtypes = [wt.HDC]
    callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    class BitmapInfo(ct.Structure):
        _fields_ = [("size", wt.DWORD), ("width", wt.LONG), ("height", wt.LONG),
                    ("planes", wt.WORD), ("bits", wt.WORD), ("compression", wt.DWORD),
                    ("image_bytes", wt.DWORD), ("xppm", wt.LONG), ("yppm", wt.LONG),
                    ("used", wt.DWORD), ("important", wt.DWORD)]

    def find_window(pid):
        handles = []

        @callback_type
        def visit(hwnd, _):
            owner = wt.DWORD()
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            name = ct.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, name, len(name))
            if owner.value == pid and user.IsWindowVisible(hwnd) and name.value == "D3D11WndClass":
                handles.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return handles[0] if handles else None

    def title(hwnd):
        buffer = ct.create_unicode_buffer(2048)
        user.GetWindowTextW(hwnd, buffer, len(buffer))
        return buffer.value

    def focus(hwnd):
        foreground = user.GetForegroundWindow()
        other_thread = user.GetWindowThreadProcessId(foreground, None)
        current = kernel.GetCurrentThreadId()
        attached = user.AttachThreadInput(current, other_thread, True)
        user.ShowWindow(hwnd, 9)
        user.BringWindowToTop(hwnd)
        user.SetForegroundWindow(hwnd)
        if attached:
            user.AttachThreadInput(current, other_thread, False)
        if user.GetForegroundWindow() != hwnd:
            raise RuntimeError("cannot focus test window")

    def key(code, ctrl=False):
        if ctrl:
            user.keybd_event(0x11, 0, 0, 0)
        user.keybd_event(code, 0, 0, 0)
        time.sleep(0.04)
        user.keybd_event(code, 0, 2, 0)
        if ctrl:
            user.keybd_event(0x11, 0, 2, 0)
        time.sleep(0.25)

    def capture(hwnd, path):
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        width, height = rect.right, rect.bottom
        screen = user.GetDC(hwnd)
        memory = gdi.CreateCompatibleDC(screen)
        bitmap = gdi.CreateCompatibleBitmap(screen, width, height)
        previous = gdi.SelectObject(memory, bitmap)
        try:
            if not user.PrintWindow(hwnd, memory, 3):  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
                raise RuntimeError("PrintWindow failed")
            gdi.SelectObject(memory, previous)
            info = BitmapInfo(ct.sizeof(BitmapInfo), width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
            pixels = ct.create_string_buffer(width * height * 4)
            if gdi.GetDIBits(screen, bitmap, 0, height, pixels, ct.byref(info), 0) != height:
                raise RuntimeError("GetDIBits failed")
            image = Image.frombuffer("RGB", (width, height), pixels, "raw", "BGRX", 0, 1).copy()
            image.save(path)
            return image
        finally:
            gdi.SelectObject(memory, previous)
            gdi.DeleteObject(bitmap)
            gdi.DeleteDC(memory)
            user.ReleaseDC(hwnd, screen)

    results = json.loads((destination / "results.json").read_text(encoding="utf-8")) if args.only and (destination / "results.json").exists() else []
    for item in selected:
        output = destination / f'{item["index"]:04}'
        output.mkdir(exist_ok=True)
        row = {"index": item["index"], "file": item["relative"], "ok": False}
        decode_report = (root / item["report"]).read_text(encoding="utf-8-sig")
        has_video = "| media: video=true" in decode_report
        standalone = item["extension"] in {".mov", ".mp4", ".mkv", ".avi", ".webm"}
        expected_size = (item.get("width", 0), item.get("height", 0))
        vector = re.search(r"vector: intrinsic=(\d+)x(\d+)", decode_report)
        if vector:
            expected_size = tuple(map(int, vector.groups()))
        process = subprocess.Popen([str(exe), item["path"]], cwd=root,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 15
            hwnd = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"app exited before ready: {process.returncode:#x}")
                hwnd = find_window(process.pid)
                current_title = title(hwnd) if hwnd else ""
                # 渐进加载时主页/缩略图也有尺寸，必须等原图尺寸；播放器标题只有文件名。
                if hwnd and (standalone and Path(item["path"]).name in current_title or
                             f" {expected_size[0]}x{expected_size[1]} " in current_title or
                             has_video and re.search(r" \d{3,}x\d{3,} ", current_title)):
                    break
                time.sleep(0.05)
            if not hwnd:
                raise RuntimeError("window missing")
            if time.monotonic() >= deadline:
                raise RuntimeError(f"original image not ready: {current_title}")
            focus(hwnd)
            user.MoveWindow(hwnd, 80, 40, 1200, 900, True)
            time.sleep(0.3)
            initial = capture(hwnd, output / "01-image.png")
            row["title"] = title(hwnd)
            time.sleep(0.43)
            second = capture(hwnd, output / "02-next-frame.png")
            difference = ImageStat.Stat(ImageChops.difference(initial, second)).mean
            row["frame_change"] = sum(difference) / 3
            if item.get("frames", 0) > 1 and row["frame_change"] < 0.01:
                time.sleep(1.13)
                third = capture(hwnd, output / "03-later-frame.png")
                row["frame_change"] = sum(ImageStat.Stat(ImageChops.difference(initial, third)).mean) / 3
            key(0x09)  # Tab 信息面板
            capture(hwnd, output / "04-info.png")
            key(0x09)
            if not has_video:
                key(ord("Q"))
                key(0x26)  # Up 放大
                capture(hwnd, output / "05-rotate-zoom.png")
            row["ok"] = standalone or re.search(r"\d+x\d+", row["title"]) is not None
            if item.get("frames", 0) > 1 and row["frame_change"] <= 0.01:
                row["ok"] = False
                row["error"] = "animated source did not change pixels during observation"
            row["screenshots"] = str(output.relative_to(root))
        except Exception as error:
            row["error"] = str(error)
        finally:
            if process.poll() is None:
                user.PostMessageW(hwnd, 0x10, 0, 0) if hwnd else None
                try:
                    code = process.wait(timeout=5)
                    row["exit"] = code
                    if code != 0:
                        row["ok"] = False
                except subprocess.TimeoutExpired:
                    # 看图器退出会等待邻图预读；WP2 所在目录可能让短超时触发，明确留痕。
                    process.kill()
                    process.wait()
                    row["close_timeout"] = True
                    row["ok"] = False
                    row["error"] = "normal window close timed out while waiting for background work"
            results = [result for result in results if result["index"] != row["index"]]
            results.append(row)
            (destination / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f'{len(results)}/{len(selected)} {"PASS" if row["ok"] else "FAIL"} {row["file"]}', flush=True)

    cards = []
    for item in results:
        file = destination / f'{item["index"]:04}' / "01-image.png"
        if file.exists():
            with Image.open(file) as image:
                image.thumbnail((300, 220))
                card = Image.new("RGB", (320, 260), "#eeeeee")
                card.paste(image, ((320 - image.width) // 2, 0))
                draw = ImageDraw.Draw(card)
                draw.text((8, 224), f'{item["index"]:04} {item["file"][:42]}', fill="black", font_size=13)
                cards.append(card)
    for start in range(0, len(cards), 12):
        sheet = Image.new("RGB", (1280, 780), "white")
        for offset, card in enumerate(cards[start:start + 12]):
            sheet.paste(card, ((offset % 4) * 320, (offset // 4) * 260))
        sheet.save(destination / f"contact-{start // 12 + 1}.jpg")
    failures = [row for row in results if not row["ok"]]
    print(f'{len(results) - len(failures)} passed, {len(failures)} failed; screenshots: {destination}', flush=True)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
