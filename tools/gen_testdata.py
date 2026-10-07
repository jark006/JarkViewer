"""Generate a small image corpus for JarkViewer decode probing.

Usage: python gen_testdata.py <output-dir>

Covers: common formats, animated formats, unusual containers, mislabeled files
(wrong extension), extension-less files, corrupt and empty files.
"""

import os
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

FFMPEG = shutil.which("ffmpeg")

made = []
failed = []


def record(name):
    made.append(name)


def save(out_dir, name, img, **kwargs):
    path = os.path.join(out_dir, name)
    fmt = kwargs.pop("format", None)
    try:
        if isinstance(img, list):
            kwargs.setdefault("save_all", True)
            kwargs.setdefault("append_images", img[1:])
            img[0].save(path, format=fmt, **kwargs)
        else:
            img.save(path, format=fmt, **kwargs)
        record(name)
    except Exception as exc:  # noqa: BLE001
        failed.append(f"{name}: {exc}")


def gradient(width=64, height=48):
    arr = np.zeros((height, width, 3), np.uint8)
    arr[:, :, 0] = (np.arange(width)[None, :] * 4) % 256
    arr[:, :, 1] = (np.arange(height)[:, None] * 5) % 256
    arr[:, :, 2] = 128
    return Image.fromarray(arr, "RGB")


def frames(count=4):
    out = []
    for i in range(count):
        img = gradient()
        arr = np.asarray(img).copy()
        arr[:, :, 2] = (i * 60) % 256
        out.append(Image.fromarray(arr, "RGB"))
    return out


