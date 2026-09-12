# ESP32-S3 16 MiB flash partition map

This diagram (`flash-partition-map.svg`) shows the S3's flash layout exactly as read off the
device's own partition table at `0x8000` — not the ESP-IDF default layout and not a template
`partitions.csv`. The table is self-verifying (its stored MD5 matches a recomputation over its own
seven entries), and the app-slot contents were independently confirmed by a full two-pass 16 MiB
dump (both passes byte-identical). The map disarms three offset traps that a stock-layout
assumption would walk into: `otadata` sits at `0x910000`, not the default `0xD000`; the region at
the default `otadata` address (`0xD000`–`0xF000`) is really just an erased gap; and `phy_init`
being all-`0xFF` is the normal state here because radio calibration lives in `nvs` instead. It also
marks the one partition that must never be touched (`nvs`, the sole copy of device identity) and
states plainly that this device has no working rollback path.

## Facts and confidence

| Fact | Confidence | Source |
|---|---|---|
| Chip ESP32-S3 rev v0.2, 16 MiB flash, JEDEC `46 40 18`, no flash encryption, no secure boot | CONFIRMED | `docs/recovery.md`, `docs/architecture.md` |
| Partition table read at `0x8000`; stored MD5 matches recomputation over its 7 entries (self-verifying) | CONFIRMED | `docs/recovery.md` |
| `nvs` at `0x009000`, 16 KiB — only copy of Wi-Fi credentials, Bluetooth pairing, device token, user settings, and radio calibration; in no archive | CONFIRMED | `docs/recovery.md` |
| `0x00D000`–`0x00F000` is an unallocated, all-`0xFF` gap — this is *not* `otadata` (the ESP-IDF default puts `otadata` here) | CONFIRMED | `docs/recovery.md`, `docs/architecture.md` |
| `phy_init` at `0x00F000`, 4 KiB, all `0xFF` — erased and never written; correct stock state, calibration actually lives in `nvs` | CONFIRMED | `docs/recovery.md` |
| `factory` at `0x010000`, 3 MiB, **BOOTED** — stock app, 2,114,864 B payload, project `BYOK`, `f764bf4-dirty`, IDF v5.5.3-dirty | CONFIRMED (verified 16 MiB dump, hash-checked) | `docs/architecture.md` |
| `ota_0` at `0x310000` and `ota_1` at `0x610000`, 3 MiB each, both entirely `0xFF` / never programmed | CONFIRMED (verified 16 MiB dump) | `docs/architecture.md` |
| `otadata` at `0x910000`, 8 KiB — both `ota_select` entries all-`0xFF`, no valid CRC ⇒ bootloader falls through to `factory`; no OTA has ever run on this unit | CONFIRMED | `docs/recovery.md`, `docs/architecture.md` |
| `assets` at `0x912000`, 256 KiB, FAT subtype `0x81`, wear-levelled — seven Lua UI files, ~159,744 B free | CONFIRMED | `docs/recovery.md` |
| Unallocated tail `0x952000`–`0x1000000`, 6.68 MiB, verified entirely `0xFF` | CONFIRMED (full dump, hash-checked) | `docs/architecture.md` |
| `esp_ota_get_next_update_partition(NULL)` returns the slot after the running one — the running image is never overwritten | CONFIRMED | `docs/flash-layout-and-updater.md` |
| `erase_region 0x910000 0x2000` blanks `otadata` and returns the device to booting `factory` — the one-command return to stock | CONFIRMED | `docs/recovery.md`, `docs/architecture.md` |
| A modified image must be a valid ESP32-S3 IDF app image (magic `0xE9`, valid header, valid appended SHA-256) and fit in `0x300000` bytes; one built firmware image measured 470,752 B (~15% of a slot) | CONFIRMED | `docs/flash-layout-and-updater.md`, `CHANGELOG.md` |
| No automatic rollback: `esp_ota_mark_app_valid_cancel_rollback` and `esp_ota_check_rollback_is_possible` are absent from the stock image; `ota_state` `UNDEFINED` and `VALID` are treated identically | CONFIRMED | `docs/flash-layout-and-updater.md`, `docs/bootloader-analysis.md` |

All facts on this diagram carry CONFIRMED grade in the source documentation — none required a
below-CONFIRMED footnote.
