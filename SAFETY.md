# Safety

This project was developed against a single, real, irreplaceable physical
device. These are the rules that kept it recoverable through every step of
that process, and they are the rules to follow if you're working on your
own unit. Read this before doing anything on real hardware.

## 1. Hardware safety rules — hard constraints, never violate

- **Never** run an `esptool` erase or write operation against the device
  unless the verified-backup-and-tested-recovery-path rule below is
  already satisfied for the region you are touching, and you understand
  exactly what you're about to overwrite.
- **Never** touch efuses.
- **Never** make Secure Boot changes.
- **Never** blindly enter download mode without a specific, understood
  reason.
- **Never** short pads or pins to probe behavior.
- **Never** apply 5V to any signal rated 3.3V.
- **Never** overwrite NVS, a partition, or the bootloader until a
  **verified backup and a tested recovery path both exist** for that
  region. "Tested" means you have actually exercised the recovery path,
  not just read the procedure — this project only started writing to the
  OTA app partition once a full 16 MB flash dump had been taken twice and
  hash-compared, and once a real SD-card update install had been run
  successfully.
- Where the device has any protection enabled (Secure Boot, flash
  encryption, read protection, etc.), **document it** — never attempt to
  defeat or bypass it. On the unit this project examined, none of these
  were enabled; see [LEGAL.md](LEGAL.md) §5 for exactly what that claim
  covers.
- The second microcontroller on this board (the Bluetooth co-processor)
  has no known download-mode route and therefore no backup path. **It is
  never written, erased, or addressed by anything in this project** —
  that's not a design preference, it's a direct consequence of this rule.
- Any action that needs physical access to the board — soldering, probing
  a test point, connecting a programmer, holding buttons in a specific
  sequence — is a deliberate, hands-on action. Don't script or automate a
  physical step you haven't done by hand first.
- After any `esptool`/download-mode session, exit by unplugging USB and
  doing a full power cycle with the front power button. Don't rely on a
  reset button alone — on this device, resetting only the ESP32-S3 leaves
  the second microcontroller and the display controller un-reset, which
  produces display artifacts, wrong contrast, and unresponsive buttons
  until a full power cycle clears it.

## 2. Evidence standard

Every hardware claim in this project's documentation carries one of four
labels, and the evidence for it is stated alongside the claim:

- **CONFIRMED** — directly observed or verified (a legible silkscreen
  marking, a working command's actual output, a value read back off the
  device).
- **STRONGLY INDICATED** — strong circumstantial evidence, not directly
  verified (e.g. it matches a known reference design closely, or a
  disassembly implies a value that was never independently measured).
- **POSSIBLE** — plausible, but weakly supported.
- **UNKNOWN** — not legible, or not yet determined.

Never guess a chip marking, part number, or silkscreen text. If it isn't
clearly legible in a photo or a capture, it's UNKNOWN, and the docs say so
— an UNKNOWN is a valid, honest answer, not a gap to paper over.

## 3. Physical actions are one at a time

- Only one small, physical experiment is ever proposed and performed at a
  time — never a batch of irreversible or ambiguous actions run together.
- Before anything irreversible, uncertain, or device-state-changing:
  stop and think it through fully before proceeding. Don't act on an
  assumption about what a step will do to the device.

## 4. Never commit sensitive data

Never commit to version control:

- Firmware dumps, NVS dumps, or filesystem dumps taken from the device.
- Credentials of any kind.
- Wi-Fi SSIDs, passwords, or other network configuration extracted from
  the device.
- MAC addresses, device serial numbers, or other values that identify a
  specific physical unit.

Raw dumps belong (if you keep them at all) in a directory that's excluded
from version control entirely — see `.gitignore`. Serial and USB captures
in particular can carry a Wi-Fi SSID, a MAC address, or a device serial in
the clear: redact and grep them before staging, and never bulk-add a
captures directory to git without reviewing what's in it first.

## 5. Document as you go

Write findings down immediately, in the relevant doc, not from memory at
the end of a session. A claim that isn't written down with its evidence
label the moment it's established is a claim that will drift or get
misremembered later. This project's own research trail — every
experiment, its result, and what was concluded from it — is what let later
findings correct earlier ones (several are recorded in these docs) instead
of silently overwriting them.

## 6. Layout

```
byok-mod/
  docs/              Written findings, specs, and design docs
  firmware/          ESP32-S3 firmware source (common/, s3/)
  host/macos/        macOS host tool and helper packages
  host/tools/        Small standalone host-side scripts
  scripts/           Repo-wide automation (build, capture, recovery)
  tests/             Automated tests, plus a manual on-device checklist
  extras/doom/       Optional sample project, kept separate from the base build
  captures/          Two curated device captures (a clean boot log, a USB enumeration
                     trace) cited by the docs
```

See [README.md](README.md) for project purpose, architecture, and quick
start; see [LEGAL.md](LEGAL.md) for the legal basis this documentation
relies on.
