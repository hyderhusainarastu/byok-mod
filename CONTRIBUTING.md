# Contributing

Thanks for your interest in byok-mod. A few notes before you send a pull
request or open an issue.

## Before you touch real hardware

If your change involves anything that talks to a physical device — a new
`esptool` invocation, a firmware change, a new serial or USB capture —
read [SAFETY.md](SAFETY.md) first. The short version: never write to
efuses or Secure Boot, never touch the second microcontroller, and never
overwrite NVS, a partition, or the bootloader without a verified backup
and a tested recovery path already in hand. Hold hardware claims to the
CONFIRMED / STRONGLY INDICATED / POSSIBLE / UNKNOWN standard in SAFETY.md
§2 — an honest UNKNOWN is worth more than a guess.

## What to send

- **Firmware / host tool changes** — please include what you tested and
  how (host-side test suite, a device install, or both — see
  [`docs/testing.md`](docs/testing.md)). If you tested on real hardware,
  say what firmware version and what you observed.
- **Documentation changes** — corrections and clarifications are welcome.
  If you're adding a new hardware claim, give it an evidence label and
  say how you established it, the same way the existing docs do.
- **New captures or diagrams** — before adding a serial/USB capture or a
  photo-derived finding, make sure it's free of anything device- or
  person-identifying (MAC addresses, serial numbers, Wi-Fi credentials,
  GPS metadata). This project ships no photos of the physical device;
  diagrams under [`docs/diagrams/`](docs/diagrams/) are the preferred way
  to illustrate a hardware finding.

## Scope

This project's firmware targets the ESP32-S3 side of the board only, and
is built to coexist with the vendor's own stock firmware in a separate OTA
slot rather than replace it outright — see the architecture rationale in
[README.md](README.md) and [`docs/architecture.md`](docs/architecture.md)
before proposing a change that touches the second microcontroller or the
vendor's own boot slot; that's out of scope by design, not by oversight.

## Licensing

By contributing, you agree that your contribution is licensed under this
project's existing licenses: [MIT](LICENSE) for code, [CC BY
4.0](LICENSE-DOCS) for documentation. Don't submit a change that includes
vendor firmware, vendor assets, or anything extracted from a firmware
image beyond the kind of short, attributed, regeneratable excerpt
described in [LEGAL.md](LEGAL.md) §4.

## Reporting an issue

Bug reports and hardware-observation reports are both welcome via GitHub
issues on this repository. For a hardware bug, include the firmware
version (`byok info`) and, if you can, the exact steps to reproduce it.
