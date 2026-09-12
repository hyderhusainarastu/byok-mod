# Sample project: Doom on the panel

**Status: working, optional, unshipped.** This is a separate, self-contained
extra under `extras/doom/` — not part of the core deliverable, not wired
into the main install flow, and safe to ignore entirely. It reuses the
Mode 2 mirror pipeline (`docs/sample-projects/mirror-and-virtual-display.md`)
to point a real Doom source port at the panel: a Doom engine renders in a
small windowed viewport on the Mac, and the mirror captures that window,
dithers it to 1-bit, and streams it to the device over the existing
USB-CDC link — exactly the way the mirror handles any other window.

## 1. The idea

Run a real Doom source port on the Mac and mirror its window onto the
panel instead of a terminal or a text editor. The mirror pipeline already
provides everything a "game" source needs: window capture, crop-to-cover
fitting into the panel's 3:1 aspect, dithering/thresholding to 1-bit,
dirty-rect diffing, and rate-limited sending over USB-CDC. No new input
handling is needed either — the Doom window has normal macOS keyboard
focus throughout, so you play it directly on the Mac keyboard while
watching the panel. The mirror is a one-way, Mac-to-panel display pipe;
it never reads a keystroke.

## 2. Chosen port: `chocolate-doom`

| Port | Distribution | Notes |
|---|---|---|
| **`chocolate-doom`** (chosen) | Homebrew formula, bottled | Deliberately the most vanilla-accurate port available — the exact original 320×200 renderer, no OpenGL, the smallest dependency set of the candidates considered, and the simplest CLI (`-window`, `-geometry WxH`, `-1`/`-2`/`-3` scale). Since the mirror downsamples whatever it captures to 240×80 1-bit anyway, a lower native resolution costs nothing, and the lack of extra HUD/menu chrome means less clutter competing for the panel's 19,200 pixels. |
| `crispy-doom` | Homebrew formula, bottled | A `chocolate-doom` fork sharing its exact CLI, with enhanced-resolution rendering — a fine drop-in alternative, not a reason to prefer it over the choice above. |
| `dsda-doom` | Homebrew formula, bottled | A speedrun-focused fork with more dependencies, a busier default HUD, and a different flag dialect (`-geom WxH[w\|f]` rather than `-geometry`). Kept as a documented fallback. |
| `gzdoom` | Homebrew cask (a full `.app`) | Not recommended: a GPU-rendered 3D engine is the wrong shape for a pipeline that immediately downsamples to a 240×80 1-bit panel, and it's a GUI app rather than a lean CLI binary. |

Install:

```sh
brew install chocolate-doom
```

## 3. The WAD: shareware `DOOM1.WAD`

**Licensing.** id Software's original 1993 shareware license, printed
inside the shareware package itself, explicitly permits free
redistribution of the shareware episode ("Knee-Deep in the Dead," episode
1 of 3) unmodified, for any non-commercial purpose, including electronic
distribution — this is why it has circulated freely for over 30 years, and
what every Doom source port's own install-time caveat ("this formula only
installs the engine, not the levels...") is pointing at. The
full/registered `DOOM.WAD` (all three episodes) is **not** covered by that
license and is never fetched by anything in this project; owning it
legitimately is out of scope here.

**Identity.** The file `extras/doom/install.sh` produces and verifies:

| | |
|---|---|
| File | `DOOM1.WAD` (shareware, v1.9) |
| Size | 4,196,020 bytes |
| SHA1 | `5b2e249b9c5133ec987b3ea77596381dc0d6bc1d` |
| MD5 | `f0cefca49926d00903cf57551d901abe` |

A note on a common mix-up: this file's identity is very widely published
across Doom-community sources as an "MD5" of `5b2e249b9c5133ec987b3ea77596381`
— but that 32-character string is actually the *first 32 characters of the
real SHA1* above, mislabeled, and repeated that way across many
independent sources for years. `install.sh` verifies against the real
SHA1 (confirmed against two independent public references that correctly
label it as such) rather than treating the truncated string as a genuine
MD5.

**Acquisition.** `extras/doom/install.sh` tries, in order: (a) the
Debian/Ubuntu `doom-wad-shareware` package (which repackages exactly the
shareware WAD the license permits redistributing), extracting `DOOM1.WAD`
from its `.deb`; (b) the same source tarball mirrored on archive.org, as a
fallback; (c) the historical idgames `doom19s.zip` archive — a 1995
DEICE-compressed self-extracting DOS package — but **only** if a DEICE
decompressor is already present on your system; the script never installs
one itself. Whichever source succeeds, the result is size- and
hash-verified against the identity table above before being accepted; the
script refuses and exits non-zero on any mismatch, and records the
verified sha256 in `extras/doom/WAD.sha256` (tracked in git — the hash
record, not the WAD itself, which is gitignored as copyrighted game data
even though the shareware episode is freely redistributable).

## 4. Install and run

```sh
extras/doom/install.sh
```

Idempotent — safe to re-run any time (after a Homebrew upgrade, or if the
WAD ever goes missing). It installs `chocolate-doom` (or confirms it's
already there) and fetches + verifies `DOOM1.WAD` into `extras/doom/wad/`.

```sh
extras/doom/byok-doom.sh [OPTIONS] [-- MIRROR_ARGS...]
```

