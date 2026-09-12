#!/bin/zsh
#
# usb-watch.sh — watch macOS USB/log state while plugging in the BYOK device
#
# WHAT THIS IS FOR
#   Run this in ONE terminal, then physically plug the BYOK (ESP32-S3) into
#   the Mac in another window/by hand. It reports, in plain words:
#     - whether an Espressif USB-Serial-JTAG device (VID 0x303A, PID 0x1001)
#       is ever seen enumerating on the USB bus,
#     - whether a /dev/cu.usbmodem* node gets published for it, and
#     - how long the device stays attached before it disconnects (if it does).
#   At the end it prints a plain-language VERDICT (A/B/C/D) summarizing what
#   happened, based on how long the device stayed attached and whether a
#   serial node showed up.
#
# USAGE
#   ./scripts/usb-watch.sh                 # run until Ctrl-C
#   ./scripts/usb-watch.sh --timeout 30     # run for at most 30 seconds
#
#   Run it, THEN plug in the device. Watch the printed lines. Press Ctrl-C
#   when you're done (or just let --timeout expire).
#
# OUTPUT
#   - Live status lines printed to this terminal (ATTACHED / SERIAL NODE /
#     DETACHED / VERDICT).
#   - A full `log stream` capture at:
#       captures/usb/<timestamp>-usb-watch-log.txt
#   - A one-time `ioreg` snapshot taken the first moment attach is detected:
#       captures/usb/<timestamp>-usb-watch-ioreg.txt
#
# SAFETY NOTES
#   - This script is host-only and READ-ONLY with respect to the device. It
#     never writes to, opens, or sends any byte to the device or to any
#     /dev/cu.* node — it only lists device nodes (`ls`), queries the IORegistry
#     (`ioreg`), and reads the unified log (`/usr/bin/log stream`). It never
#     runs esptool, never opens a serial connection, never enters download
#     mode, and never touches efuses/secure-boot/NVS/bootloader.
#   - `log` is shadowed by a zsh function/builtin in some shells, so this
#     script always calls `/usr/bin/log` explicitly, never a bare `log`.
#   - Physical button presses on the device are performed by the owner, not
#     this script. As a standing hardware-safety reminder: do NOT hold the
#     front button together with a back button for 10 seconds (that is a
#     known/suspected recovery or reset combination on this class of
#     device) unless that is the specific, deliberate experiment being run
#     with a verified recovery path in place. This script does not press
#     any buttons and cannot cause that combination to happen — it is a
#     reminder for the human at the keyboard.
#   - Press Ctrl-C at any time to stop; the background `log stream` process
#     is always cleaned up on exit (normal, timeout, or Ctrl-C).
#
# REQUIRES: zsh (for $EPOCHREALTIME float timestamps), ioreg, log, ls — all
# standard on macOS. No installs, no sudo, no serial port ever opened.

emulate -L zsh
unsetopt NOMATCH
zmodload zsh/datetime 2>/dev/null

# --- resolve ROOT relative to this script's location -----------------------
SCRIPT_DIR="${0:A:h}"
ROOT="${SCRIPT_DIR:h}"
CAPTURE_DIR="$ROOT/captures/usb"
mkdir -p "$CAPTURE_DIR"

# --- args --------------------------------------------------------------
TIMEOUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    *)
      print -u2 "usb-watch.sh: unknown argument: $1"
      exit 2
      ;;
  esac
done

TS="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$CAPTURE_DIR/${TS}-usb-watch-log.txt"
IOREG_FILE="$CAPTURE_DIR/${TS}-usb-watch-ioreg.txt"

print "usb-watch.sh starting"
print "  ROOT:        $ROOT"
print "  log capture: $LOG_FILE"
if [[ -n "$TIMEOUT" ]]; then
  print "  timeout:     ${TIMEOUT}s"
else
  print "  timeout:     none (Ctrl-C to stop)"
fi
print "  Plug the BYOK device in now (or it's already in)."
print ""

# --- background log stream (always via /usr/bin/log, never bare `log`) -----
/usr/bin/log stream --style compact --predicate 'eventMessage CONTAINS[c] "303a" OR eventMessage CONTAINS[c] "JTAG/serial" OR eventMessage CONTAINS[c] "usbmodem" OR eventMessage CONTAINS[c] "IOSerialBSDClient" OR eventMessage CONTAINS[c] "terminateDevice"' > "$LOG_FILE" 2>&1 &
LOG_PID=$!

