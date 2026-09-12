# Design rationale

This records why the project is built the way it is — the technical decisions behind the
architecture, the install mechanism, the transport, and the safety gates that shaped how the
hardware was approached. It is a rationale document, not a changelog: each entry states the
decision and why it was made, not the day-by-day process of arriving at it. Where a later decision
changes an earlier one, that is noted in place rather than left as two contradictory records.

---

### D-001 — Separate evidence, derived output, and secrets by directory

Recon photographs, firmware dumps, and working notes have very different handling requirements — a
photograph is permanent evidence that must never be edited or overwritten, a firmware dump can
contain credentials and must never be committed, and a finding written up in a document is meant to
be shared. Keeping these in clearly separate places makes the safety rules enforceable by path
rather than by vigilance: a tool whose output would land somewhere it shouldn't is a bug, not a
judgement call.

### D-002 — Four evidence labels, and UNKNOWN is a valid answer

Every hardware claim in this project's documentation carries one of **CONFIRMED / STRONGLY
INDICATED / POSSIBLE / UNKNOWN**, together with the specific evidence behind it. A marking that
isn't clearly legible is recorded as UNKNOWN — never guessed, never reconstructed from what a
reference design "should" have. The asymmetry that justifies this: a wrong pinout asserted with
confidence can damage an irreplaceable board; an honest UNKNOWN costs nothing but a slightly less
tidy-looking document.

### D-003 — No device write before both a verified backup and a tested recovery path exist

This is an **AND**, not an OR, and it governed every device-touching step of the project: no
`esptool` erase or write, no efuse operation, no secure-boot change, and no blind entry into
download mode, until a hash-verified backup of whatever would be modified existed *and* a recovery
path had actually been exercised end to end — not just written down. The backup half was met by a
two-pass full-flash read that matched byte-for-byte (see
[flash-layout-and-updater.md](flash-layout-and-updater.md) §1 and [recovery.md](recovery.md) §1); the tested
recovery path half was met only once the stock-firmware install rehearsal below (D-016) actually
succeeded on hardware. All physical actions on the device — cables, buttons, probes — were
performed by a person, one step at a time, never scripted or automated.

### D-005 / D-006 — Finding the BOOT/IO0 access point without guessing

Rather than hold an unidentified button at power-on and risk triggering a hidden factory-reset or
firmware-erase path in the stock UI, the button field was ruled out first, by careful macro
photography: none of the board's switches carries a `BOOT`, `IO0`, `DL`, `FLASH` or similar
function label. That pointed instead at a pair of unlabelled 6-pad footprints on the board, one
beside each MCU's reset switch, that turned out — on closer inspection — to have the geometry of a
factory spring-pin programming jig (six signal pads plus three mechanical alignment holes). If the
S3's `IO0` line is broken out anywhere accessible without opening the case further, it is almost
certainly one of those six pads — see [pinout.md](pinout.md) §3 for the full geometry and the
bounded, multimeter-based procedure that would actually identify which one, without ever shorting a
pad on a guess.

### D-007 — The USB PHY hand-over and the boot recovery window

Investigating why a USB-Serial-JTAG debug device would briefly appear on the host and then vanish
during boot led to the single most useful mechanism this project found: the S3's two USB
personalities share one physical PHY, and a single firmware call hands it from the debug interface
to the USB-OTG host stack at a fixed, repeatable point in the boot sequence (measured at
4.242–4.243 s across independent trials, with near-zero jitter). Several stock boot arms — a
missing SD card, Disk Mode, and a console mode entered by holding a specific button — never make
that call at all, leaving the debug interface enumerated indefinitely. This is what makes
`esptool`'s own DTR/RTS auto-reset able to reach ROM download mode with no `GPIO0`/`BOOT` strap
required, as long as the reset is issued while the debug interface still owns the PHY. See
[architecture.md](architecture.md) §6 for the full mechanism and how the replacement firmware turns
this into a deliberate, guaranteed recovery window rather than a boot-order accident.

### D-008 — Planning the first read-only device session in strict, reversible order

