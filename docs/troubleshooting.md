# Troubleshooting

Seven issues found and fixed during development, folded here by symptom
and numbered so other files can cite them.
Each entry gives what was observed, the root cause with its evidence
grade, and the fix (applied or proposed — noted per entry). See
`docs/hardware.md`, `docs/display.md`, and `docs/protocol.md` for the
background these entries assume.

## 1. Blank display after boot (only one self-test phase visible, resting screen blank)

**Symptom.** On boot, only one phase of the display self-test rendered;
the others, and the final resting screen, stayed blank. `byok text`
returned without error afterward, but nothing appeared on the panel.

**Cause 1 — STRONGLY INDICATED: holding the front WAKE button (GPIO6)
during the ~8-second self-test power-cycles the board.** WAKE is the same
physical button used to power the unit on. The power-off handler of the
time drove the external power-hold GPIO high the instant a 3-second hold
was detected, with no wait for the button to actually be released. If a
finger was still on the button at that instant — easy to do while
watching an unfamiliar, silent boot sequence — the power-hold latch drops
while the button is still physically pressed, which the external power-on
circuit reads as a fresh power-on request. The board power-cycles
mid-self-test, restarting it from phase 1 each time.

**Cause 2 — POSSIBLE, self-flagged as an open risk at the time: an
unverified bulk-write display path.** A bulk I²C write mode (one
multi-byte burst per refresh instead of ~2,400 individual one-byte
transactions) was enabled by default despite never having been visually
verified against a real, watched panel. The I²C driver's ACK checking is
disabled, so a silently-truncated burst reports `ESP_OK` with zero I²C
errors even if the panel only latched part of the data — a sparse,
mostly-blank frame (like the border-and-banner self-test screen) degrades
much more gracefully under a partial burst than a dense one, which is
consistent with some phases rendering and others not.

**Fix.** Cause 1: wait for a debounced button release before driving the
power-hold GPIO (this is a deliberate divergence from the original
firmware's behavior, adopted specifically because it can't itself cause a
re-power, only delay the cut). Cause 2: fall back to the known-correct
per-byte I²C path as the default until the bulk path has been watched end
to end on a real panel; keep the bulk path available behind a build
option for whoever does that verification.

## 2. Image shifted up on the panel by 15 rows

**Symptom.** First working image on the physical panel: recognizable, but
the whole picture sat 15 pixel rows too high. The top border and title
line were pushed off the top of the glass; the vacated rows at the bottom
showed random pixels (uninitialized controller RAM), not wrapped content.

**Cause — STRONGLY INDICATED.** 15 is not a multiple of 8, which rules out
a page-addressing bug, and the image was not mirrored within its 8-row
bands, which rules out a framebuffer bit-order error (that finding also
promoted the framebuffer's bit-order convention from a previously-unverified
assumption to confirmed correct). An exhaustive scan of the panel
controller's command stream showed the byte range that would program the
UC1611 controller's row-granular "Set Scroll Line" register is never sent
at all, by either the original firmware or this project's own driver —
both rely on whatever value the register happens to power on with, and on
this unit that value is 15.

**Fix.** Explicitly program the scroll line register to 0 during display
bring-up, immediately after the panel's initialization sequence, as an
additive step (not a modification to the vendor-matched sequence itself).

## 3. Boot loops when installing an update from the SD card

**Symptom.** Placing a firmware tarball on the SD card and rebooting
produced a repeating panic-and-reboot cycle instead of installing the
update, on every boot as long as the update file was present.

**Cause — CONFIRMED.** The update installer ran directly on the startup
task, whose stack was sized only for launching other tasks (a bare 3,584
bytes). The installer's own local buffers alone (a 512-byte header buffer
plus a 4,096-byte streaming buffer) already exceeded that entire budget —
before counting any of the flash-write, storage, or hashing call frames
underneath them. FreeRTOS's own stack-overflow detector caught this
reliably, every time, the instant the installer began its write.

**Fix.** Run the SD-card update installer on its own short-lived task with
a stack sized for what it actually needs (10 KB, chosen with headroom
over the known buffer sizes), and have startup block on that task instead
of growing the startup task's own stack permanently for the sake of a
one-time boot check. A regression test enforces both that the installer
is never called directly from the startup task again, and that its
dedicated task's stack keeps a healthy margin over the installer's largest
known buffer.

## 4. Device won't stay off — restarts on its own after a WAKE-hold power-off (USB connected)

**Symptom.** Holding the front WAKE button powered the device off as
expected, but it powered itself back on a few seconds later — without any
software reboot indicator, i.e. as a genuine power interruption, not a
crash.

**Cause — STRONGLY INDICATED.** The original firmware refuses to cut power
at all while the device is on USB power; this project's shutdown path did
not reproduce that refusal. Dropping the external power-hold latch while
USB is attached collapses the supply rail far enough to brown out the
chip and release every pin — including the one asserting the power cut —
and with USB still supplying power, the rail re-establishes itself and the
board cold-boots. On battery alone there's no USB source to re-establish
the rail, which is consistent with the original firmware's power-off
working correctly on battery and this project's issue only showing up on
USB power.

