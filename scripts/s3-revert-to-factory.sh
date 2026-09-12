#!/bin/zsh
#
# s3-revert-to-factory.sh — the ONE sanctioned esptool WRITE against this
#                            device: erase ONLY the otadata partition so
#                            the stock bootloader falls through to the
#                            untouched factory app (original 1.1.0 build)
#
# WHAT THIS IS FOR
#   scripts/s3-backup-window.sh is read-only and refuses outright to build
#   any write/erase/efuse esptool command (see its own header). This script
#   is the single, deliberate exception carved out of that rule: it exists
#   to run exactly one erase — `erase_region 0x910000 0x2000`, the
#   otadata partition's exact, full, device-verified extent
#   (docs/flash-layout-and-updater.md §1, "The partition map, read from
#   the device") — and
#   nothing else. Erasing otadata reproduces this device's actual,
#   verified stock state (8 KiB of 0xFF) byte-for-byte; the ESP-IDF
#   bootloader then finds no valid ota_select entry and falls through to
#   `factory` (docs/bootloader-analysis.md §3.2,
#   bootloader_common_ota_select_invalid: ota_seq == 0xFFFFFFFF is
#   "invalid"). `factory` has never been written by anything in this
#   project — it is the original 1.1.0 build, verified byte-identical to
#   the 2026-09-02 flash dump. This is a revert of BOOT SELECTION, not a
#   restore of any image: no bytes of any app/bootloader/partition-table
#   are written by this script, ever.
#
#   Background and full reasoning: docs/recovery.md §4.3 and §4 (the
#   preferred-smallest-restore table), docs/usb-and-boot-modes.md,
#   docs/firmware.md §7's abort-path fallback,
#   docs/design-rationale.md D-003 / D-008 / D-014 / D-018 / D-019.
#
# GATE STATUS (docs/design-rationale.md D-003, as of 2026-09-03)
#   D-003 is an AND of two conjuncts, both now MET:
#     (a) verified backup:       MET 2026-09-02 (two-pass 16 MiB S3 flash
#                                 dump, sha256-matched both passes,
#                                 docs/flash-layout-and-updater.md)
#     (b) tested recovery path:  MET 2026-09-03 (D-016 Stage A SD-updater
#                                 rehearsal via the vendor's own stock
#                                 1.1.3 tar, docs/design-rationale.md D-018)
#   Neither conjunct being met widens what esptool itself is allowed to
#   do. Every esptool erase/write against this device remains separately
#   gated per D-008's "show-command-first" rule (docs/design-rationale.md D-018,
#   consequence 1): this script never runs without --yes, and always
#   prints the full plan and the exact command line for every step first
#   — --yes IS the owner's specific, fresh go-ahead for this exact
#   command, at this exact moment. This script does not, and cannot,
#   satisfy that gate on the owner's behalf.
#
# WHY THIS IS SAFE TO CARVE OUT AS AN EXCEPTION TO THE READ-ONLY RULE
#   - The region is a COMPILE-TIME CONSTANT (OTADATA_OFFSET_HEX /
#     OTADATA_SIZE_HEX below), not a CLI argument. There is no flag on
#     this script that changes it. Any argument that even looks like an
#     attempt to (--offset/--address/--size/--region/...) is rejected
#     outright, before anything else runs.
#   - Before erasing, the script independently re-derives the same region
#     by reading and parsing the device's OWN partition table at 0x8000
#     and locating its `otadata` entry (type=data/0x01, subtype=ota/0x00,
#     label "otadata"). If that entry's offset/size does not match the
#     hardcoded constants exactly, the script aborts WITHOUT erasing —
#     the hardcoded region is never trusted blind.
#   - The assembled esptool command array is scanned, defense-in-depth,
#     immediately before every invocation: any "write" or "efuse"
#     substring anywhere refuses the whole run; any "erase" substring is
#     permitted ONLY if it is exactly `$CMD_ERASE_REGION` followed by the
#     two hardcoded tokens, contiguously, and refuses otherwise. This is
#     the one place in this codebase that check is *not* an unconditional
#     "refuse always" (that is what s3-backup-window.sh's identical-in-
#     spirit check does) — here it is "refuse unless it is exactly the
#     one sanctioned call."
#   - `read_flash`/`chip_id` are the only other esptool verbs this script
#     ever constructs — never `write_flash`, `write_flash_status`,
#     `erase_flash` (whole-chip), or any `espefuse` command.
#
# STEPS
#   1. chip-id, --before default-reset --after no-reset: drives the chip
#      into ROM download mode over the ~8 s (our firmware) / ~4 s (vendor
#      firmware) USB-Serial-JTAG window, or immediately if console mode
#      (persistent) is already active. See docs/usb-and-boot-modes.md.
#   2. read-flash 0x910000 0x2000 -> otadata-before.bin. sha256 recorded.
#      Decoded with the same seq/label/ota_state/crc logic as
#      docs/flash-layout-and-updater.md / docs/bootloader-analysis.md §2
#      (esp_ota_select_entry_t: u32 ota_seq, 20-byte label, u32 ota_state,
#      u32 crc; crc = zlib.crc32(pack('<I', ota_seq), 0xFFFFFFFF), which
#      is exactly esp_rom_crc32_le(0xFFFFFFFF, &ota_seq, 4) --
#      docs/bootloader-analysis.md §4/§5, verified against its own worked
#      example, ota_seq=1 -> crc=0x4743989a).
#   3. read-flash 0x8000 0x1000 -> partition-table.bin; parse it (same
#      32-byte esp_partition_info_t entry format as
#      s3-backup-window.sh's built-in decoder: magic 0xAA50 LE, type,
#      subtype, offset, size, 16-byte label, flags); find the entry with
#      type=data(0x01)/subtype=ota(0x00)/label="otadata"; ABORT (no erase)
#      if its offset/size don't exactly equal the hardcoded constants.
#      Only then: erase-region 0x910000 0x2000 -- the ONLY write-class
#      command this script ever runs.
#   4. read-flash 0x910000 0x2000 -> otadata-after.bin; verify every byte
#      is 0xFF; print PASS/FAIL.
#   5. Leave the chip in download mode (default) with instructions to
#      unplug USB and power off/on, OR (--reset-after) run one final
#      no-op chip-id with --after hard-reset.
#
# AFTER THIS SCRIPT RUNS
#   The device's NEXT cold boot runs whatever is currently on the SD
#   card. If BYOK-Mod firmware is what boots, this alone restores stock
#   boot selection (factory = original 1.1.0). If the VENDOR firmware is
#   what boots instead, its own stock updater will run the installer
#   against /Updates/BYOK.tar on whatever card is inserted at that
#   moment (docs/recovery.md "SD updater path", A1 CONFIRMED) -- so the
#   owner must choose deliberately what is on that card (the vendor's own
#   stock tar, this project's tar, or neither) BEFORE powering back on.
#   This script prints that choice explicitly in Step 5 and does not make
#   it for the owner.
#
# OUTPUT
#   backups/original/revert-<date>/          (otadata-before.bin,
#                                              otadata-after.bin,
#                                              partition-table.bin,
#                                              sha256 + decode summaries)
#                                              -- gitignored (backups/)
#   captures/serial/<ts>-esptool-revert.txt   -- full session log,
#                                              MAC- and usbmodem-node-
#                                              redacted before this script
#                                              exits (same redaction
#                                              approach as
#                                              serial-listen.sh)
#
# USAGE
#   ./scripts/s3-revert-to-factory.sh                       # DRY RUN
#   ./scripts/s3-revert-to-factory.sh --yes
#   ./scripts/s3-revert-to-factory.sh --yes --timeout 8
#   ./scripts/s3-revert-to-factory.sh --yes --reset-after
#
# REQUIRES: zsh, python3, esptool (`python3 -m pip install --user esptool`),
# git (gitignore confirmation), shasum, perl (log redaction). No sudo. No
# installs performed by this script. No `--port` is ever hardcoded to a
# real device path in this repository -- the owner supplies it (or the
# default glob resolves it) at the moment of use.

