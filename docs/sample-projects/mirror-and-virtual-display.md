# Sample project: Mac screen mirror and virtual display

**Status: working, parked.** This mirrors a region of the Mac's screen — a
window, a display, or a purpose-made virtual display — onto the panel. It
was built and demonstrated end to end on real hardware, then deliberately
set aside: the shipped product is the USB dashboard and the standalone
clock, not a live screen mirror. The code, tests, and this documentation
stay in the tree as a working reference and a starting point for anyone
who wants to pick it back up. Nothing here is required to build or run the
core project.

Everything in this document is host-side. None of it changes firmware, and
none of it is part of `SAFETY.md`'s device-contact concerns beyond the
normal serial link the rest of the project already uses.

## 1. Why it's parked, not deleted

The mirror pipeline proved that the hardware and the wire protocol can
carry a live, continuously-updating image, not just a static dashboard —
that was worth building and worth keeping. But the panel is a slow, small,
monochrome display, and turning it into a good secondary-screen experience
would need real firmware speed work (see §5 on the frame-rate ceiling)
that this project chose not to pursue for the primary deliverable. The
decision: keep the mirror pipeline, its tests, and its documentation in
the repository exactly as they stand, as a working reference — no further
feature work, no further on-device polish is planned for it here. If you
want to build on it, this document plus the source under `host/macos/` is
the complete, accurate starting point.

The `byok mirror` CLI subcommand is a stub that prints a pointer to this
document; the actual, working entry point is the standalone module,
invoked directly:

```sh
cd host/macos
source .venv/bin/activate
python -m byok.mirror --window <ID>   # or --display <ID>
```

## 2. Three ways to mirror

The panel (240×80, monochrome, 3:1 aspect) is far too small and far too
slow to mirror a whole Retina display 1:1. Three narrower approaches cover
what's actually useful:

| Mode | What it shows | Capture source |
|---|---|---|
| **Window mirror** | one chosen window, scaled down | a window, captured directly |
| **Region mirror** | a fixed rectangle of a real display | a display, cropped |
| **Virtual-display mirror** | a *separate* small desktop the Mac genuinely owns, at (a multiple of) the panel's own resolution | a purpose-made virtual display, captured |

The first two need nothing beyond the Mac's own built-in screen-capture
API. The third is the highest-fidelity option — nothing is scaled, so text
is crisp — but it depends on a private, undocumented macOS API and is the
most fragile of the three. Window and region mirroring are the practical
starting point; the virtual display is worth reaching for only if a true
1:1 layout turns out to matter.

## 3. Capture: `ScreenMirrorHelper`

Capture uses **ScreenCaptureKit**, Apple's supported screen/window capture
framework (macOS 12.3+), not the older, deprecated `CGDisplayStream` or
`CGWindowListCreateImage` APIs — ScreenCaptureKit is also the only one of
the three that hands back per-frame dirty rectangles, which the pipeline
needs for efficient partial updates.

`host/macos/ScreenMirrorHelper/` is a small Swift Package executable
(`swift build -c release`, no third-party dependencies — only system
frameworks: ScreenCaptureKit, Accelerate/vImage, CoreGraphics, CoreMedia,
CoreVideo, ImageIO) that does capture-and-convert only. It has no idea the
BYOK device, USB, or the wire protocol exist — that boundary keeps the
Swift surface small and easy to reason about, and keeps every protocol
detail in one place, the Python side.

```
ScreenMirrorHelper list
ScreenMirrorHelper capture (--display <id> | --window <id>)
                   [--width W] [--height H] [--fps N]
                   [--threshold T] [--cursor | --no-cursor]
                   [--png-dir DIR] [--debug]
```

- `list` enumerates displays and windows and prints one line each
  (`DISPLAY id=… width=… height=…`, `WINDOW id=… title="…" app="…" pid=…
  width=… height=… onScreen=…`).
- `capture` picks a target by id, streams frames via `SCStream`, converts
  each to 8-bit grayscale (Rec. 709 luma), scale-to-fits it into
  `--width`×`--height` with letterbox padding on the short axis, diffs it
  against the previous frame at `--threshold` (0–255 per pixel) for a
  changed-region bounding box, and writes either a binary `SMH1` frame
  record to stdout or, with `--png-dir`, numbered debug PNGs.
- Defaults are `--width 240 --height 80 --fps 2 --threshold 8 --cursor` —
  240×80 is the panel's own confirmed resolution, so the default
  invocation already produces panel-sized frames.

**Wire format.** Every non-PNG frame is one `SMH1` record on stdout, all
multi-byte integers big-endian:

