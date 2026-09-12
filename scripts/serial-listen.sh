#!/bin/zsh
#
# serial-listen.sh — passively capture the BYOK (ESP32-S3) boot/console log
#
# WHAT THIS IS FOR
#   The BYOK device enumerates on plug-in as an Espressif USB-Serial-JTAG
#   CDC device (typically /dev/cu.usbmodem101), publishes its serial node
#   ~0.3 s after attach, and disappears ~4.2 s later when the app switches
#   the USB PHY. During that ~4 s window the S3 mirrors its ESP-IDF boot
#   log (colour-coded "I (ms) TAG: ..." lines, banner "Starting BYOK
#   version %s") to that port. This script waits for the node to appear,
#   captures everything it emits until it disappears (or a timeout), and
#   summarizes the result.
#
#   By default it runs in FOLLOW mode: if the port disappears (a device
#   reset / re-enumeration, e.g. USB PHY switch, reboot, or replug), the
#   script does NOT exit. It logs that the port disappeared, keeps the
#   capture files open in append mode, and waits up to --reconnect-wait
#   seconds for a node matching the same pattern to reappear. When it
#   does, it reopens it read-only (identical exec 3< + stty logic as the
#   first open) and keeps appending to the same capture files, marking the
#   boundary with a "===== RE-OPENED ... (session N) =====" line. This
#   repeats — across as many sessions as needed — until the overall
#   --timeout expires or Ctrl-C is pressed. Pass --once to restore the old
#   single-session behaviour (exit as soon as the port disappears once).
#
# USAGE
#   ./scripts/serial-listen.sh                       # follow mode (default), 90s timeout, default port glob
#   ./scripts/serial-listen.sh --timeout 30
#   ./scripts/serial-listen.sh --port /dev/cu.usbmodem101
#   ./scripts/serial-listen.sh --port '/dev/cu.usbmodem*' --timeout 60
#   ./scripts/serial-listen.sh --yes                 # skip the confirmation prompt
#   ./scripts/serial-listen.sh --once                # old behaviour: stop at the first disappearance
#   ./scripts/serial-listen.sh --reconnect-wait 15    # follow mode, but give up re-enumeration after 15s
#
#   Run it, THEN plug in the device (or it's already in). Ctrl-C stops
#   early; the timeout is a hard ceiling on total run time (waiting for
#   the port to appear, capturing, and — in follow mode — waiting for
#   re-enumeration all count against it).
#
#   Before opening the resolved port, the script (a) confirms at least one
#   Espressif (VID 0x303a) USB device is currently enumerated, (b) refuses
#   if `lsof` shows another process already holding that node open (e.g. an
#   in-progress esptool session), and (c) prints what it resolved and asks
#   for interactive confirmation unless --yes is passed. None of this opens
#   the /dev node itself. In follow mode, the confirmation prompt is shown
#   ONLY before the first open of the run — every subsequent reopen of the
#   same node path (after a disappearance/re-enumeration) happens
#   automatically, with no further prompt; the (a)/(b) safety checks above
#   still run before each reopen.
#
# OUTPUT (all under captures/serial/)
#   <timestamp>-boot-log.txt        raw bytes read from the port, verbatim,
#                                    across ALL sessions of one run, in
#                                    order, separated by RE-OPENED marker
#                                    lines when follow mode reconnects
#   <timestamp>-boot-log-clean.txt  same, with ANSI colour escapes stripped
#   <timestamp>-usb-events.txt      unified-log USB attach/detach events
#
# PASSIVE / READ-ONLY — IMPORTANT
#   This script never writes a single byte to the serial port. The device
#   node is opened strictly read-only: `exec 3<"$PORT"` (a `<` redirect,
#   never `>` and never `<>`) BEFORE any `stty` call, and the reader is
#   plain `cat` reading from that read-only fd into a capture file. No
#   `screen`, no `cu`, no `printf`/`echo` to the port, nothing sent on
#   RTS/DTR toggles beyond what a plain read-only open implies. This holds
#   for every open in a run, including automatic reopens in follow mode.
#
#   Note: merely *opening* a USB-CDC port on macOS asserts DTR and RTS
#   together as a side effect of the open() call itself — this is NOT the
#   ESP32-S3's strapping-pin reset pattern (which needs DTR/RTS driven
#   independently, in a specific sequence, to pull GPIO0/EN). A plain
#   open here does not trigger bootloader/download mode. And even if it
#   did cause an ordinary reset, that is non-destructive — the device
#   just reboots and re-emits its boot log, which is exactly what this
#   script is trying to capture. `clocal` is set so open never blocks
#   waiting for a carrier-detect signal that a CDC device won't assert.
#
# REQUIRES: zsh, stty, cat, perl (for ANSI stripping), /usr/bin/log — all
# stock macOS. No installs, no sudo, no esptool, no serial writes ever.

