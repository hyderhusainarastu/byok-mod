# Doom on the BYOK panel

**Purpose.** Run once, keep it on the Mac to reinstall any time. This is a
separate, optional extra — it is **not** part of the shipped BYOK deliverable,
lives entirely under this directory, and is excluded from the final package
and the main project README's flow. It reuses the existing Mode 2 mirror
pipeline (`host/macos/byok/mirror.py`) to point a real Doom source port at
the panel: `chocolate-doom` renders in a small windowed viewport on the Mac,
and the mirror captures that window, dithers it to 1-bit, and streams it to
the device over the existing USB-CDC link — the same way Mode 2 mirrors any
other window.

Full background, port choice, and flag rationale: `docs/sample-projects/doom.md`.

## Install

```sh
extras/doom/install.sh
```

Idempotent — safe to re-run any time (e.g. after a `brew` upgrade, or if the
WAD ever goes missing). It:

1. Installs `chocolate-doom` via Homebrew (bottled formula, no `sudo`), or
   confirms it's already installed and prints its version.
2. Fetches the shareware `DOOM1.WAD` into `extras/doom/wad/` (gitignored —
   copyrighted game data, even though the shareware episode is freely
   redistributable, is not project source) and verifies it against the
   known-good identity (size 4,196,020 bytes; SHA1
   `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d`) before accepting it. Refuses
   and exits non-zero if a downloaded file doesn't match. The verified
   sha256 is recorded in `extras/doom/WAD.sha256` (tracked in git — the hash
   record, not the WAD itself).

Only the shareware WAD is ever fetched — see **Licensing** below.

## Run

```sh
extras/doom/byok-doom.sh [OPTIONS] [-- MIRROR_ARGS...]
```

- Checks the device is enumerated (`/dev/cu.usbmodemBYOKMOD*`), refuses if
  not.
- Launches `chocolate-doom` windowed (640x400) in the background.
- Waits for its window (about 11s total: a 1s head start plus up to 10s of
  retries), finds its `ScreenMirrorHelper` window id, and starts
  `python -m byok.mirror` pointed at it with `--fit band --band middle
  --full-every 1` plus the contrast-tuning flags below.
- `chocolate-doom`'s own stdout/stderr are captured to a log file under
  `$TMPDIR` (path printed at startup, e.g. `[byok-doom] starting
  chocolate-doom (output logging to /tmp/byok-doom-chocolate-doom.<pid>.log)...`)
  rather than left to interleave silently with `byok-doom`'s own log lines.
  If `chocolate-doom` exits unexpectedly (immediately, or before its window
  appears) or the window-wait times out, the script prints a clear
  `ERROR:` message with the exit code and the last lines of that log,
  exits non-zero, and does not leave a `chocolate-doom` process running.
- `--fps N` sets the requested mirror frame rate (default **4**, the
  per-byte I2C path's ceiling). `--bulk` is shorthand for `--fps 30` and
  first tries `byok display --bulk on` to flip the device into the
  bulk-write I2C path; if that command isn't available on your install, it
  prints the manual step and continues anyway (see **Bulk-write
  prerequisite** below).
- `--invert auto|on|off` and `--dither NAME` are passed straight through to
  `python -m byok.mirror`. Defaults are **`--invert off --dither none`** —
  see **Contrast** below for why, and what to try if it still looks wrong.
- `--mirror-args "STR"` appends extra words (plain whitespace split, no
  quoting/escaping) to the mirror invocation. For anything with spaces or
  shell metacharacters, use `--` instead: everything after a literal `--`
  on the command line is appended to the mirror invocation verbatim, e.g.
  `extras/doom/byok-doom.sh -- --invert on --dither bayer`. Flags are
  applied left to right and argparse keeps the last value for a repeated
  flag, so anything given via `--mirror-args`/`--` overrides this
  launcher's own flags, including `--fit`/`--band`/`--full-every`.
- **Ctrl-C stops both** the mirror and `chocolate-doom` together.

## Contrast

**Owner report, 2026-09-04, on the real panel:** *"the contrast is horrible,
I can barely see anything, everything is too light."* Root cause: Doom is
almost entirely dark by design, and the mirror's own `--invert auto`
(`host/macos/byok/mirror.py`'s `decide_invert()`, driven by per-frame mean
luminance with hysteresis) saw that darkness and auto-inverted it to a light
background — exactly backwards for this source — and the previous default
`--dither bayer` on top of that inverted image washed out what little
contrast remained.

The launcher now defaults to:

- **`--invert off`** — shows what the game actually renders (dark
  background, bright highlights) instead of an auto-inverted negative.
- **`--dither none`** — paired with the mirror's own default
  `--threshold-method otsu` (not overridden here), this takes the
  `quantize_none()` + `otsu_threshold()` route in `mirror.py` (~L779-798):
  a hard, per-frame Otsu-optimal black/white cut with no dither noise,
  instead of `quantize_bayer()`'s ordered 4x4 checkerboard of on/off
  pixels. Bayer dithering represents Doom's smooth sector-light shading
  more faithfully, but that same checkerboard reads as "washed out" /
  lower-contrast on a 240x80 1-bit panel at normal viewing distance — the
  hard Otsu cut is the more legible ("punchy") choice when visibility, not
  shading fidelity, is the complaint.
- No `--bpp` change: the panel is **monochrome 1bpp hardware**
  (`docs/display.md`), so `--bpp 2` is not an available experiment on this
  device at all — this isn't a "leave it for later" choice, the hardware
  doesn't support it. `--bpp` is left unset so the mirror uses the
  device's own `native_bpp`.

**If it still looks wrong**, note there is no numeric quantization-threshold
flag to hand-tune — `mirror.py`'s own `--threshold` flag is the dirty-rect
diff's changed-pixel threshold, **not** the quantization cut point (see its
`--help`). The two things actually worth trying, in order:

