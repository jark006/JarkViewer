"""帮助页回归：六种语言的条目结构、分类切换、固定导航、纵向滚动和窄窗口截图。

python -I tools/test_help_page.py --exe x64/Release/JarkViewer.exe
依赖 Pillow/numpy，会操作鼠标键盘；程序、设置和输出均在独立 TEMP 目录。
控件坐标按当前帮助页布局和 DPI 换算，布局变化时须同步。--languages 0,2,5 可定向检查。
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--languages", default="0,1,2,3,4,5")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-help-page-"))
    exe = root / "JarkViewer.exe"
    shutil.copy2(args.exe.resolve(), exe)
    print(f"Artifacts: {root}", flush=True)
    results = []

    def check(ok, name, **details):
        results.append({"name": name, "ok": bool(ok), **details})
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        if not ok:
            raise RuntimeError(f"{name}: {details}")

    report = root / "languages.txt"
    process = subprocess.run([exe, "--probe", "--lang-test", "--out", report], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=20)
    check(process.returncode == 0, "语言自检正常结束")
    blocks = re.findall(r"\[help:(\d+)\]\n(.*?)\n\[/help\]", report.read_text(encoding="utf-8"), re.S)
    check(len(blocks) == 30, "六种语言均包含五组操作", groups=len(blocks))
    counts = {205: 8, 206: 9, 207: 11, 208: 4, 209: 10}
    catalogs = []
    for language in range(6):
        catalog = {}
        for index, (id_text, body) in enumerate(blocks[language * 5:language * 5 + 5]):
            id_number = int(id_text)
            rows = body.splitlines()
            check(id_number == 205 + index and len(rows) == counts[id_number],
                  f"语言 {language} 分组 {id_number} 条目完整", rows=len(rows))
            for row in rows:
                parts = row.split("\t")
                check(len(parts) == 2 and all(part.strip() for part in parts) and
                      all(key.strip() for key in parts[-1].split("|")),
                      f"语言 {language} 条目结构有效", row=row)
            catalog[id_number] = rows
        catalogs.append(catalog)
    check(all([row.split("\t")[1] for row in catalog[206]] ==
              [row.split("\t")[1] for row in catalogs[0][206]] for catalog in catalogs),
          "六种语言的看图快捷键一致")
    print("PASS language catalogs: 6 x 42 actions", flush=True)

    user, gdi, kernel = ct.windll.user32, ct.windll.gdi32, ct.windll.kernel32
    user.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
    user.SetProcessDpiAwarenessContext(ct.c_void_p(-4))
    for name, inputs, output in [
        ("GetForegroundWindow", [], wt.HWND),
        ("GetWindowThreadProcessId", [wt.HWND, ct.POINTER(wt.DWORD)], wt.DWORD),
        ("GetClientRect", [wt.HWND, ct.POINTER(wt.RECT)], wt.BOOL),
        ("GetClassNameW", [wt.HWND, wt.LPWSTR, ct.c_int], ct.c_int),
        ("GetDpiForWindow", [wt.HWND], wt.UINT),
        ("ClientToScreen", [wt.HWND, ct.POINTER(wt.POINT)], wt.BOOL),
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
            name = ct.create_unicode_buffer(128)
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            user.GetClassNameW(hwnd, name, len(name))
            if owner.value == pid and user.IsWindowVisible(hwnd) and name.value == "D3D11WndClass":
                found.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def move(hwnd, x, y):
        point = wt.POINT(round(x), round(y))
        user.ClientToScreen(hwnd, ct.byref(point))
        user.SetCursorPos(point.x, point.y)
        time.sleep(0.08)
        return point

    def click(hwnd, x, y):
        move(hwnd, x, y)
        user.mouse_event(2, 0, 0, 0, 0)
        time.sleep(0.06)
        user.mouse_event(4, 0, 0, 0, 0)
        time.sleep(0.22)
        move(hwnd, 20, 20)

    def capture(hwnd, name):
        rect = wt.RECT()
        user.GetClientRect(hwnd, ct.byref(rect))
        screen = user.GetDC(hwnd)
        memory = gdi.CreateCompatibleDC(screen)
        bitmap = gdi.CreateCompatibleBitmap(screen, rect.right, rect.bottom)
        previous = gdi.SelectObject(memory, bitmap)
        try:
            check(bool(user.PrintWindow(hwnd, memory, 3)), "截取帮助页窗口")
            gdi.SelectObject(memory, previous)
            info = ct.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, rect.right, -rect.bottom,
                                                      1, 32, 0, rect.right * rect.bottom * 4, 0, 0, 0, 0))
            pixels = ct.create_string_buffer(rect.right * rect.bottom * 4)
            check(gdi.GetDIBits(screen, bitmap, 0, rect.bottom, pixels, info, 0) == rect.bottom, "读回截图像素")
            image = Image.frombuffer("RGB", (rect.right, rect.bottom), pixels, "raw", "BGRX", 0, 1).copy()
            image.save(root / f"{name}.png")
            return np.asarray(image)
        finally:
            gdi.SelectObject(memory, previous)
            gdi.DeleteObject(bitmap)
            gdi.DeleteDC(memory)
            user.ReleaseDC(hwnd, screen)

    for language in map(int, args.languages.split(",")):
        for mode in (1, 2):
            settings = bytearray(4096)
            settings[:16] = b"JarkViewerSetting"
            struct.pack_into("<4iI", settings, 32, 0, 0, 1100, 850, 1)
            struct.pack_into("<III", settings, 52, 100, 100, 1)
            settings[68:72] = bytes([1, 1, 1, 1])
            struct.pack_into("<IIII", settings, 72, 0, 0, 5, mode)
            (root / "JarkViewer.db").write_bytes(settings)
            process = subprocess.Popen([exe, "--lang", str(language)], cwd=root,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                hwnd, deadline = None, time.monotonic() + 10
                while not hwnd and time.monotonic() < deadline:
                    hwnd = find_window(process.pid)
                    time.sleep(0.1)
                check(bool(hwnd), "测试窗口就绪")
                other = user.GetWindowThreadProcessId(user.GetForegroundWindow(), None)
                current = kernel.GetCurrentThreadId()
                attached = user.AttachThreadInput(current, other, True)
                user.BringWindowToTop(hwnd)
                user.SetForegroundWindow(hwnd)
                if attached:
                    user.AttachThreadInput(current, other, False)
                check(user.GetForegroundWindow() == hwnd, "测试窗口已聚焦")
                user.keybd_event(0x72, 0, 0, 0)  # F3 直接打开帮助
                time.sleep(0.08)
                user.keybd_event(0x72, 0, 2, 0)
                time.sleep(0.4)
                move(hwnd, 20, 20)
                rect = wt.RECT()
                user.GetClientRect(hwnd, ct.byref(rect))
                scale = user.GetDpiForWindow(hwnd) / 96
                width, height = min(680 * scale, rect.right - 8 * scale), min(640 * scale, rect.bottom - 8 * scale)
                left, top = (rect.right - width) / 2, (rect.bottom - height) / 2
                content = np.s_[round(top + 196 * scale):round(top + height - 14 * scale),
                                round(left + 14 * scale):round(left + width - 30 * scale)]
                navigation = np.s_[round(top + 72 * scale):round(top + 182 * scale),
                                   round(left + 14 * scale):round(left + width - 14 * scale)]
                prefix = f"lang-{language}-theme-{mode}"
                initial, last = None, None
                for category in range(3):
                    if category:
                        click(hwnd, left + (width - 28 * scale) * (category + 0.5) / 3 + 14 * scale,
                              top + 162 * scale)
                    image = capture(hwnd, f"{prefix}-category-{category}")
                    if initial is None:
                        initial = image
                    if last is not None:
                        diff = np.mean(np.any(image[content] != last[content], axis=2))
                        check(diff > 0.05, "分类切换显示不同正文", language=language, theme=mode, category=category)
                    point = move(hwnd, left + width / 2, top + height / 2)
                    user.PostMessageW(hwnd, 0x020A, ((-120 * 50) & 0xFFFF) << 16,
                                      (point.x & 0xFFFF) | ((point.y & 0xFFFF) << 16))
                    time.sleep(0.25)
                    move(hwnd, 20, 20)
                    bottom = capture(hwnd, f"{prefix}-category-{category}-bottom")
                    check(np.mean(np.any(bottom[content] != image[content], axis=2)) > 0.05,
                          "帮助正文可以滚动至后续内容", language=language, category=category)
                    check(np.array_equal(bottom[navigation], image[navigation]), "滚动不移动标题和分类导航")
                    last = image
                click(hwnd, left + 100 * scale, top + 162 * scale)
                returned = capture(hwnd, f"{prefix}-returned")
                check(np.array_equal(returned[content], initial[content]), "切回分类恢复页首内容")
                # 通过右下角抓手缩至设置窗口最小尺寸，覆盖长翻译的纵向折行。
                move(hwnd, left + width - 2, top + height - 2)
                user.mouse_event(2, 0, 0, 0, 0)
                for step in range(1, 7):
                    move(hwnd, left + width - 2 - 40 * scale * step / 6,
                         top + height - 2 - 60 * scale * step / 6)
                user.mouse_event(4, 0, 0, 0, 0)
                time.sleep(0.2)
                move(hwnd, 20, 20)
                narrow = capture(hwnd, f"{prefix}-narrow")
                check(np.mean(np.any(narrow[content] != returned[content], axis=2)) > 0.05,
                      "窄窗口重新排版", language=language, theme=mode)
                user.PostMessageW(hwnd, 0x10, 0, 0)
                check(process.wait(timeout=15) == 0, "帮助页正常退出")
                print(f"PASS language={language}, theme={mode}: categories / scroll / narrow", flush=True)
            finally:
                user.mouse_event(4, 0, 0, 0, 0)
                if process.poll() is None:
                    process.kill()
                    process.wait()
    print(f"PASS {len(results)} checks; screenshots: {root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