emulate -L zsh
unsetopt NOMATCH
zmodload zsh/datetime 2>/dev/null

# --- resolve ROOT relative to this script's location ------------------------
SCRIPT_DIR="${0:A:h}"
ROOT="${SCRIPT_DIR:h}"
CAPTURE_DIR="$ROOT/captures/serial"
mkdir -p "$CAPTURE_DIR"

# --- args --------------------------------------------------------------
TIMEOUT=90
PORT_PATTERN='/dev/cu.usbmodem*'
ASSUME_YES=0
ONCE=0
RECONNECT_WAIT=30
while [[ $# -gt 0 ]]; do
  case "$1" in
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    --port)
      PORT_PATTERN="$2"
      shift 2
      ;;
    --yes)
      ASSUME_YES=1
      shift
      ;;
    --once)
      ONCE=1
      shift
      ;;
    --reconnect-wait)
      RECONNECT_WAIT="$2"
      shift 2
      ;;
    *)
      print -u2 "serial-listen.sh: unknown argument: $1"
      exit 2
      ;;
  esac
done

# Redacts a MAC-derived (or any other hex-serial-derived) usbmodem node name
# before it is echoed to stdout below (port-found/about-to-open/re-opening/
# port-disappeared lines all name $PORT) -- same rule the post-capture
# REDACT_PERL pass further down applies to the two log files, defined here
# too so it is in scope before the capture loop's first use of $PORT.
# Shells out to perl rather than zsh pattern-substitution so it doesn't
# depend on `setopt extended_glob` being on.
redact_display() {
  print -r -- "$1" | perl -pe 's/\busbmodem[0-9A-Fa-f]{8,}/usbmodem<REDACTED>/g'
}

TS="$(date +%Y%m%d-%H%M%S)"
BOOT_LOG="$CAPTURE_DIR/${TS}-boot-log.txt"
CLEAN_LOG="$CAPTURE_DIR/${TS}-boot-log-clean.txt"
USB_EVENTS_LOG="$CAPTURE_DIR/${TS}-usb-events.txt"

print "serial-listen.sh starting"
print "  ROOT:          $ROOT"
print "  port pattern:  $(redact_display "$PORT_PATTERN")"
print "  timeout:       ${TIMEOUT}s"
if (( ONCE )); then
  print "  mode:          --once (stop at first port disappearance)"
else
  print "  mode:          follow (default) — reconnect-wait ${RECONNECT_WAIT}s per re-enumeration"
fi
print "  boot log:      $BOOT_LOG"
print "  usb events:    $USB_EVENTS_LOG"
print "  (read-only — this script never writes to the serial port)"
print ""

# --- background unified-log stream (always /usr/bin/log, never bare `log`) --
/usr/bin/log stream --style compact --predicate 'eventMessage CONTAINS[c] "303a" OR eventMessage CONTAINS[c] "usbmodem" OR eventMessage CONTAINS[c] "terminateDevice"' > "$USB_EVENTS_LOG" 2>&1 &
LOG_PID=$!

READER_PID=""
INTERRUPTED=0
TRAPINT() {
  INTERRUPTED=1
  return 0
}

