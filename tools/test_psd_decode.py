"""PSD/PSB 像素回归；输入、设置、导出与报告均放在独立 TEMP 目录。

python -I tools/test_psd_decode.py --real-corpus D:/Downloads/test/psd
"""

import argparse
import ctypes as ct
from ctypes import wintypes as wt
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

import numpy as np
from PIL import Image, ImageCms

REPO = Path(__file__).resolve().parents[1]


def section(data, wide=False):
    return struct.pack(">Q" if wide else ">I", len(data)) + data


def fixture(version, mode, depth, compression, alpha=False, spot=False, tagged=False, width=17, height=5, icc=b""):
    base = {1: 1, 3: 3, 4: 4}[mode]
    maximum = (1 << depth) - 1
    y, x = np.indices((height, width))
    channels = [((x * (37 + c * 3) + y * 71 + c * 53) % 256) * (maximum // 255)
                for c in range(base)]
    opacity = np.array([255, 128, 64, 0, 255], np.uint64)[y] * (maximum // 255)
    rgb = np.stack(channels[:3] if mode == 3 else [channels[0]] * 3, axis=-1)
    if mode == 4:
        rgb = np.stack(channels[:3], axis=-1).astype(np.uint64) * channels[3][..., None]
        rgb = (rgb * 255 + maximum * maximum // 2) // (maximum * maximum)
    else:
        rgb = (rgb * 255 + maximum // 2) // maximum
    expected = np.concatenate([rgb, ((opacity * 255 + maximum // 2) // maximum
                                     if alpha else np.full((height, width), 255))[..., None]], axis=-1).astype(np.uint8)
    if alpha:
        if mode != 4:
            channels = [(c * opacity + maximum // 2) // maximum + maximum - opacity for c in channels]
            expected[opacity == 0, :3] = 0
        channels.append(opacity)
    if spot:
        channels.append(np.zeros((height, width), np.uint16))
    rows = []
    for channel in channels:
        for row in channel:
            values = row.astype(np.uint16)
            if compression == 3:
                values = np.concatenate([values[:1], (values[1:] - values[:-1]) & maximum])
            rows.append(values.astype(np.uint8 if depth == 8 else ">u2").tobytes())
    if compression == 1:
        packed = []
        for row in rows:
            if len(set(row)) == 1:
                packed.append(bytes([128, 257 - len(row), row[0], 128]))
            else:
                packed.append(b"\x80" + b"".join(bytes([len(row[i:i + 128]) - 1]) + row[i:i + 128]
                                                for i in range(0, len(row), 128)) + b"\x80")
        payload = b"".join(struct.pack(">I" if version == 2 else ">H", len(r)) for r in packed) + b"".join(packed)
    else:
        payload = b"".join(rows)
        if compression >= 2:
            payload = zlib.compress(payload)
    count = struct.pack(">h", -1 if alpha else 1)
    if tagged:
        layers = section(b"", version == 2) + section(b"")
        layers += b"8BIMLr16" + section(count, version == 2) + b"\0\0"
    else:
        layers = section(count, version == 2) + section(b"")
    header = struct.pack(">4sH6xHIIHH", b"8BPS", version, len(channels), height, width, depth, mode)
    # 非空奇数长度 Pascal 名称与资源 payload，检查两种独立 padding。
    resources = b"8BIM" + struct.pack(">H", 1000) + b"\x02ab\0" + section(b"x") + b"\0"
    if icc:
        resources += b"8BIM" + struct.pack(">H", 1039) + b"\0\0" + section(icc) + b"\0" * (len(icc) % 2)
    return header + section(b"") + section(resources) + section(layers, version == 2) + struct.pack(">H", compression) + payload, expected


def linear_gray_icc():
    # 最小的 ICC v2 灰度显示 profile，线性 TRC 与 sRGB 有显著差异。
    white = struct.pack(">3i", 63190, 65536, 54061)
    tags = [(b"wtpt", b"XYZ " + b"\0" * 4 + white),
            (b"kTRC", b"curv" + b"\0" * 4 + struct.pack(">IH", 1, 256))]
    header = bytearray(128)
    struct.pack_into(">I", header, 8, 0x02100000)
    header[12:24] = b"mntrGRAYXYZ "
    struct.pack_into(">6H", header, 24, 2000, 1, 1, 0, 0, 0)
    header[36:40], header[68:80] = b"acsp", white
    table, payload = struct.pack(">I", len(tags)), b""
    for name, value in tags:
        table += name + struct.pack(">II", 128 + 4 + len(tags) * 12 + len(payload), len(value))
        payload += value + b"\0" * (-len(value) % 4)
    struct.pack_into(">I", header, 0, len(header) + len(table) + len(payload))
    return bytes(header) + table + payload


def layer_channel(plane, version, depth, compression):
    rows = []
    for values in plane.astype(np.uint16) * (257 if depth == 16 else 1):
        if compression == 3:
            values = np.concatenate([values[:1], values[1:] - values[:-1]])
        row = values.astype(np.uint8 if depth == 8 else ">u2").tobytes()
        if compression == 1:
            row = b"".join(bytes([len(row[i:i + 128]) - 1]) + row[i:i + 128] for i in range(0, len(row), 128))
        rows.append(row)
    payload = b"".join(rows)
    if compression == 1:
        payload = b"".join(struct.pack(">I" if version == 2 else ">H", len(row)) for row in rows) + payload
    if compression >= 2:
        payload = zlib.compress(payload)
    return struct.pack(">H", compression) + payload


def layered_fixture(version, depth, compression, opaque=True, saved_composite=False, blend=b"norm", hidden_group=False):
    width, height = 13, 7
    y, x = np.indices((5, 8))
    foreground = np.stack([(x * 41) % 256, (y * 67) % 256,
                           (x * 17 + y * 29) % 256, (x * 31 + y * 23) % 256], axis=-1).astype(np.uint8)
    accent = np.empty((4, 6, 4), np.uint8)
    accent[:] = [40, 90, 240, 160]
    layers = [(Image.new("RGBA", (width, height), "white"), (0, 0), opaque, 255, 255, False),
              (Image.fromarray(foreground), (2, -1), True, 128, 192, True),
              (Image.new("RGBA", (width, height), "red"), (0, 0), False, 255, 255, True),
              (Image.fromarray(accent), (-2, 4), True, 192, 255, True),
              (Image.new("RGBA", (2, 2), "black"), (30, 30), True, 255, 255, False)]
    expected = Image.new("RGBA", (width, height))
    records, pixels = [], []
    for index, (image, (left, top), visible, opacity, fill, has_alpha) in enumerate(layers):
        a = np.asarray(image)
        channels = [(c, layer_channel(a[..., c], version, depth, compression)) for c in range(3)]
        if has_alpha:
            channels.insert(0, (-1, layer_channel(a[..., 3], version, depth, compression)))
        # 正的额外通道是独立 alpha/专色，不是透明蒙版。
        channels.append((3, layer_channel(np.zeros(a.shape[:2], np.uint8), version, depth, compression)))
        extra = section(b"") + section(b"") + b"\0" * 4
        extra += b"8BIMiOpa" + section(bytes([fill, 0, 0, 0]))
        if hidden_group and index == 0:
            extra += b"8BIMlsct" + section(struct.pack(">I", 1))
        record = struct.pack(">4iH", top, left, top + image.height, left + image.width, len(channels))
        record += b"".join(struct.pack(">h", c) + struct.pack(">Q" if version == 2 else ">I", len(data))
                           for c, data in channels)
        record += b"8BIM" + blend + bytes([opacity, 0, 8 if visible else 10, 0]) + section(extra)
        records.append(record)
        pixels.extend(data for _, data in channels)
        if visible:
            patch = np.asarray(image).copy()
            effective_opacity = (opacity * fill + 127) // 255
            patch[..., 3] = (patch[..., 3].astype(np.uint32) * effective_opacity + 127) // 255
            canvas = Image.new("RGBA", expected.size)
            canvas.paste(Image.fromarray(patch), (left, top))
            expected = Image.alpha_composite(expected, canvas)
    info = struct.pack(">h", len(layers)) + b"".join(records) + b"".join(pixels)
    info += b"\0" * (len(info) % 2)
    if depth == 16:
        layers_section = section(b"", version == 2) + section(b"")
        layers_section += b"8BIMLr16" + section(info, version == 2) + b"\0" * (-len(info) % 4)
    else:
        layers_section = section(info, version == 2) + section(b"")
    version_info = struct.pack(">IBIII", 1, int(saved_composite), 0, 0, 1)
    resources = b"8BIM" + struct.pack(">H", 1057) + b"\0\0" + section(version_info) + b"\0"
    header = struct.pack(">4sH6xHIIHH", b"8BPS", version, 3, height, width, depth, 3)
    dummy = b"\0\0" + b"\xff" * (width * height * 3 * (depth // 8))
    data = header + section(b"") + section(resources) + section(layers_section, version == 2) + dummy
    if saved_composite:
        expected = Image.new("RGBA", expected.size, "white")
    return data, np.asarray(expected).copy()


def has_saved_composite(data):
    offset = 26
    length = struct.unpack_from(">I", data, offset)[0]
    offset += 4 + length
    length = struct.unpack_from(">I", data, offset)[0]
    offset += 4
    end = offset + length
    while offset < end:
        resource_id = struct.unpack_from(">H", data, offset + 4)[0]
        name_length = data[offset + 6]
        offset += 6 + ((name_length + 2) // 2) * 2
        length = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if resource_id == 1057:
            return data[offset + 4] != 0
        offset += length + length % 2
    return True


def run(exe, root, name, args, expected_exit=0):
    report = root / f"{name}.txt"
    result = subprocess.run([str(exe), "--probe", *map(str, args), "--out", str(report)],
                            cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            timeout=120, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode != expected_exit:
        raise AssertionError(f"{name}: exit={result.returncode}\n{report.read_text(encoding='utf-8-sig')}")
    return report.read_text(encoding="utf-8-sig")


def thumbnail_decoder(dll_path):
    """直接加载本轮 DLL，避免依赖系统注册或 Explorer 的旧缩略图缓存。"""
    dll = ct.WinDLL(str(dll_path))
    guid = lambda value: ct.create_string_buffer(uuid.UUID(value).bytes_le)
    factory = ct.c_void_p()
    dll.DllGetClassObject.argtypes = [ct.c_void_p, ct.c_void_p, ct.POINTER(ct.c_void_p)]
    hr = dll.DllGetClassObject(guid("6677a695-a3cd-4e32-8823-ead9c293a770"),
                              guid("00000001-0000-0000-c000-000000000046"), ct.byref(factory))
    assert hr == 0, hex(hr & 0xffffffff)

    def method(obj, index, *argtypes):
        vtable = ct.cast(obj, ct.POINTER(ct.POINTER(ct.c_void_p))).contents
        return ct.WINFUNCTYPE(ct.c_long, ct.c_void_p, *argtypes)(vtable[index])

    shlwapi, gdi = ct.WinDLL("shlwapi"), ct.WinDLL("gdi32")
    shlwapi.SHCreateMemStream.argtypes = [ct.c_void_p, wt.UINT]
    shlwapi.SHCreateMemStream.restype = ct.c_void_p
    gdi.GetObjectW.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p]
    gdi.DeleteObject.argtypes = [ct.c_void_p]

    class Bitmap(ct.Structure):
        _fields_ = [("type", wt.LONG), ("width", wt.LONG), ("height", wt.LONG),
                    ("stride", wt.LONG), ("planes", wt.WORD), ("bits", wt.WORD), ("pixels", ct.c_void_p)]

    def decode(data, edge=4096):
        initializer, provider, stream, bitmap = ct.c_void_p(), ct.c_void_p(), ct.c_void_p(), ct.c_void_p()
        try:
            hr = method(factory, 3, ct.c_void_p, ct.c_void_p, ct.c_void_p)(factory, None,
                guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), ct.byref(initializer))
            assert hr == 0
            buffer = ct.create_string_buffer(data)
            stream = ct.c_void_p(shlwapi.SHCreateMemStream(buffer, len(data)))
            assert stream
            assert method(initializer, 3, ct.c_void_p, wt.DWORD)(initializer, stream, 0) == 0
            assert method(initializer, 0, ct.c_void_p, ct.c_void_p)(initializer,
                guid("e357fccd-a995-4576-b01f-234630154e96"), ct.byref(provider)) == 0
            alpha = ct.c_int()
            hr = method(provider, 3, wt.UINT, ct.c_void_p, ct.c_void_p)(provider, edge, ct.byref(bitmap), ct.byref(alpha))
            if hr != 0:
                return None
            info = Bitmap()
            assert gdi.GetObjectW(bitmap, ct.sizeof(info), ct.byref(info)) == ct.sizeof(info)
            pixels = ct.string_at(info.pixels, info.stride * abs(info.height))
            return np.asarray(Image.frombytes("RGBA", (info.width, abs(info.height)), pixels, "raw", "BGRA")).copy()
        finally:
            if bitmap:
                gdi.DeleteObject(bitmap)
            for obj in (provider, initializer, stream):
                if obj:
                    method(obj, 2)(obj)

    # 保持 DLL 与 factory 存活，调用者生命周期结束时由进程回收 factory。
    decode.factory = factory
    decode.dll = dll
    return decode


def psb_as_psd(data):
    """仅重包装合成图以供 Pillow 独立解码，不改原文件或像素压缩数据。"""
    offset = 26
    sections = []
    for fmt in (">I", ">I", ">Q"):
        length = struct.unpack_from(fmt, data, offset)[0]
        offset += struct.calcsize(fmt)
        sections.append(data[offset:offset + length])
        offset += length
    channels, height = struct.unpack_from(">HI", data, 12)
    compression = struct.unpack_from(">H", data, offset)[0]
    offset += 2
    payload = data[offset:]
    if compression == 1:
        count = channels * height
        lengths = struct.unpack_from(f">{count}I", payload)
        payload = struct.pack(f">{count}H", *lengths) + payload[count * 4:]
    return data[:4] + b"\0\1" + data[6:26] + section(sections[0]) + section(sections[1]) + section(b"") + struct.pack(">H", compression) + payload


def identity(path):
    stat = path.stat()
    return (stat.st_size, stat.st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=REPO / "x64/Release/JarkViewer.exe")
    parser.add_argument("--real-corpus", type=Path)
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="jv-psd-regression-"))
    print(root, flush=True)
    app = root / "app"
    app.mkdir()
    exe = app / "JarkViewer.exe"
    shutil.copy2(args.exe, exe)
    dll = args.exe.resolve().with_name("JarkThumbnailProvider.dll")
    shutil.copy2(dll, app / dll.name)
    thumbnail = thumbnail_decoder(app / dll.name)
    inputs, outputs = root / "inputs", root / "outputs"
    inputs.mkdir()
    expectations = {}
    for version in (1, 2):
        for depth in (8, 16):
            for compression in (0, 1, 2, 3):
                for opaque in (False, True):
                    data, expected = layered_fixture(version, depth, compression, opaque)
                    name = f"layers-v{version}-d{depth}-c{compression}-opaque{int(opaque)}"
                    (inputs / (name + (".psb" if version == 2 else ".psd"))).write_bytes(data)
                    expectations[name] = expected
                    actual = thumbnail(data)
                    premultiplied = expected.copy()
                    premultiplied[..., :3] = (expected[..., :3].astype(np.uint32) * expected[..., 3:] + 127) // 255
                    assert actual is not None and actual.shape == expected.shape, name
                    assert np.abs(actual.astype(int) - premultiplied.astype(int)).max() <= 1, name
        # 合成图真实存在且为纯白时必须使用它，不能按“像素全白”猜测需要重建图层。
        data, expected = layered_fixture(version, 8, 1, saved_composite=True)
        name = f"real-white-composite-v{version}"
        (inputs / (name + (".psb" if version == 2 else ".psd"))).write_bytes(data)
        expectations[name] = expected
        assert np.array_equal(thumbnail(data), expected), name
    for version in (1, 2):
        for mode in (1, 3, 4):
            for depth in (8, 16):
                for compression in (0, 1, 2, 3):
                    for alpha in (False, True):
                        name = f"v{version}-m{mode}-d{depth}-c{compression}-a{int(alpha)}"
                        path = inputs / (name + (".psb" if version == 2 else ".psd"))
                        data, expected = fixture(version, mode, depth, compression, alpha, spot=True, tagged=depth == 16)
                        path.write_bytes(data)
                        expectations[name] = expected
                        actual = thumbnail(data)
                        premultiplied = expected.copy()
                        premultiplied[..., :3] = (expected[..., :3].astype(np.uint32) * expected[..., 3:] + 127) // 255
                        assert actual is not None and actual.shape == expected.shape, name
                        assert np.abs(actual.astype(int) - premultiplied.astype(int)).max() <= 1, name
    for version in (1, 2):
        icc = linear_gray_icc()
        data, expected = fixture(version, 1, 16, 3, icc=icc)
        reference = ImageCms.profileToProfile(Image.fromarray(expected[..., 0]),
            ImageCms.ImageCmsProfile(io.BytesIO(icc)), ImageCms.createProfile("sRGB"), outputMode="RGB")
        expected[..., :3] = np.asarray(reference)
        name = f"gray-icc-v{version}"
        (inputs / (name + (".psb" if version == 2 else ".psd"))).write_bytes(data)
        expectations[name] = expected
        assert np.abs(thumbnail(data).astype(int) - expected.astype(int)).max() <= 1, name
    # 单行 RLE 数据超过 65535 字节，检查 PSB 的 32 位行长度没有被截为 PSD 的 16 位。
    data, expected = fixture(2, 3, 16, 1, width=40000, height=1)
    (inputs / "wide-row.psb").write_bytes(data)
    expectations["wide-row"] = expected
    assert np.abs(thumbnail(data, 50000).astype(int) - expected.astype(int)).max() <= 1
    # 文件头路由必须覆盖错扩展名、无扩展名；PSB 关联表另由源码检查覆盖。
    data, expected = fixture(2, 4, 8, 1)
    for name in ("misnamed.jpg", "extensionless"):
        (inputs / name).write_bytes(data)
        expectations[Path(name).stem] = expected
    files = sorted(inputs.iterdir())
    text = run(exe, root, "generated-probe", [*files, "--full"])
    assert text.count("[   Still]") == len(files), text
    run(exe, root, "generated-batch", ["--batch", *files, "--to", "png", "--out-dir", outputs])
    for stem, expected in expectations.items():
        actual = np.asarray(Image.open(outputs / f"{stem}.png").convert("RGBA"))
        # 批量转换会统一铺白底；透明通道本身由上面的 DLL 像素断言验证。
        flattened = expected.copy()
        alpha = expected[..., 3:].astype(np.uint32)
        flattened[..., :3] = (expected[..., :3].astype(np.uint32) * alpha + 255 * (255 - alpha) + 127) // 255
        flattened[..., 3] = 255
        assert actual.shape == expected.shape and np.abs(actual.astype(int) - flattened.astype(int)).max() <= 1, stem
    # CMYK 坏输入不会被只支持 RGB 的旧解码器误判为成功。
    malformed = {"truncated-header": data[:25], "truncated-rle": data[:-1],
                 "section-overflow": data[:26] + b"\xff" * 8,
                 "bad-rle-row-length": data[:-100]}
    for compression in (2, 3):
        zipped, _ = fixture(2, 4, 16, compression)
        malformed[f"zip-{compression}-missing-trailer"] = zipped[:-3]
        malformed[f"zip-{compression}-trailing-data"] = zipped + b"\0"
    raw, _ = fixture(2, 4, 8, 0)
    malformed["truncated-raw"] = raw[:-1]
    malformed["invalid-channels"] = raw[:12] + b"\0\3" + raw[14:]
    malformed["invalid-dimensions"] = raw[:14] + b"\0" * 4 + raw[18:]
    malformed["psb-section-overflow"] = struct.pack(">4sH6xHIIHH", b"8BPS", 2, 4, 5, 17, 8, 4) + section(b"") + section(b"") + b"\xff" * 8
    for version in (1, 2):
        data, _ = layered_fixture(version, 8, 1, blend=b"mul ")
        malformed[f"no-composite-unsupported-blend-v{version}"] = data
        data, _ = layered_fixture(version, 8, 1, opaque=False, hidden_group=True)
        malformed[f"no-composite-hidden-group-v{version}"] = data
        data, _ = layered_fixture(version, 8, 1)
        # 保留资源中的“无合成图”标记，删除图层像素；旧兜底不能将白色占位当作成功。
        at = 26 + 4 + 4
        at += struct.unpack_from(">I", data, 30)[0]
        malformed[f"no-composite-truncated-layers-v{version}"] = data[:at + (8 if version == 2 else 4) + 10]
    bad_paths = []
    for name, bad in malformed.items():
        assert thumbnail(bad) is None, name
        path = root / (name + ".psb")
        path.write_bytes(bad)
        bad_paths.append(path)
    text = run(exe, root, "malformed-probe", [*bad_paths, "--full"], 1)
    assert text.count("[  FAILED]") == len(bad_paths), text
    real_results = []
    if args.real_corpus:
        real = sorted(p for p in args.real_corpus.iterdir() if p.suffix.lower() in (".psd", ".psb"))
        assert real, "no PSD/PSB samples"
        before = {p: identity(p) for p in real}
        try:
            run(exe, root, "real-probe", [*real, "--full"])
            destination = root / "real-output"
            run(exe, root, "real-batch", ["--batch", *real, "--to", "png", "--out-dir", destination])
            for path in real:
                raw = path.read_bytes()
                if not has_saved_composite(raw):
                    reference_path = path.with_suffix(".png")
                    assert reference_path.exists(), f"missing PNG reference: {path}"
                    reference = Image.open(reference_path)
                else:
                    reference = Image.open(io.BytesIO(psb_as_psd(raw) if path.suffix.lower() == ".psb" else raw))
                if reference.info.get("icc_profile"):
                    profile = ImageCms.ImageCmsProfile(io.BytesIO(reference.info["icc_profile"]))
                    reference = ImageCms.profileToProfile(reference, profile, ImageCms.createProfile("sRGB"), outputMode="RGB")
                elif reference.mode == "RGBA":
                    reference = Image.alpha_composite(Image.new("RGBA", reference.size, "white"), reference).convert("RGB")
                else:
                    reference = reference.convert("RGB")
                actual = Image.open(destination / (path.stem + ".png")).convert("RGB")
                error = np.abs(np.asarray(actual).astype(int) - np.asarray(reference).astype(int))
                assert actual.size == reference.size and error.max() <= 2, (path, error.max())
                thumb = thumbnail(raw, 512)
                assert thumb is not None and thumb.shape[1] == 512, path
                Image.fromarray(thumb).save(root / (path.stem + "-thumbnail.png"))
                real_results.append({"file": str(path), "size": actual.size, "max_pixel_error": int(error.max()),
                                     "mean_pixel_error": float(error.mean())})
        finally:
            assert before == {p: identity(p) for p in real}, "original inputs changed"
    result = {"generated": len(files), "thumbnail_pixels": len(expectations) - 2,
              "malformed_rejected": len(bad_paths), "real": real_results}
    (root / "results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
