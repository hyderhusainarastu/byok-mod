#!/usr/bin/env python3
"""Fake ScreenMirrorHelper for tests/host/test_mirror.py.

A standalone script (run as a real subprocess, exactly like
`byok.mirror.spawn_capture` would run the real Swift binary) that emits
synthetic `SMH1` binary frame records to stdout without touching the
screen, ScreenCaptureKit, or any real capture API. It only understands
enough of the real helper's `capture` CLI to be a drop-in stdin-less
stand-in: `--width`/`--height`/`--fps` are accepted and ignored beyond
sizing frames (the fps limiter under test lives in `byok.mirror`, not
here, so this script never sleeps between frames).

Extra test-only flags (not part of the real helper's CLI):

  --frames N          number of frames to emit before exiting 0 (default 3)
  --pattern P          "static" (identical canvas every frame) or
                        "moving-block" (a small black square that slides
                        right by --block-size pixels each frame, otherwise
                        white -- exercises bounded dirty-rect diffing)
  --block-size N       side length of the moving block (default 4)
  --permission-fail    ignore every other flag, print a message to stderr,
                        and exit 3 (models Screen Recording permission
                        denied -- byok.mirror.HelperPermissionError)
  --exit-code N        after emitting --frames frames, exit with this code
                        instead of 0 (models a helper crash)
"""
import argparse
import struct
import sys

MAGIC = b"SMH1"


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("capture", nargs="?")  # accepts the real subcommand positionally, ignored
    p.add_argument("--display", default=None)
    p.add_argument("--window", default=None)
    p.add_argument("--width", type=int, default=32)
    p.add_argument("--height", type=int, default=16)
    p.add_argument("--fps", type=float, default=2)
    p.add_argument("--threshold", type=int, default=8)
    p.add_argument("--frames", type=int, default=3)
    p.add_argument("--pattern", choices=("static", "moving-block"), default="static")
    p.add_argument("--block-size", type=int, default=4)
    p.add_argument("--permission-fail", action="store_true")
    p.add_argument("--exit-code", type=int, default=0)
    args, _unknown = p.parse_known_args()

    if args.permission_fail:
        sys.stderr.write(
            "FakeHelper: Screen Recording permission is not granted "
            "(synthetic, for tests/host/test_mirror.py).\n"
        )
        sys.stderr.flush()
        return 3

    w, h = args.width, args.height
    out = sys.stdout.buffer

    for i in range(args.frames):
        canvas = bytearray(b"\xff" * (w * h))  # white background
        if args.pattern == "moving-block":
            bs = args.block_size
            x0 = min(max(0, w - bs), i * bs)
            y0 = max(0, (h - bs) // 2)
            for y in range(y0, min(h, y0 + bs)):
                row = y * w
                for x in range(x0, min(w, x0 + bs)):
                    canvas[row + x] = 0x00  # black block
        header = struct.pack(">4sHHHHHH", MAGIC, w, h, 0, 0, w, h)
        out.write(header)
        out.write(bytes(canvas))
        out.flush()

    return args.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
