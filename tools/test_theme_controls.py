"""浅色控件截图回归：未选中轮廓、悬停/选中反馈，以及实时切回深色。

python -I tools/test_theme_controls.py --exe x64/Release/JarkViewer.exe
依赖 Pillow/numpy 和 PowerShell 7；串行驱动测试窗口，所有设置与截图位于独立 TEMP 目录。
通过白色连通区域定位空复选框/单选框；设置行布局或 4096 字节设置结构变化时需同步。
"""

import argparse
import ctypes as ct
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

import numpy as np
from PIL import Image


def empty_controls(image, scale):
    white = np.all(image == 255, axis=2)
    seen = np.zeros(white.shape, dtype=bool)
    boxes = []
    for y0, x0 in zip(*np.where(white)):
        if seen[y0, x0]:
            continue
        pending = [(y0, x0)]
        seen[y0, x0] = True
        count, left, top, right, bottom = 0, x0, y0, x0, y0
        while pending:
            y, x = pending.pop()
            count += 1
            left, top, right, bottom = min(left, x), min(top, y), max(right, x), max(bottom, y)
            for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if 0 <= yy < white.shape[0] and 0 <= xx < white.shape[1] and white[yy, xx] and not seen[yy, xx]:
                    seen[yy, xx] = True
                    pending.append((yy, xx))
        width, height = right - left + 1, bottom - top + 1
        if 15 * scale <= width <= 36 * scale and abs(width - height) <= 2:
            boxes.append((int(left), int(top), int(width), int(height), count / (width * height)))
    # 已选中的勾/圆点会分割或占据白色区域，不会满足下列填充比例。
    squares = [box for box in boxes if box[4] > 0.9]
    circles = [box for box in boxes if 0.7 < box[4] < 0.88]
    return squares, circles


def luminance(rgb):
    values = np.asarray(rgb, dtype=float) / 255
    linear = np.where(values <= 0.04045, values / 12.92, ((values + 0.055) / 1.055) ** 2.4)
    return linear @ np.array([0.2126, 0.7152, 0.0722])


def outline_contrast(image, box, scale):
    x, y, width, height, _ = box
    cy, pad = y + height // 2, max(3, round(3 * scale))
    # 取完整轮廓；圆周的横纵极点可能恰好落在半像素上，只取四个极点会低估抗锯齿后的轮廓。
    edge = image[y - pad:y + height + pad, x - pad:x + width + pad].reshape(-1, 3)
    background = np.median(image[cy, x - 3 * pad:x - pad], axis=0)
    return float((luminance(background) + 0.05) / (np.min(luminance(edge)) + 0.05))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-theme-controls-"))
    exe = root / "JarkViewer.exe"
    shutil.copy2(args.exe.resolve(), exe)
    capture_script = Path(__file__).resolve().with_name("capture_window.ps1")
    user = ct.windll.user32
    user.SetProcessDpiAwarenessContext.argtypes = [ct.c_void_p]
    user.SetProcessDpiAwarenessContext(ct.c_void_p(-4))
    dpi = user.GetDpiForSystem()
    scale = dpi / 96
    frame = user.GetSystemMetricsForDpi(32, dpi) + user.GetSystemMetricsForDpi(92, dpi)
    title = frame + user.GetSystemMetricsForDpi(4, dpi)
    results = []
    print(f"Artifacts: {root}", flush=True)

    def check(ok, name, **details):
        results.append({"name": name, "ok": bool(ok), **details})
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"{'PASS' if ok else 'FAIL'} {name}: {details}", flush=True)
        if not ok:
            raise RuntimeError(name)

    def capture(name, mode=1, hover=None, click=None):
        # 固定布局的测试设置：header[32]/RECT[16]/showCmd，UI_Mode 位于偏移 84。
        settings = bytearray(4096)
        settings[:16] = b"JarkViewerSetting"
        struct.pack_into("<4iI", settings, 32, 0, 0, 1000, 800, 1)
        struct.pack_into("<III", settings, 52, 100, 100, 1)
        settings[68:72] = bytes([1, 1, 1, 1])
        struct.pack_into("<IIII", settings, 72, 0, 0, 5, mode)
        (root / "JarkViewer.db").write_bytes(settings)
        command = ["pwsh", "-NoProfile", "-File", str(capture_script), "-Exe", str(exe),
                   "-Argument", "--lang 0", "-Keys", "{F1}", "-Out", str(root / f"{name}.png"),
                   "-WaitMs", "800", "-AfterKeysMs", "400", "-LogicWidth", "1000"]
        if click:
            command.extend(["-Click", f"{click[0] - frame},{click[1] - title}"])
        command.extend(["-Hover", f"{hover[0] - frame},{hover[1] - title}" if hover else "20,20"])
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, timeout=20)
        return np.asarray(Image.open(root / f"{name}.png").convert("RGB"))

    normal = capture("light")
    squares, circles = empty_controls(normal, scale)
    check(bool(squares) and bool(circles), "定位未选中的复选框和单选框", squares=len(squares), circles=len(circles))
    checkbox, radio = squares[0], circles[0]
    for kind, box in (("checkbox", checkbox), ("radio", radio)):
        ratio = outline_contrast(normal, box, scale)
        # 轮廓以 3:1 为目标；截图上的抗锯齿会降低极薄边缘的实测对比。
        check(ratio >= 2.8, f"{kind} 未选中轮廓清晰", contrast=round(ratio, 3))
        x, y, width, height, _ = box
        center = x + width // 2, y + height // 2
        hovered = capture(f"{kind}-hover", hover=center)
        selected = capture(f"{kind}-selected", click=center)
        region = np.s_[y:y + height, x:x + width]
        check(np.count_nonzero(np.any(hovered[region] != normal[region], axis=2)) > width,
              f"{kind} 悬停反馈生效")
        # 内部出现蓝色勾/圆点，确认点击确实切换了状态，而非只截到悬停底色。
        inside = selected[y + height//3:y + 2*height//3, x + width//3:x + 2*width//3].astype(int)
        check(np.count_nonzero(inside[:, :, 2] - inside[:, :, 0] > 40) > 3,
              f"{kind} 选中标记可见")

    # 常规页的单选行依次是切图、排序、幻灯片、主题、语言，主题行是倒数第二行。
    row_ys = sorted(set(box[1] for box in circles))
    check(len(row_ys) == 5, "主题单选行定位有效", rows=len(row_ys))
    dark_box = max((box for box in circles if box[1] == row_ys[-2]), key=lambda box: box[0])
    dark_center = dark_box[0] + dark_box[2]//2, dark_box[1] + dark_box[3]//2
    switched_dark = capture("light-to-dark", click=dark_center)
    direct_dark = capture("dark", mode=2)
    for kind, box in (("checkbox", checkbox), ("radio", radio)):
        x, y, width, height, _ = box
        pad = round(3 * scale)
        region = np.s_[y - pad:y + height + pad, x - pad:x + width + pad]
        check(np.array_equal(switched_dark[region], direct_dark[region]),
              f"{kind} 切回深色后没有残留浅色边框")
    print(f"PASS {len(results)} checks; screenshots: {root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
