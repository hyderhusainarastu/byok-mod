# Legal

This document explains what this project is, legally, and what it is not.
It is not a substitute for your own legal advice, and nothing here should
be read as a promise about how any particular court or jurisdiction would
treat this project.

## 1. No affiliation

This project is not affiliated with, endorsed by, sponsored by, or
otherwise connected to the company that makes the BYOK device, or to any
of its partners, distributors, or licensors. Any references to "BYOK" in
this repository are references to the third-party hardware this project
targets, made solely to describe that compatibility.

## 2. Trademarks

"BYOK" and any related names, logos, and marks are the property of their
respective owners. This project uses those terms only in a **nominative**
sense — to identify the specific device this project's software and
documentation are written for — and does not claim any rights in those
marks. This project has its own name, **byok-mod**, which is what it is
distributed under; no endorsement or sponsorship by the mark owner is
implied by either name.

## 3. No vendor firmware, assets, or data distributed here

This repository does not contain, and has never contained, any part of the
vendor's firmware, the vendor's UI assets, or any data extracted from a
vendor firmware image. Specifically:

- No `BYOK.bin`, `BYOK-Pico.bin`, `assets.tar`, or `BYOK.tar` (stock or
  otherwise) is committed to this repository.
- No firmware, filesystem, or NVS dump taken from a physical device is
  committed to this repository.
- Where our packaging script (`scripts/make-update-tar.sh`) needs a stock
  `assets.tar` to build a complete update archive, **you supply your own**
  — extracted from your own device or obtained through the maker's own
  channels — and it is never fetched, cached, or redistributed by this
  project.
- The one shareware file this project's optional Doom extra touches
  (`DOOM1.WAD`) is fetched by a script at install time from third-party
  mirrors of the original 1993 shareware release (a Debian/Ubuntu package,
  archive.org, or a historical idgames mirror — see
  [docs/sample-projects/doom.md](docs/sample-projects/doom.md) §3 for the
  exact sources and the hash verification that gates acceptance), never
  shipped in this repository, and is not vendor firmware in the first
  place — see §6.

Everything under version control here is either original code and
documentation we wrote, or short, clearly attributed excerpts used under
§4 below.

## 4. How the information in these docs was obtained

The facts in this repository's documentation were obtained by observing a
physical device we own: photographing it, passively watching its USB and
serial behavior, and reading (never modifying) its own flash contents with
standard, publicly available tools (`esptool`, `objdump`, and similar).
Nothing here required defeating any protection — see §5.

Two distinct legal bases apply to the material in these docs:

- **Facts and interfaces are not copyrightable.** Under 17 U.S.C. §102(b)
  (and the equivalent idea/expression distinction in most jurisdictions),
  copyright does not extend to facts, procedures, processes, or systems of
  operation — a GPIO number, an I²C address, a partition offset, a
  register value, or a wire-protocol layout is a fact about how the
  hardware works, not a copyrightable expression of it.
- **Interoperability reverse engineering is a recognized exception where
  original expression was necessarily examined.** Where the documentation
  quotes or paraphrases small excerpts of disassembled vendor code to
  explain how something is decoded, that examination is done for the
  purpose of achieving interoperability with the device — the basis
  recognized in 17 U.S.C. §1201(f) (circumvention exception for
  interoperability) in the United States, and in Articles 5(3) and 6 of EU
  Directive 2009/24/EC on the legal protection of computer programs, both
  of which permit the kind of observation, testing, and limited excerpting
  this project relies on. This is also the reasoning underlying *Sega
  Enterprises Ltd. v. Accolade, Inc.* and *Sony Computer Entertainment,
  Inc. v. Connectix Corp.* — reverse engineering to achieve interoperability,
  without copying the examined work's expression into the new work, is
  fair use / permitted use in the jurisdictions those cases address.

In practice, this means the documentation states facts (addresses,
offsets, timings, register values) freely, and where a disassembly excerpt
is shown at all, it is short (at most a handful of lines), annotated, and
accompanied by the exact command needed to regenerate it yourself from
your own copy of the vendor firmware — the point is to let you verify the
claim, not to hand you the vendor's code.

## 5. No technical protection measure was circumvented

On the single unit this project examined:

- **Secure Boot v2 was not enabled.**
- **Flash encryption was off.**
- **The stock SD-card updater performs an integrity check only** (a
  checksum/CRC over the update archive), not a cryptographic signature
  check — there is no signature to bypass.
- **No eFuse was read for the purpose of defeating a protection, and no
  eFuse was ever burned** by this project.

These are observations about the configuration of the specific device
examined, not a claim about every unit the vendor has ever shipped or a
promise about firmware versions this project has not examined. Where a
protection *is* documented as present (see the hardware safety notes in
[SAFETY.md](SAFETY.md)), this project does not attempt to defeat or bypass
it.

## 6. Third-party components

- **ESP-IDF** (Espressif's IoT Development Framework), used to build
  `firmware/s3/`, is licensed under the **Apache License 2.0**. Its
  license terms apply to ESP-IDF itself, independent of this project's own
  MIT license for the firmware source we wrote against it.
- **chocolate-doom**, used only by the optional `extras/doom/` sample
  project, is licensed under the **GNU General Public License**. It is a
  separate dependency, installed by `extras/doom/install.sh` via Homebrew
  — it is not vendored, modified, or redistributed by this repository.
- **esptool**, licensed **GPL-2.0-or-later**. One short excerpt of its
  USB-Serial-JTAG reset sequence is quoted, with attribution and an
  upstream link, in
  [docs/usb-and-boot-modes.md](docs/usb-and-boot-modes.md) for
  interoperability reference; esptool itself is not vendored or shipped by
  this repository.
- The shareware **`DOOM1.WAD`** that same script fetches is copyrighted
  game data, not project source. It is freely redistributable as
  shareware under id Software's original 1993 shareware license, and this
  project does not redistribute it either way — it is fetched at install
  time from third-party mirrors of that same shareware release (not from
  id Software directly; see
  [docs/sample-projects/doom.md](docs/sample-projects/doom.md) §3),
  verified against a known-good hash, and kept only in a gitignored local
  directory.

## 7. Warranty and risk

This project — its code and its documentation — is provided **"as is,"
without warranty of any kind**, express or implied, to the fullest extent
permitted by law, as stated in [LICENSE](LICENSE) (code, MIT) and
[LICENSE-DOCS](LICENSE-DOCS) (documentation, CC BY 4.0).

Installing this firmware modifies the application partition of a physical
device. We describe, and ourselves used, a verified path back to the
vendor's own firmware, but we cannot guarantee that your specific unit,
cable, SD card, or firmware version will behave identically to the one we
examined. You are solely responsible for any consequence of following
this documentation or running this software against your own hardware,
including but not limited to data loss, loss of function, or damage to the
device.

## 8. If you are the maker

If you are the manufacturer of the BYOK device, or represent them, and
have a concern about this project — a trademark concern, a request to
adjust how the device is referred to, or anything else — please open an
issue on this repository. That is the contact route for this project;
please do not attempt to route such a request through a personal email
address, as none is published here.