cleanup() {
  if [[ -n "$READER_PID" ]] && kill -0 "$READER_PID" 2>/dev/null; then
    kill "$READER_PID" 2>/dev/null
    wait "$READER_PID" 2>/dev/null
  fi
  exec 3<&- 2>/dev/null
  if kill -0 "$LOG_PID" 2>/dev/null; then
    kill "$LOG_PID" 2>/dev/null
    wait "$LOG_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

script_start=$EPOCHREALTIME

# --- wait for a port matching PORT_PATTERN, polling every 0.05s -------------
# Always bounded by the overall TIMEOUT. If $1 is non-empty, additionally
# bounded by that many seconds from now (used for the reconnect-wait cap in
# follow mode). Sets $PORT and returns 0 on success, 1 on giving up.
wait_for_port() {
  local extra="$1"
  local local_deadline=""
  if [[ -n "$extra" ]]; then
    local_deadline=$(( EPOCHREALTIME + extra ))
  fi
  PORT=""
  while (( ! INTERRUPTED )); do
    local now=$EPOCHREALTIME
    if (( now - script_start >= TIMEOUT )); then
      return 1
    fi
    if [[ -n "$local_deadline" ]] && (( now >= local_deadline )); then
      return 1
    fi
    # literal path (e.g. a fixed device path or a FIFO used for testing)
    if [[ -e "$PORT_PATTERN" ]]; then
      PORT="$PORT_PATTERN"
      return 0
    fi
    # glob pattern (e.g. the default /dev/cu.usbmodem*) — (N) makes it a
    # null expansion instead of an error when nothing matches yet.
    local matches=(${~PORT_PATTERN}(N))
    if (( ${#matches} > 0 )); then
      PORT="$matches[1]"
      return 0
    fi
    sleep 0.05
  done
  return 1
}

: > "$BOOT_LOG"

SESSION=0
TIMED_OUT=0

while (( ! INTERRUPTED )); do
  if (( SESSION == 0 )); then
    wait_for_port ""
    rc=$?
  else
    print "$(date '+%H:%M:%S') port disappeared, waiting for re-enumeration… (up to ${RECONNECT_WAIT}s)"
    wait_for_port "$RECONNECT_WAIT"
    rc=$?
  fi

  if (( rc != 0 )) || [[ -z "$PORT" ]]; then
    if (( SESSION == 0 )); then
      print "VERDICT: port never appeared"
      exit 0
    else
      print "$(date '+%H:%M:%S') gave up waiting for re-enumeration (reconnect-wait ${RECONNECT_WAIT}s or overall timeout reached)"
    fi
    break
  fi

  SESSION=$(( SESSION + 1 ))
  print "$(date '+%H:%M:%S') port found: $(redact_display "$PORT") (session $SESSION)"

  # --- identity + busy checks BEFORE opening anything (every session) ----
  # Neither of these opens the /dev node: `ioreg` queries IOKit's registry
  # (same mechanism scripts/usb-watch.sh already uses), and `lsof` inspects
  # open-file tables. Both are read-only host-side queries.
  vid_count=$(ioreg -p IOUSB -l -w 0 2>/dev/null | grep -c 'idVendor" = 12346')
  if (( vid_count == 0 )); then
    print -u2 "refusing to open $(redact_display "$PORT"): no Espressif (VID 0x303a) USB device is currently enumerated"
    print -u2 "(this does not prove $(redact_display "$PORT") itself is that device -- ioreg's port-to-node mapping"
    print -u2 " isn't reliably greppable per-node from the shell -- but it rules out an unrelated,"
    print -u2 " non-Espressif CDC node matching the bare glob)"
    exit 1
  fi
  if command -v lsof >/dev/null 2>&1; then
    busy_pid="$(lsof -t -- "$PORT" 2>/dev/null | head -n 1)"
    if [[ -n "$busy_pid" ]]; then
      print -u2 "refusing to open $(redact_display "$PORT"): already held by pid $busy_pid (lsof -t \"$(redact_display "$PORT")\")"
      print -u2 "if that is an in-progress esptool/flash session, DO NOT proceed."
      exit 1
    fi
  fi

  if (( SESSION == 1 )); then
    if (( ! ASSUME_YES )); then
      print "About to open: $(redact_display "$PORT")"
      print "  At least one Espressif (VID 0x303a) USB device is enumerated, and no other"
      print "  process currently holds this node open."
      if (( ! ONCE )); then
        print "  Follow mode: if the port later disappears and a matching node reappears"
        print "  (device reset / re-enumeration), it will be reopened automatically —"
        print "  this confirmation is only asked for this, the first open of the run."
      fi
      print -n "Proceed? [y/N] "
      if [[ -t 0 ]]; then
        read -r reply
      else
        reply=""
      fi
      if [[ "$reply" != [yY] && "$reply" != [yY][eE][sS] ]]; then
        print "aborted"
        exit 1
      fi
    fi
  else
    print "$(date '+%H:%M:%S') re-opening $(redact_display "$PORT") automatically (subsequent reopens of the same node path do not prompt)"
  fi

  # --- open read-only and start the (backgrounded) reader ------------------
  # Read-only open BEFORE stty, exactly as specified: `<`, never `>` or `<>`.
  exec 3<"$PORT"

  if [[ -c "$PORT" ]]; then
    # real serial device: configure raw mode, no echo, no flow control.
    # Baud is required syntax for stty but irrelevant for a USB-CDC ACM port.
    # clocal: don't block/hang waiting for a carrier-detect signal.
    stty -f "$PORT" raw -echo -ixon -ixoff 115200 clocal 2>/dev/null
  else
    print "  (not a character device — skipping stty, e.g. FIFO test path)"
  fi

  if (( SESSION > 1 )); then
    print "===== RE-OPENED $PORT at $(date '+%H:%M:%S') (session $SESSION) =====" >> "$BOOT_LOG"
  fi

  cat <&3 >> "$BOOT_LOG" 2>/dev/null &
  READER_PID=$!

  print "$(date '+%H:%M:%S') capturing... (reader pid $READER_PID, session $SESSION)"

  # --- wait for the node to disappear, or overall timeout, or Ctrl-C -------
  while (( ! INTERRUPTED )); do
    now=$EPOCHREALTIME
    if (( now - script_start >= TIMEOUT )); then
      print "$(date '+%H:%M:%S') timeout (${TIMEOUT}s) reached; stopping capture"
      TIMED_OUT=1
      break
    fi
    if [[ ! -e "$PORT" ]]; then
      print "$(date '+%H:%M:%S') port disappeared: $(redact_display "$PORT")"
      break
    fi
    sleep 0.1
  done

  if (( INTERRUPTED )); then
    print "$(date '+%H:%M:%S') interrupted; stopping capture"
  fi

  # stop the reader for this session now (cleanup on EXIT will also do
  # this, but we want the file settled before the next session or
  # post-processing runs).
  if [[ -n "$READER_PID" ]] && kill -0 "$READER_PID" 2>/dev/null; then
    kill "$READER_PID" 2>/dev/null
    wait "$READER_PID" 2>/dev/null
  fi
  exec 3<&- 2>/dev/null

  if (( INTERRUPTED )); then
    break
  fi
  if (( TIMED_OUT )); then
    break
  fi
  if (( ONCE )); then
    break
  fi
  # follow mode, port disappeared naturally, time remains: loop back around
  # and wait (up to --reconnect-wait) for a matching node to reappear.
done

# --- post-process: strip ANSI colour escapes (covers ALL sessions, since --
# --- BOOT_LOG was only truncated once, before the loop, and every session
# --- appended to it) ---------------------------------------------------------
perl -pe 's/\x1b\[[0-9;]*[a-zA-Z]//g; s/\r//g' "$BOOT_LOG" > "$CLEAN_LOG" 2>/dev/null

# --- redaction pass: strip Wi-Fi SSIDs, secrets, and full MACs from BOTH --
# --- the raw log and the clean log, in place. Value-class excludes CR/LF, --
# --- ESC (so a value in the raw log stops before its trailing ANSI reset --
# --- code instead of swallowing it), and quote/comma delimiters (so a --
# --- quoted or comma-terminated value like SSID='foo', Usage=65 is bounded --
# --- correctly). SAFETY.md §4: Wi-Fi SSIDs/passwords/tokens extracted from --
# --- the device must never be committed. ------------------------------------
REDACT_PERL=$(cat <<'PERL_EOF'
my $n = 0;
$n += s/((?:Loaded\s+last\s+(?:connected\s+)?SSID|SSID|ssid)\s*[:=]\s*'?)([^\r\n\x1b'",]+)/$1<SSID-REDACTED>/gi;
$n += s/(Connecting\s+to\s+)([^\r\n\x1b'",]+)/$1<SSID-REDACTED>/gi;
$n += s/(\bAP:\s*)([^\r\n\x1b'",]+)/$1<SSID-REDACTED>/gi;
$n += s/((?:password|passphrase|passwd|psk|token)\s*[:=]\s*"?)([^\r\n\x1b'",]+)/$1<REDACTED>/gi;
$n += s/\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){2}):(?:[0-9A-Fa-f]{2}:){2}[0-9A-Fa-f]{2}\b/$1:xx:xx:xx/g;
# macOS's /dev/cu.usbmodem<serial> node name embeds the device's USB serial
# descriptor verbatim -- pre-0.1.1 firmware derived that descriptor from the
# raw station MAC (byok_usb_cdc.c), so the node name itself leaked the MAC
# into any capture or log line that mentions the port path (this is exactly
# how it showed up in captures/serial/20260903-024945-boot-log*.txt, before
# 0.1.1's non-MAC serial fix). Redact defensively regardless of firmware
# version or platform quirk: any "usbmodem" immediately followed by 8+ hex
# characters, in either raw or clean logs.
$n += s/\busbmodem[0-9A-Fa-f]{8,}/usbmodem<REDACTED>/g;
print STDERR "$n";
PERL_EOF
)


redact_log() {
  local f="$1"
  [[ -f "$f" ]] || { print -r -- 0; return; }
  local out
  out="$(perl -0777 -pi -e "$REDACT_PERL" "$f" 2>&1 1>/dev/null)"
  [[ "$out" == <-> ]] || out=0
  print -r -- "$out"
}

raw_redactions=$(redact_log "$BOOT_LOG")
clean_redactions=$(redact_log "$CLEAN_LOG")
total_redactions=$(( raw_redactions + clean_redactions ))
print "redaction pass: ${total_redactions} substitutions"

bytes=$(wc -c < "$CLEAN_LOG" 2>/dev/null | tr -d ' ')
lines=$(wc -l < "$CLEAN_LOG" 2>/dev/null | tr -d ' ')
[[ -z "$bytes" ]] && bytes=0
[[ -z "$lines" ]] && lines=0

print ""
print "===================================="
print "Sessions:       $SESSION"
print "Bytes captured: $bytes"
print "Lines captured: $lines"
print ""
print "--- first 5 lines ---"
head -n 5 "$CLEAN_LOG" 2>/dev/null
print ""
print "--- last 15 lines ---"
tail -n 15 "$CLEAN_LOG" 2>/dev/null
print ""

markers=(
  "Starting BYOK" "button" "Button" "console" "UP" "Reset check" "SD card"
  "Disk mode" "breadcrumb" "USB" "HID" "gclcd" "display" "partition" "boot"
  "rst:" "ESP-ROM" "Pico" "PICO" "wifi" "ble" "RE-OPENED"
)
print "--- marker grep ---"
for m in "${markers[@]}"; do
  hits="$(grep -n -F -- "$m" "$CLEAN_LOG" 2>/dev/null)"
  if [[ -n "$hits" ]]; then
    print "[$m]"
    print -r -- "$hits"
  fi
done

print ""
print "Raw log:    $BOOT_LOG"
print "Clean log:  $CLEAN_LOG"
print "USB events: $USB_EVENTS_LOG"
print ""

if (( SESSION == 0 )); then
  print "VERDICT: port never appeared"
elif (( bytes == 0 )); then
  print "VERDICT: no data received across $SESSION session(s) (port opened but silent)"
else
  print "VERDICT: captured $lines lines across $SESSION session(s)"
fi
