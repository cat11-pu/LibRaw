"""Pixel-exact comparison: LibRaw vs the independent reference decoder.

For each of the 50 valid DNGs, dump the unprocessed Bayer mosaic through
LibRaw (bin/dump_raw) and through gen/dng.py's read_dng(), then compare
width, height and every single pixel.
"""
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "gen"))
from dng import read_dng  # noqa: E402

BIN = os.path.join(ROOT, "bin", "dump_raw")
VALID = os.path.join(ROOT, "valid")

ENV = dict(os.environ)
ENV["ASAN_OPTIONS"] = "detect_leaks=1:allocator_may_return_null=1"
ENV["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"


def compare_one(path, tmp):
    ref_w, ref_h, cfa, ref_px = read_dng(path)
    p = subprocess.run([BIN, path, tmp], env=ENV, capture_output=True,
                       text=True, timeout=120)
    if p.returncode != 0:
        return dict(ok=False, reason="libraw error: %s" % p.stderr.strip())
    with open(tmp, "rb") as f:
        d = f.read()
    if d[:4] != b"RWD1":
        return dict(ok=False, reason="bad dump magic")
    w, h = struct.unpack_from("<II", d, 4)
    if (w, h) != (ref_w, ref_h):
        return dict(ok=False, reason="geometry %dx%d != %dx%d"
                    % (w, h, ref_w, ref_h))
    got = struct.unpack_from("<%dH" % (w * h), d, 12)
    if list(got) == ref_px:
        return dict(ok=True, width=w, height=h, pixels=w * h)
    diffs = [(i, a, b) for i, (a, b) in enumerate(zip(ref_px, got)) if a != b]
    return dict(ok=False, reason="%d/%d pixels differ" % (len(diffs), w * h),
                first=diffs[:5])


def main():
    with open(os.path.join(VALID, "manifest.json")) as f:
        manifest = json.load(f)
    tmp = os.path.join(ROOT, "build", "compare.tmp")
    passed, failed = [], []
    total_px = 0
    for entry in manifest:
        path = os.path.join(VALID, entry["file"])
        r = compare_one(path, tmp)
        r["file"] = entry["file"]
        if r["ok"]:
            passed.append(r)
            total_px += r["pixels"]
        else:
            failed.append(r)
            print("FAIL %s: %s" % (entry["file"], r["reason"]))
    print("\n=== pixel-exact comparison summary ===")
    print("files: %d  identical: %d  mismatched: %d  pixels checked: %d"
          % (len(manifest), len(passed), len(failed), total_px))
    if failed:
        print("mismatching files:")
        for r in failed:
            print("  %s: %s" % (r["file"], r["reason"]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
