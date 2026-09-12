#!/bin/zsh
#
# s3-backup-window.sh — read-only esptool session over the BYOK's ~4.2s
#                        USB-Serial-JTAG (USJ) boot window
#
# WHAT THIS IS FOR
#   The BYOK's ESP32-S3 exposes USB-Serial-JTAG as /dev/cu.usbmodem101 for
#   ~4.2s after every boot/reset, then the app switches the shared USB PHY
#   to OTG and the node disappears. esptool's built-in USBJTAGSerialReset
#   strategy can drive the chip into ROM download mode over USJ via DTR/RTS
#   toggling alone (no GPIO0/BOOT pin) if invoked inside that window; once
#   in download mode the chip stays there — no app runs — as long as every
#   subsequent call uses `--after no-reset` (spelled `no_reset` on the
#   esptool version installed here — see SYNTAX DETECTION below). Full
#   background: docs/usb-and-boot-modes.md. Session plan/authorization gate
#   this script implements: docs/design-rationale.md D-008.
#
#   This script (a) waits for the node to appear, (b) immediately runs
#   READ-ONLY esptool commands against it, (c) logs everything to
#   captures/serial/. It never runs write_flash, erase_flash, erase_region,
#   write_flash_status, or any espefuse/secure-boot command — see SAFETY
#   GATES below.
#
# SUBCOMMANDS
#   identify           Wait for the port, then run chip-id, flash-id,
#                       read-flash-status, and a 64KB timed probe read of
#                       the bootloader region (0x0-0x10000). Prints elapsed
#                       time per step. Leaves the chip in download mode
#                       (--after no-reset throughout) so partition-table
#                       and dump can chain onto the same session without
#                       re-triggering a reset.
#
#   partition-table     Assumes the chip is already in download mode (i.e.
#                       run identify first, or the chip is otherwise known
#                       to still be in ROM download mode). Reads
#                       0x8000-0xC000 into partition-table.bin and decodes
#                       it (via esptool's own gen_esp32part module if the
#                       installed esptool ships one, else a built-in
#                       fallback decoder — see PARTITION TABLE DECODING).
#
#   dump                Reads the FULL flash TWICE (size auto-detected from
#                       flash-id, default 16MB / 0x1000000 if detection is
#                       unavailable) into backups/original/s3-flash-<date>/,
#                       sha256-compares the two passes, and prints
#                       PASS/FAIL. WARNING: esptool's ESP32-S3 read_flash
#                       over USJ has a currently-unresolved upstream
#                       performance bug (esptool#936) measured at roughly
#                       8-11.5 KB/s, i.e. ~25-35 minutes PER PASS at 16MB
#                       (~1-1.5 hours total for both passes). See
#                       docs/usb-and-boot-modes.md (e).
#
# SAFETY GATES (all unconditional)
#   - DRY RUN BY DEFAULT: every subcommand only PRINTS the exact esptool
#     command line(s) it would run and exits 0. Nothing is executed, no
#     port is opened, no device is touched, unless the explicit --yes flag
#     is given. This is how the script is tested with no device present.
#   - Any argument to this script containing "write", "erase", or "efuse"
#     (case-insensitive substring match) is refused immediately, before
#     anything else runs. The same check is repeated on every assembled
#     esptool command array as defense-in-depth. This script never
#     constructs a write_flash / erase_flash / erase_region /
#     write_flash_status / espefuse command — there is no code path here
#     that could.
#   - partition-table and dump refuse to run unless
#     backups/original/ is confirmed gitignored (`git check-ignore`) —
#     flash-derived binaries are never written somewhere that could
#     accidentally get committed.
#
# SYNTAX DETECTION (esptool v4 vs v5)
#   esptool v4.x spells subcommands and --before/--after values with
#   underscores (chip_id, flash_id, read_flash, read_flash_status,
#   default_reset, no_reset, hard_reset). esptool v5.x switched to dashes
#   (chip-id, flash-id, read-flash, read-flash-status, default-reset,
#   no-reset, hard-reset). This script detects which is installed by
#   grepping `python3 -m esptool --help` for "chip-id" vs "chip_id" and
#   picks the matching spelling for every command it prints/runs — do not
#   hardcode either form when reading this script's output.
#
# PARTITION TABLE DECODING
#   `python3 -m esptool.gen_esp32part` is tried first (present on some
#   esptool installs, e.g. when installed via ESP-IDF). If it's not
#   importable (as on a plain `pip install esptool`, where this module is
#   not packaged), this script falls back to a small built-in Python
#   decoder that parses the standard 32-byte ESP-IDF partition-table entry
#   format directly (magic 0xAA50 little-endian, MD5-checksum entries
#   marked 0xEBEB skipped, 0xFFFF/all-0xFF terminates the table).
#
# USAGE
#   ./scripts/s3-backup-window.sh identify                    # dry run
#   ./scripts/s3-backup-window.sh identify --yes               # execute
#   ./scripts/s3-backup-window.sh identify --yes --timeout 8
#   ./scripts/s3-backup-window.sh partition-table --yes
#   ./scripts/s3-backup-window.sh dump --yes
#   ./scripts/s3-backup-window.sh dump --port /dev/cu.usbmodem101 --yes
#
# REQUIRES: zsh, python3, esptool (`python3 -m pip install --user esptool`),
# git (for the gitignore confirmation), shasum. No sudo. No non-esptool
# installs performed by this script.

