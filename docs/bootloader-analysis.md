# Stock second-stage bootloader — OTA rollback analysis

**Question.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, paired with an
`esp_ota_mark_app_valid_cancel_rollback()` call in `app_main`, is ESP-IDF's standard way to get "a
bad update reverts itself", and it is the obvious thing to reach for in a project that installs its
own firmware alongside a vendor's. It assumes the bootloader honours `ota_state`. This project
**never replaces the bootloader** ([SAFETY.md](../SAFETY.md) §1; the bootloader at `0x0` heads the
forbidden-regions table in [design-rationale.md](design-rationale.md) D-011), so the only bootloader
that will ever start this project's image is the stock one already at `0x0`. Does *it* implement app
rollback?

**Answer: no. Categorically no.** Graded **CONFIRMED** — established by disassembly of the actual
carved bootloader, cross-checked against the IDF v5.5.3 source it was built from, and validated
with a positive control (a purpose-built `ROLLBACK_ENABLE=y` bootloader, which shows exactly the
code the stock one lacks). That answer is why the shipped configuration is what it is: §8 records
the two settings it produced.

Everything below is host-only static analysis. No device was contacted.

---

## 1. Subject and method

| Item | Value |
|---|---|
| Image | Stock second-stage bootloader region, carved from a full-flash backup of the examined unit (32,768 B) |
| Build stamp | `v5.5.3-dirty`, `Mar 20 2026 08:13:32` |
| Chip id | `9` (ESP32-S3) |
| Segments | `0x3fce2820` (0x11f8, data) · `0x403c8700` (0xcf4) · `0x403cb700` (0x332c, text) |
| Entry | `0x403c893c` |
| IDF source of record | `~/esp/esp-idf` @ `v5.5.3` — same version the image reports |
| Disassembler | `xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=<seg load addr>` |

**Why strings alone are not enough.** The stock bootloader was built at log level **WARN**: the
binary contains 47 `E (%lu) %s: …` and 4 `W (%lu) %s: …` format strings and **zero** `I (`/`D (`
ones. Every log line that is unique to the rollback path is `ESP_LOGD` (`bootloader_utility.c`
lines 400, 448) and is therefore compiled out either way. A "no rollback strings ⇒ no rollback"
argument would be **invalid here**, so this analysis does not use one. The one string-level
conclusion that *is* sound is for anti-rollback (below), because that path has an `ESP_LOGE`.

---

## 2. What IDF v5.5.3 actually does (read from source, not memory)

`components/bootloader_support/src/bootloader_utility.c`,
`bootloader_utility_get_selected_boot_partition()`:

* **Lines 396–405, `#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`** — before selecting anything,
  loop over both otadata copies; any copy in `ESP_OTA_IMG_PENDING_VERIFY` (1) is rewritten to
  `ESP_OTA_IMG_ABORTED` (4) via `write_otadata()`. *This is the actual rollback trigger.*
* **Lines 446–452, same `#ifdef`** — after `boot_index` is computed, if the selected copy is in
  `ESP_OTA_IMG_NEW` (0) it is rewritten to `ESP_OTA_IMG_PENDING_VERIFY` (1). *This is what arms
  the one-shot.*
* **Lines 454–458, `#ifdef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`** — eFuse secure-version update.

`components/bootloader_support/src/bootloader_common_loader.c:74`, compiled **unconditionally**:

```c
bool bootloader_common_ota_select_invalid(const esp_ota_select_entry_t *s)
{
    return s->ota_seq == UINT32_MAX || s->ota_state == ESP_OTA_IMG_INVALID || s->ota_state == ESP_OTA_IMG_ABORTED;
}
```

So **without** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the bootloader's *entire* interaction with
`ota_state` is this predicate: states `3` (INVALID) and `4` (ABORTED) make a copy unselectable;
states `0` (NEW), `1` (PENDING_VERIFY), `2` (VALID) and `0xFFFFFFFF` (UNDEFINED) are all equally
bootable, are never compared, and are **never rewritten**. This confirms, from source, what the
question above set out to check.

`esp_ota_select_entry_t` (`esp_flash_partitions.h:78`) — the 32-byte record, one per 4 KiB sector:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | `ota_seq` |
| `0x04` | 20 | `seq_label[20]` |
| `0x18` | 4 | `ota_state` |
| `0x1c` | 4 | `crc` (CRC32 of `ota_seq` **only**) |

