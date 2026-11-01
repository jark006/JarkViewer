"""WASD 反向平移实机回归：四个方向、旋转后方向、鼠标拖动、设置切换与重启兼容。

python -I tools/test_wasd_pan.py --exe x64/Release/JarkViewer.exe
依赖 Pillow/numpy；操作鼠标键盘，所有语料、设置、截图与报告使用独立 TEMP 目录。
设置控件坐标按当前中文常规页的两列布局和 DPI 换算，布局变化时须同步。
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
from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-wasd-"))
    exe = root / "JarkViewer.exe"
    shutil.copy2(args.exe.resolve(), exe)
    source = root / "marker.png"
    image = Image.new("RGB", (2400, 1800), (45, 85, 130))
    ImageDraw.Draw(image).rectangle((1180, 880, 1219, 919), fill=(255, 0, 0))
    image.save(source)
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    settings_file = root / "JarkViewer.db"
    settings = bytearray(4096)
    settings[:17] = b"JarkViewerSetting"
    struct.pack_into("<4iI", settings, 32, 0, 0, 1000, 800, 1)
    struct.pack_into("<III", settings, 52, 100, 100, 1)
    settings[67] = 1  # 关闭鸟瞰，避免红色缩略图参与像素定位。
    settings[70:72] = bytes([1, 1])
    struct.pack_into("<IIII", settings, 72, 0, 0, 5, 2)
    settings[1210] = 1  # 避免静止重采样影响红色标记的位置测量。
    # 只改原 reserve 第一个 uint32，其余保留字节和关联清单必须原样往返。
    settings[1216:3296] = bytes((index * 13 + 7) % 256 for index in range(2080))
    settings[3296:3304] = b"png,jpg\0"
    settings_file.write_bytes(settings)
    print(f"Artifacts: {root}", flush=True)
    results = []

    def check(ok, name, **details):
        results.append({"name": name, "ok": bool(ok), **details})
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"{'PASS' if ok else 'FAIL'} {name}: {details}", flush=True)
        if not ok:
            raise RuntimeError(name)

    user, gdi, kernel = ct.windll.user32, ct.windll.gdi32, ct.windll.kernel32
    user.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
    user.SetProcessDpiAwarenessContext(ct.c_void_p(-4))
    for name, inputs, output in [
        ("GetForegroundWindow", [], wt.HWND),
        ("GetWindowThreadProcessId", [wt.HWND, ct.POINTER(wt.DWORD)], wt.DWORD),
        ("GetClientRect", [wt.HWND, ct.POINTER(wt.RECT)], wt.BOOL),
        ("ClientToScreen", [wt.HWND, ct.POINTER(wt.POINT)], wt.BOOL),
        ("GetWindowTextW", [wt.HWND, wt.LPWSTR, ct.c_int], ct.c_int),
        ("GetClassNameW", [wt.HWND, wt.LPWSTR, ct.c_int], ct.c_int),
        ("GetDpiForWindow", [wt.HWND], wt.UINT),
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

    def find_window(pid):
        found = []

        @callback_type
        def visit(hwnd, _):
            owner = wt.DWORD()
            name, title = ct.create_unicode_buffer(128), ct.create_unicode_buffer(1024)
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            user.GetClassNameW(hwnd, name, len(name))
            user.GetWindowTextW(hwnd, title, len(title))
            if owner.value == pid and user.IsWindowVisible(hwnd) and name.value == "D3D11WndClass" and "2400x1800" in title.value:
                found.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def key(code):
        code = ord(code) if isinstance(code, str) else code
        user.keybd_event(code, 0, 0, 0)
        time.sleep(0.05)
        user.keybd_event(code, 0, 2, 0)
        time.sleep(0.4)

    def move(hwnd, x, y):
        point = wt.POINT(round(x), round(y))
        user.ClientToScreen(hwnd, ct.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.1)

    def capture(hwnd, name):
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        screen = user.GetDC(hwnd)
        memory = gdi.CreateCompatibleDC(screen)
        bitmap = gdi.CreateCompatibleBitmap(screen, rect.right, rect.bottom)
        previous = gdi.SelectObject(memory, bitmap)
        try:
            if not user.PrintWindow(hwnd, memory, 3):
                raise RuntimeError("PrintWindow failed")
            gdi.SelectObject(memory, previous)
            info = ct.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, rect.right, -rect.bottom,
                                                      1, 32, 0, rect.right * rect.bottom * 4, 0, 0, 0, 0))
            pixels = ct.create_string_buffer(rect.right * rect.bottom * 4)
            if gdi.GetDIBits(screen, bitmap, 0, rect.bottom, pixels, info, 0) != rect.bottom:
                raise RuntimeError("GetDIBits failed")
            image = Image.frombuffer("RGB", (rect.right, rect.bottom), pixels, "raw", "BGRX", 0, 1).copy()
            image.save(root / f"{name}.png")
            return np.asarray(image)
        finally:
            gdi.SelectObject(memory, previous)
            gdi.DeleteObject(bitmap)
            gdi.DeleteDC(memory)
            user.ReleaseDC(hwnd, screen)

    def marker(hwnd, name):
        image = capture(hwnd, name)
        red = (image[:, :, 0] > 220) & (image[:, :, 1] < 40) & (image[:, :, 2] < 40)
        ys, xs = np.where(red)
        check(len(xs) >= 1000, "原图标记已放大可见", screenshot=name, pixels=len(xs))
        return np.array([np.mean(xs), np.mean(ys)])

    def toggle(hwnd):
        key(0x70)  # F1
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        scale = user.GetDpiForWindow(hwnd) / 96
        width, height = min(680 * scale, rect.right - 8 * scale), min(640 * scale, rect.bottom - 8 * scale)
        left, top = (rect.right - width) / 2, (rect.bottom - height) / 2
        move(hwnd, left + 178 * scale, top + 270 * scale)  # 第六行右列：WASD平移反向
        user.mouse_event(2, 0, 0, 0, 0)
        time.sleep(0.06)
        user.mouse_event(4, 0, 0, 0, 0)
        time.sleep(0.2)
        capture(hwnd, f"settings-{len(results)}")
        key(0x1B)
        move(hwnd, 100, 100)

    def directions(hwnd, label, reverse):
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        step = (rect.right + rect.bottom) // 16
        sign = -1 if reverse else 1
        origin = marker(hwnd, f"{label}-center")
        for code, opposite, delta in [("W", "S", (0, step)), ("A", "D", (step, 0)),
                                      ("S", "W", (0, -step)), ("D", "A", (-step, 0))]:
            key(code)
            displacement = marker(hwnd, f"{label}-{code}") - origin
            check(np.max(np.abs(displacement - np.array(delta) * sign)) <= 1,
                  f"{label} {code} 方向与步幅", got=displacement.tolist(), expected=(np.array(delta) * sign).tolist())
            key(opposite)
            check(np.max(np.abs(marker(hwnd, f"{label}-{code}-restored") - origin)) <= 1, "反向按键回到中心")
        # 鼠标拖图方向不跟随键盘选项改变。
        move(hwnd, rect.right / 2, rect.bottom / 2)
        user.mouse_event(2, 0, 0, 0, 0)
        for n in range(1, 7):
            move(hwnd, rect.right / 2 + n * 8, rect.bottom / 2 + n * 5)
        user.mouse_event(4, 0, 0, 0, 0)
        time.sleep(0.4)
        displacement = marker(hwnd, f"{label}-mouse") - origin
        check(np.max(np.abs(displacement - (48, 30))) <= 1, f"{label} 鼠标拖动保持同向", got=displacement.tolist())
        move(hwnd, rect.right / 2 + 48, rect.bottom / 2 + 30)
        user.mouse_event(2, 0, 0, 0, 0)
        for n in range(1, 7):
            move(hwnd, rect.right / 2 + 48 - n * 8, rect.bottom / 2 + 30 - n * 5)
        user.mouse_event(4, 0, 0, 0, 0)
        time.sleep(0.4)
        check(np.max(np.abs(marker(hwnd, f"{label}-mouse-restored") - origin)) <= 1, "拖回原位置")

    def verify_settings(value):
        saved = settings_file.read_bytes()
        check(len(saved) == 4096 and struct.unpack_from("<I", saved, 1212)[0] == value,
              "选项按原设置布局持久化", expected=value)
        check(saved[1216:3296] == settings[1216:3296] and saved[3296:] == settings[3296:],
              "保留区域与关联清单字节未改变")

    for phase in ("initial", "restart", "invalid"):
        if phase == "invalid":
            saved = bytearray(settings_file.read_bytes())
            struct.pack_into("<I", saved, 1212, 99)
            settings_file.write_bytes(saved)
        process = subprocess.Popen([exe, "--lang", "0", source], cwd=root,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            hwnd, deadline = None, time.monotonic() + 15
            while not hwnd and time.monotonic() < deadline:
                hwnd = find_window(process.pid)
                time.sleep(0.1)
            check(bool(hwnd), "原图加载完成")
            other = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
            current = kernel.GetCurrentThreadId()
            attached = user.AttachThreadInput(current, other, True)
            user.ShowWindow(hwnd, 9)
            user.BringWindowToTop(hwnd)
            user.SetForegroundWindow(hwnd)
            if attached:
                user.AttachThreadInput(current, other, False)
            check(user.GetForegroundWindow() == hwnd, "测试窗口获得焦点")
            move(hwnd, 100, 100)
            time.sleep(0.3)
            key("5")  # 初始适应窗口 -> 1:1
            if phase == "initial":
                directions(hwnd, "default", False)
                toggle(hwnd)
                directions(hwnd, "reversed", True)
            elif phase == "restart":
                directions(hwnd, "persisted", True)
                key("E")
                directions(hwnd, "rotated", True)
                toggle(hwnd)
                directions(hwnd, "disabled", False)
            else:
                directions(hwnd, "invalid-default", False)
            user.PostMessageW(hwnd, 0x10, 0, 0)
            check(process.wait(timeout=15) == 0, "正常退出")
            verify_settings(1 if phase == "initial" else 0)
        finally:
            user.mouse_event(4, 0, 0, 0, 0)
            if process.poll() is None:
                process.kill()
                process.wait()
    check(hashlib.sha256(source.read_bytes()).hexdigest() == source_hash, "原图文件未被修改")
    print(f"PASS {len(results)} checks; screenshots: {root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