emulate -L zsh
unsetopt NOMATCH
zmodload zsh/datetime 2>/dev/null

# --- resolve ROOT relative to this script's location ------------------------
SCRIPT_DIR="${0:A:h}"
ROOT="${SCRIPT_DIR:h}"
CAPTURE_DIR="$ROOT/captures/serial"
BACKUPS_ROOT="$ROOT/backups/original"

# ------------------------------------------------------------------------
# usage
# ------------------------------------------------------------------------
usage() {
  cat <<'EOF'
Usage: s3-backup-window.sh <identify|partition-table|dump> [options]

Subcommands:
  identify           Wait for the USJ port, then run chip-id, flash-id,
                      read-flash-status, and a 64KB timed probe read.
  partition-table     Read 0x8000-0xC000 and decode the partition table.
                      Assumes the chip is already in download mode.
  dump                Read the full flash TWICE and sha256-compare the
                      two passes (PASS/FAIL). ~25-35 min per pass over USJ.

Options:
  --port PATTERN      Serial port path or glob (default: /dev/cu.usbmodem*)
  --timeout SECONDS   Max seconds to wait for the port to appear (default: 15)
  --yes               Actually execute esptool. Without it, every
                       subcommand only prints the exact command line(s) it
                       would run (DRY RUN) and exits 0 — no port is opened.
  -h, --help          Show this help.

This script is READ-ONLY: any argument containing "write", "erase", or
"efuse" is refused outright, and it never assembles a write/erase/efuse
esptool command. See the header comment in this file for full details.
EOF
}