---

## 3. Disassembly of the stock `bootloader_utility_get_selected_boot_partition`

> **Attribution.** `bootloader_utility_get_selected_boot_partition()` is Espressif's own function,
> compiled unmodified into the stock bootloader from `components/bootloader_support/src/bootloader_utility.c`
> in ESP-IDF v5.5.3 (Apache License 2.0). The annotated listing below is a disassembly of that
> stock binary, produced for interoperability analysis — it is not our code, and nothing here
> replaces or redistributes ESP-IDF itself. Upstream source:
> <https://github.com/espressif/esp-idf/blob/v5.5.3/components/bootloader_support/src/bootloader_utility.c>.

Located at **`0x403cd0dc`** — identified positively by its two `ESP_LOGE` literals
(`"ota data partition invalid, falling back to factory"` at `0x3fce2bc4`, referenced from
`0x403cd177`; `"…and no factory"` at `0x3fce2c08`, from `0x403cd18c`) and by its two distinctive
return constants `-1` (`FACTORY_INDEX`) and `-99` (`INVALID_INDEX`).

Annotated (comments added; instruction text verbatim from objdump):

```
403cd0dc: entry   a1, 96              ; 2 × 32-byte otadata on stack: a1+0, a1+32
403cd0df: l32i.n  a8, a2, 0           ; bs->ota_info.offset
403cd0e3: beqz    a8, 0x403cd195      ;   == 0 -> return FACTORY_INDEX
403cd0ea: call8   0x403ccf88          ; bootloader_common_read_otadata(&bs->ota_info, otadata)
403cd0ef: bnez    a10, 0x403cd19d     ;   != ESP_OK -> return INVALID_INDEX (-99)
403cd0f2: l32r    a6, 0x3fce2700      ; &ota_has_initial_contents
403cd0f8: s8i     a2, a6, 0           ; ota_has_initial_contents = false
403cd0fb: call8   0x403cc884          ; bootloader_common_ota_select_invalid(&otadata[0])   <-- FIRST
403cd0fe: bnez.n  a10, 0x403cd108     ;                                                          THING
403cd100: l32i    a8, a7, 152         ; bs->app_count
403cd103: beqz.n  a8, 0x403cd114
403cd105: j       0x403cd14c
403cd108: addi    a10, a1, 32
403cd10e: call8   0x403cc884          ; bootloader_common_ota_select_invalid(&otadata[1])
403cd111: beqz    a10, 0x403cd100
403cd114: l32i.n  a8, a7, 8           ; bs->factory.offset
403cd116: bnez    a8, 0x403cd195      ;   -> return FACTORY_INDEX      ("Defaulting to factory")
403cd119: …                           ; ota_seq==UINT32_MAX / crc checks -> ota_has_initial_contents=1
403cd14c: or      a10, a1, a1
403cd14f: call8   0x403cc814          ; bootloader_common_get_active_otadata(otadata)
403cd152: beqi    a10, -1, 0x403cd16a ;   -1 -> the two LOGE error paths
403cd155: slli    a10, a10, 5         ; active * sizeof(esp_ota_select_entry_t) == active * 32
403cd158: add     a10, a1, a10
403cd15b: l32i    a2, a10, 0          ; ota_seq
403cd15e: l32i    a8, a7, 152         ; bs->app_count
403cd161: addi.n  a2, a2, -1
403cd163: remu    a2, a2, a8          ; boot_index = (ota_seq - 1) % app_count
403cd166: j       0x403cd1a0          ; <<<< STRAIGHT TO retw
403cd1a0: retw.n
```

**Four independent absences, all of which would have to be present for rollback to work:**

1. **No PENDING_VERIFY → ABORTED loop.** Between `read_otadata` returning and the first
   `bootloader_common_ota_select_invalid` call there are exactly three instructions
   (`l32r`/`or`/`s8i` setting `ota_has_initial_contents = false`). The `#ifdef` block at source
   lines 396–405 is simply not there.
2. **No NEW → PENDING_VERIFY store.** After `remu` computes `boot_index` the function jumps
   unconditionally to `retw.n`. There is no load of `+0x18` off the selected entry, no `movi …, 1`,
   no `s32i`.