INTERRUPTED=0
TRAPINT() {
  INTERRUPTED=1
  return 0
}

cleanup() {
  if kill -0 "$LOG_PID" 2>/dev/null; then
    kill "$LOG_PID" 2>/dev/null
    wait "$LOG_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

# --- state -----------------------------------------------------------------
attached=0
serial=0
first_attach_time=0
attach_time=0
first_attach_seen=0
serial_ever=0
serial_node_path=""
ioreg_saved=0
last_attach_duration=0
attach_cycles=0

script_start=$EPOCHREALTIME

while (( ! INTERRUPTED )); do
  now=$EPOCHREALTIME

  if [[ -n "$TIMEOUT" ]]; then
    elapsed_total=$(( now - script_start ))
    if (( elapsed_total >= TIMEOUT )); then
      break
    fi
  fi

  vid_count=$(ioreg -p IOUSB -l -w 0 2>/dev/null | grep -c 'idVendor" = 12346')
  node="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)"

  new_attached=0
  (( vid_count > 0 )) && new_attached=1

  new_serial=0
  [[ -n "$node" ]] && new_serial=1

  if (( new_attached != attached )); then
    if (( new_attached == 1 )); then
      attach_time=$now
      attach_cycles=$(( attach_cycles + 1 ))
      if (( first_attach_seen == 0 )); then
        first_attach_seen=1
        first_attach_time=$now
      fi
      rel=$(( now - first_attach_time ))
      relf=$(printf "%.1f" $rel)
      print "$(date '+%H:%M:%S') ATTACHED: Espressif device present (t=+${relf}s)"

      if (( ioreg_saved == 0 )); then
        ioreg -r -c IOUSBHostDevice -l -w 0 > "$IOREG_FILE" 2>/dev/null
        ioreg_saved=1
      fi
    else
      dur=$(( now - attach_time ))
      last_attach_duration=$dur
      durf=$(printf "%.1f" $dur)
      print "$(date '+%H:%M:%S') DETACHED after ${durf}s"
    fi
    attached=$new_attached
  fi

  if (( new_serial != serial )); then
    if (( new_serial == 1 )); then
      serial_ever=1
      serial_node_path="$node"
      rel=$(( first_attach_seen ? now - first_attach_time : 0 ))
      relf=$(printf "%.1f" $rel)
      print "$(date '+%H:%M:%S') SERIAL NODE: ${node} published (t=+${relf}s)"
    fi
    serial=$new_serial
  fi

  sleep 0.2
done

now_exit=$EPOCHREALTIME

if (( attached == 1 )); then
  # still attached when the script stopped watching (timeout/Ctrl-C) —
  # treat "how long it stayed attached" as attached-so-far.
  last_attach_duration=$(( now_exit - attach_time ))
  still_attached=1
else
  still_attached=0
fi

print ""
print "===================================="
print "VERDICT:"

if (( first_attach_seen == 0 )); then
  print "(A) No Espressif device seen"
else
  durf=$(printf "%.1f" $last_attach_duration)
  if (( ! serial_ever )) && (( last_attach_duration < 8 )); then
    print "(B) Device appeared but died after ~${durf} s and no serial node: normal mode ran (button not registered / wrong button)"
  elif (( serial_ever )) && (( last_attach_duration >= 8 )); then
    print "(C) Device stayed attached >= 8 s (measured ~${durf} s) and serial node published: console mode reached — this is the button"
  else
    if (( serial_ever )); then
      serial_word="was"
    else
      serial_word="was NOT"
    fi
    if (( still_attached )); then
      attached_word="yes"
    else
      attached_word="no"
    fi
    print "(D) Other: attached for ~${durf} s, serial node ${serial_word} published, still attached at exit: ${attached_word}, attach/detach cycles observed: ${attach_cycles}"
  fi
fi

if [[ -n "$serial_node_path" ]]; then
  print "Serial node observed: $serial_node_path"
else
  print "Serial node observed: (none)"
fi

print "Log capture:   $LOG_FILE"
if (( ioreg_saved == 1 )); then
  print "ioreg snapshot: $IOREG_FILE"
fi
print "Note: this script never opened /dev/cu.* and sent nothing to the device."