emulate -L zsh
unsetopt NOMATCH
zmodload zsh/datetime 2>/dev/null

# --- resolve ROOT relative to this script's location ------------------------
SCRIPT_DIR="${0:A:h}"
ROOT="${SCRIPT_DIR:h}"
CAPTURE_DIR="$ROOT/captures/serial"
BACKUPS_ROOT="$ROOT/backups/original"

# ------------------------------------------------------------------------
# THE compile-time constant: otadata's exact, full, device-verified
# extent. Not a CLI argument anywhere in this script -- see header.
# ------------------------------------------------------------------------
readonly OTADATA_OFFSET_HEX="0x910000"
readonly OTADATA_SIZE_HEX="0x2000"
readonly OTADATA_SIZE_DEC=8192
readonly PARTTABLE_OFFSET_HEX="0x8000"
readonly PARTTABLE_READ_SIZE_HEX="0x1000"

# ------------------------------------------------------------------------
# usage
# ------------------------------------------------------------------------
usage() {
  cat <<'EOF'
Usage: s3-revert-to-factory.sh [options]

Erases ONLY the otadata partition (0x910000, length 0x2000 -- a compile-
time constant, not an argument) so the stock bootloader falls through to
the untouched factory app. This is the one esptool WRITE-class operation
sanctioned in this repository; scripts/s3-backup-window.sh remains
read-only. See this script's own header comment for the full plan,
safety reasoning, and docs/recovery.md §4.3 / docs/design-rationale.md D-003.

Options:
  --port PATTERN      Serial port path or glob (default: /dev/cu.usbmodem*)
  --timeout SECONDS   Max seconds to wait for the port to appear (default: 20)
  --reset-after       After Step 4's PASS, run one final no-op esptool
                      call with --after hard-reset to leave the app
                      running, instead of leaving the chip in download
                      mode (the default -- see Step 5 in the plan).
  --yes               Actually execute. Without it, the full plan and
                      every exact command line is printed and the script
                      exits 0 (DRY RUN) -- no port is opened, nothing is
                      touched.
  -h, --help          Show this help.

Refused outright, before anything else runs: any argument containing
"write" or "efuse" (case-insensitive), and any argument that looks like
an attempt to override the hardcoded otadata offset/size/region.
EOF
}

