"""批处理窗口退出/重开回归。所有文件位于新的 TEMP 目录，会串行驱动鼠标与键盘。

python -I tools/test_batch_lifecycle.py --exe x64/Release/JarkViewer.exe
坐标按当前 ImGui 批处理布局推算，布局改动后需同步本脚本。
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-batch-ui-"))
    exe = root / "JarkViewer.exe"
    shutil.copy2(args.exe.resolve(), exe)
    image = Image.new("RGB", (3200, 1800), (20, 80, 140))
    image.save(root / "00.jpg")
    for index in range(1, 64):
        shutil.copy2(root / "00.jpg", root / f"{index:02}.jpg")
    original = hashlib.sha256((root / "00.jpg").read_bytes()).hexdigest()
    user = ct.windll.user32
    kernel = ct.windll.kernel32
    user.SetProcessDPIAware()
    user.GetForegroundWindow.restype = wt.HWND
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ct.POINTER(wt.DWORD)]
    user.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    user.GetClientRect.argtypes = [wt.HWND, ct.POINTER(wt.RECT)]
    user.ClientToScreen.argtypes = [wt.HWND, ct.POINTER(wt.POINT)]
    user.GetDpiForWindow.argtypes = [wt.HWND]
    callback_type = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def window(pid):
        found = []

        @callback_type
        def visit(hwnd, _):
            owner = wt.DWORD()
            user.GetWindowThreadProcessId(hwnd, ct.byref(owner))
            class_name = ct.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, class_name, len(class_name))
            if owner.value == pid and user.IsWindowVisible(hwnd) and class_name.value == "D3D11WndClass":
                found.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def key(code, ctrl=False):
        if ctrl:
            user.keybd_event(0x11, 0, 0, 0)
        user.keybd_event(code, 0, 0, 0)
        time.sleep(0.05)
        user.keybd_event(code, 0, 2, 0)
        if ctrl:
            user.keybd_event(0x11, 0, 2, 0)

    print(f"Artifacts: {root}", flush=True)
    for scenario in ("preview", "worker"):
        process = subprocess.Popen([str(exe), str(root / "00.jpg")])
        try:
            hwnd = None
            deadline = time.monotonic() + 10
            while not hwnd and time.monotonic() < deadline:
                hwnd = window(process.pid)
                time.sleep(0.1)
            if not hwnd:
                raise RuntimeError("main window missing")
            foreground = user.GetForegroundWindow()
            foreground_thread = user.GetWindowThreadProcessId(foreground, None)
            current_thread = kernel.GetCurrentThreadId()
            user.AttachThreadInput(current_thread, foreground_thread, True)
            user.ShowWindow(hwnd, 9)
            user.BringWindowToTop(hwnd)
            user.SetForegroundWindow(hwnd)
            user.AttachThreadInput(current_thread, foreground_thread, False)
            if user.GetForegroundWindow() != hwnd:
                raise RuntimeError("cannot focus test window")
            time.sleep(0.7)
            key(ord("B"), ctrl=True)
            time.sleep(0.7)
            rect = wt.RECT()
            user.GetClientRect(hwnd, ct.byref(rect))
            scale = user.GetDpiForWindow(hwnd) / 96
            width, height = min(700 * scale, rect.right - 8 * scale), min(580 * scale, rect.bottom - 8 * scale)
            left, top = (rect.right - width) / 2, (rect.bottom - height) / 2

            def click(x, y):
                point = wt.POINT(round(left + x * scale), round(top + y * scale))
                user.ClientToScreen(hwnd, ct.byref(point))
                user.SetCursorPos(point.x, point.y)
                time.sleep(0.12)
                user.mouse_event(2, 0, 0, 0, 0)
                time.sleep(0.06)
                user.mouse_event(4, 0, 0, 0, 0)

            if scenario == "preview":
                click(132, 310)  # “缩放”：启动后台尺寸预览
                time.sleep(0.15)
            else:
                click(90, 509)  # 默认 JPEG → PNG；输出不同扩展名，不会跳过任务
                deadline = time.monotonic() + 10
                while not list(root.glob("*.png")) and time.monotonic() < deadline:
                    time.sleep(0.05)
                if not list(root.glob("*.png")):
                    raise RuntimeError("batch did not produce any output; start-button coordinates stale")
                click(680, 14)  # 处理中关掉标题栏，再重新打开，不能清空 worker 的结果
                key(ord("B"), ctrl=True)
                time.sleep(0.15)
            print(f"closing {scenario} hwnd={hwnd} pid={process.pid}", flush=True)
            user.PostMessageW(hwnd, 0x10, 0, 0)  # 正常退出，不用 kill 掩盖析构错误
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                debugger = Path("C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe")
                if debugger.exists():
                    with (root / (scenario + "-stack.txt")).open("wb") as stack:
                        subprocess.run([str(debugger), "-p", str(process.pid), "-c", "~* k 20; .detach; q"],
                                       stdout=stack, stderr=subprocess.STDOUT, timeout=15)
                raise
            if code != 0:
                raise RuntimeError(f"{scenario} exit={code:#x}")
            print(f"PASS {scenario}: normal shutdown returned 0", flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    for file in root.glob("*.jpg"):
        if hashlib.sha256(file.read_bytes()).hexdigest() != original:
            raise RuntimeError(f"batch changed source: {file}")
    outputs = list(root.glob("*.png"))
    for file in outputs:
        with Image.open(file) as output:
            output.verify()
    if list(root.glob(".jark-*.tmp")):
        raise RuntimeError("temporary file survived shutdown")
    print(f"PASS {len(outputs)} complete outputs, 64 sources preserved; artifacts: {root}", flush=True)


if __name__ == "__main__":
    main()
