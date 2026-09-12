# byok.dashboard -- dependency notes

This subpackage is deliberately written to run with **zero required
third-party dependencies** beyond what `byok` already needs (Pillow, per
`host/macos/pyproject.toml`, maintained separately -- not edited here).
Everything below is optional; the code degrades gracefully without it.

## Hard requirement (inherited, not new)

- **Pillow** (`PIL`) -- image composition/drawing/quantization. Already a
  `byok` dependency (`Pillow>=10.4.0,<11` in `host/macos/pyproject.toml`).
  This package does not add a new Pillow constraint; it just uses what's
  already declared there.

## Optional

- **PyYAML** (`pyyaml`) -- `config.py` uses it when importable for full
  YAML support. Not installed? `config.py` falls back to a small,
  dependency-free parser covering the documented subset of YAML this
  schema actually needs (nested mappings, block/flow lists, scalars). See
  the module docstring in `config.py` for exactly what the fallback parser
  does and doesn't support. If you want full YAML (anchors, flow mappings,
  block scalars, multi-document files, ...) in your own config, install
  PyYAML into whichever interpreter runs this code:

      pip install pyyaml

- **`nowplaying-cli`** -- `widgets/now_playing.py` uses it *only if
  already on PATH* (macOS has no supported command-line "now playing"
  API). This package never installs it. Get it yourself, if you want it,
  from https://github.com/kirtan-shah/nowplaying-cli . Without it, the
  widget silently falls back to a "Not playing" placeholder.

- **A TTF/OTF font file** -- `fonts.py` will use one if you point
  `fonts.path` (top-level config key) or a widget's `options.font` at it.
  Without one, it tries a few fonts that ship with every macOS install
  (Helvetica, Menlo), then falls back to Pillow's own built-in scalable
  default font (`ImageFont.load_default(size=...)`, Pillow >= 10.1), then
  finally to Pillow's fixed-size built-in bitmap font if even that isn't
  available. No font file is ever required.

## Everything else

`widgets/calendar.py`, `widgets/reminders.py`, `widgets/mac_stats.py`,
`widgets/network.py`, `widgets/git.py` all shell out to command-line
tools/AppleScript that ship with macOS itself (`osascript`, `top`,
`vm_stat`, `sysctl`, `ifconfig`, `ipconfig`, `pmset`, `git`) -- nothing to
install, and every call is wrapped in try/except with a timeout so a
missing tool, a denied permission, or a slow response degrades to a
placeholder ("--", "No events", ...) instead of raising. `widgets/qr.py`
is a from-scratch pure-Python QR encoder with no dependency at all (see
its module docstring for scope/caveats).

## Running the test suite / preview without Pillow pre-installed

If `python3 -c "import PIL"` fails and `host/macos/.venv` doesn't already
exist with Pillow in it, create a dedicated venv:

    python3 -m venv host/macos/.venv-dash
    host/macos/.venv-dash/bin/pip install "Pillow>=10.4.0,<11"
    host/macos/.venv-dash/bin/python3 -m unittest tests.host.test_dashboard -v

**Full coverage needs both Pillow and PyYAML in the same interpreter.** Neither
`host/macos/.venv`/`.venv-dash` (Pillow, no PyYAML) nor the system `/usr/bin/python3`
(PyYAML, no Pillow — and `test_dashboard.py` imports Pillow at module scope, so it can't
even run there) had both as of this writing, which meant `test_pyyaml_and_fallback_parser_agree`
— the only test that checks the fallback parser against real PyYAML — was always skipped, on
every interpreter available. Since no interpreter had PyYAML, the fallback parser was also the
one *actually running* in every other test, untested against the real thing. Fix:

    host/macos/.venv-dash/bin/pip install pyyaml

Do this (or add `pyyaml` alongside `pytest` in `host/macos/pyproject.toml`'s
`[project.optional-dependencies] dev` — not done here, that file is
maintained separately) before trusting a green run as full coverage.