3. **No `write_otadata` call anywhere in the function.** Its call8 targets are exactly:
   `0x403ccf88` (`read_otadata`), `0x403cc884` (`ota_select_invalid`), `0x403cc814`
   (`get_active_otadata`), `0x403cc6fc` (`ota_select_crc`), `0x403cc608` (log timestamp).
4. **No `esp_flash_encryption_enabled` call** (`0x403cbb38`). Under the `#ifdef`, that call is the
   first thing in the block (`bool write_encrypted = …`). Absent.

### 3.1 Corroboration — `write_otadata` has exactly one call site in the whole image

The literal `"E (%lu) %s: Error in write_otadata operation. err = 0x%x"` lives at `0x3fce2aa8`.
Scanning all three segments for 32-bit words equal to that address yields **exactly one** literal
pool entry (`0x403cb8b4`), referenced by **exactly one** `l32r` (at `0x403ccf7b`). That single site
sits inside the function at **`0x403ccf28`**, which decodes unambiguously as `set_actual_ota_seq()`
with `write_otadata()` inlined into it:

```
403ccf28: entry  a1, 64
403ccf2b: bltz   a3, ret              ; if (index <= FACTORY_INDEX) …
403ccf34: beqz   a8, ret              ; if (!ota_has_initial_contents) …
403ccf37: movi   a12, 32 / movi a11, 255 / call ROM memset   ; memset(&otadata, 0xFF, sizeof)
403ccf44: movi   a8, 2                ; ESP_OTA_IMG_VALID
403ccf46: addi.n a3, a3, 1            ; otadata.ota_seq = index + 1
403ccf4a: s32i.n a8, a1, 24           ; otadata.ota_state = 2     <-- offset 0x18, struct confirmed
403ccf4c: s32i.n a3, a1, 0            ; otadata.ota_seq
403ccf4e: call8  0x403cc6fc           ; bootloader_common_ota_select_crc
403ccf51: s32i.n a10, a1, 28          ; otadata.crc
403ccf53: call8  0x403cbb38           ; esp_flash_encryption_enabled
403ccf5a: srli   a10, a7, 12          ; offset / FLASH_SECTOR_SIZE
403ccf5d: call8  0x403cca84           ; bootloader_flash_erase_sector
403ccf66: movi.n a12, 32              ; sizeof(esp_ota_select_entry_t)
403ccf6c: call8  0x403ccaa8           ; bootloader_flash_write(offset, &otadata, 32, enc)
```

Note it uses `bs->ota_info.offset` with **no `+ 0x1000 * i` term** — matching source line 501's
`+ FLASH_SECTOR_SIZE * 0` and *not* the rollback call sites, which are indexed. If
`ROLLBACK_ENABLE` were set there would be two further call sites and `write_otadata` would have
been emitted out-of-line and called from `get_selected_boot_partition`; it is not, and it is not.

This is also the **only** circumstance in which the stock bootloader writes `otadata` at all:
first boot of a device that has no `factory` partition and blank otadata. Our partition table
*has* a factory partition, so even that path is unreachable here.

### 3.2 `bootloader_common_ota_select_invalid` at `0x403cc884` — decoded

```
403cc887: l32i.n a9, a2, 0      ; ota_seq
403cc88b: movi.n a2, 1
403cc88d: beqi   a9, -1, ret    ; ota_seq == UINT32_MAX -> true
403cc890: l32i.n a8, a8, 24     ; ota_state
403cc892: addi   a8, a8, -3     ; state - 3
403cc895: saltu  a2, a2, a8     ; 1 <u (state-3)  ->  true iff state-3 > 1
403cc898: addi.n a2, a2, -1
403cc89a: neg    a2, a2
403cc89d: extui  a2, a2, 0, 8   ; result = (state == 3 || state == 4)
```

Exact match for the source. **`3` and `4` are the only `ota_state` values the stock bootloader
reacts to.**

### 3.3 Anti-rollback: also off — CONFIRMED, independently

