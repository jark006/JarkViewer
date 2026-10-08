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


def make_rotated_video(out_dir):
    """把 clip.mp4 的 tkhd 矩阵改成 -90° 旋转，产出竖拍视频样例。

    ffmpeg 不再支持 `-metadata:s:v rotate=90`，而 `-display_rotation` 只是读取端选项，
    所以这里直接改二进制：tkhd 盒里矩阵固定在第 48 字节（8 头 + 40 字段），
    9 个 32 位定点数；-90° 即 [[0,1,0],[-1,0,0],[0,0,1]]（1 写作 0x10000）。
    """
    src = os.path.join(out_dir, "clip.mp4")
    dst = os.path.join(out_dir, "clip_rot90.mp4")
    if not os.path.exists(src):
        failed.append("clip_rot90.mp4: 缺少 clip.mp4")
        return
    data = bytearray(open(src, "rb").read())
    box = data.find(b"tkhd")
    if box < 4:
        failed.append("clip_rot90.mp4: 找不到 tkhd 盒")
        return
    struct.pack_into(">9i", data, box + 4 + 40, 0, 65536, 0, -65536, 0, 0, 0, 0, 1073741824)
    with open(dst, "wb") as fh:
        fh.write(data)
    record("clip_rot90.mp4")


def run_ffmpeg(out_dir, name, args, source="testsrc=size=480x360:rate=25:duration=3",
               audio=None):
    if not FFMPEG:
        failed.append(f"{name}: ffmpeg not found")
        return
    path = os.path.join(out_dir, name)
    # 视频需大于 MIN_VIDEO_BUFF_SIZE(64KiB) 才会被解码，故生成足够长的素材
    cmd = [FFMPEG, "-y", "-loglevel", "error", "-f", "lavfi", "-i", source]
    if audio:
        cmd += ["-f", "lavfi", "-i", audio, "-shortest"]
    cmd += args + [path]
    try:
        subprocess.run(cmd, check=True, capture_output=True)
        record(name)
    except subprocess.CalledProcessError as exc:
        failed.append(f"{name}: {exc.stderr.decode('utf-8', 'replace').strip()[:120]}")


def _make_motion_photo(out_dir, name, image, video_path):
    """构造 Google MotionPhoto JPEG：JPEG + XMP(MicroVideoOffset) + 追加在尾部的 MP4。"""
    import io

    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", quality=90)
    jpeg = buffer.getvalue()

    with open(video_path, "rb") as handle:
        video = handle.read()

    xmp = ('<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF '
           'xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about="" '
           'xmlns:GCamera="http://ns.google.com/photos/1.0/camera/" GCamera:MicroVideo="1" '
           'GCamera:MicroVideoVersion="1" '
           f'GCamera:MicroVideoOffset="{len(video)}" '
           'GCamera:MicroVideoPresentationTimestampUs="1500000"/></rdf:RDF></x:xmpmeta>')
    payload = b"http://ns.adobe.com/xap/1.0/\x00" + xmp.encode("utf-8")
    segment = b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload

    with open(os.path.join(out_dir, name), "wb") as handle:
        handle.write(jpeg[:2] + segment + jpeg[2:] + video)
    record(name)


