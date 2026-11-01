"""重构回归入口：生成语料、隔离程序/设置、检查报告并保存 JSON 汇总。

python -I tools/test_refactor_audit.py --gui --stress-rounds 4
所有写入均在独立临时目录；--real-corpus 指定的实机语料只读。
依赖 Python + Pillow/numpy、ffmpeg、PowerShell 7，以及已完成的 Release 构建。
"""

import argparse
import concurrent.futures
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent
NO_CONSOLE = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=REPO / "x64/Release/JarkViewer.exe")
    parser.add_argument("--gui", action="store_true", help="串行运行会操作鼠标/键盘的界面回归")
    parser.add_argument("--stress-rounds", type=int, default=4)
    parser.add_argument("--real-corpus", type=Path, help="每个子目录取至多三份 <=64MiB 的真实样本，只读")
    parser.add_argument("--cases", default="*", help="逗号分隔的用例名称/通配符；依赖语料始终生成")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="JarkViewer-refactor-"))
    (root / "app").mkdir()
    exe = root / "app/JarkViewer.exe"
    shutil.copy2(args.exe.resolve(), exe)
    with exe.open("rb") as executable:
        digest = hashlib.file_digest(executable, "sha256").hexdigest()
    (root / "metadata.json").write_text(json.dumps({"executable": str(args.exe.resolve()),
        "sha256": digest, "cases": args.cases, "gui": args.gui,
        "head": subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "HEAD"],
                                         text=True, creationflags=NO_CONSOLE).strip()}, indent=2), encoding="utf-8")
    results = []
    print(f"Reports: {root}", flush=True)

    def record(row):
        if row.get("filtered"):
            return
        results.append(row)
        (root / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"{'PASS' if row['ok'] else 'FAIL'} {row['name']} ({row.get('seconds', 0):.1f}s)", flush=True)

    def command(name, argv, timeout=180, validator=None, report=None):
        if not name.startswith("generate") and not any(fnmatch.fnmatchcase(name, pattern)
                                                        for pattern in args.cases.split(",")):
            return {"name": name, "filtered": True}
        started = time.monotonic()
        log = root / f"{name}.log"
        try:
            with log.open("wb") as output:
                process = subprocess.run(list(map(str, argv)), cwd=REPO, stdout=output,
                                         stderr=subprocess.STDOUT, timeout=timeout, creationflags=NO_CONSOLE)
            text = report.read_text(encoding="utf-8-sig") if report and report.exists() else ""
            ok = validator(process.returncode, text) if validator else process.returncode == 0
            return {"name": name, "ok": ok, "exit": process.returncode,
                    "seconds": round(time.monotonic() - started, 2), "report": str(report or log),
                    "assertions": len(re.findall(r"\[ok\]|^PASS ", text, re.MULTILINE)),
                    "failures": [line for line in text.splitlines() if "FAIL" in line],
                    "skips": [line for line in text.splitlines() if "跳过" in line]}
        except (subprocess.TimeoutExpired, OSError) as error:
            return {"name": name, "ok": False, "seconds": round(time.monotonic() - started, 2),
                    "error": str(error), "report": str(report or log)}

    def probe(name, flags, files=(), timeout=180, validator=None):
        report = root / f"{name}.txt"
        return command(name, [exe, "--probe", *flags, *files, "--out", report], timeout,
                       validator or (lambda code, text: code == 0 and bool(text) and "FAIL" not in text), report)

    for script in ("check_source_invariants.ps1", "verify_source_invariant_checks.ps1"):
        record(command(script[:-4], ["pwsh", "-NoProfile", "-File", REPO / "tools" / script]))
    corpus = root / "corpus"
    record(command("generate", [sys.executable, "-I", REPO / "tools/gen_testdata.py", corpus]))
    if not (corpus / "sound.mp4").exists():
        print("生成语料不完整，见 generate.log", flush=True)
        return 1

    synthetic = ["navigation", "color", "resample", "cache", "spectrum", "svg", "sort", "exif", "batch"]
    for name in synthetic:
        record(probe(name, [f"--{name}-test"]))
    record(probe("annotate", ["--annotate"]))
    record(probe("language", ["--lang-test"]))
    record(probe("thumbnail", ["--thumbnail-test", "--out-dir", root / "thumbnails"]))
    record(probe("shell-thumbnail", ["--shell-thumbnail", "--out-dir", root / "shell"], [corpus / "alpha.png"]))

    corrupt = {"corrupt.png", "empty.png", "text.txt", "fake.livp", "truncated.jpg"}

    def decode_expected(code, text):
        failures = {Path(line.split(" | ", 1)[1]).name for line in text.splitlines()
                    if line.startswith("[  FAILED]") and " | " in line}
        return code == 1 and failures == corrupt and "51 ok, 5 failed, 56 total" in text

    record(probe("decode-generated", [], sorted(corpus.iterdir()), validator=decode_expected))
    # 每种格式取八个不同位置的截断，失败必须干净返回，不能崩溃或一直等待。
    malformed = root / "malformed"
    malformed.mkdir()
    for source_name in ("icon.ico", "still.png", "still.jpg", "vector.svg", "still.qoi",
                        "still.pcx", "still.tga", "float.pfm", "anim.gif"):
        source = (corpus / source_name).read_bytes()
        files = []
        for index in range(8):
            file = malformed / f"{Path(source_name).stem}-{index}{Path(source_name).suffix}"
            file.write_bytes(source[:max(1, len(source) * (index + 1) // 9)])
            files.append(file)
        record(probe(f"truncated-{source_name.replace('.', '-')}", [], files, 20,
                     validator=lambda code, text: code in (0, 1) and "8 total" in text))
    # video/hw 入口接收独立媒体；实况容器交给 playback 先提取其中的视频。
    media = [corpus / name for name in ("clip.mp4", "clip.mkv", "clip.avi", "clip.mov", "clip.webm",
                                        "clip_rot90.mp4", "sound.mp4", "sound.webm", "tiny.mp4")]
    record(probe("video", ["--video-test"], media, 240))
    record(probe("hardware", ["--hw-test"], media))
    record(probe("playback", ["--playback-test"], [*media, corpus / "motionphoto.jpg", corpus / "live.livp"]))
    record(probe("audio", ["--audio-test"], [corpus / "sound.mp4"]))

    audio = root / "audio"
    audio.mkdir()
    for extension, codec in [("wav", "pcm_s16le"), ("flac", "flac"), ("mp3", "libmp3lame"),
                             ("m4a", "aac"), ("ogg", "libvorbis"), ("opus", "libopus")]:
        file = audio / f"sound.{extension}"
        record(command(f"generate-{extension}", ["ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i",
                                                "sine=frequency=440:sample_rate=44100:duration=2", "-c:a", codec, file]))
        record(probe(f"audio-{extension}", ["--audio-test"], [file]))
    record(probe("audio-state", ["--video-test"], sorted(audio.iterdir()), 180))

    # 真正并发的 seek/暂停/恢复/销毁，用同一音频引擎与 D3D11VA 争用场景找时序问题。
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
        pending = [workers.submit(probe, f"stress-{index}", ["--video-test"],
                                  [corpus / "sound.mp4", corpus / "clip.mp4", audio / "sound.flac"], 120)
                   for index in range(max(0, args.stress_rounds))]
        for future in concurrent.futures.as_completed(pending):
            record(future.result())

    # 失败的批量命令必须返回非零，避免 CI 把一份“失败 1”的报告算作通过。
    record(probe("batch-exit", ["--batch", "--to", "unsupported-encoder", "--out-dir", root / "bad-output"],
                 [corpus / "still.png"], validator=lambda code, text: code == 1 and "失败 1" in text))

    if args.real_corpus:
        for folder in sorted(args.real_corpus.iterdir()):
            if not folder.is_dir():
                continue
            files = sorted((p for p in folder.rglob("*") if p.is_file() and 0 < p.stat().st_size <= 64 << 20),
                           key=lambda p: p.stat().st_size)
            for index, file in enumerate(files[:3]):
                record(probe(f"real-{folder.name}-{index}", [], [file], 180))

    if args.gui:
        from PIL import Image, ImageDraw
        record(command("batch-lifecycle", [sys.executable, "-I", REPO / "tools/test_batch_lifecycle.py",
                                            "--exe", exe], 60))
        navigation = root / "navigation"
        navigation.mkdir()
        for index in range(7):
            image = Image.new("RGB", (2400, 1600), (40 + 25 * index, 100, 200 - 20 * index))
            draw = ImageDraw.Draw(image)
            for x in range(0, 2400, 120):
                draw.line((x, 0, x, 1600), fill="white", width=4)
            for y in range(0, 1600, 100):
                draw.line((0, y, 2400, y), fill="white", width=4)
            draw.text((400, 300), str(index), fill="black", font_size=600)
            image.save(navigation / f"{index:02}.png")
        checker = root / "checker.mp4"
        record(command("generate-checker", ["ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i",
                       "nullsrc=s=320x240:d=5:r=25,format=yuv420p,geq=lum='if(eq(mod(floor(X/8)+floor(Y/8),2),0),16,235)':cb=128:cr=128",
                       "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", checker]))
        cases = [
            ("test_language_override.ps1", []),
            ("test_placement_memory.ps1", ["-Media", corpus / "sound.mp4", "-Image", corpus / "still.png", "-WorkDir", root / "placement"]),
            ("test_player_resize.ps1", ["-Video", corpus / "sound.mp4", "-OutDirectory", root / "resize"]),
            ("test_player_scaling.ps1", ["-Video", checker, "-OutDirectory", root / "scaling"]),
            ("test_navigation.ps1", ["-Image", navigation / "03.png", "-OutDirectory", root / "navigation-ui", "-CheckSettings"]),
            ("test_editor_text.ps1", ["-Image", navigation / "03.png", "-Out", root / "editor.png"]),
            ("test_cache_budget.ps1", ["-Image", navigation / "03.png", "-Copies", "12", "-OutDirectory", root / "cache-ui"]),
        ]
        for script, flags in cases:
            # 每个界面用新的目录，避免前一个脚本保存的窗口/鸟瞰开关影响后一个测试。
            isolated = root / script[:-4]
            isolated.mkdir()
            gui_exe = isolated / "JarkViewer.exe"
            shutil.copy2(exe, gui_exe)
            record(command(script[:-4], ["pwsh", "-NoProfile", "-File", REPO / "tools" / script,
                                        "-Exe", gui_exe, *flags], 240))

    failures = [row["name"] for row in results if not row["ok"]]
    print(f"{len(results) - len(failures)} passed, {len(failures)} failed; reports: {root}", flush=True)
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