Two proofs. (a) The `#ifndef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` arm is the one compiled: the
function calls `bootloader_common_ota_select_invalid` and `bootloader_common_get_active_otadata`,
which exist *only* in that arm (the anti-rollback arm calls
`get_active_otadata_with_check_anti_rollback` and compares `ota_seq`/`crc` inline instead).
(b) `check_anti_rollback()`'s `ESP_LOGE(TAG, "Failed to get partition description %d", err)` is an
**error**-level string that survives the WARN log level, and it is absent from the binary. Also
absent: any `esp_efuse_check_secure_version` machinery. `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n`.

### 3.4 Positive control — the same test run against a known-`y` bootloader

A detection method that finds nothing is worth only as much as its proof that it can find
something. The control is a bootloader built from this project's own tree with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — a throwaway build made for this test only; the shipped
`sdkconfig.defaults` sets `=n` (§8.1). Disassembling that build's
`build/bootloader/bootloader.elf` copy of `bootloader_utility_get_selected_boot_partition` with the
same toolchain shows precisely the code the stock image lacks:

```
call8 <bootloader_common_read_otadata>
call8 <esp_flash_encryption_enabled>      ; <-- absent in stock
bnei  a8, 1, …   / movi.n a8, 4 / s32i.n a8, a1, 24 / call8 <write_otadata$isra$0>   ; PENDING_VERIFY(1) -> ABORTED(4), copy 0
bnei  a8, 1, …   / movi.n a8, 4 / s32i   a8, a1, 56 / call8 <write_otadata$isra$0>   ;                     copy 1  (a1+32+24)
call8 <bootloader_common_ota_select_invalid>
call8 <bootloader_common_get_active_otadata>
…
movi.n a8, 1 / s32i.n a8, a10, 24 / call8 <write_otadata$isra$0>                      ; NEW(0) -> PENDING_VERIFY(1)
```

So the detection method demonstrably *does* fire when the feature is present. Its silence on the
stock image is a real negative, not a blind spot. **This is what upgrades the finding from
"strongly indicated" to CONFIRMED.**

---

## 4. The stock application

Same conclusion, and it must be: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is a single project-wide
Kconfig symbol — one `sdkconfig` produces both halves of an IDF build — so a bootloader built with
it off implies an app built with it off. Confirmed directly anyway:

* The `factory` slot, carved from the same full-flash backup — `esp_app_desc`: project `BYOK`, version `f764bf4-dirty`, built
  `Mar 20 2026 08:12:43`, `idf_ver v5.5.3-dirty`, `secure_version 0`. Built 49 seconds before the
  bootloader; same build, same config.
* The app is built at **INFO** log level (colour-coded `I (%lu) %s: …` strings present), so
  `ESP_LOGE` strings certainly survive. `esp_ota_begin()`'s rollback guard string
  **`"Running app has not confirmed state (ESP_OTA_IMG_PENDING_VERIFY)"` is absent** — and
  `esp_ota_begin` is definitely linked and called (the updater's own
  `"Failed to begin OTA"` / `"esp_ota_set_boot_partition failed"` strings are present). Grade:
  **CONFIRMED**.
* Consequence, via `set_new_state_otadata()` (`esp_ota_ops.c:113`): the stock updater's
  `esp_ota_set_boot_partition()` writes **`ota_state = ESP_OTA_IMG_UNDEFINED (0xFFFFFFFF)`**, not
  `ESP_OTA_IMG_NEW`. The IDF docs say the same
  (`docs/en/api-reference/system/ota.rst:141`): *"`ESP_OTA_IMG_UNDEFINED` state is set by
  `esp_ota_set_boot_partition()` function if `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` option is not
  enabled."*

### 4.1 Live otadata on the device