| Field | Type | Meaning |
|---|---|---|
| `magic` | 4 bytes | ASCII `"SMH1"` |
| `width` | u16 | canvas width in pixels |
| `height` | u16 | canvas height in pixels |
| `dirty_x`, `dirty_y`, `dirty_w`, `dirty_h` | u16 each | changed-region bounding box; `dirty_w == dirty_h == 0` means nothing changed since the last frame |
| pixels | `width * height` bytes | 8-bit grayscale, row-major |

16-byte header, then exactly `width * height` payload bytes — a reader
just does `header = read(16); frame = read(w*h)`. The first frame of a
capture is always reported fully dirty. Letterbox padding is `0x00`
(black); the helper doesn't know or care that the panel's own blank value
is inverted (`0xFF`) — that mapping is entirely the Python side's job.

**Permissions.** Screen Recording (TCC) is checked with the
non-prompting `CGPreflightScreenCaptureAccess()`. If it isn't granted,
both `list` and `capture` print a one-paragraph message pointing at
**System Settings → Privacy & Security → Screen Recording** and exit
**3**, before making any call that could trigger a system prompt. The
helper never calls anything that requests access on your behalf — you
grant it once, by hand, the normal way.

**Two crashes fixed during bring-up, worth knowing about if you extend
this code:**

- A bare, top-level-`async` `main.swift` that calls `dispatchMain()` after
  crossing an `await` traps with SIGTRAP on this toolchain — the
  top-level-async runtime and `dispatchMain()` fight over how the process
  stays alive. Fix: block on a `DispatchSemaphore` instead; frame delivery
  runs on its own queue, so blocking the entry point can't deadlock it.
- A bare CLI binary (no `.app` bundle, no `Info.plist`) that calls
  ScreenCaptureKit's window-based capture path can hit a `CGS_REQUIRE_INIT`
  assertion and abort — some CoreGraphics calls need an active window-server
  connection that a headless binary never opens. Fix: `import AppKit` and
  touch `NSApplication.shared` once at the very top of the entry point,
  before dispatching to any subcommand.

## 4. The pixel pipeline

```
capture frame (grayscale, letterboxed to the target size, dirty rect attached)
  → crop / re-fit to the panel's exact aspect ratio ("fit"/"band" modes, below)
  → optional invert (auto / on / off, by mean luminance with hysteresis)
  → threshold or dither → 1 bpp
  → pack: MSB-leftmost, rows padded to bytes
  → dirty-rect union with a post-quantize re-diff → PARTIAL_REFRESH / DRAW_BITMAP, or a full frame
```

`host/macos/byok/mirror.py` (`python -m byok.mirror`) is the consumer:
it spawns `ScreenMirrorHelper capture` sized to the *connected device's*
own reported geometry (never hard-coded), reads `SMH1` frames off its
stdout, and drives the rest of the pipeline.

```
python -m byok.mirror --list
python -m byok.mirror (--display ID | --window ID)
                       [--fps N] [--fit contain|fill|stretch|band] [--band top|middle|bottom]
                       [--scale N] [--invert auto|on|off] [--threshold-method otsu|mean|fixed]
                       [--dither bayer|floyd|none] [--threshold N]
                       [--region x,y,w,h] [--full-every N] [--preview PATH]
                       [--bpp 1|2] [--port DEVICE] [--dry-run] [-v | -vv]
```

`--fit`/`--dither` default differently for `--window` (`fill`/`none`)
than `--display` (`contain`/`bayer`).

### Fit modes: why `fill`/`band` exist