if [[ $# -eq 0 ]]; then
  : # no args is the normal DRY RUN invocation; fall through to parsing
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

# ------------------------------------------------------------------------
# safety gate 1: refuse write/efuse anywhere in argv, and refuse any
# argument that looks like an attempt to override the hardcoded region --
# BEFORE anything else runs, including argument parsing proper.
# ------------------------------------------------------------------------
refuse() {
  print -u2 "s3-revert-to-factory.sh: REFUSED — $1"
  exit 3
}

assert_no_forbidden_args() {
  local arg lower
  for arg in "$@"; do
    lower="${arg:l}"
    if [[ "$lower" == *write* || "$lower" == *efuse* ]]; then
      refuse "argument contains a forbidden term (write/efuse): '$arg'. This script never constructs a write_flash/write_flash_status/espefuse command."
    fi
    case "$lower" in
      --offset*|--address*|--addr*|--otadata*|--region*|--size*|--length*)
        refuse "'$arg' looks like an attempt to override the otadata offset/size. Those are compile-time constants ($OTADATA_OFFSET_HEX, $OTADATA_SIZE_HEX) hardcoded in this script and are never accepted as arguments."
        ;;
    esac
  done
}
assert_no_forbidden_args "$@"

PORT_ARG='/dev/cu.usbmodem*'
TIMEOUT=20
YES=0
RESET_AFTER=0

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
    --reset-after)
      RESET_AFTER=1
      shift
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
      print -u2 "s3-revert-to-factory.sh: unknown argument: $1"
      usage
      exit 2
      ;;
  esac
done