**Fix (proposed).** Read the USB attach state before cutting power and
refuse the cut while attached — matching the original firmware's own
behavior — failing open (proceeding with the power-off) if that state
can't be read, so a detection failure can't reintroduce a "can never power
off" regression. Also reproduce two smaller pieces of the original
firmware's shutdown sequence: a backlight fade before the cut, and a short
settle delay immediately before it.

## 5. Device crashes and reboots on a WAKE-hold power-off attempt (battery only)

**Symptom.** With the USB cable disconnected, holding WAKE did nothing at
the expected 3-second threshold; around 8 seconds later the device
restarted (a backlight blink, then a fresh boot) rather than powering
off and staying off.

**Cause — STRONGLY INDICATED.** With the cable removed there is no USB
supply to re-establish power (ruling out the mechanism above), which
narrows this to a software fault. The shutdown path suspends the task
scheduler while it draws a "powering off" message and then refreshes the
display — but that refresh calls down into an I²C driver function that
blocks with a nonzero timeout, and blocking on any queue, semaphore, or
mutex with a nonzero timeout while the scheduler is suspended is an
unconditional precondition failure in the underlying RTOS: it asserts
immediately, before any actual waiting happens, every time, independent of
whether the display hardware would have responded. The resulting crash
triggers the project's configured panic-and-reboot path, whose fixed
reboot delay plus ordinary boot time accounts for the observed ~8-second
total almost exactly.

**Fix (proposed).** Stop suspending the scheduler around the shutdown
display draw — call the display functions directly, accepting the small,
bounded risk of a torn "powering off" frame if another task happens to
touch the display in the same brief window, which is a strictly better
outcome than a guaranteed crash. A real internal lock on the display
driver (rather than a scheduler-wide suspend) is the more thorough fix and
is recommended as separate follow-up work, not bundled with this one.

## 6. Dashboard: small text renders garbled, and the clock ticks unevenly (2s, 1s, 2s, 1s)

**Symptom.** Two lines of the dashboard's small-panel layout — a
system-status line and a now-playing line — rendered with visibly broken
letterforms, while the clock and date lines (sharing the identical font
and rendering code) stayed crisp. Separately, the on-panel clock's
displayed seconds advanced unevenly instead of ticking once per second.

**Cause 1 — CONFIRMED.** Two widgets capped their auto-fit font size at
half their row's height for no functional reason, while the other two
widgets on the same 16-pixel-tall row used the full row height. That
forced the affected widgets down to an 8-pixel font — small enough that
hard 1-bit thresholding of anti-aliased letterforms drops whole strokes
(a curve or thin stroke that straddles the threshold boundary differently
column to column loses a column entirely, and at 8 pixels tall a single
lost column is the stroke). Reproduced directly: the same string at a
larger, unforced font size renders cleanly through the identical
rendering and thresholding code.

**Cause 2 — CONFIRMED.** The render loop slept a fixed interval *after*
each cycle's work finished, rather than aligning to a fixed wall-clock
schedule. One widget's per-cycle system-status read reliably cost roughly
390–400 ms, pushing the real frame period to about 1.4 seconds against a
requested 1.0-second interval. Because the clock is displayed to
whole-second resolution, that non-integer real period aliases against the
integer display, producing a displayed-second delta sequence dominated by
alternating 1-then-2 — exactly the reported pattern, and not a literal
fixed timer.

**Fix.** Cause 1: use the same, uncapped font-size expression across all
affected widgets. Cause 2: align the render loop's sleep to a fixed
wall-clock schedule (track a target next-tick time and sleep only the
remaining time to it) rather than sleeping a flat interval after the
work completes; secondarily, reduce the per-cycle cost of the expensive
status read.

## 7. Dashboard crashes on a sequence-gap notification

**Symptom.** The dashboard loop terminated unexpectedly with an unhandled
exception while a second, short-lived host process happened to talk to
the device at the same time.

**Cause — STRONGLY INDICATED.** Three independent gaps compounded:

1. The serial connection was opened without an exclusive lock, so a
   second process could open the same port concurrently even though the
   link is documented as single-consumer. The second process's own
   connection handshake reset the device's sequence-number tracking out
   from under the dashboard loop's already-established connection state.
2. The protocol's sequence-gap notification is explicitly documented as
   informational — the device reports the gap and then still processes
   the frame normally — but the host's transport layer treated any reply
   carrying the expected sequence number as the final answer, so it
   returned the informational gap notification as if it were the real
   response instead of continuing to wait for it.
3. The exception type that notification produced on the host side was not
   a subclass of the error type the dashboard loop's error handling
   already caught, so it propagated all the way out and ended the process
   instead of triggering the loop's existing reconnect-and-resend
   recovery.

Any one of these three alone would not have crashed anything; all three
together did.

**Fix.** Open the serial port exclusively, so a second process's open
fails immediately and loudly instead of silently succeeding; make the
transport layer wait past an informational sequence-gap reply for the
real response that follows it, as the protocol always specified; and
widen the dashboard loop's (and the mirror's) error handling to also catch
the exception type a genuine device-reported error produces, so any
non-gap error still triggers the existing recovery path rather than
crashing the process. As defense in depth, anything that might talk to the
device outside of an already-running dashboard or mirror loop should
check for one first and prefer an out-of-band notification channel over
opening a second, competing connection to the same port.