The first working run against a real window (586×488, roughly square) hit
an obvious problem: requesting a capture at exactly the panel's own 3:1
aspect ratio (`--fit contain`) let the helper's own aspect-preserving
scale-to-fit letterbox most of the panel in black, leaving only a small
box of real content in the middle. `--fit fill`/`--fit band` instead
request a **square** capture (side length = the panel's longer dimension)
— a far better generic fit for arbitrary window/display aspect ratios —
then crop the panel-shaped strip that best "covers" that square (a
CSS `object-fit: cover`-style crop, selectable via `--band
top|middle|bottom`) and scale that to the panel size. For the 586×488
test case this took the real content from a ~96×80 box (60% wasted on
letterbox) to filling the panel entirely. `--fit stretch` uses the same
square capture but resizes without cropping, distorting whatever aspect
mismatch remains. `--scale N` (1–6) multiplies the requested capture size
before the final downscale, trading a little extra capture cost for real
anti-aliased downsampling of small text.

### Dithering and threshold

Two dithers plus a plain threshold are available, and the right choice
depends heavily on the source:

- **Ordered/Bayer** — stable and deterministic (identical input always
  gives identical output), which is exactly what makes dirty-rect diffing
  work well: a static region stays byte-for-byte identical between
  frames. Best for photographic/continuous-tone sources.
- **Floyd–Steinberg** — best-looking on a single still image, but its
  error diffusion propagates: one changed pixel can alter every pixel
  after it in scan order, which explodes the "changed" region on motion
  and defeats the dirty-rect optimization. Not used by the mirror's
  defaults.
- **Threshold (no dither)** — for text specifically, this usually beats
  either dither: dithering an anti-aliased 9-pixel glyph produces noise,
  not extra detail. `--threshold-method otsu` (the default alongside
  `--dither none`) picks the histogram split point that maximizes
  between-class variance rather than assuming contrast straddles a fixed
  midpoint — it correctly handles a dark-background/light-text window
  instead of guessing wrong on it. `mean` is a cheaper fallback; `fixed`
  is the old always-127.5 behavior.

**Invert.** `--invert auto` (the default) computes each frame's mean
luminance and inverts a dark source so its background maps to the panel's
light/"off" level rather than painting a negative image, with hysteresis
(two thresholds, not one) so a frame that hovers near the boundary doesn't
flip every cycle. The first live run against a genuinely dark source
(id Software's *Doom*, in the sample project of that name) showed exactly
why this matters — see `docs/sample-projects/doom.md`'s **Contrast**
section for that specific case, where auto-invert turns out to be the
*wrong* default and is turned off deliberately.

### Dirty rectangles, twice over

Two independent dirty-rect sources feed the send decision, and both
matter: the helper's own pre-dither diff (cheap, from raw grayscale) is
read off the wire but is **not** trusted for the actual send decision,
because dithering and quantization can both hide a real change (two
grays that dither to the same pattern) and invent an apparent one
(dither noise on an otherwise-unchanged gradient). The authoritative diff
runs after quantization, on the packed 1-bpp buffer, comparing against
the previously *quantized* frame. Send strategy, matching the dashboard's
own wire behavior: the first frame is always sent full; every
`--full-every` frames after that (default 60) is also sent full; every
other frame that has changed goes out as one `DRAW_BITMAP` +
`PARTIAL_REFRESH` per changed page-band, or a `FRAME_*` partial-stream
sequence for a rect too large for one `DRAW_BITMAP`. A transport error
mid-cycle reconnects with backoff and forces the retry to be full.

## 5. Quality limits — the real ceiling is the panel, not the Mac

Host-side per-frame processing (crop/fit, luminance, quantize, pack) has
been measured at roughly 5–7 ms on ordinary hardware for a 240×80/240×240
frame — negligible next to either display-refresh figure below. **The
frame-rate ceiling is entirely the panel's own I²C timing:**

- The per-byte I²C path this project ships with today measures **≈220 ms**
  for a full 240×80 refresh (2,412 one-byte I²C transactions) — a
  practical ceiling around **4–4.5 fps**, and the mirror caps `--fps` at
  4.0 to match. At this rate, motion shows up as discrete jumps rather
  than smooth movement; text and slow-changing content (a terminal, a
  status line) are the realistic targets, not fast-moving video.
- A bulk-write I²C path exists in the driver (one multi-byte burst per
  refresh instead of 2,400+ separate transactions) and measured **≈29 ms**
  per full refresh in early testing — which would put 15–30 fps within
  reach — but that path has never been visually re-verified against a
  watched, real panel on the current driver, and ships disabled by
  default for that reason. See `docs/troubleshooting.md`'s blank-display
  entry for why an unverified bulk path is a real risk, not just a
  formality: the display driver's own ACK checking is disabled, so a
  silently-truncated burst would not raise an I²C error at all.

Until the bulk path is verified end to end on real hardware, treat 4 fps
as the honest ceiling for anything built on this pipeline.

## 6. Virtual display: `VirtualDisplayHelper`

For the highest-fidelity mode — a *real* display the Mac genuinely owns at
the panel's own geometry, so nothing is scaled and text renders crisp —
`host/macos/VirtualDisplayHelper/` is a standalone Swift CLI that creates
a macOS virtual display using a **private, undocumented CoreGraphics
API** (the same one used by third-party tools such as DeskPad and
BetterDisplay). It is a separate package from `ScreenMirrorHelper` and
does not touch it.

**This is explicitly the least safe piece of this sample project to build
on.** The classes involved are not in Apple's public documentation, carry
no compatibility promise, and can change or disappear in any macOS
release; an app using them cannot ship on the Mac App Store. That's an
acceptable trade for a personal tool on one Mac — it is not something to
depend on for anything you expect to keep working across macOS updates
without maintenance.

### How it works

```
CGVirtualDisplayDescriptor   name, physical size, max pixel bounds, IDs, a dispatch queue, a termination handler
CGVirtualDisplayMode         one (width, height, refreshRate) the display advertises
CGVirtualDisplaySettings     the list of modes + a hiDPI flag
CGVirtualDisplay             the live display; created from a descriptor, then applySettings:, exposes displayID
```

No private headers are vendored — only the selectors this project calls,
declared as `@objc` protocols and resolved at runtime via
`NSClassFromString` plus `unsafeBitCast` (a checked `as?` cast always
fails here, because the private class never declares conformance to the
protocol even though it responds to every selector `unsafeBitCast` lets
`objc_msgSend` dispatch to). `CGVirtualDisplayMode`/`CGVirtualDisplay`
have no plain `-init`, so the helper does the alloc/init sequence by hand:
`+alloc` taken unretained (so ARC doesn't own alloc's +1), then the
`initWith…` selector consumes that +1 as an "init"-family return.

No entitlements or code signing are needed for this helper specifically —
it's a plain, non-sandboxed SwiftPM binary, so it reaches the relevant
system service directly. (A sandboxed `.app` wrapping this code would need
a mach-lookup entitlement exception, but that's not this helper's
situation.) Creating the display needs no special permission; *capturing*
it afterward is ScreenCaptureKit's job and needs the same Screen Recording
permission as any other capture.

### Accepted resolutions

A display requested at exactly the panel's own 240×80 can come up with
**no selectable resolution** — the private API registers the mode without
error, but macOS filters out modes it considers too small before offering
them in System Settings, and there's no documented minimum to target
since the API itself is undocumented. The workaround: also register 2×,
3×, and 4× same-aspect modes, so at least one survives filtering and the
mirror can downsample by an integer factor:

| Requested | Presents (with `--hidpi`) | Capture → downsample |
|---|---|---|
| 240×80 | often filtered out | 1× — unreliable |
| 480×160 | 240×80 points @2× | capture 480×160, downsample 2× |
| 720×240 | 360×120 points @2× | capture 720×240, downsample 3× |
| 960×320 | 480×160 points @2× | capture 960×320, downsample 4× |

If `create` prints a `displayID` but no usable size shows up in System
Settings, bump the target to `480×160` (or `720×240`) with `--hidpi`.

### Usage

```sh
cd host/macos/VirtualDisplayHelper
swift build -c release

# create and hold the display open (leave this terminal running)
.build/release/VirtualDisplayHelper create --width 480 --height 160 --name "BYOK" --hidpi
# prints displayID=<n>; a display named "BYOK" should appear in
# System Settings > Displays — arrange it, drag a window onto it

# in a second terminal, using the id from above:
cd host/macos
.venv/bin/byok mirror --display <n> --fit fill --fps 2
```

Ctrl-C the mirror, then Ctrl-C the `create` process, in that order — the
virtual display disappears the instant `create` exits (any exit: Ctrl-C,
`kill`, a crash, logout), and the Displays layout returns to normal.
Nothing is persisted; there's nothing to clean up. If a `create` process
is ever lost track of: `pkill -INT -f VirtualDisplayHelper`.

Both halves of this — window mirroring and virtual-display mirroring —
have been demonstrated working on the physical panel: a real virtual
display was created, appeared in System Settings, was arranged like any
other display, and the mirror pipeline captured and drove the panel from
it end to end.

### Risks

- **Displays layout changes** while the virtual display exists and again
  when it's removed — expected, and it reverts on its own.
- **Mirroring toggles.** If macOS decides to mirror the new display onto
  the built-in screen (or vice versa), check the "Use as"/mirroring
  setting in System Settings > Displays.
- **Process death = display death.** Any exit of the `create` process
  removes the display immediately — don't run it as a throwaway.
- **Private API.** These classes are undocumented and could change or
  disappear in a future macOS release. The helper degrades safely: if a
  class is missing, `create` prints a clear error and exits non-zero
  rather than crashing.
- Don't leave `create` running unattended for long stretches — it holds
  the Displays layout in a re-arranged state for as long as it runs.

## 7. Files

```
host/macos/ScreenMirrorHelper/
  Sources/ScreenMirrorHelper/main.swift   # list / capture, SMH1 wire format, CGS/dispatch fixes

host/macos/VirtualDisplayHelper/
  Sources/VirtualDisplayHelper/
    main.swift        # create / list / probe, arg parsing, signal teardown
    PrivateAPI.swift   # @objc protocol declarations + NSClassFromString factory

host/macos/byok/mirror.py    # the pixel pipeline, dirty-rect logic, and device I/O
tests/host/test_mirror.py    # unit tests against a fake ScreenMirrorHelper subprocess
```

See also `docs/host-tools.md` for the rest of the macOS companion package,
and `docs/protocol.md` for the wire messages the mirror sends
(`FRAME_BEGIN`/`FRAME_DATA`/`FRAME_END`, `DRAW_BITMAP`, `PARTIAL_REFRESH`,
`SET_MODE(MIRROR)`) — nothing in this document required a protocol
change; those messages already covered everything the mirror needed.