# ------------------------------------------------------------------------
# esptool subcommand/flag syntax detection (v4 underscore vs v5 dash) --
# same approach as scripts/s3-backup-window.sh, extended with erase-region.
# ------------------------------------------------------------------------
detect_esptool() {
  if ! command -v python3 >/dev/null 2>&1; then
    print -u2 "s3-revert-to-factory.sh: python3 not found on PATH."
    exit 1
  fi

  local version_raw
  version_raw="$(python3 -m esptool version 2>&1)"
  if [[ $? -ne 0 ]]; then
    print -u2 "s3-revert-to-factory.sh: 'python3 -m esptool version' failed:"
    print -u2 "$version_raw"
    print -u2 "Install with: python3 -m pip install --user esptool"
    exit 1
  fi
  ESPTOOL_VERSION_STR="${${(f)version_raw}[1]}"

  ESPTOOL_HELP="$(python3 -m esptool --help 2>&1)"
  if print -r -- "$ESPTOOL_HELP" | grep -q -- 'chip-id'; then
    ESPTOOL_SYNTAX="dash (v5+)"
    CMD_CHIP_ID="chip-id"
    CMD_READ_FLASH="read-flash"
    CMD_ERASE_REGION="erase-region"
    BEFORE_DEFAULT="default-reset"
    BEFORE_NO="no-reset"
    AFTER_NO="no-reset"
    AFTER_HARD="hard-reset"
  elif print -r -- "$ESPTOOL_HELP" | grep -q -- 'chip_id'; then
    ESPTOOL_SYNTAX="underscore (v4.x)"
    CMD_CHIP_ID="chip_id"
    CMD_READ_FLASH="read_flash"
    CMD_ERASE_REGION="erase_region"
    BEFORE_DEFAULT="default_reset"
    BEFORE_NO="no_reset"
    AFTER_NO="no_reset"
    AFTER_HARD="hard_reset"
  else
    print -u2 "s3-revert-to-factory.sh: could not detect esptool subcommand spelling from --help output:"
    print -u2 "$ESPTOOL_HELP"
    exit 1
  fi
}
detect_esptool

# ------------------------------------------------------------------------
# wait for the serial node to appear (poll every 0.05s, up to $2 seconds)
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

refuse_if_port_busy() {
  local port="$1"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi
  local busy_pid
  busy_pid="$(lsof -t -- "$port" 2>/dev/null | head -n 1)"
  if [[ -n "$busy_pid" ]]; then
    print -u2 "ERROR: $port is already held open by pid $busy_pid (lsof -t \"$port\")."
    print -u2 "       If that is an in-progress esptool/flash session, DO NOT proceed."
    return 1
  fi
  return 0
}

# ------------------------------------------------------------------------
# redact_display <string> -- same rule as serial-listen.sh's
# redact_display(): strips a usbmodem node's embedded hex serial before
# echoing a port path to the terminal.
# ------------------------------------------------------------------------
redact_display() {
  print -r -- "$1" | perl -pe 's/\busbmodem[0-9A-Fa-f]{8,}/usbmodem<REDACTED>/g'
}

# ------------------------------------------------------------------------
# confirm a path is covered by .gitignore before writing anything under it
# ------------------------------------------------------------------------
confirm_gitignored() {
  local target_path="$1"
  if git -C "$ROOT" check-ignore -q "$target_path" 2>/dev/null; then
    print "confirmed gitignored: ${target_path#$ROOT/}"
  else
    print -u2 "s3-revert-to-factory.sh: REFUSING — $target_path is not covered by .gitignore."
    print -u2 "Not writing flash-derived binaries somewhere that could be committed."
    exit 4
  fi
}