The `otadata` partition, carved from the same backup, both sectors: `ota_seq = 0xFFFFFFFF`,
`seq_label` all-`0xFF`, `ota_state = 0xFFFFFFFF`, `crc = 0xFFFFFFFF`. Fully erased ⇒ both copies
"invalid" ⇒ factory boots. Consistent with D-014 (a freshly-erased `otadata` is exactly the state
ESP-IDF's own slot-selection logic starts from). No OTA has ever run on this unit.

---

## 5. What `esp_ota_mark_app_valid_cancel_rollback()` writes — exact bytes

The call is **not** compiled out when `ROLLBACK_ENABLE=n`. `esp_ota_ops.c:944` →
`esp_ota_current_ota_is_workable(true)` → if `otadata[active].ota_state != ESP_OTA_IMG_VALID`, set
it to `ESP_OTA_IMG_VALID` and call `rewrite_ota_seq()` (`esp_ota_ops.c:526`), which does:

```c
two_otadata[sec_id].ota_seq = seq;                                       /* unchanged value */
two_otadata[sec_id].crc     = bootloader_common_ota_select_crc(&two_otadata[sec_id]);
esp_partition_erase_range(ota_data_partition, sec_id * 0x1000, 0x1000);  /* FULL 4 KiB SECTOR ERASE */
esp_partition_write(ota_data_partition, 0x1000 * sec_id, &two_otadata[sec_id], 32);
```

Projected onto a virgin device, before D-016's Stage A rehearsal (stock updater would put us in
`ota_0`, otadata currently blank ⇒
`esp_rewrite_ota_data` takes the "both invalid" branch, `next_otadata = 0`, `seq = 1`):

| Address | Before (written by stock updater) | After our `mark_app_valid` |
|---|---|---|
| `0x910000` | `01 00 00 00` (`ota_seq = 1`) | `01 00 00 00` — unchanged |
| `0x910004`–`0x910017` | `FF ×20` (`seq_label`) | `FF ×20` — unchanged |
| **`0x910018`** | **`FF FF FF FF`** (`UNDEFINED`) | **`02 00 00 00`** (`VALID`) ← **the only field that changes** |
| `0x91001C` | `9A 98 43 47` (`crc = 0x4743989A`) | `9A 98 43 47` — recomputed, identical (`crc` covers `ota_seq` only, which did not change) |
| `0x910020`–`0x910FFF` | `FF …` | `FF …` — erased and left erased |
| `0x911000`–`0x911FFF` | untouched | untouched |

(`crc = esp_rom_crc32_le(0xFFFFFFFF, &ota_seq, 4)`; `0x4743989A` for `ota_seq = 1`.)

**That is not, however, this device's actual layout.** Under D-016, Stage A ran first: it installed
stock 1.1.3 into `ota_0`, writing `otadata[0] = {seq 1, UNDEFINED, crc 0x4743989A}` at `0x910000`.
This project's own image then landed in `ota_1` (confirmed in
[architecture.md](architecture.md) and D-016), and the stock updater wrote the **other** copy:
`otadata[1] = {seq 2, UNDEFINED, crc 0x55F63774}` at `0x911000`. On this actual device,
`esp_ota_mark_app_valid_cancel_rollback()` erases and rewrites **`0x911000`**, flipping
`0x911018` from `FF FF FF FF` to `02 00 00 00`. Same one-field change, same 4 KiB erase, same
verdict — only the address moves from the virgin-device baseline above. (Here the interrupted-erase
failure mode is *better* still: losing `otadata[1]` leaves `otadata[0]` valid, so the device boots
stock 1.1.3 in `ota_0`.)

**Three things follow.**

1. **It is a write outside this project's own OTA slot.** `otadata` spans `0x910000`–`0x911FFF`;
   this project's slot is `ota_1 @ 0x610000` (D-016). [SAFETY.md](../SAFETY.md) §1 forbids
   overwriting a partition without a verified backup and a tested recovery path for that region,
   and `otadata` is named explicitly in D-011's forbidden-regions table. And it is not a 4-byte
   poke: it is a **4 KiB sector erase** followed by a 32-byte rewrite. That is why the call at
   `firmware/s3/main/app_main.c:1893` is compiled in only behind `CONFIG_BYOK_ALLOW_OTADATA_WRITE`
   (§8.2), which is off by default.
2. **It happens once, not per boot.** The `ota_state != ESP_OTA_IMG_VALID` guard means the second
   and subsequent boots are a pure read. Small mercy; does not change (1).
3. **With the stock bootloader it accomplishes exactly nothing.** `0xFFFFFFFF` and `0x00000002`
   are treated identically by `bootloader_common_ota_select_invalid` (§3.2) — neither is `3` nor
   `4`. The write changes no boot behaviour whatsoever. It is a forbidden-region write that buys
   zero safety.

There *is* one non-zero-risk failure mode: power loss between the erase and the 32-byte write
leaves that otadata copy all-`0xFF`. In this specific configuration that degrades gracefully — both
copies invalid ⇒ the bootloader falls back to `factory` (stock) — so it is not a brick. But it is an
avoidable flash erase on a partition this project has declared off-limits, which is the whole reason
it sits behind a build flag rather than running unconditionally.