1. **Still too light or too dark:**
   ```sh
   extras/doom/byok-doom.sh -- --dither bayer
   ```
   Restores ordered dithering — softer, more graduated, sometimes reads
   better than a single hard cut depending on the scene.
2. **Still off:**
   ```sh
   extras/doom/byok-doom.sh -- --threshold-method mean
   ```
   A cheaper, differently-shaped split point than the default Otsu cut.
   Only affects the `--dither none` path.

If the image looks like a clean negative of what it should be (dark should
be light and vice versa), the fix is polarity, not contrast:
```sh
extras/doom/byok-doom.sh -- --invert on
```

If it's noisy/grainy rather than too light/dark: `--dither bayer` trades
some of that noise for softer edges; `--dither none` (the default) is the
less-noisy, harder-edged end of that same trade.

**Keyboard focus must be on the Doom window**, not the terminal, for input
to reach the game — the mirror is a one-way (Mac → panel) display pipe; it
adds no keyboard handling of its own.

### Controls (played on the Mac keyboard)

| Key | Action |
|---|---|
| Arrow keys | Move forward/back, turn left/right |
| Ctrl | Fire |
| Space | Use / open door / flip switch |
| Shift (hold) | Run |
| 1–7 | Select weapon |
| Tab | Automap |
| Esc | Menu |

## Bulk-write prerequisite

`--fps` above the per-byte path's ~4.5 fps ceiling only reflects real,
achievable throughput once the device is actually running the bulk-write I2C
path (one multi-byte burst per refresh, ~29 ms, vs. ~220 ms per-byte) *and*
that path has been re-verified correct on real hardware — see `docs/sample-projects/doom.md`
§2(A) for exactly what "re-verified" means and why it isn't optional
(`BYOK_LCD_I2C_DISABLE_ACK_CHECK=1` means "0 I2C errors" alone doesn't prove
the image was drawn correctly). Requesting a higher `--fps` without the
device actually being in bulk mode just over-requests a panel that can't
keep up — frames queue or drop, it doesn't make the per-byte path faster.

## Expected experience

**At `--fps 4` (default, per-byte path, no prerequisites beyond a device):**
each panel update costs a real ~220 ms hardware refresh, so the practical
cadence is close to one panel frame every ~250 ms. Turning and strafing show
up as discrete jumps, not smooth motion; anything requiring a fast reaction
(dodging a fireball, an imp closing distance) is close to unplayable in real
time. It plays more like a slow, turn-by-turn puzzle version of Doom, watched
through a strobe — but it is a real, working, end-to-end proof of the whole
chain (Doom → capture → dither → I2C → panel).

**At `--fps 30` (`--bulk`, once the bulk path is confirmed working — see
above):** each refresh costs ~29 ms, so 15-30 fps is genuinely achievable on
the panel-timing side. At that rate Doom becomes actually playable — real-
time turning, dodgeable projectiles, legible combat.

## Uninstall

```sh
brew uninstall chocolate-doom
rm -rf extras/doom/wad
```

(`extras/doom/wad/` is gitignored and safe to delete any time — `install.sh`
will re-fetch and re-verify it on the next run.)

## Licensing

Only id Software's shareware episode (`DOOM1.WAD`, "Knee-Deep in the Dead")
is used here — its original 1993 license explicitly permits free, unmodified,
non-commercial redistribution, which is why it has circulated freely for over
30 years and is what every Doom source port's own install caveat ("this
formula only installs the engine, not the levels... free levels are
available online") is pointing at. The full/registered `DOOM.WAD` (all three
episodes) is **not** covered by that license and is never fetched by
anything in this directory; owning it legitimately (Steam/GOG/Bethesda.net
"Ultimate Doom") is out of scope here.