# ------------------------------------------------------------------------
# assert_command_safe <esptool argv...>
#   Defense-in-depth scan of the ASSEMBLED command, immediately before
#   every invocation (dry run or real). "write"/"efuse" anywhere refuses
#   outright. "erase" anywhere is permitted ONLY as the exact, contiguous
#   sequence: $CMD_ERASE_REGION $OTADATA_OFFSET_HEX $OTADATA_SIZE_HEX --
#   any other erase verb, or the right verb with different args, refuses.
# ------------------------------------------------------------------------
assert_command_safe() {
  local -a cmd=("$@")
  local i n=${#cmd} word lower
  for (( i = 1; i <= n; i++ )); do
    word="${cmd[$i]}"
    lower="${word:l}"
    if [[ "$lower" == *write* || "$lower" == *efuse* ]]; then
      refuse "assembled esptool command contains a forbidden term: '$word'"
    fi
    if [[ "$lower" == *erase* ]]; then
      if [[ "$word" != "$CMD_ERASE_REGION" ]]; then
        refuse "assembled esptool command contains a forbidden erase verb: '$word' (only '$CMD_ERASE_REGION $OTADATA_OFFSET_HEX $OTADATA_SIZE_HEX' is ever permitted)"
      fi
      if (( i + 2 > n )) || [[ "${cmd[$((i+1))]}" != "$OTADATA_OFFSET_HEX" || "${cmd[$((i+2))]}" != "$OTADATA_SIZE_HEX" ]]; then
        refuse "assembled '$CMD_ERASE_REGION' call has non-hardcoded arguments (got '${cmd[$((i+1))]:-<missing>} ${cmd[$((i+2))]:-<missing>}'; only '$OTADATA_OFFSET_HEX $OTADATA_SIZE_HEX' is ever permitted)"
      fi
    fi
  done
}

# ------------------------------------------------------------------------
# run_esptool_step <description> <logfile> <esptool argv...>
# ------------------------------------------------------------------------
run_esptool_step() {
  local desc="$1" logfile="$2"
  shift 2
  local -a cmd=("$@")
  assert_command_safe "${cmd[@]}"

  print ""
  print -r -- "--- $desc ---"
  print "\$ ${cmd[*]}"

  if (( ! YES )); then
    print "DRY RUN (pass --yes to execute)"
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
  else
    print "OK ($desc), ${elapsed}s"
  fi
  return $rc
}

# ------------------------------------------------------------------------
# decode_otadata <bin-file>  -- prints seq/label/ota_state/crc per sector
# ------------------------------------------------------------------------
decode_otadata() {
  local file="$1"
  python3 - "$file" <<'PYEOF'
import struct
import sys
import zlib

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

STATE_NAMES = {
    0x00000000: "NEW",
    0x00000001: "PENDING_VERIFY",
    0x00000002: "VALID",
    0x00000003: "INVALID",
    0x00000004: "ABORTED",
    0xFFFFFFFF: "UNDEFINED",
}

for i, off in enumerate((0x0000, 0x1000)):
    entry = data[off:off + 32]
    if len(entry) < 32:
        print("sector %d @ 0x%04x: <truncated, only %d bytes>" % (i, off, len(entry)))
        continue
    seq, label_raw, state, crc = struct.unpack_from("<I20sII", entry, 0)
    label = label_raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    expected_crc = zlib.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    crc_valid = crc == expected_crc
    bootloader_invalid = (seq == 0xFFFFFFFF) or (state in (0x00000003, 0x00000004))
    state_name = STATE_NAMES.get(state, "0x%08x" % state)
    print(
        "sector %d @ 0x%04x: seq=0x%08x label=%r ota_state=0x%08x (%s) "
        "crc=0x%08x (expected 0x%08x, %s) -> bootloader treats this entry as %s"
        % (
            i, off, seq, label, state, state_name, crc, expected_crc,
            "valid" if crc_valid else "MISMATCH",
            "INVALID" if bootloader_invalid else "valid",
        )
    )
PYEOF
}

# ------------------------------------------------------------------------
# verify_otadata_matches_partition_table <parttable-bin>
#   Parses the 32-byte esp_partition_info_t entries (same format as
#   s3-backup-window.sh's built-in decoder), finds type=data(0x01)/
#   subtype=ota(0x00)/label="otadata", and exits nonzero if its
#   offset/size do not exactly equal the hardcoded constants.
# ------------------------------------------------------------------------
verify_otadata_matches_partition_table() {
  local file="$1"
  python3 - "$file" "$OTADATA_OFFSET_HEX" "$OTADATA_SIZE_HEX" <<'PYEOF'
import struct
import sys

path, expected_offset_hex, expected_size_hex = sys.argv[1:4]
expected_offset = int(expected_offset_hex, 16)
expected_size = int(expected_size_hex, 16)

with open(path, "rb") as f:
    data = f.read()

found = None
off = 0
while off + 32 <= len(data):
    entry = data[off:off + 32]
    magic = struct.unpack_from("<H", entry, 0)[0]
    if entry == b"\xff" * 32 or magic == 0xFFFF:
        break
    if magic == 0xEBEB:
        off += 32
        continue
    if magic != 0x50AA:
        print("(stopping partition-table scan: unexpected magic 0x%04x at 0x%x)" % (magic, off))
        break
    ptype, psub = struct.unpack_from("<BB", entry, 2)
    poff, psize = struct.unpack_from("<II", entry, 4)
    label = entry[12:28].split(b"\x00", 1)[0].decode("ascii", "replace")
    if ptype == 0x01 and psub == 0x00 and label == "otadata":
        found = (poff, psize)
        break
    off += 32

if found is None:
    print("ERROR: no partition-table entry with type=data(0x01)/subtype=ota(0x00)/label='otadata' found", file=sys.stderr)
    sys.exit(1)

poff, psize = found
print("partition table says otadata: offset=0x%06x size=0x%06x label=otadata" % (poff, psize))
if poff != expected_offset or psize != expected_size:
    print(
        "ABORT: partition-table otadata entry (offset=0x%06x size=0x%06x) does NOT match "
        "this script's hardcoded region (offset=%s size=%s) -- refusing to erase"
        % (poff, psize, expected_offset_hex, expected_size_hex),
        file=sys.stderr,
    )
    sys.exit(1)

print("OK: partition-table otadata entry matches the hardcoded region exactly")
PYEOF
}

# ------------------------------------------------------------------------
# verify_all_ff <bin-file> <expected-size-dec>
# ------------------------------------------------------------------------
verify_all_ff() {
  local file="$1" expected_size="$2"
  python3 - "$file" "$expected_size" <<'PYEOF'
import sys

path, expected_size = sys.argv[1], int(sys.argv[2])
with open(path, "rb") as f:
    data = f.read()

non_ff = sum(1 for b in data if b != 0xFF)
if len(data) != expected_size:
    print("FAIL: read %d bytes, expected %d" % (len(data), expected_size))
    sys.exit(1)
if non_ff != 0:
    print("FAIL: %d of %d bytes are not 0xFF" % (non_ff, len(data)))
    sys.exit(1)
print("PASS: all %d bytes are 0xFF" % len(data))
PYEOF
}

# ------------------------------------------------------------------------
# redact_log <file> -- same approach as serial-listen.sh: MAC addresses
# and usbmodem<hex> node names, in place.
# ------------------------------------------------------------------------
REDACT_PERL=$(cat <<'PERL_EOF'
my $n = 0;
$n += s/\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){2}):(?:[0-9A-Fa-f]{2}:){2}[0-9A-Fa-f]{2}\b/$1:xx:xx:xx/g;
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