---

## 6. Scenario table — what actually happens, with the STOCK bootloader

Assumes our image is installed in `ota_1` via the stock SD updater (D-016; the vendor's rehearsal
copy from Stage A occupies `ota_0`), `otadata[1] = {seq 2, state UNDEFINED}`.

| # | Scenario | What the stock bootloader does | Result | Would `ROLLBACK_ENABLE` in *our* sdkconfig change it? |
|---|---|---|---|---|
| **1** | **Our image crashes at boot, repeatedly** (valid image, panics in `app_main` or earlier C code) | Selects `ota_1` (highest valid `ota_seq` — `otadata[1]`'s seq 2 — with state not 3/4). `esp_image_verify` passes. Loads and jumps. App panics → default `ESP_SYSTEM_PANIC_PRINT_REBOOT` → reset → bootloader re-reads the *same* otadata → selects `ota_1` again. | **Infinite boot loop. No rollback. No fallback to factory.** The stock SD-updater recovery route is unavailable (it lives in the stock app, which never runs). Recovery requires the owner: our own early button-hold path if it is reached, otherwise USB/serial download mode. | **No.** The revert is bootloader-side code that does not exist in this bootloader. Setting the symbol in our build changes only our *app*; it cannot add the PENDING_VERIFY→ABORTED transition. |
| **1b** | Our image is **corrupt / truncated / bad checksum** | `esp_image_verify` fails ⇒ `bootloader_utility_load_boot_image` walks *backwards* from `start_index` down to `FACTORY_INDEX` (`bootloader_utility.c:594`), then forwards through the other OTA slots. | **Safe.** Falls through to `factory` (stock app) automatically. This fallback is unconditional and has nothing to do with `ota_state`. | No — already works. |
| **2** | **Our image boots fine but never calls `mark_app_valid`** | `ota_state` stays `UNDEFINED (0xFFFFFFFF)`. Not `3`, not `4` ⇒ fully bootable. Never rewritten. | **Boots our slot, forever. Indistinguishable from scenario 3.** There is no "pending verify" window and no deadline. | **No.** With `ROLLBACK_ENABLE=y` in our app, only *our own* `esp_ota_set_boot_partition` would write `NEW (0)` instead of `UNDEFINED`; the stock bootloader treats `0` as bootable too and never promotes it to `1`, so the state simply sits at `NEW` forever. |
| **3** | **Our image is fine** and, with `CONFIG_BYOK_ALLOW_OTADATA_WRITE` on, calls `mark_app_valid` | Same selection as #2. The call performs one 4 KiB erase + 32-byte write at `0x911000`, flipping `+0x18` from `FFFFFFFF` to `00000002`. | **Boots our slot.** The write is behaviourally inert (§5, item 3), which is why it is gated off by default. | No. |
| **4** | Owner drops the pristine vendor `BYOK.tar` back in `/Updates/` (D-011 reversal) — requires the **stock** app to be running | Not our path, listed for completeness. | Works as D-011 describes, and is unaffected by any of this. | No. |

**The single most important line in this table is #1.** It is the reason
[design-rationale.md](design-rationale.md) D-011/D-014 and [recovery.md](recovery.md) describe the
return-to-original routes as *deliberate operator actions* — a host protocol command, a boot-time
button hold, or a download-mode session — and never as anything automatic. On this device nothing
reverts on its own. A valid-but-crashing image boot-loops until a person intervenes.

---

## 7. What Espressif documents about setting this in the app but not the bootloader

The symbol lives in `components/bootloader/Kconfig.app_rollback` — under the **Bootloader config →
Application Rollback** menu — and its help text describes the behaviour entirely in bootloader
terms:

> "After updating the app, **the bootloader** runs a new app with the `ESP_OTA_IMG_PENDING_VERIFY`
> state set. … If the app is working, then it is marked as valid. Otherwise, it is marked as not
> valid and rolls back to the previous working app."

`docs/en/api-reference/system/ota.rst` makes the division explicit and, crucially, states what
happens when the option is **off** — which is our operative case, since the bootloader is what
decides:

> **:94** — "If `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` option is not enabled (by default), then the
> use of the following functions `esp_ota_mark_app_valid_cancel_rollback` and
> `esp_ota_mark_app_invalid_rollback_and_reboot` are **optional**, and `ESP_OTA_IMG_NEW` and
> `ESP_OTA_IMG_PENDING_VERIFY` states are **not used**."

> **:103–113 (Rollback Process)** — every step that moves `ota_state` forward is attributed to
> *the bootloader*: "**The bootloader** checks for the `ESP_OTA_IMG_PENDING_VERIFY` state if it is
> set, then it will be written to `ESP_OTA_IMG_ABORTED`. … **The bootloader** checks the selected
> application for `ESP_OTA_IMG_NEW` state if it is set, then it will be written to
> `ESP_OTA_IMG_PENDING_VERIFY`."

> **:141–145** — the state-authorship table: `UNDEFINED` ← `esp_ota_set_boot_partition` when the
> option is off; `NEW` ← `esp_ota_set_boot_partition` when it is on; `ABORTED` and
> `PENDING_VERIFY` ← "**set in a bootloader**".

So: enabling the symbol in a build whose bootloader is not the one that ships is, per Espressif's
own description, **half a feature**. The app-side half (write `NEW`, refuse `esp_ota_begin` while
`PENDING_VERIFY`, invalidate the inactive otadata slot) compiles in; the bootloader-side half —
the *only* half that actually rolls anything back — does not exist. Espressif does not warn about
this split explicitly because in a normal project the two are built together from one `sdkconfig`;
our project is the abnormal case that breaks the assumption, because D-011 deliberately keeps the
vendor bootloader.

---

## 8. Why the shipped configuration is what it is

*(`sdkconfig.defaults` already ships `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n`, and
`app_main.c`'s `esp_ota_mark_app_valid_cancel_rollback()` call is already gated behind
`CONFIG_BYOK_ALLOW_OTADATA_WRITE`. This section records the reasoning behind both, since the
finding above is what settled them.)*

### 8.1 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` (the line is kept, not deleted)

Kept explicit rather than deleted, so the reasoning survives in the file and nobody re-adds `y` in
six months.

Reasons, in order of weight:

1. **It cannot do what the comment claims.** Its only load-bearing effect is on a bootloader we
   never flash (§3, §7). The safety net it advertises does not exist on this device.
2. **A false safety claim would be worse than no claim.** Scenario #1 above shows a bad first boot
   boot-looping, not self-healing — which is exactly why [design-rationale.md](design-rationale.md)'s
   D-011/D-014 and [recovery.md](recovery.md) state plainly that there is no automatic rollback on
   this device, rather than describing this symbol's nominal behaviour.
3. **It changes our app's behaviour in a way that only ever misleads.** With it on, our own
   `esp_ota_set_boot_partition` (the BOOT_ORIGINAL protocol command, one of D-011's two
   return-to-original routes) would stamp
   `ota_state = ESP_OTA_IMG_NEW`, a state this bootloader never advances. Worse, it is a latent
   trap: if a rollback-capable bootloader ever *did* boot this flash, every slot we had pointed at
   would be one un-confirmed reboot away from being marked `ABORTED`.
4. **It arms `esp_ota_begin`'s `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` guard** (`esp_ota_ops.c:164`)
   and `esp_ota_invalidate_inactive_ota_data_slot()` (line 203, another otadata write) — two
   behaviours predicated on a state machine that will never run.

Keep `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n` as-is (it already matches stock, and IDF forbids it
without `ROLLBACK_ENABLE` anyway — `depends on BOOTLOADER_APP_ROLLBACK_ENABLE`).

### 8.2 `esp_ota_mark_app_valid_cancel_rollback()` is gated behind `CONFIG_BYOK_ALLOW_OTADATA_WRITE`

`firmware/s3/main/app_main.c:1893`. Gating it (rather than leaving it unconditional) is not a
judgement call:

* It performs a **4 KiB erase + write at `0x911000`** (D-016 layout, §5), i.e. outside `ota_1`,
  this project's own slot. That is exactly the class of write `CONFIG_BYOK_ALLOW_OTADATA_WRITE` was
  created to hold back, and `otadata` is one of the rows in D-011's forbidden-regions table.
* It is **not** compiled out by `ROLLBACK_ENABLE=n`; it is unconditional IDF code (§5).
* Per IDF docs :94, with the option off the call is explicitly **optional**.
* Per §3.2, with this bootloader it is behaviourally **inert**.

Gating (rather than deleting) matches the project's existing precedent for BOOT_ORIGINAL and the
boot-time EXECUTE path, keeps the intent legible, and costs nothing: with the gate off the call
simply does not compile in, and the `#else` branch logs at DEBUG rather than WARN, because that is
the expected state and not a problem. Deleting the call outright would also work; gating instead
keeps the decision in one legible place should the constraint this document is built on — the
bootloader at `0x0` being off limits, per D-011's forbidden-regions table — ever change.

### 8.3 What this means for recovery, in practice

There is no automatic rollback on this device — [design-rationale.md](design-rationale.md) and
[recovery.md](recovery.md) already say so plainly, and this analysis is the evidence behind that
statement. A few consequences worth spelling out:

* **The real first-boot safety net is the boot-time button-hold path** (D-011) — sampled before
  display init, USB init, PSRAM-hungry allocations, and any `ESP_ERROR_CHECK` that can abort. It
  is the only host-independent recovery for a valid-but-crashing image; anything that can panic
  ahead of it removes the net entirely.
* **Corrupt images are genuinely safe** (scenario 1b) — the bootloader's walk-backwards-to-factory
  fallback is unconditional. The risk is concentrated entirely in "image verifies but crashes".
* **The SD-updater reversal route does not cover scenario #1**, because it needs the stock app to
  be the one running. Recovery from a boot loop is USB/download-mode territory and needs owner
  action — [recovery.md](recovery.md)'s prerequisite table says so.

---

## 9. Evidence grades

| Claim | Grade | Basis |
|---|---|---|
| Stock bootloader has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` | **CONFIRMED** | Disassembly §3 (four independent absences) + single-call-site corroboration §3.1 + positive control §3.4 |
| Stock bootloader has `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n` | **CONFIRMED** | §3.3 — wrong `#ifdef` arm compiled *and* an ERROR-level string absent at WARN log level |
| Stock bootloader reacts only to `ota_state ∈ {3, 4}` | **CONFIRMED** | §3.2, instruction-level decode matching source exactly |
| Stock app has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` | **CONFIRMED** | Shared project Kconfig + §4 string absence at INFO log level |
| Stock updater writes `ota_state = 0xFFFFFFFF (UNDEFINED)` | **CONFIRMED** | `esp_ota_ops.c:113` `set_new_state_otadata()` + IDF docs ota.rst:141 |
| Exact bytes `mark_app_valid` changes (§5 table) | **CONFIRMED** for offsets/semantics; **STRONGLY INDICATED** for the projected `ota_seq` / `crc` values | Offsets and state values read from source and confirmed against the disassembled struct access at `+0x18`/`+0x1c`; the specific `ota_seq`/`crc` values in each row depend on the stock updater's slot-selection order (D-014), which is not itself observed at the instruction level on hardware |
| Scenario table §6 | **CONFIRMED** for the bootloader's own behaviour; **STRONGLY INDICATED** for the app-side reset loop | Bootloader half is disassembled; the "panic → reboot → same selection" half assumes the default `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT`, which is our build's setting but is an app-side default, not a bootloader property |

## 10. Reproducing this

```sh
# 1. split the carved image into segments (header: magic e9, 3 segs, chip_id 9)
#    seg0 -> 0x3fce2820, seg1 -> 0x403c8700, seg2 -> 0x403cb700
# 2. disassemble the text segment at its load address
OD=~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump
$OD -D -b binary -m xtensa --adjust-vma=0x403cb700 seg2.bin > seg2.dis
# 3. find get_selected_boot_partition by its two LOGE literals, then read 0x403cd0dc..0x403cd1a0
# 4. positive control: build this project's firmware once with
#    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (a throwaway build -- the shipped
#    sdkconfig.defaults keeps =n), then disassemble that bootloader
$OD -d --disassemble=bootloader_utility_get_selected_boot_partition \
   firmware/s3/build/bootloader/bootloader.elf
```

Every step above is host-side static analysis of images already held on disk. No device was
contacted, and no bootloader, partition table, or flash region was written to produce any of it.
