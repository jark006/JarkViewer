"""打印预览回归：逐像素核对抖动、窗口缩放、反相/模式切换及滑块悬停滚轮。

python -I tools/test_print_preview.py --exe x64/Release/JarkViewer.exe
依赖 Pillow/numpy；会操作鼠标键盘，程序、语料、设置和截图均在独立 TEMP 目录。
控件坐标按当前英文 ImGui 打印窗口及 DPI 换算，布局改变时须同步。
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image


def dither_reference(width, height, brightness=80, contrast=100, gray=128):
    # 独立实现整数误差扩散语义（负数除法向零截断），用已知均匀灰图核对实时调整。
    adjusted = max(0, min(255, (gray - 128) * (contrast / 100) ** 3 + 128))
    value = round((adjusted / 255) ** ((2 - max(1, min(199, brightness)) / 100) ** 3) * 255)
    rows = [bytearray([value]) * width for _ in range(height)]
    for y, row in enumerate(rows):
        for x in range(width):
            old = row[x]
            row[x] = 0 if old < 128 else 255
            error = old - row[x]

            def spread(target, index, weight):
                delta = (abs(error) * weight // 16) * (1 if error >= 0 else -1)
                target[index] = max(0, min(255, target[index] + delta))

            if x + 1 < width:
                spread(row, x + 1, 7)
            if y + 1 < height:
                if x > 0:
                    spread(rows[y + 1], x - 1, 3)
                spread(rows[y + 1], x, 5)
                if x + 1 < width:
                    spread(rows[y + 1], x + 1, 1)
    return np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(height, width)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--wheel-only", action="store_true", help="只验证亮度/对比度滑块滚轮")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-print-preview-"))
    print(f"Artifacts: {root}", flush=True)
    user, gdi, kernel = ct.windll.user32, ct.windll.gdi32, ct.windll.kernel32
    user.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
    user.SetProcessDpiAwarenessContext(ct.c_void_p(-4))  # PER_MONITOR_AWARE_V2
    for name, inputs, output in [
        ("GetForegroundWindow", [], wt.HWND),
        ("GetWindowThreadProcessId", [wt.HWND, ct.POINTER(wt.DWORD)], wt.DWORD),
        ("GetClientRect", [wt.HWND, ct.POINTER(wt.RECT)], wt.BOOL),
        ("ClientToScreen", [wt.HWND, ct.POINTER(wt.POINT)], wt.BOOL),
        ("GetClassNameW", [wt.HWND, wt.LPWSTR, ct.c_int], ct.c_int),
        ("GetWindowTextW", [wt.HWND, wt.LPWSTR, ct.c_int], ct.c_int),
        ("GetDpiForWindow", [wt.HWND], wt.UINT),
        ("MoveWindow", [wt.HWND, ct.c_int, ct.c_int, ct.c_int, ct.c_int, wt.BOOL], wt.BOOL),
        ("PostMessageW", [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM], wt.BOOL),
        ("GetDC", [wt.HWND], wt.HDC),
        ("ReleaseDC", [wt.HWND, wt.HDC], ct.c_int),
        ("PrintWindow", [wt.HWND, wt.HDC, wt.UINT], wt.BOOL),
    ]:
        function = getattr(user, name)
        function.argtypes, function.restype = inputs, output
    for name, inputs, output in [
        ("CreateCompatibleDC", [wt.HDC], wt.HDC),
        ("CreateCompatibleBitmap", [wt.HDC, ct.c_int, ct.c_int], wt.HBITMAP),
        ("SelectObject", [wt.HDC, wt.HGDIOBJ], wt.HGDIOBJ),
        ("GetDIBits", [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ct.c_void_p, ct.c_void_p, wt.UINT], ct.c_int),
        ("DeleteObject", [wt.HGDIOBJ], wt.BOOL),
        ("DeleteDC", [wt.HDC], wt.BOOL),
    ]:
        function = getattr(gdi, name)
        function.argtypes, function.restype = inputs, output
    callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def find_window(pid, size):
        handles = []

        @callback_type
        def visit(hwnd, _):
            owner = wt.DWORD()
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            name, title = ct.create_unicode_buffer(128), ct.create_unicode_buffer(2048)
            user.GetClassNameW(hwnd, name, len(name))
            user.GetWindowTextW(hwnd, title, len(title))
            if owner.value == pid and user.IsWindowVisible(hwnd) and name.value == "D3D11WndClass" and \
                    f"{size[0]}x{size[1]}" in title.value:
                handles.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return handles[0] if handles else None

    def focus(hwnd):
        other_thread = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
        current_thread = kernel.GetCurrentThreadId()
        attached = user.AttachThreadInput(current_thread, other_thread, True)
        user.ShowWindow(hwnd, 9)
        user.BringWindowToTop(hwnd)
        user.SetForegroundWindow(hwnd)
        if attached:
            user.AttachThreadInput(current_thread, other_thread, False)
        if user.GetForegroundWindow() != hwnd:
            raise RuntimeError("cannot focus test window")

    def mouse(hwnd, x, y):
        point = wt.POINT(round(x), round(y))
        user.ClientToScreen(hwnd, ct.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.12)

    def click(hwnd, x, y):
        mouse(hwnd, x, y)
        user.mouse_event(2, 0, 0, 0, 0)
        time.sleep(0.08)
        user.mouse_event(4, 0, 0, 0, 0)
        time.sleep(0.35)

    def wheel(hwnd, x, y, delta):
        mouse(hwnd, x, y)
        point = wt.POINT(round(x), round(y))
        user.ClientToScreen(hwnd, ct.byref(point))
        user.PostMessageW(hwnd, 0x020A, (delta & 0xFFFF) << 16,
                          (point.x & 0xFFFF) | ((point.y & 0xFFFF) << 16))
        time.sleep(0.2)

    def capture(hwnd, path):
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        width, height = rect.right, rect.bottom
        screen = user.GetDC(hwnd)
        memory = gdi.CreateCompatibleDC(screen)
        bitmap = gdi.CreateCompatibleBitmap(screen, width, height)
        previous = gdi.SelectObject(memory, bitmap)
        try:
            if not user.PrintWindow(hwnd, memory, 3):
                raise RuntimeError("PrintWindow failed")
            gdi.SelectObject(memory, previous)
            info = ct.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32,
                                                      0, width * height * 4, 0, 0, 0, 0))
            pixels = ct.create_string_buffer(width * height * 4)
            if gdi.GetDIBits(screen, bitmap, 0, height, pixels, info, 0) != height:
                raise RuntimeError("GetDIBits failed")
            image = Image.frombuffer("RGB", (width, height), pixels, "raw", "BGRX", 0, 1).copy()
            image.save(path)
            return np.asarray(image)
        finally:
            gdi.SelectObject(memory, previous)
            gdi.DeleteObject(bitmap)
            gdi.DeleteDC(memory)
            user.ReleaseDC(hwnd, screen)

    results = []
    cases = [] if args.wheel_only else [("large", (4000, 3000)), ("portrait", (601, 853)), ("small", (113, 73))]
    cases.append(("wheel", (397, 293)))
    for label, size in cases:
        folder = root / label
        folder.mkdir()
        exe = folder / "JarkViewer.exe"
        shutil.copy2(args.exe.resolve(), exe)
        source = folder / "gray.png"
        gray_value = 96 if label == "wheel" else 128  # 偏离 128，才能从预览判断对比度确有变化。
        Image.new("RGB", size, (gray_value,) * 3).save(source)
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        process = subprocess.Popen([str(exe), "--lang", "2", str(source)], cwd=folder,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            hwnd, deadline = None, time.monotonic() + 15
            while not hwnd and time.monotonic() < deadline:
                hwnd = find_window(process.pid, size)
                time.sleep(0.1)
            if not hwnd:
                raise RuntimeError("original image did not become ready")
            focus(hwnd)
            work = wt.RECT()
            user.SystemParametersInfoW(48, 0, ct.byref(work), 0)  # SPI_GETWORKAREA
            user.MoveWindow(hwnd, work.left + 20, work.top + 20,
                            min(1201, work.right - work.left - 40), min(901, work.bottom - work.top - 40), True)
            time.sleep(0.3)
            user.keybd_event(17, 0, 0, 0)
            user.keybd_event(ord("P"), 0, 0, 0)
            time.sleep(0.08)
            user.keybd_event(ord("P"), 0, 2, 0)
            user.keybd_event(17, 0, 2, 0)
            time.sleep(0.5)
            rect = wt.RECT()
            user.GetClientRect(hwnd, ct.byref(rect))
            scale = user.GetDpiForWindow(hwnd) / 96
            width, height = min(760 * scale, rect.right - 8 * scale), min(620 * scale, rect.bottom - 8 * scale)
            left, top = (rect.right - width) / 2, (rect.bottom - height) / 2
            click(hwnd, left + 434 * scale, top + 54 * scale)  # B/W dither

            def verify(stage, invert=False, brightness=80, contrast=100):
                image = capture(hwnd, folder / f"{stage}.png")
                # 主画布为均匀灰，工具窗口下半部只有预览包含纯黑/纯白；不靠写死预览尺寸定位。
                x0, y0 = round(left + 12 * scale), round(top + 116 * scale)
                area = image[y0:rect.bottom - 16, x0:rect.right - 16]
                binary = np.all(area == 0, axis=2) | np.all(area == 255, axis=2)
                ys = np.flatnonzero(binary.sum(axis=1) > max(8, area.shape[1] * 0.1))
                xs = np.flatnonzero(binary.sum(axis=0) > max(8, area.shape[0] * 0.1))
                if not len(xs) or not len(ys):
                    raise RuntimeError(f"{stage}: binary preview missing; controls or layout may have changed")
                preview = area[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]
                h, w = preview.shape[:2]
                expected = dither_reference(w, h, brightness, contrast, gray_value)
                if invert:
                    expected = 255 - expected
                mismatches = int(np.count_nonzero(np.any(preview != expected[:, :, None], axis=2)))
                row = {"case": label, "stage": stage, "width": w, "height": h,
                       "dpi": round(scale * 96), "brightness": brightness, "contrast": contrast,
                       "mismatched_pixels": mismatches}
                results.append(row)
                (root / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                if mismatches:
                    raise RuntimeError(f"{label}/{stage}: {mismatches}/{w*h} pixels differ from 1:1 dither reference")
                print(f"PASS {label}/{stage}: {w}x{h}, exact 1:1 pixels", flush=True)
                return w, h, (x0 + xs[0], y0 + ys[0], x0 + xs[-1] + 1, y0 + ys[-1] + 1)

            before = verify("initial")
            if label == "wheel":
                # 无需点击/聚焦滑块，按实际滚轮消息验证步进、限值与参数输入归属。
                brightness_x, contrast_x, slider_y = left + 160 * scale, left + 510 * scale, top + 90 * scale
                steps = [
                    ("brightness-up", brightness_x, slider_y, 120, 81, 100),
                    ("brightness-down", brightness_x, slider_y, -120, 80, 100),
                    ("contrast-up", contrast_x, slider_y, 120, 80, 101),
                    ("contrast-down-two", contrast_x, slider_y, -240, 80, 99),
                    ("contrast-restored", contrast_x, slider_y, 120, 80, 100),
                    ("brightness-three", brightness_x, slider_y, 360, 83, 100),
                    ("brightness-back-three", brightness_x, slider_y, -360, 80, 100),
                    ("brightness-half", brightness_x, slider_y, 60, 80, 100),
                    ("brightness-full", brightness_x, slider_y, 60, 81, 100),
                    ("brightness-reset", brightness_x, slider_y, -120, 80, 100),
                    ("brightness-leftover", brightness_x, slider_y, 60, 80, 100),
                    ("contrast-half-isolated", contrast_x, slider_y, 60, 80, 100),
                    ("contrast-full", contrast_x, slider_y, 60, 80, 101),
                    ("contrast-reset", contrast_x, slider_y, -120, 80, 100),
                    ("brightness-half-isolated", brightness_x, slider_y, 60, 80, 100),
                    ("outside-sliders", left + 40 * scale, top + 160 * scale, 1200, 80, 100),
                    ("brightness-new-half", brightness_x, slider_y, 60, 80, 100),
                    ("brightness-upper-limit", brightness_x, slider_y, 24000, 200, 100),
                    ("brightness-upper-half", brightness_x, slider_y, 60, 200, 100),
                    ("brightness-upper-reverse", brightness_x, slider_y, -120, 199, 100),
                    ("brightness-lower-limit", brightness_x, slider_y, -24000, 0, 100),
                    ("brightness-lower-half", brightness_x, slider_y, -60, 0, 100),
                    ("brightness-lower-reverse", brightness_x, slider_y, 120, 1, 100),
                    ("brightness-restored", brightness_x, slider_y, 9480, 80, 100),
                    ("contrast-upper-limit", contrast_x, slider_y, 24000, 80, 200),
                    ("contrast-upper-reverse", contrast_x, slider_y, -120, 80, 199),
                    ("contrast-lower-limit", contrast_x, slider_y, -24000, 80, 0),
                    ("contrast-lower-reverse", contrast_x, slider_y, 120, 80, 1),
                    ("contrast-final", contrast_x, slider_y, 12000, 80, 101),
                    ("brightness-final", brightness_x, slider_y, 120, 81, 101),
                ]
                for stage, x, y, delta, brightness, contrast in steps:
                    wheel(hwnd, x, y, delta)
                    verify(stage, brightness=brightness, contrast=contrast)
                # 打印参数按“关闭预览窗口”记忆；先点预览标题栏关闭，再正常退出主窗口。
                click(hwnd, left + width - 22 * scale, top + 14 * scale)
                user.PostMessageW(hwnd, 0x10, 0, 0)
                if process.wait(timeout=15) != 0:
                    raise RuntimeError("abnormal shutdown")
                # 4096 字节设置：header[32] + RECT[16] + showCmd[4]，之后是两个 uint32。
                settings = (folder / "JarkViewer.db").read_bytes()
                if len(settings) != 4096 or struct.unpack_from("<II", settings, 52) != (81, 101):
                    raise RuntimeError("wheel values were not saved as brightness=81, contrast=101")
                if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError("wheel adjustment modified source file")
                print("PASS wheel: exact saved values 81/101, source preserved", flush=True)
                continue

            # 缩小工具窗口高度，保证在小屏幕/高 DPI 下也能实际改变预览尺寸。
            mouse(hwnd, left + width - 2, top + height - 2)
            user.mouse_event(2, 0, 0, 0, 0)
            for step in range(1, 9):
                mouse(hwnd, left + width - 2, top + height - 2 - 90 * scale * step / 8)
            user.mouse_event(4, 0, 0, 0, 0)
            time.sleep(0.4)
            after = verify("resized")
            if before[:2] == after[:2]:
                raise RuntimeError("resize did not change preview dimensions")
            click(hwnd, left + 504 * scale, top + 54 * scale)  # Invert
            verify("inverted", invert=True)
            click(hwnd, left + 504 * scale, top + 54 * scale)
            click(hwnd, left + 175 * scale, top + 54 * scale)  # Grayscale
            gray = capture(hwnd, folder / "grayscale.png")
            x1, y1, x2, y2 = after[2]
            middle = gray[(y1 + y2)//2 - 8:(y1 + y2)//2 + 8, (x1 + x2)//2 - 8:(x1 + x2)//2 + 8]
            if not np.all((middle > 0) & (middle < 255)):
                raise RuntimeError("grayscale mode did not restore continuous tones")
            click(hwnd, left + 434 * scale, top + 54 * scale)
            verify("dither-again")
            user.PostMessageW(hwnd, 0x10, 0, 0)
            if process.wait(timeout=15) != 0:
                raise RuntimeError("abnormal shutdown")
            if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                raise RuntimeError("preview modified source file")
        finally:
            user.mouse_event(4, 0, 0, 0, 0)
            if process.poll() is None:
                process.kill()
                process.wait()
    print(f"PASS {len(results)} pixel comparisons; screenshots: {root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