def _make_livp(out_dir, name, image, video_path):
    """构造 iOS 实况照片：zip 内含一张静态图 + 一段 mov。"""
    import io
    import zipfile

    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", quality=90)

    with zipfile.ZipFile(os.path.join(out_dir, name), "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("image.jpg", buffer.getvalue())
        with open(video_path, "rb") as handle:
            archive.writestr("video.mov", handle.read())
    record(name)


def _png_text(entries):
    from PIL.PngImagePlugin import PngInfo
    info = PngInfo()
    for key, value in entries.items():
        info.add_text(key, value)
    return info


def _png_chunk(chunk_type, payload):
    import zlib as _zlib
    data = chunk_type + payload
    return (struct.pack(">I", len(payload)) + data +
            struct.pack(">I", _zlib.crc32(data) & 0xFFFFFFFF))


def _ztxt_chunk(keyword, text):
    import zlib as _zlib
    return _png_chunk(b"zTXt", keyword.encode() + b"\x00\x00" + _zlib.compress(text.encode("latin-1", "replace")))


def _itxt_chunk(keyword, text, compress=False):
    import zlib as _zlib
    body = text.encode("utf-8")
    if compress:
        body = _zlib.compress(body)
    payload = keyword.encode() + b"\x00" + bytes([1 if compress else 0]) + b"\x00" + b"\x00" + b"\x00" + body
    return _png_chunk(b"iTXt", payload)


def _write_png_with_raw_chunk(out_dir, name, image, chunk):
    """在 IHDR 之后插入一个自定义文本块（即固定偏移解析读不到的位置）。"""
    import io

    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    data = buffer.getvalue()
    ihdr_end = 8 + 12 + 13  # signature + IHDR(length+type+data+crc)
    with open(os.path.join(out_dir, name), "wb") as handle:
        handle.write(data[:ihdr_end] + chunk + data[ihdr_end:])
    record(name)


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

    # AI 生图提示词：PNG 文本块（tEXt/zTXt/iTXt）与 JPEG EXIF UserComment
    webui_params = ("masterpiece, best quality, 1girl, silver hair, city lights\n"
                    "Negative prompt: lowres, bad anatomy, watermark\n"
                    'Steps: 28, Sampler: DPM++ 2M Karras, CFG scale: 7, Seed: 123456789, '
                    'Size: 512x768, Model hash: a1b2c3d4, Model: anything-v5')
    comfy_json = '{"3": {"class_type": "KSampler", "inputs": {"seed": 42, "steps": 20}}, "9": {"class_type": "SaveImage"}}'

    save(out_dir, "ai_webui.png", img, pnginfo=_png_text({"parameters": webui_params}))
    save(out_dir, "ai_comfy.png", img, pnginfo=_png_text({"prompt": comfy_json}))
    save(out_dir, "ai_both.png", img, pnginfo=_png_text({"parameters": webui_params, "prompt": comfy_json}))

    # 压缩文本块（zTXt）与 UTF-8 文本块（iTXt）：旧实现按固定偏移读取，这两种布局都读不到
    _write_png_with_raw_chunk(out_dir, "ai_ztxt.png", img, _ztxt_chunk("parameters", webui_params))
    _write_png_with_raw_chunk(out_dir, "ai_itxt.png", img, _itxt_chunk("parameters", webui_params))
    _write_png_with_raw_chunk(out_dir, "ai_itxt_z.png", img, _itxt_chunk("parameters", webui_params, compress=True))

    exif = Image.Exif()
    exif[0x9286] = b"ASCII\x00\x00\x00" + webui_params.encode("utf-8")
    save(out_dir, "ai_usercomment.jpg", img, exif=exif, quality=95)

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

    # 带显示旋转矩阵的竖拍视频：手机实况视频的常态是"编码尺寸是横的、显示要按
    # display matrix 转 90°"。ffmpeg 各版本都不再接受 -metadata rotate=，
    # 直接把 clip.mp4 的 tkhd 矩阵改掉最可靠（解码器按它转帧，播放端要拿旋转后的尺寸）。
    make_rotated_video(out_dir)

    # 带音轨的视频（AAC / Opus），用于验证音频解码与实况照片播放
    run_ffmpeg(out_dir, "sound.mp4", ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "2000k",
                                      "-c:a", "aac", "-b:a", "128k"],
               audio="sine=frequency=440:sample_rate=48000:duration=3")
    run_ffmpeg(out_dir, "sound.webm", ["-c:v", "libvpx-vp9", "-b:v", "2000k",
                                       "-c:a", "libopus", "-b:a", "96k"],
               audio="sine=frequency=660:sample_rate=48000:duration=3")

    # 实况照片：Android MotionPhoto（JPEG+尾部MP4）与 iOS LIVP（zip 内含 jpg+mov）
    sound_path = os.path.join(out_dir, "sound.mp4")
    if os.path.exists(sound_path):
        _make_motion_photo(out_dir, "motionphoto.jpg", img, sound_path)
        run_ffmpeg(out_dir, "livp_video.mov", ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-b:v", "2000k",
                                               "-c:a", "aac", "-b:a", "128k"],
                   audio="sine=frequency=520:sample_rate=48000:duration=3")
        mov_path = os.path.join(out_dir, "livp_video.mov")
        if os.path.exists(mov_path):
            _make_livp(out_dir, "live.livp", img, mov_path)

    print(f"generated {len(made)} files in {out_dir}")
    if failed:
        print("skipped/failed:")
        for item in failed:
            print("  -", item)


if __name__ == "__main__":
    main()
