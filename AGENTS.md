# AGENTS.md

Instructions for AI coding agents working with byok-mod (ESP32-S3 firmware
notes and macOS host tool for the BYOK v2.1 writing device). No PROMPT.md: this
is a CLI and documentation repo, not a skill.

## Using this in your project

- Never flash, erase or write to the device without a verified full flash
  backup (two-pass dump, compared byte for byte) and a tested recovery path.
  See SAFETY.md and docs/recovery.md.
- Never write efuses. Never touch the PICO (Bluetooth) chip.
- Do not run `idf.py flash` or `idf.py monitor` against a real device. The one
  supported install path is the stock SD-card updater (docs/firmware.md).
- `scripts/s3-revert-to-factory.sh` is destructive and needs an explicit
  `--yes`. Only run it when the user asks.
- Do not add vendor firmware, vendor assets or update archives to the repo.
  Ask the user to supply their own stock archive.
- Host tool, from `host/macos`:
  `python3 -m venv .venv && . .venv/bin/activate && pip install -e '.[dev]'`
- Commands: `byok info`, `byok status`, `byok text "..."`, `byok image FILE`,
  `byok dashboard --interval 1`, `byok notify "msg"`. Full reference:
  docs/host-tools.md. Commands that talk to hardware need the device attached;
  confirm with the user before running them.
- Firmware build (compiles only, flashes nothing): `source scripts/idf-env.sh`,
  then in `firmware/s3`: `idf.py set-target esp32s3 && idf.py build`.

## Working on this repo

- Test command, no device needed: `BYOK_FORCE_MOCK=1 pytest tests/host tests/static`
  (the mock transport never opens a serial port). C unit tests: `make` in
  `tests/proto` and `tests/tar`.
- Docs are CC BY 4.0 (docs/LICENSE-DOCS), code is MIT (LICENSE).
- Hardware claims in docs carry an evidence label. Keep that standard; do not
  upgrade a claim without a test result.
- Do not commit captures, serial numbers, SSIDs, MAC addresses, NVS dumps,
  backups or `.env` files. See .gitignore.
- Do not write outside the repo.