# ------------------------------------------------------------------------
# main
# ------------------------------------------------------------------------
DATE="$(date +%Y%m%d)"
TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$BACKUPS_ROOT/revert-$DATE"
LOGFILE="$CAPTURE_DIR/${TS}-esptool-revert.txt"
OTADATA_BEFORE="$OUT_DIR/otadata-before.bin"
OTADATA_AFTER="$OUT_DIR/otadata-after.bin"
PARTTABLE_BIN="$OUT_DIR/partition-table.bin"
PORT="$PORT_ARG"

print "s3-revert-to-factory.sh"
print "  esptool:       $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
print "  region:        otadata @ $OTADATA_OFFSET_HEX, length $OTADATA_SIZE_HEX (compile-time constant)"
print "  port:          $(redact_display "$PORT_ARG")  (poll 0.05s, timeout ${TIMEOUT}s)"
print "  output dir:    ${OUT_DIR#$ROOT/}  (gitignored)"
print "  log file:      ${LOGFILE#$ROOT/}"
print "  reset-after:   $(( RESET_AFTER )) (0 = leave in download mode, 1 = --after hard-reset as final step)"
print "  D-003 gate:    backup verified 2026-09-02; SD recovery rehearsed 2026-09-03"
print "                 (esptool erase/write stays separately gated per D-008 --"
print "                 --yes below IS that specific, fresh go-ahead)"
if (( YES )); then
  print "  mode:          EXECUTE (--yes given)"