def run_ffmpeg(out_dir, name, args, source="testsrc=size=480x360:rate=25:duration=3"):
    if not FFMPEG:
        failed.append(f"{name}: ffmpeg not found")
        return
    path = os.path.join(out_dir, name)
    # 视频需大于 MIN_VIDEO_BUFF_SIZE(64KiB) 才会被解码，故生成足够长的素材
    cmd = [FFMPEG, "-y", "-loglevel", "error", "-f", "lavfi", "-i", source] + args + [path]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
        record(name)
    except subprocess.CalledProcessError as exc:
        failed.append(f"{name}: {exc.stderr.decode('utf-8', 'replace').strip()[:120]}")


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    img = gradient()
    rgba = img.convert("RGBA")
    rgba.putalpha(180)
    gray = img.convert("L")

    save(out_dir, "still.png", img)
    save(out_dir, "still.jpg", img, quality=92)
    save(out_dir, "still.bmp", img)
    save(out_dir, "still.tif", img)
    save(out_dir, "still.webp", img)
    save(out_dir, "still.ppm", img, format="PPM")
    save(out_dir, "still.pgm", gray, format="PPM")
    save(out_dir, "still.pbm", img.convert("1"), format="PPM")
    save(out_dir, "still.pcx", img, format="PCX")
    save(out_dir, "still.tga", img, format="TGA")
    save(out_dir, "alpha.png", rgba)
    save(out_dir, "alpha.tga", rgba, format="TGA")
    save(out_dir, "icon.ico", img, sizes=[(64, 48), (32, 24)])
    save(out_dir, "multi.tif", frames(3), save_all=True)
    save(out_dir, "anim.gif", frames(4), save_all=True, duration=100, loop=0)
    save(out_dir, "anim.apng.png", frames(4), save_all=True, duration=100, loop=0)
    save(out_dir, "anim.webp", frames(4), save_all=True, duration=100, loop=0)

    # 无扩展名 / 未知扩展名（内容正确，扩展名骗人）
    copies = [
        ("still.png", "noext_png"),
        ("still.png", "mislabeled.jpg"),
        ("still.jpg", "mislabeled.png"),
        ("still.webp", "mislabeled.gif"),
        ("still.tif", "mislabeled_png.tiff"),
        ("still.png", "unknown_ext.dat"),
    ]
    for source, name in copies:
        src_path = os.path.join(out_dir, source)
        if os.path.exists(src_path):
            shutil.copy(src_path, os.path.join(out_dir, name))
            record(name)
        else:
            failed.append(f"{name}: missing source {source}")

    # SVG（文本矢量）
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48" viewBox="0 0 64 48">'
           '<rect width="64" height="48" fill="#3070c0"/>'
           '<circle cx="32" cy="24" r="16" fill="#f0d040"/></svg>')
    with open(os.path.join(out_dir, "vector.svg"), "w", encoding="utf-8") as handle:
        handle.write(svg)
    record("vector.svg")

    xml_svg = '<?xml version="1.0" encoding="UTF-8"?>\n' + svg
    with open(os.path.join(out_dir, "vector_xml.svg"), "w", encoding="utf-8") as handle:
        handle.write(xml_svg)
    record("vector_xml.svg")

    # PFM（浮点图，自带解码器）
    height, width = 16, 24
    with open(os.path.join(out_dir, "float.pfm"), "wb") as handle:
        handle.write(b"PF\n%d %d\n-1.0\n" % (width, height))
        data = np.zeros((height, width, 3), "<f4")
        data[:, :, 0] = 1.0
        data[:, :, 1] = 0.5
        data[:, :, 2] = 0.25
        handle.write(data.tobytes())
    record("float.pfm")

    # QOI（简单容器，手写最小实现）
    pixels = np.asarray(img, np.uint8).reshape(-1, 3)
    with open(os.path.join(out_dir, "still.qoi"), "wb") as handle:
        handle.write(b"qoif" + struct.pack(">IIBB", img.width, img.height, 3, 0))
        prev = np.zeros(3, np.uint8)
        for pixel in pixels:
            if np.array_equal(pixel, prev):
                handle.write(b"\xc0")  # QOI_OP_RUN(0)
            else:
                handle.write(b"\xfe" + bytes(pixel))  # QOI_OP_RGB
                prev = pixel
        handle.write(b"\x00" * 7 + b"\x01")
    record("still.qoi")

    # EXIF 方向（1/3 不改变宽高，6/8 旋转 90 度，宽高互换）
    for orientation in (1, 3, 6, 8):
        exif = Image.Exif()
        exif[0x0112] = orientation
        save(out_dir, f"ori{orientation}.jpg", img, exif=exif, quality=95)

    # 损坏 / 非法内容
    with open(os.path.join(out_dir, "corrupt.png"), "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n" + b"garbage" * 32)
    with open(os.path.join(out_dir, "empty.png"), "wb") as handle:
        pass
    with open(os.path.join(out_dir, "text.txt"), "w", encoding="utf-8") as handle:
        handle.write("this is not an image at all\n")
    with open(os.path.join(out_dir, "fake.livp"), "wb") as handle:
        handle.write(b"PK\x03\x04" + b"\x00" * 64)
    with open(os.path.join(out_dir, "truncated.jpg"), "wb") as handle:
        with open(os.path.join(out_dir, "still.jpg"), "rb") as source:
            handle.write(source.read(256))
    for name in ("corrupt.png", "empty.png", "text.txt", "fake.livp", "truncated.jpg"):
        record(name)

    # 视频容器（按动态照片路径解码前 N 帧）
    # 注意：小于 MIN_VIDEO_BUFF_SIZE(64KiB) 的视频会被解码器直接拒绝，
    # 因此这里用高码率编码，保证体积足够大。
    run_ffmpeg(out_dir, "clip.mp4", ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "3000k"])
    run_ffmpeg(out_dir, "clip.mkv", ["-c:v", "mpeg4", "-b:v", "3000k"])
    run_ffmpeg(out_dir, "clip.avi", ["-c:v", "mpeg4", "-b:v", "3000k"])
    run_ffmpeg(out_dir, "clip.mov", ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "3000k"])
    run_ffmpeg(out_dir, "clip.webm", ["-c:v", "libvpx-vp9", "-b:v", "3000k"])
    run_ffmpeg(out_dir, "tiny.mp4", ["-c:v", "libx264", "-pix_fmt", "yuv420p"],
               source="testsrc=size=64x48:rate=5:duration=1")  # 期望被拒绝（小于 64KiB 阈值）
    run_ffmpeg(out_dir, "still.avif", ["-frames:v", "1", "-c:v", "libaom-av1", "-f", "avif"])

    print(f"generated {len(made)} files in {out_dir}")
    if failed:
        print("skipped/failed:")
        for item in failed:
            print("  -", item)


if __name__ == "__main__":
    main()
