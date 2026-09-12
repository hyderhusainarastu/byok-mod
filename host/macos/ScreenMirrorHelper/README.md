# ScreenMirrorHelper

A small Swift Package that captures a macOS display or window with
ScreenCaptureKit, converts frames to 8-bit grayscale, letterbox-scales them
to a fixed size, and streams length-implied binary records to stdout for a
Python consumer to dither and pack for the BYOK panel.

This tool is **device-independent groundwork** for BYOK Mode 2 (mirror): it
knows nothing about the BYOK device, USB, or the wire protocol described in
`docs/protocol.md`. See `docs/host-tools.md`, section 8 ("Capture helper
(implemented)"), for the full design, the wire format, and the planned
Python consumer (`host/macos/byok/mirror.py`, not part of this package).

## Build

Requires the Xcode Command Line Tools (Swift 6.3+) — no other dependencies.

```sh
swift build -c release
# binary at .build/release/ScreenMirrorHelper
```

## Usage

```
ScreenMirrorHelper list
ScreenMirrorHelper capture (--display <id> | --window <id>)
                   [--width W] [--height H] [--fps N]
                   [--threshold T] [--cursor | --no-cursor]
                   [--png-dir DIR]
```

- **`list`** prints available displays and windows, one per line, e.g.:

  ```
  DISPLAY id=1 width=1512 height=982
  WINDOW id=1234 title="Terminal — zsh" app="Terminal" pid=511 width=800 height=500 onScreen=true
  ```

- **`capture`** captures one display or window (by the `id` `list` printed)
  and writes one `SMH1` binary record per frame to stdout, at up to `--fps`
  frames/second (default 2). Each frame is grayscale, scaled to fit inside
  `--width`×`--height` (default 240×80 — the BYOK panel's confirmed native
  resolution) preserving aspect ratio, with black letterbox padding on
  whichever axis doesn't fill exactly. `--threshold` (default 8, 0–255)
  controls how large a per-pixel grayscale delta counts as "changed" when
  computing the dirty bounding rectangle against the previous frame.

  With `--png-dir DIR`, frames are written as numbered PNGs
  (`frame-000000.png`, …) to that directory instead of the binary stream —
  useful for eyeballing the pipeline without a device or a Python consumer.

Exit codes: `0` ok, `2` bad usage, `3` Screen Recording permission not
granted, `1` other runtime error.

## Permissions

`capture`, and even `list`, need the **Screen Recording** permission (TCC)
granted to whatever terminal/binary is running this tool. This helper
**never requests or prompts for it** — it only checks the current state
with `CGPreflightScreenCaptureAccess()` (documented by Apple as
non-prompting) and, if it's not granted, prints a one-line message and
exits `3`. Grant it by hand: System Settings → Privacy & Security → Screen
Recording, then re-run the command. This makes both subcommands safe to run
non-interactively/in CI: they never hang waiting on a system dialog, and
never trigger one.

## Wire format (`SMH1` frame record)

All multi-byte integers are **big-endian**. No explicit length field — the
payload size is always `width * height` bytes, derived from the header:

| Field | Type | Meaning |
|---|---|---|
| `magic` | 4 bytes | ASCII `"SMH1"` |
| `width` | u16 | canvas width in pixels |
| `height` | u16 | canvas height in pixels |
| `dirty_x` | u16 | changed-region bounding box (canvas pixels) |
| `dirty_y` | u16 | " |
| `dirty_w` | u16 | `0` with `dirty_h == 0` means nothing changed |
| `dirty_h` | u16 | " |
| pixels | `width * height` bytes | 8-bit grayscale, row-major, top-left origin |

The first frame of a session has no prior frame to diff against and is
reported fully dirty (`dirty_x = dirty_y = 0`, `dirty_w = width`,
`dirty_h = height`).

## Design notes

- Grayscale uses Rec. 709 luma (`0.2126R + 0.7152G + 0.0722B`) to match the
  formula already documented for the eventual on-device pixel pipeline in
  `docs/host-tools.md` §4, so a `--png-dir` preview looks like what the
  panel will show, modulo dithering.
- Scaling is done ourselves via `vImageScale_Planar8` at the source's full
  native pixel resolution (via `CGDisplayCopyDisplayMode` for a display, so
  HiDPI is accounted for), rather than relying on
  `SCStreamConfiguration.scalesToFit` — this guarantees deterministic,
  exactly-sized, aspect-preserving letterboxed output regardless of how a
  given macOS release implements that flag's crop-vs-scale behavior.
- No third-party dependencies, by design — only system frameworks
  (`ScreenCaptureKit`, `Accelerate`, `CoreGraphics`, `CoreMedia`,
  `CoreVideo`, `ImageIO`, `UniformTypeIdentifiers`, `Foundation`).