else
  print "  mode:          DRY RUN (pass --yes to execute)"
fi

print ""
print "PLAN"
print "  Step 1: enter download mode"
print "  Step 2: read otadata (before), sha256 + decode"
print "  Step 3: read partition table @ $PARTTABLE_OFFSET_HEX, verify its otadata entry"
print "          matches $OTADATA_OFFSET_HEX/$OTADATA_SIZE_HEX exactly, THEN erase otadata"
print "          (the ONLY write-class command this script ever runs)"
print "  Step 4: read otadata (after), verify every byte is 0xFF -> PASS/FAIL"
print "  Step 5: leave the chip in download mode (default) or, with --reset-after,"
print "          run one final no-op command with --after hard-reset"

if (( YES )); then
  confirm_gitignored "$BACKUPS_ROOT"
  mkdir -p "$OUT_DIR" "$CAPTURE_DIR"
  {
    print "s3-revert-to-factory.sh — $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    print "esptool: $ESPTOOL_VERSION_STR ($ESPTOOL_SYNTAX syntax)"
    print "region: otadata @ $OTADATA_OFFSET_HEX length $OTADATA_SIZE_HEX"
  } > "$LOGFILE"

  print ""
  print "waiting for $(redact_display "$PORT_ARG") ..."
  found="$(wait_for_port "$PORT_ARG" "$TIMEOUT")"
  if [[ -z "$found" ]]; then
    print -u2 "ERROR: no matching port appeared within ${TIMEOUT}s. Nothing was executed."
    print "ERROR: no matching port appeared within ${TIMEOUT}s. Nothing was executed." >> "$LOGFILE"
    redact_log "$LOGFILE" >/dev/null
    exit 1
  fi
  PORT="$found"
  print "port found: $(redact_display "$PORT")"
  if ! refuse_if_port_busy "$PORT"; then
    redact_log "$LOGFILE" >/dev/null
    exit 1
  fi
fi

rc_total=0

# --- Step 1: enter download mode ---
run_esptool_step "Step 1: enter download mode (chip-id)" "$LOGFILE" \
  python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_DEFAULT" --after "$AFTER_NO" "$CMD_CHIP_ID"
if (( $? != 0 )); then
  if (( YES )); then
    print -u2 "Step 1 failed -- aborting before touching otadata. See $LOGFILE."
    redact_log "$LOGFILE" >/dev/null
  fi
  exit 1
fi

# --- Step 2: read otadata (before) ---
run_esptool_step "Step 2: read otadata (before)" "$LOGFILE" \
  python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" "$OTADATA_OFFSET_HEX" "$OTADATA_SIZE_HEX" "$OTADATA_BEFORE"
if (( $? != 0 )); then
  if (( YES )); then
    print -u2 "Step 2 failed -- aborting before touching otadata. See $LOGFILE."
    redact_log "$LOGFILE" >/dev/null
  fi
  exit 1
fi

if (( YES )); then
  h_before="$(shasum -a 256 "$OTADATA_BEFORE" | awk '{print $1}')"
  print "otadata-before.bin sha256: $h_before"
  print "otadata-before.bin sha256: $h_before" >> "$LOGFILE"
  print "decoding otadata (before)..."
  decode_otadata "$OTADATA_BEFORE" | tee -a "$LOGFILE"
fi

# --- Step 3a: read partition table and verify the otadata entry ---
run_esptool_step "Step 3a: read partition table (verify otadata entry before erasing)" "$LOGFILE" \
  python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" "$PARTTABLE_OFFSET_HEX" "$PARTTABLE_READ_SIZE_HEX" "$PARTTABLE_BIN"