Every device-touching command was planned, shown, and run in a fixed order before it was ever
executed: read the chip identity, read the flash identity, read the 3 KiB partition table (rather
than assuming ESP-IDF's default partition offsets), time a small read to measure real throughput,
and only then attempt a full flash read. Reading the actual partition table first — rather than
planning against a template layout — mattered concretely: this device's `otadata` partition sits
far outside where ESP-IDF's own defaults would put it (see [architecture.md](architecture.md) §8),
and any procedure written against the default layout would have silently targeted the wrong region
of flash. Nothing beyond read operations was ever authorized by this plan.

### D-009 — The full flash-backup go/no-go

Before committing to a two-pass, ~70-minute full 16 MiB flash read, seven conditions were checked
and confirmed: the small identification read was internally self-consistent (checksums and hashes
all validated), no flash encryption was active, no secure boot was active, the exact flash size was
known from the die itself, the read operation is structurally incapable of altering device state,
the destination was excluded from version control, and the time cost was bounded and measured in
advance. All seven held, and the backup was accepted only because both passes matched
byte-for-byte — a single clean-looking pass is not treated as a backup.

### D-010 — Architecture: modify only the ESP32-S3; the PICO is never touched

Five candidate architectures were scored against a weighted matrix — reversibility, risk to the
device, preservation of stock function, number of independent recovery paths, and fitness for
purpose — modifying only the S3, modifying only the PICO, modifying both, a host-only approach that
never writes the device at all, and an external-hardware interposer on the display connector.
Modifying only the S3 won decisively, for two compounding reasons: every capability the project
needs — display, USB, Wi-Fi, SD card, buttons — is already confirmed to live on the S3, so the PICO
has nothing to contribute to a display project; and the PICO has no backup and no known
download-mode route, so any write to it would be unrecoverable if it went wrong. The host-only
approach was the honest safety benchmark and scored well, but was rejected on capability grounds,
not safety: a live, host-rendered display feed is not achievable through a mass-storage file drop.
The choice was made safe, not just preferable, by two independently confirmed facts: the bootloader
has neither secure boot nor flash encryption enabled, so it accepts an unsigned custom image, and
the device's own partition table showed two entirely free 3 MiB application slots alongside the
untouched original firmware.

### D-011 — Installation and dual-boot strategy

The one and only install route is the stock SD-card updater: a two-member archive dropped at
`/Updates/` on the SD card, extracted and installed by code the vendor firmware already runs on
every boot, writing no fixed flash address directly. There is deliberately no second install route.
In particular, **`esptool` is never used to install firmware on the real device**: its role here is
read-only — chip and flash identification, partition-table reads, and the two full-flash backup
passes — plus one reserved recovery use, writing a previously backed-up region back to the exact
offset it came from if the device can no longer be reached any other way. Choosing a single install
path means there is exactly one write mechanism to reason about, and it is the vendor's own, which
computes its target slot from the device's live boot selector instead of taking a flash address
from whoever typed the command. See [firmware.md](firmware.md) and
[README.md](../README.md) for the same policy stated operationally.

Returning to the original firmware works two independent ways: a protocol command sent by a
connected host, and a boot-time button hold sampled in the first moments of startup, before either
USB or the display come up — the latter exists specifically so a return-to-stock path still works
even when the replacement firmware is too broken to answer over USB at all.

**Forbidden flash regions.** Whatever install route is in play, the following regions are never
erased and never written — not by the stock updater, not by this project's own firmware, and not by
any tool this project runs as part of normal work. This list is the concrete form of
[SAFETY.md](../SAFETY.md) §1's backup-and-tested-recovery rule, and the offsets are this unit's own,
read from its partition table (see [flash-layout-and-updater.md](flash-layout-and-updater.md) §1).

One exception spans the whole table, and it is the reserved recovery use named in this record's
opening paragraph: restoring a previously backed-up region to the exact offset it came from, by hand, when the
device can no longer be reached any other way. [recovery.md](recovery.md) §4 is that procedure, and
every command in it writes or erases one of these regions on purpose. It is a restore of bytes
already read off this same device, performed deliberately by a person as a last resort — not an
install route, not automated, and never a write of new content into a region below:

| Region | Offset | Why it is off limits |
|---|---|---|
| 2nd-stage bootloader | `0x000000` | The only bootloader that will ever start any image on this device; it is never replaced, so its behaviour is a fixed constraint rather than something this project can change. |
| Partition table | `0x008000` | Every offset below is derived from it; a bad write here makes the flash unreadable to recovery tooling as well as to the device. |
| `nvs` | `0x009000` | Wi-Fi credentials, Bluetooth pairing, device token, and radio calibration — the only copy in existence, present in no vendor archive. This project uses its own NVS namespace and never performs a full NVS erase. |
| `phy_init` | `0x00F000` | Erased is its correct stock state; "repairing" it would be a change, not a fix. |
| `factory` | `0x010000` | The original, irreplaceable firmware image, and the fallback the bootloader's own unconditional validity walk lands on. |
| `otadata` | `0x910000` | The boot selector. Two writes are compiled in behind one explicit build flag (`CONFIG_BYOK_ALLOW_OTADATA_WRITE`, off by default, and off in every shipped release): the deliberate revert-to-original path above, which is an erase of this partition and nothing else; and the one-time `esp_ota_mark_app_valid_cancel_rollback()` at first boot of a new image (`firmware/s3/main/app_main.c:1893`), which is a 4 KiB sector erase **plus** a 32-byte rewrite. Both are listed in [firmware.md](firmware.md) §3's gate table, and [bootloader-analysis.md](bootloader-analysis.md) §8.2 explains why the second one is inert on this device's bootloader anyway. |
| `assets` | `0x912000` | The stock Lua UI bundle; replacing it is how a stock install would be made unbootable-looking without touching an app slot at all. |
| The co-processor's flash | — | No download-mode route and therefore no backup path; see D-010 and D-015. |

The only region this project's own firmware writes is the OTA app slot it was installed into
(`ota_1`, D-016), plus its own NVS namespace.

### D-012 — Transport: USB CDC-ACM first, Wi-Fi later, behind identical framing

USB CDC-ACM over the S3's native USB peripheral is the primary transport: it needs no driver, uses
the same cable that already charges the device, and has deterministic latency with no association
step — all of which matter more for a live status link than raw bandwidth does. Wi-Fi is deferred,
not rejected: it costs an association step, a provisioning UI, extra power draw, and an open TCP
port on the user's network that CDC simply doesn't have, and it will sit behind the identical frame
format once added. The on-die USB-Serial-JTAG debug interface deliberately carries no protocol
traffic at all — it stays a plain console, so that a bug in the main protocol can never take the
recovery channel down with it.

### D-013 — Rendering happens on the host; the device only ever receives pixels

The device composes nothing on its own beyond a small built-in vocabulary (clear the screen, draw
text, draw a rectangle) reserved for boot and status screens shown before a host connects, and error
screens shown after one disconnects. Every other pixel reaching the panel was composed on the host
and sent as a packed framebuffer or a dirty rectangle. This confirmed an earlier provisional
decision to prefer host-side rendering once the two facts it depended on were settled: the display
is monochrome and small enough that even a full frame is a modest transfer, and the device's own I²C
bus — not the USB link — is the actual bottleneck, since every byte reaching the panel is its own
one-byte I²C transaction. That bottleneck is exactly why dirty-rectangle updates are part of the
protocol from its first version, rather than an optimisation bolted on afterward — see
[protocol.md](protocol.md).

### D-014 — OTA slot assignment and the revert mechanism

Of the two free application slots the device's own partition table exposed, this project's firmware
was assigned the one adjacent to the untouched original-firmware slot — the same slot ESP-IDF's own
"pick the next update partition" logic would choose from a freshly-erased state — with the second
slot held in reserve as the genuine alternating partner for the project's own future self-updates.
Reverting to the original firmware is a single erase of the boot-selector partition, not a restore:
an erased boot selector is bit-for-bit this unit's actual as-shipped state, so there is no image to
get wrong and no offset that could collide with anything else in flash.

### D-015 — Update archives never contain a PICO firmware image

Every update archive this project builds — including the rehearsal archive below — ships exactly
two members: the S3 application image and the Lua asset bundle. The co-processor's own firmware
image is omitted, unconditionally, from every archive built here. Three facts justify this: the
vendor's own updater treats the S3, PICO, and asset updates as three independent, individually
reported sub-updates, so a missing member is a clean skip rather than a failure; the link protocol
between the two chips carries no version handshake at all, meaning a newer S3 firmware paired with
an older PICO firmware is a configuration the stock design already supports, not one this project
invents; and the PICO — unlike the S3 — has no backup and no known recovery route, so any write to
it that went wrong would be unrecoverable.

### D-016 — Rehearsing the recovery path with the lowest-stakes real write available

**"Stage A"** is the shorthand this project's code and documents use for the rehearsal described in
this record: the one install performed before any of this project's own firmware existed, in which
the vendor's own current stock release was written into the first free OTA slot (`ota_0`) through
the vendor's own SD-card updater. Wherever "Stage A" appears in a source comment, a build
configuration, or a script, it means exactly this event and nothing else; D-018 records that it
succeeded and what followed from that.

Before writing anything of its own, this project used the exact same install mechanism to write the
vendor's own current stock release back onto the device. This is deliberately the cheapest possible
real write available: a known-good image, written into an already-erased slot, through the same
path the vendor's own updates use. It served three purposes at once — it is a genuine end-to-end
rehearsal of the packaging and install pipeline, it is what actually satisfies the "tested recovery
path" half of D-003's gate (a written procedure is not a tested one), and it gained a concrete,
useful capability in its own right (a newer vendor release with an always-on USB console mode, in
place of the roughly four-second boot window described under D-007). This rehearsal succeeded
unattended, end to end, on real hardware. It is also why the vendor's rehearsal image occupies the
first OTA slot and this project's own image occupies the second, rather than the reverse assumed
under D-014 before the rehearsal was approved.

### D-017 — Arming the SD updater's own flash write, on its own gate

The SD-based updater component was built to be a byte-for-byte behavioural match for the vendor's
own updater — the same size and chip-identity checks, the same validate-then-select ordering, and
the same refusal to touch the original-firmware slot, the bootloader, the partition table, or `nvs`.
Its actual flash-write step sits behind its own dedicated build flag, kept separate from the flag
that governs the "return to original firmware" protocol command and the boot-time button hold, so
the two capabilities can be enabled independently. Arming the write reproduces an operation the
vendor's own firmware already performs on this exact hardware, not a new one; a corrupt or
truncated image is rejected before the boot selector is ever written, and even an image that
somehow got selected despite that check still falls back to the original firmware in the
bootloader's own unconditional validity check.

### D-018 — The recovery-path rehearsal succeeded; the safety gate closed

Once the rehearsal in D-016 ran successfully — an unattended install and reboot into the newly
installed vendor image, verified against the device's own status display — the second half of
D-003's gate was finally met: not just a documented recovery path, but a tested one. From this
point, flash writes into the OTA slots via the SD-updater path (never via direct low-level flash
tools) were authorized for this project's own firmware.

### D-019 — Removing a guard that blocked the very recovery path it was meant to protect

An early version of the SD updater added a check that refused to overwrite a target OTA slot unless
its existing contents belonged to this same project — added specifically to keep the armed updater
(D-017) from silently overwriting the rehearsal's stock image. In practice, that same check also
blocked the one recovery route this project's whole safety story depends on: reinstalling the
pristine stock image over a slot that already holds the stock image must simply work, not require an
extra opt-in flag every time. The check was removed entirely. It was never the thing protecting the
original-firmware slot in the first place — that protection comes from the target-slot selection
never being able to return that slot at all, which is unaffected by removing the check.

### D-020 — The screen-mirroring mode is retained as reference, not shipped

A live Mac-screen mirror — both a window-capture mode and a full virtual-display device that macOS
recognises as a normal display — was built and proven working on real hardware, including a
functioning virtual display visible in the system's own display settings. It does not ship in the
final product, by scope decision rather than any technical limitation: what ships is the USB
dashboard, a standalone clock mode, and a verified path back to the original firmware. The mirror
code, its tests, and its documentation are kept in this repository as a working reference for anyone
who wants to build on it — see [sample-projects/mirror-and-virtual-display.md](sample-projects/mirror-and-virtual-display.md).

### D-021 — Optional extras: a Doom port, a bulk-write display mode, and on-device settings persistence

Three smaller, separately-scoped additions round out the project. A Doom port for the panel reuses
the mirror pipeline built for D-020, lives entirely in its own directory with its own install
script, and is excluded from the main install flow — see
[sample-projects/doom.md](sample-projects/doom.md). A bulk-write mode for the display driver exists
to make that port's frame rate usable, alongside the byte-at-a-time transfer the stock protocol
otherwise requires (see [display.md](display.md)). On-device settings persistence (the active
preset, saved note text, idle mode, and backlight level) is stored under this project's own NVS
namespace — never the vendor's namespace, and this project never performs a full NVS erase.

### D-023 — Exclusive serial port access, and a notification path that doesn't fight the dashboard for it

Two host-side processes contending for the same serial port at once — a long-running status loop
and a separate one-off request — produced a sequence-gap error that crashed the loop: the port
accepted a second connection silently, and the second connection's handshake reset the first
connection's session out from under it. The fix is two independent, compounding layers, both kept
even though either alone would have prevented this specific failure. First, the serial port is now
opened exclusively, so a second open fails loudly instead of silently succeeding. Second, a one-off
request checks whether a long-running process already holds the port and, if so, queues its request
to that process instead of ever attempting to open the port itself. Both layers are kept because the
underlying failure needed three separate things to go wrong at once — the double-open succeeding, an
overly narrow exception type going uncaught, and a diagnostic reply being mistaken for a final
answer — and fixing only one of the three would have left the other two as latent risk for the next
piece of code that happens to touch this transport. See [host-tools.md](host-tools.md) for the
transport layer this produced.