if [[ $# -eq 0 || "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

# ------------------------------------------------------------------------
# safety gate: refuse write/erase/efuse anywhere in argv, before anything
# else runs
# ------------------------------------------------------------------------
assert_no_forbidden_args() {
  local arg lower
  for arg in "$@"; do
    lower="${arg:l}"
    if [[ "$lower" == *write* || "$lower" == *erase* || "$lower" == *efuse* ]]; then
      print -u2 "s3-backup-window.sh: REFUSED — argument contains a forbidden term (write/erase/efuse): '$arg'"
      print -u2 "This script is read-only only; write/erase/efuse operations are never permitted here."
      exit 3
    fi
  done
}
assert_no_forbidden_args "$@"

SUBCOMMAND="$1"
shift

PORT_ARG='/dev/cu.usbmodem*'
TIMEOUT=15
YES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT_ARG="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    --yes)
      YES=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      print -u2 "s3-backup-window.sh: unknown argument: $1"
      usage
      exit 2
      ;;
  esac
done

# ------------------------------------------------------------------------
# esptool subcommand/flag syntax detection (v4 underscore vs v5 dash)
# ------------------------------------------------------------------------
detect_esptool() {
  if ! command -v python3 >/dev/null 2>&1; then
    print -u2 "s3-backup-window.sh: python3 not found on PATH."
    exit 1
  fi

  local version_raw
  version_raw="$(python3 -m esptool version 2>&1)"
  if [[ $? -ne 0 ]]; then
    print -u2 "s3-backup-window.sh: 'python3 -m esptool version' failed:"
    print -u2 "$version_raw"
    print -u2 "Install with: python3 -m pip install --user esptool"
    exit 1
  fi
  # esptool's `version` subcommand prints two lines ("esptool.py vX.Y.Z"
  # then a bare "X.Y.Z"); keep just the first for a one-line summary.
  ESPTOOL_VERSION_STR="${${(f)version_raw}[1]}"

  ESPTOOL_HELP="$(python3 -m esptool --help 2>&1)"
  if print -r -- "$ESPTOOL_HELP" | grep -q -- 'chip-id'; then
    ESPTOOL_SYNTAX="dash (v5+)"
    CMD_CHIP_ID="chip-id"
    CMD_FLASH_ID="flash-id"
    CMD_READ_FLASH="read-flash"
    CMD_READ_FLASH_STATUS="read-flash-status"
    BEFORE_DEFAULT="default-reset"
    BEFORE_NO="no-reset"
    AFTER_NO="no-reset"
  elif print -r -- "$ESPTOOL_HELP" | grep -q -- 'chip_id'; then
    ESPTOOL_SYNTAX="underscore (v4.x)"
    CMD_CHIP_ID="chip_id"
    CMD_FLASH_ID="flash_id"
    CMD_READ_FLASH="read_flash"
    CMD_READ_FLASH_STATUS="read_flash_status"
    BEFORE_DEFAULT="default_reset"
    BEFORE_NO="no_reset"
    AFTER_NO="no_reset"
  else
    print -u2 "s3-backup-window.sh: could not detect esptool subcommand spelling from --help output:"
    print -u2 "$ESPTOOL_HELP"
    exit 1
  fi
}
detect_esptool

# ------------------------------------------------------------------------
# wait for the serial node to appear (poll every 0.05s, up to $1 seconds)
# prints the matched path on stdout and returns 0, or prints nothing and
# returns 1 on timeout.
# ------------------------------------------------------------------------
wait_for_port() {
  local pattern="$1" timeout="$2"
  local start=$EPOCHREALTIME
  local -a matches
  while true; do
    local now=$EPOCHREALTIME
    if (( now - start >= timeout )); then
      return 1
    fi
    if [[ -e "$pattern" ]]; then
      print -r -- "$pattern"
      return 0
    fi
    matches=(${~pattern}(N))
    if (( ${#matches} > 0 )); then
      print -r -- "$matches[1]"
      return 0
    fi
    sleep 0.05
  done
}

# ------------------------------------------------------------------------
# refuse_if_port_busy <port>
#   `lsof`-only check (no device I/O) that another process already has
#   this exact node open -- most importantly an in-progress esptool
#   session. Prints an error and returns 1 if so; silent, returns 0
#   otherwise. Called right after wait_for_port resolves a concrete path,
#   before any esptool invocation (dry-run or real).
# ------------------------------------------------------------------------
refuse_if_port_busy() {
  local port="$1"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi
  local busy_pid
  busy_pid="$(lsof -t -- "$port" 2>/dev/null | head -n 1)"
  if [[ -n "$busy_pid" ]]; then
    print -u2 "ERROR: $port is already held open by pid $busy_pid (lsof -t \"$port\")."
    print -u2 "       If that is an in-progress esptool/flash session, DO NOT proceed --"
    print -u2 "       refusing rather than risk two processes touching the same node."
    return 1
  fi
  return 0
}

# ------------------------------------------------------------------------
# on a nonzero exit code, grep the log tail for known esptool failure
# strings and print the matching plain-language hint
# ------------------------------------------------------------------------
print_failure_hint() {
  local logfile="$1"
  local tail_text
  tail_text="$(tail -n 60 "$logfile" 2>/dev/null)"
  if print -r -- "$tail_text" | grep -qi "wrong boot mode\|invalid head of packet"; then
    print "HINT: esptool reported a 'wrong boot mode' / framing error — the app was"
    print "      running (chip never entered download mode). Wait for the next"
    print "      boot/reset and retry inside the ~4.2s window."
  elif print -r -- "$tail_text" | grep -qi "failed to connect\|no serial data received\|timed out waiting for packet\|could not open\|port is busy or doesn't exist\|inappropriate ioctl"; then
    print "HINT: esptool could not connect in time — the device likely left the"
    print "      ~4.2s USJ window before esptool's reset sequence landed, or the"
    print "      port disappeared. Reset/replug the device and retry immediately."
  else
    print "HINT: unrecognized failure — inspect $logfile for the full esptool output."
  fi
}

# ------------------------------------------------------------------------
# run_esptool_step <description> <logfile> <esptool argv...>
#   - always echoes the exact command line
#   - dry run (YES=0): prints "DRY RUN", appends the plan to the logfile,
#     returns 0, never executes anything
#   - real run (YES=1): defense-in-depth forbidden-term scan on the
#     assembled argv, times the call, tees output to the logfile, prints
#     elapsed time and exit code, prints a failure hint on nonzero exit
# ------------------------------------------------------------------------
run_esptool_step() {
  local desc="$1" logfile="$2"
  shift 2
  local -a cmd=("$@")

  local word lower
  for word in "${cmd[@]}"; do
    lower="${word:l}"
    if [[ "$lower" == *write* || "$lower" == *erase* || "$lower" == *efuse* ]]; then
      print -u2 "s3-backup-window.sh: REFUSED — assembled esptool command contains a forbidden term: '$word'"
      exit 3
    fi
  done

  print ""
  print -r -- "--- $desc ---"
  print "\$ ${cmd[*]}"

  if (( ! YES )); then
    print "DRY RUN (pass --yes to execute)"
    {
      print -r -- "--- $desc ---"
      print "\$ ${cmd[*]}"
      print "DRY RUN (pass --yes to execute)"
    } >> "$logfile"
    return 0
  fi

  {
    print -r -- "--- $desc ---"
    print "\$ ${cmd[*]}"
    print "started: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  } >> "$logfile"

  local t0=$EPOCHREALTIME
  "${cmd[@]}" >>"$logfile" 2>&1
  local rc=$?
  local t1=$EPOCHREALTIME
  local elapsed=$(( t1 - t0 ))

  printf "elapsed: %.2fs\n" "$elapsed"
  printf "elapsed: %.2fs\nexit code: %d\n" "$elapsed" "$rc" >> "$logfile"

  if (( rc != 0 )); then
    print "FAILED ($desc), rc=$rc, ${elapsed}s"
    print_failure_hint "$logfile"
  else
    print "OK ($desc), ${elapsed}s"
  fi
  return $rc
}

# ------------------------------------------------------------------------
# confirm a path is covered by .gitignore before writing flash-derived
# binaries under it; refuses (exit 4) if not
# ------------------------------------------------------------------------
confirm_gitignored() {
  # NOTE: deliberately not named "path" — that's zsh's special array tied
  # to $PATH, and `local path=...` silently breaks command lookup (git
  # included) for the rest of this function's scope.
  local target_path="$1"
  if git -C "$ROOT" check-ignore -q "$target_path" 2>/dev/null; then
    print "confirmed gitignored: ${target_path#$ROOT/}"
  else
    print -u2 "s3-backup-window.sh: REFUSING — $target_path is not covered by .gitignore."
    print -u2 "Not writing flash-derived binaries somewhere that could be committed."
    exit 4
  fi
}

# ------------------------------------------------------------------------
# decode_partition_table <bin-file>
#   tries esptool's own gen_esp32part module first, falls back to a
#   built-in parser of the standard 32-byte partition-entry format
# ------------------------------------------------------------------------
decode_partition_table() {
  local file="$1"
  local out
  out="$(python3 -m esptool.gen_esp32part "$file" 2>&1)"
  if (( $? == 0 )); then
    print -r -- "$out"
    return 0
  fi

  print "(python3 -m esptool.gen_esp32part not available in this esptool install — using built-in decoder)"
  python3 - "$file" <<'PYEOF'
import struct
import sys

TYPE_NAMES = {0x00: "app", 0x01: "data"}
APP_SUBTYPE = {0x00: "factory", 0x20: "test"}
for _i in range(16):
    APP_SUBTYPE[0x10 + _i] = "ota_%d" % _i
DATA_SUBTYPE = {
    0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
    0x04: "nvs_keys", 0x05: "efuse_em", 0x06: "undefined",
    0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs",
}


def subtype_name(t, st):
    if t == 0x00:
        return APP_SUBTYPE.get(st, "0x%02x" % st)
    if t == 0x01:
        return DATA_SUBTYPE.get(st, "0x%02x" % st)
    return "0x%02x" % st


path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

print("%-16s %-6s %-10s %-10s %-14s %s" % ("Name", "Type", "SubType", "Offset", "Size", "Flags"))
off = 0
count = 0
while off + 32 <= len(data):
    entry = data[off:off + 32]
    if entry == b"\xff" * 32:
        break
    magic = struct.unpack_from("<H", entry, 0)[0]
    if magic == 0xFFFF:
        break
    if magic == 0xEBEB:
        # MD5-checksum entry, not a partition
        off += 32
        continue
    if magic != 0x50AA:
        print("  (stopping: unexpected magic 0x%04x at offset 0x%x)" % (magic, off))
        break
    ptype, psub = struct.unpack_from("<BB", entry, 2)
    poff, psize = struct.unpack_from("<II", entry, 4)
    label = entry[12:28].split(b"\x00", 1)[0].decode("ascii", "replace")
    flags = struct.unpack_from("<I", entry, 28)[0]
    print(
        "%-16s %-6s %-10s 0x%06x   0x%06x (%d B)  0x%08x"
        % (label, TYPE_NAMES.get(ptype, hex(ptype)), subtype_name(ptype, psub), poff, psize, psize, flags)
    )
    count += 1
    off += 32

print("")
print("%d partition entries decoded from %s" % (count, path))
PYEOF
}

# ------------------------------------------------------------------------
# subcommand: identify
# ------------------------------------------------------------------------
cmd_identify() {
  local TS LOGFILE BIN_OUT_DIR PROBE_FILE PORT rc_total=0
  TS="$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$CAPTURE_DIR"
  LOGFILE="$CAPTURE_DIR/${TS}-esptool-identify.txt"
  BIN_OUT_DIR="$BACKUPS_ROOT/s3-identify-$TS"
  PROBE_FILE="$BIN_OUT_DIR/probe-64k.bin"
  PORT="$PORT_ARG"

  print "s3-backup-window.sh identify"
  print "  esptool:    $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
  print "  port:       $PORT_ARG  (poll 0.05s, timeout ${TIMEOUT}s)"
  print "  log file:   $LOGFILE"
  print "  probe file: $PROBE_FILE"
  if (( YES )); then
    print "  mode:       EXECUTE (--yes given)"
  else
    print "  mode:       DRY RUN (pass --yes to execute)"
  fi

  {
    print "s3-backup-window.sh identify — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    print "esptool: $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
  } > "$LOGFILE"

  if (( YES )); then
    confirm_gitignored "$BACKUPS_ROOT"
    mkdir -p "$BIN_OUT_DIR"
    print ""
    print "waiting for $PORT_ARG ..."
    local found
    found="$(wait_for_port "$PORT_ARG" "$TIMEOUT")"
    if [[ -z "$found" ]]; then
      print -u2 "ERROR: no matching port appeared within ${TIMEOUT}s."
      print -u2 "HINT: device left the ~4.2s window before esptool connected, or was"
      print -u2 "      never plugged in / reset. Reset or replug the device, then"
      print -u2 "      re-run this command immediately."
      print "ERROR: no matching port appeared within ${TIMEOUT}s." >> "$LOGFILE"
      return 1
    fi
    PORT="$found"
    print "port found: $PORT"
    if ! refuse_if_port_busy "$PORT"; then
      return 1
    fi
  fi

  run_esptool_step "chip-id" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_DEFAULT" --after "$AFTER_NO" "$CMD_CHIP_ID"
  (( $? != 0 )) && rc_total=1

  run_esptool_step "flash-id (already in download mode)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_FLASH_ID"
  (( $? != 0 )) && rc_total=1

  run_esptool_step "read-flash-status (read-only status-register read, --bytes 3)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH_STATUS" --bytes 3
  (( $? != 0 )) && rc_total=1

  run_esptool_step "read-flash 0x0 0x10000 (64KB timed probe, bootloader region)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" 0x0 0x10000 "$PROBE_FILE"
  (( $? != 0 )) && rc_total=1

  print ""
  print "log: $LOGFILE"
  if (( ! YES )); then
    print "DRY RUN complete — nothing executed, no port was touched."
  fi
  return $rc_total
}

# ------------------------------------------------------------------------
# subcommand: partition-table
# ------------------------------------------------------------------------
cmd_partition_table() {
  local TS LOGFILE BIN_OUT_DIR PT_FILE PORT rc
  TS="$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$CAPTURE_DIR"
  LOGFILE="$CAPTURE_DIR/${TS}-esptool-partition-table.txt"
  BIN_OUT_DIR="$BACKUPS_ROOT/s3-parttable-$TS"
  PT_FILE="$BIN_OUT_DIR/partition-table.bin"
  PORT="$PORT_ARG"

  print "s3-backup-window.sh partition-table"
  print "  esptool:    $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
  print "  port:       $PORT_ARG"
  print "  assumes chip already in download mode (e.g. run 'identify' first)"
  print "  output:     $PT_FILE"
  if (( YES )); then
    print "  mode:       EXECUTE (--yes given)"
  else
    print "  mode:       DRY RUN (pass --yes to execute)"
  fi

  { print "s3-backup-window.sh partition-table — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"; } > "$LOGFILE"

  if (( YES )); then
    confirm_gitignored "$BACKUPS_ROOT"
    mkdir -p "$BIN_OUT_DIR"
    print ""
    print "waiting for $PORT_ARG ..."
    local found
    found="$(wait_for_port "$PORT_ARG" "$TIMEOUT")"
    if [[ -z "$found" ]]; then
      print -u2 "ERROR: no matching port appeared within ${TIMEOUT}s."
      return 1
    fi
    PORT="$found"
    print "port found: $PORT"
    if ! refuse_if_port_busy "$PORT"; then
      return 1
    fi
  fi

  run_esptool_step "read-flash 0x8000 0x4000 (partition-table region)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" 0x8000 0x4000 "$PT_FILE"
  rc=$?

  if (( ! YES )); then
    print ""
    print "DRY RUN complete — nothing executed, no port was touched."
    return 0
  fi

  if (( rc != 0 )); then
    print -u2 "read-flash failed (rc=$rc); cannot decode."
    return $rc
  fi

  print ""
  print "decoding partition table..."
  decode_partition_table "$PT_FILE" | tee -a "$LOGFILE"
}

# ------------------------------------------------------------------------
# subcommand: dump
# ------------------------------------------------------------------------
cmd_dump() {
  local DATE TS OUT_DIR LOGFILE PORT
  DATE="$(date +%Y%m%d)"
  TS="$(date +%Y%m%d-%H%M%S)"
  OUT_DIR="$BACKUPS_ROOT/s3-flash-$DATE"
  mkdir -p "$CAPTURE_DIR"
  LOGFILE="$CAPTURE_DIR/${TS}-esptool-dump.txt"
  PORT="$PORT_ARG"

  print "s3-backup-window.sh dump"
  print "  esptool:    $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
  print "  port:       $PORT_ARG"
  print "  output dir: $OUT_DIR  (gitignored)"
  print "  WARNING: esptool ESP32-S3 read-flash over USJ has an unresolved"
  print "           upstream throughput bug (esptool#936), observed at"
  print "           ~8-11.5 KB/s -> roughly 25-35 minutes PER PASS at 16MB."
  print "           This subcommand reads TWO full passes: budget ~1-1.5"
  print "           hours total, unattended, before the PASS/FAIL compare."
  if (( YES )); then
    print "  mode:       EXECUTE (--yes given)"
  else
    print "  mode:       DRY RUN (pass --yes to execute)"
  fi

  { print "s3-backup-window.sh dump — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"; } > "$LOGFILE"

  if (( YES )); then
    confirm_gitignored "$BACKUPS_ROOT"
    mkdir -p "$OUT_DIR"
    print ""
    print "waiting for $PORT_ARG ..."
    local found
    found="$(wait_for_port "$PORT_ARG" "$TIMEOUT")"
    if [[ -z "$found" ]]; then
      print -u2 "ERROR: no matching port appeared within ${TIMEOUT}s."
      return 1
    fi
    PORT="$found"
    print "port found: $PORT"
    if ! refuse_if_port_busy "$PORT"; then
      return 1
    fi
  fi

  local FLASH_SIZE_BYTES=16777216
  if (( YES )); then
    run_esptool_step "flash-id (determine flash size; already in download mode)" "$LOGFILE" \
      python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_FLASH_ID"
    local detected_mb
    detected_mb="$(grep -i 'detected flash size' "$LOGFILE" | tail -1 | grep -oE '[0-9]+' | head -1)"
    if [[ -n "$detected_mb" ]]; then
      FLASH_SIZE_BYTES=$(( detected_mb * 1024 * 1024 ))
      print "detected flash size: ${detected_mb}MB"
    else
      print "could not parse flash size from flash-id output; defaulting to 16MB"
    fi
  else
    print ""
    print "(dry run — flash size not probed; planned command lines below assume"
    print " the default 16MB / 0x1000000)"
  fi

  local FLASH_SIZE_HEX SIZE_LABEL PASS1 PASS2
  FLASH_SIZE_HEX="$(printf '0x%X' $FLASH_SIZE_BYTES)"
  SIZE_LABEL="$(( FLASH_SIZE_BYTES / 1048576 ))MB"
  PASS1="$OUT_DIR/esp32-s3-full-${DATE}-${SIZE_LABEL}-pass1.bin"
  PASS2="$OUT_DIR/esp32-s3-full-${DATE}-${SIZE_LABEL}-pass2.bin"

  run_esptool_step "read-flash 0x0 $FLASH_SIZE_HEX pass 1 (~25-35 min)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" 0x0 "$FLASH_SIZE_HEX" "$PASS1"
  local rc1=$?

  run_esptool_step "read-flash 0x0 $FLASH_SIZE_HEX pass 2 (~25-35 min)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" 0x0 "$FLASH_SIZE_HEX" "$PASS2"
  local rc2=$?

  if (( ! YES )); then
    print ""
    print "DRY RUN complete — nothing executed, no port was touched."
    return 0
  fi

  if (( rc1 != 0 || rc2 != 0 )); then
    print -u2 "one or both read-flash passes failed; skipping sha256 compare."
    return 1
  fi

  print ""
  print "computing sha256..."
  local h1 h2
  h1="$(shasum -a 256 "$PASS1" | awk '{print $1}')"
  h2="$(shasum -a 256 "$PASS2" | awk '{print $1}')"
  print "pass1: $h1  $PASS1"
  print "pass2: $h2  $PASS2"
  {
    print "pass1 sha256: $h1  $PASS1"
    print "pass2 sha256: $h2  $PASS2"
  } >> "$LOGFILE"

  if [[ "$h1" == "$h2" ]]; then
    print "PASS: both passes match (sha256 $h1)"
    print "PASS: both passes match (sha256 $h1)" >> "$LOGFILE"
    return 0
  else
    print "FAIL: passes do not match — do NOT trust this as a verified backup"
    print "FAIL: passes do not match" >> "$LOGFILE"
    return 1
  fi
}

case "$SUBCOMMAND" in
  identify)
    cmd_identify
    ;;
  partition-table)
    cmd_partition_table
    ;;
  dump)
    cmd_dump
    ;;
  *)
    print -u2 "s3-backup-window.sh: unknown subcommand: $SUBCOMMAND"
    usage
    exit 2
    ;;
esac
exit $?