if (( $? != 0 )); then
  if (( YES )); then
    print -u2 "Step 3a failed -- aborting before touching otadata. See $LOGFILE."
    redact_log "$LOGFILE" >/dev/null
  fi
  exit 1
fi

if (( YES )); then
  print ""
  print "verifying otadata region against the device's own partition table..."
  if ! verify_otadata_matches_partition_table "$PARTTABLE_BIN" | tee -a "$LOGFILE"; then
    print -u2 "ABORTED: partition-table verification failed -- otadata was NOT erased. Chip is still in download mode. See $LOGFILE."
    redact_log "$LOGFILE" >/dev/null
    exit 1
  fi
fi

# --- Step 3b: erase otadata -- THE ONLY write-class command ---
run_esptool_step "Step 3b: erase otadata (the sanctioned write)" "$LOGFILE" \
  python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_ERASE_REGION" "$OTADATA_OFFSET_HEX" "$OTADATA_SIZE_HEX"
rc=$?
(( rc != 0 )) && rc_total=1
if (( YES && rc != 0 )); then
  print -u2 "Step 3b (erase) reported a nonzero exit -- check $LOGFILE carefully before proceeding."
  print -u2 "Chip is still in download mode; do not reboot until Step 4 has been checked."
fi

# --- Step 4: read otadata (after), verify all 0xFF ---
run_esptool_step "Step 4: read otadata (after) — verify erase" "$LOGFILE" \
  python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_NO" "$CMD_READ_FLASH" "$OTADATA_OFFSET_HEX" "$OTADATA_SIZE_HEX" "$OTADATA_AFTER"
if (( $? != 0 )); then
  rc_total=1
  if (( YES )); then
    print -u2 "Step 4 (read-back) failed -- cannot verify the erase. Chip is still in download mode. See $LOGFILE."
  fi
fi

if (( YES )); then
  h_after="$(shasum -a 256 "$OTADATA_AFTER" 2>/dev/null | awk '{print $1}')"
  [[ -n "$h_after" ]] && { print "otadata-after.bin sha256: $h_after"; print "otadata-after.bin sha256: $h_after" >> "$LOGFILE"; }
  print ""
  if verify_all_ff "$OTADATA_AFTER" "$OTADATA_SIZE_DEC" | tee -a "$LOGFILE"; then
    :
  else
    rc_total=1
  fi
fi

# --- Step 5: leave download mode, or hand back control ---
if (( RESET_AFTER )); then
  run_esptool_step "Step 5: leave download mode (--after hard-reset, no-op)" "$LOGFILE" \
    python3 -m esptool --port "$PORT" --chip esp32s3 --before "$BEFORE_NO" --after "$AFTER_HARD" "$CMD_CHIP_ID"
  print ""
  print "Step 5: --reset-after was given -- the device is cold-booting now."
else
  print ""
  print "Step 5: chip left in ROM download mode on purpose (default; pass --reset-after"
  print "        to have this script issue the final hard-reset itself)."
  print "        To finish: unplug the USB cable, then power the device fully off and"
  print "        back on with its own power button (not RESET-S3-only -- see"
  print "        docs/recovery.md 'Operational notes': a bare RESET leaves the PICO and"
  print "        display controller un-reset)."
fi
print ""
print "IMPORTANT — the owner must decide what boots next, deliberately:"
print "  - If BYOK-Mod firmware is what boots, this otadata erase alone restores stock"
print "    boot selection (factory = the original, untouched 1.1.0 build)."
print "  - If the VENDOR firmware is what boots instead, its own stock updater will run"
print "    the installer against whatever /Updates/BYOK.tar happens to be on the SD card"
print "    at that moment (docs/recovery.md 'SD updater path'). Check or remove the"
print "    card's /Updates contents BEFORE powering back on if that is not desired."

if (( YES )); then
  redactions="$(redact_log "$LOGFILE")"
  print ""
  print "log: ${LOGFILE#$ROOT/} (${redactions} redaction substitutions)"
else
  print ""
  print "DRY RUN complete — nothing executed, no port was touched, no files were written."
fi

exit $rc_total