This checks the device is enumerated, launches `chocolate-doom` windowed
(640×400) in the background, waits for its window to appear (finding it
via `ScreenMirrorHelper list`), and starts `python -m byok.mirror` pointed
at it with `--fit band --band middle --full-every 1` plus the contrast
defaults below. Ctrl-C stops both the mirror and the game together.
`chocolate-doom`'s own output is captured to a log file (path printed at
startup) rather than left to interleave with the launcher's own log
lines, so a crash during startup or a window that never appears reports a
clear error with the actual reason instead of failing silently.

Flags:

- `--fps N` — requested mirror frame rate (default **4**, the per-byte I²C
  path's ceiling — see §6).
- `--bulk` — shorthand for `--fps 30`; first tries to flip the device into
  the bulk-write I²C path if that control is available on your firmware
  build, otherwise prints the manual step and continues anyway.
- `--invert auto|on|off` and `--dither NAME` — passed straight through to
  the mirror. Defaults are **`--invert off --dither none`** — see
  **Contrast**, below.
- `--mirror-args "STR"` / `-- MIRROR_ARGS...` — extra arguments appended to
  the mirror invocation (the `--` form supports spaces/shell
  metacharacters that `--mirror-args`'s plain whitespace-split can't).

## 5. Contrast

Doom is almost entirely dark by design. The mirror's own `--invert auto`
(mean per-frame luminance with hysteresis, described in
`docs/sample-projects/mirror-and-virtual-display.md`) sees that darkness
and auto-inverts it to a light background — exactly backwards for this
source — and ordered dithering on top of an inverted dark image washes out
what little contrast remains. First on-device testing produced exactly
this: an image reported as far too light and hard to read at all.

The launcher's defaults fix this directly:

- **`--invert off`** — shows what the game actually renders (dark
  background, bright highlights) instead of an auto-inverted negative.
- **`--dither none`** — paired with the mirror's default
  `--threshold-method otsu`, this takes the hard, per-frame Otsu-optimal
  black/white cut instead of an ordered 4×4 checkerboard of on/off pixels.
  Bayer dithering represents Doom's smooth sector-light shading more
  faithfully, but that same checkerboard reads as washed-out on a 240×80
  1-bit panel at normal viewing distance — a hard cut is the more legible,
  "punchier" choice when visibility, not shading fidelity, is what
  matters.
- `--bpp` is left unset: the panel is monochrome 1bpp hardware, so a 2bpp
  mode isn't an available experiment on this device at all.

If it still looks wrong, in order:

1. Too light or too dark: `extras/doom/byok-doom.sh -- --dither bayer` —
   restores ordered dithering, softer and more graduated.
2. Still off: `extras/doom/byok-doom.sh -- --threshold-method mean` — a
   cheaper, differently-shaped split point (only affects the `--dither
   none` path).
3. A clean negative (dark should be light and vice versa): the fix is
   polarity, not contrast — `extras/doom/byok-doom.sh -- --invert on`.

## 6. Frame rate: the per-byte path and the bulk-write path

The panel's own I²C timing sets the ceiling, not host CPU:

- **Today, `--fps 4` (default):** each full refresh costs the per-byte
  path's measured ~220 ms, so the practical cadence is close to one panel
  frame every ~250 ms. Turning and strafing show up as discrete jumps
  rather than smooth motion; anything needing a fast reaction (dodging a
  fireball, an imp closing distance) is close to unplayable in real time —
  it plays more like a slow, turn-by-turn puzzle version of Doom watched
  through a strobe. It is nonetheless a real, working, end-to-end proof of
  the whole chain: Doom → capture → dither → I²C → panel.
- **With `--bulk` (`--fps 30`), once the bulk-write I²C path is confirmed
  correct on real hardware:** each refresh would cost roughly ~29 ms,
  putting 15–30 fps within reach and making the game genuinely
  playable — real-time turning, dodgeable projectiles, legible combat.

The bulk path exists in the firmware but ships **disabled by default**
because it has never been visually verified against a watched panel on
the current driver — see `docs/troubleshooting.md`'s blank-display entry
for exactly why an unverified bulk-write path is a real risk (the I²C ACK
check is disabled, so a silently-truncated burst wouldn't raise an error
at all). Requesting `--bulk` without that verification just over-requests
a panel that can't keep up; frames queue or drop, they don't arrive
faster.

## 7. Controls (played on the Mac keyboard)

| Key | Action |
|---|---|
| Arrow keys | Move forward/back, turn left/right |
| Ctrl | Fire |
| Space | Use / open door / flip switch |
| Shift (hold) | Run |
| 1–7 | Select weapon |
| Tab | Automap |
| Esc | Menu |

Keyboard focus must be on the Doom window, not the terminal, for input to
reach the game.

## 8. Uninstall

```sh
brew uninstall chocolate-doom
rm -rf extras/doom/wad
```

`extras/doom/wad/` is gitignored and safe to delete any time —
`install.sh` re-fetches and re-verifies it on the next run.

## 9. Licensing recap

Only the shareware episode is ever fetched by anything in this directory.
The full/registered `DOOM.WAD` is out of scope, is never downloaded by
this project's scripts, and would need to be legitimately purchased
separately by anyone who wants it.

See also: `docs/sample-projects/mirror-and-virtual-display.md` for the
underlying mirror pipeline, and `extras/doom/README.md` for the
command-reference version of this document.
