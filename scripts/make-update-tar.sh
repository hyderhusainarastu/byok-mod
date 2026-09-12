#!/usr/bin/env bash
#
# make-update-tar.sh — build a stock-format BYOK.tar update package
#
# WHAT THIS IS FOR
#   The stock BYOK updater (S3 firmware) installs whatever tar it finds at
#   /Updates/BYOK.tar on the SD card, provided the tar contains exactly the
#   three members it expects. This script builds that tar on the host, from
#   either a custom BYOK.bin (our own S3 image) plus stock BYOK-Pico.bin /
#   assets.tar, or purely from a stock release (for a recovery rehearsal —
#   proving the packaging pipeline itself is byte-correct before it is ever
#   trusted with a non-stock image).
#
#   This script is 100% host-only. It never touches a serial device, never
#   runs esptool against a --port, never writes to an SD card, and never
#   asks the user to plug anything in. It only reads local files and writes
#   files under the directory passed to --out.
#
# USAGE
#   make-update-tar.sh --s3 <BYOK.bin> [--pico <BYOK-Pico.bin>] \
#       [--assets <assets.tar>] [--stock-from <release tar>] --out <dir>
#
#   make-update-tar.sh --stock-only [--stock-from <release tar>] --out <dir>
#       Re-packs the stock release's own three members through the exact
#       same staging/tar/verify pipeline, then asserts the result is
#       member-identical (same bytes, per member) to the stock release tar.
#       This is the "recovery rehearsal" artifact: if this doesn't come out
#       identical, the pipeline itself is not trustworthy for anything else.
#
# OPTIONS
#   --s3 PATH          Our S3 app image (BYOK.bin). Required unless --stock-only.
#   --pico PATH         Pico app image (BYOK-Pico.bin). Default: extracted from
#                        --stock-from (i.e. use the stock Pico image unchanged).
#                        A --pico that is not byte-identical to the stock member
#                        is REFUSED unless --i-know-this-pico-is-not-stock is also
#                        given (the PICO write is unrecoverable — see
#                        docs/flash-layout-and-updater.md §2.5 and §2.7) — and even then
#                        it must pass a basic size sanity check.
#   --i-know-this-pico-is-not-stock
#                        Required alongside a --pico that differs from the stock
#                        member's content. Without it, a non-stock --pico is a
#                        hard error, not a warning.
#   --no-pico             Omit BYOK-Pico.bin entirely: build a TWO-member tar
#                        (BYOK.bin, assets.tar). Recommended for the first install
#                        of our own firmware — see docs/flash-layout-and-updater.md §2.5
#                        and docs/design-rationale.md D-016 ("Stage A"), both of
#                        which defer the PICO write as the one unrecoverable step.
#                        Mutually exclusive with --pico and
#                        --i-know-this-pico-is-not-stock.
#   --assets PATH        Lua assets tar (assets.tar). Default: extracted from
#                        --stock-from (i.e. use the stock assets unchanged).
#   --stock-from PATH    A stock release tar to source stock members / defaults
#                        from. YOU SUPPLY THIS — no stock firmware, assets or
#                        update archive ships with this repository; take one off
#                        your own device or from the maker's own update channel.
#                        If omitted, the script looks for a single file matching
#                        backups/stock-releases/fw-*.tar under
#                        the repo root (a git-ignored path you can create
#                        yourself as a convenience) and errors out with
#                        instructions if that is missing or ambiguous. Verified
#                        against a sibling SHA256SUMS file unless
#                        --skip-stock-verify is given.
#   --skip-stock-verify   Skip verifying --stock-from against a sibling
#                        SHA256SUMS entry (needed for a deliberately unlisted
#                        tar, e.g. a not-yet-released vendor build).
#   --stock-only          Build the tar entirely from --stock-from's own members
#                        (no --s3/--pico/--assets allowed) and verify the result
#                        is member-identical to the original. See USAGE above.
#   --out DIR            Output directory. Required. Created if missing. Written:
#                          DIR/BYOK.tar, DIR/SHA256SUMS (BYOK.tar itself, plain
#                          shasum -c-able), DIR/MEMBER-SHA256SUMS (each tar
#                          member's content hash), DIR/MANIFEST.txt
#   --stamp STRING       Timestamp string to record in MANIFEST.txt. Default:
#                        the output of `date '+%Y-%m-%d %H:%M:%S %Z'` at run time.
#   -h, --help            Show this help and exit.
#
# OUTPUT ARCHIVE FORMAT
#   DIR/BYOK.tar is written with `tar --format ustar`, containing EXACTLY:
#     BYOK.bin, BYOK-Pico.bin, assets.tar     (in that order, bare names, no
#                                              "./" prefix, no PAX/GNU extended
#                                              headers). Verified three ways:
#     `tar -tvf`, the python3 `tarfile` module, and a raw per-header byte walk
#     checking the POSIX `ustar\0` / `00` magic+version fields directly (belt
#     and suspenders — `tarfile.format` is not a reliable read-time signal).
#   NOTE: this member ORDER (BYOK.bin, BYOK-Pico.bin, assets.tar) intentionally
#   differs from the vendor's own tars, which list BYOK.bin, assets.tar,
#   BYOK-Pico.bin (confirmed via `tar -tvf` on every archived release). The
#   updater only looks members up by name, not position, so this is safe —
#   see docs/firmware.md §6. Verification below compares
#   members by NAME and CONTENT, never by tar-level byte position.
#
# VALIDATION
#   The S3 image (--s3, or the stock BYOK.bin in --stock-only mode) is
#   validated with `python3 -m esptool image_info --version 2` (a local,
#   read-only static parse of the .bin file — no --port, no device contact):
#   it must be detected as chip ESP32-S3 with a valid checksum and a valid
#   validation hash, and its size must not exceed 0x300000 (3,145,728) bytes,
#   the size of a single OTA/factory app slot on this device (see
#   docs/flash-layout-and-updater.md §1, "The partition map, read from the
#   device").
#
# SAFETY
#   Read-only w.r.t. everything except the files this script itself writes
#   under --out (and a host-side scratch directory it cleans up on exit).
#   Never touches /dev/cu.*, /dev/tty.*, esptool --port, or any SD card.
#   Building BYOK.tar is NOT the same as installing it on the device — see
#   docs/firmware.md for the (separately gated) on-device steps.

set -euo pipefail

# --- helpers -----------------------------------------------------------

SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SLOT_SIZE=3145728   # 0x300000 — one OTA/factory app partition, device-verified

# No verified app-slot size exists for the PICO (ESP32 classic, BYOK-Pico.bin)
# -- its own partition table has never been read (docs/hardware.md,
# docs/recovery.md: UNKNOWN). This is a generous not-obviously-wrong sanity
# ceiling (~10x the largest Pico image on file, 784,672 B per
# docs/firmware.md), not a certified slot size -- it exists only to
# catch a wildly wrong file, not to promise the image will fit or flash.
PICO_SANITY_MAX=8388608

log()  { printf '%s\n' "$*"; }
hdr()  { printf '\n== %s ==\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    # Print the block comment above (between the shebang and `set -euo`) as help text.
    awk 'NR>1 && /^# ?/{sub(/^# ?/,""); print; next} NR>1 && !/^#/{exit}' "$SCRIPT_PATH"
}

sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }
size_of()   { wc -c < "$1" | tr -d ' '; }

WORKDIRS=()
cleanup() {
    local d
    if [[ "${#WORKDIRS[@]}" -gt 0 ]]; then
        for d in "${WORKDIRS[@]}"; do
            if [[ -d "$d" ]]; then
                rm -rf "$d"
            fi
        done
    fi
}
trap cleanup EXIT

mktempdir() {
    local d
    d="$(mktemp -d "${TMPDIR:-/tmp}/make-update-tar.XXXXXX")"
    WORKDIRS+=("$d")
    printf '%s\n' "$d"
}

# --- argument parsing ----------------------------------------------------

S3_ARG=""
PICO_ARG=""
ASSETS_ARG=""
STOCK_FROM=""
OUT_DIR=""
STOCK_ONLY=0
STAMP=""
NO_PICO=0
I_KNOW_PICO_NOT_STOCK=0
SKIP_STOCK_VERIFY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --s3)          S3_ARG="${2:?--s3 needs a path}"; shift 2 ;;
        --pico)        PICO_ARG="${2:?--pico needs a path}"; shift 2 ;;
        --assets)      ASSETS_ARG="${2:?--assets needs a path}"; shift 2 ;;
        --stock-from)  STOCK_FROM="${2:?--stock-from needs a path}"; shift 2 ;;
        --out)         OUT_DIR="${2:?--out needs a path}"; shift 2 ;;
        --stamp)       STAMP="${2:?--stamp needs a string}"; shift 2 ;;
        --stock-only)  STOCK_ONLY=1; shift ;;
        --no-pico)     NO_PICO=1; shift ;;
        --i-know-this-pico-is-not-stock) I_KNOW_PICO_NOT_STOCK=1; shift ;;
        --skip-stock-verify) SKIP_STOCK_VERIFY=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "unrecognized argument: $1 (see --help)" ;;
    esac
done

[[ -n "$OUT_DIR" ]] || die "--out is required (see --help)"

if [[ "$STOCK_ONLY" -eq 1 ]]; then
    [[ -z "$S3_ARG" && -z "$PICO_ARG" && -z "$ASSETS_ARG" && "$NO_PICO" -eq 0 ]] \
        || die "--stock-only cannot be combined with --s3/--pico/--assets/--no-pico — it repacks --stock-from's own members only"
else
    [[ -n "$S3_ARG" ]] || die "--s3 is required unless --stock-only is given (see --help)"
fi

if [[ "$NO_PICO" -eq 1 ]]; then
    [[ -z "$PICO_ARG" ]] || die "--no-pico cannot be combined with --pico"
    [[ "$I_KNOW_PICO_NOT_STOCK" -eq 0 ]] || die "--no-pico cannot be combined with --i-know-this-pico-is-not-stock (there is no Pico member to be non-stock)"
fi

[[ -n "$STAMP" ]] || STAMP="$(date '+%Y-%m-%d %H:%M:%S %Z')"

# --- resolve --stock-from -------------------------------------------------

if [[ -z "$STOCK_FROM" ]]; then
    RELEASES_DIR="$ROOT_DIR/backups/stock-releases"
    shopt -s nullglob
    candidates=( "$RELEASES_DIR"/fw-*.tar )
    shopt -u nullglob
    if [[ "${#candidates[@]}" -eq 0 ]]; then
        die "no stock release tar supplied, and no default found.

This script needs a stock update archive to take the BYOK-Pico.bin and
assets.tar members from. No stock firmware, assets or update archive ships
with this repository — you supply your own, taken off your own device or from
the maker's own update channel (see LEGAL.md section 3).

Either pass it explicitly:

    --stock-from /path/to/your/stock-update.tar

or drop it, named fw-<version>.tar, into the git-ignored convenience location
this script checks by default:

    $RELEASES_DIR/

Pair it with a sibling SHA256SUMS file so the archive can be verified, or pass
--skip-stock-verify to proceed without that check."
    fi
    if [[ "${#candidates[@]}" -ne 1 ]]; then
        die "ambiguous default stock release tar — ${#candidates[@]} files match fw-*.tar under $RELEASES_DIR; pass --stock-from explicitly. Candidates:
$(printf '  %s\n' "${candidates[@]}")"
    fi
    STOCK_FROM="${candidates[0]}"
fi
[[ -f "$STOCK_FROM" ]] || die "--stock-from file not found: $STOCK_FROM"

hdr "Stock source"
log "Using stock release tar: $STOCK_FROM"
STOCK_FROM_HASH="$(sha256_of "$STOCK_FROM")"
log "  sha256: $STOCK_FROM_HASH"

if [[ "$SKIP_STOCK_VERIFY" -eq 1 ]]; then
    log "  (--skip-stock-verify given: NOT checked against a sibling SHA256SUMS)"
else
    STOCK_FROM_DIR="$(cd "$(dirname "$STOCK_FROM")" && pwd)"
    STOCK_FROM_BASE="$(basename "$STOCK_FROM")"
    STOCK_SUMS_FILE="$STOCK_FROM_DIR/SHA256SUMS"
    if [[ ! -f "$STOCK_SUMS_FILE" ]]; then
        die "no sibling SHA256SUMS found next to --stock-from ($STOCK_SUMS_FILE) — cannot verify $STOCK_FROM_BASE against a known-good hash. Pass --skip-stock-verify to proceed anyway with a deliberately unlisted tar."
    fi
    STOCK_EXPECTED_LINE="$(grep -E "  \\Q$STOCK_FROM_BASE\\E\$" "$STOCK_SUMS_FILE" || true)"
    if [[ -z "$STOCK_EXPECTED_LINE" ]]; then
        die "$STOCK_FROM_BASE is not listed in $STOCK_SUMS_FILE — cannot verify it against a known-good hash. Pass --skip-stock-verify to proceed anyway with a deliberately unlisted tar."
    fi
    STOCK_EXPECTED_HASH="$(printf '%s\n' "$STOCK_EXPECTED_LINE" | awk '{print $1}')"
    if [[ "$STOCK_EXPECTED_HASH" != "$STOCK_FROM_HASH" ]]; then
        die "--stock-from ($STOCK_FROM) sha256 ($STOCK_FROM_HASH) does not match the value listed in $STOCK_SUMS_FILE ($STOCK_EXPECTED_HASH) — the source tar may be tampered or corrupted. Refusing to proceed. Pass --skip-stock-verify only if this mismatch is understood and deliberate."
    fi
    log "  verified against $STOCK_SUMS_FILE: OK"
fi

stock_listing="$(tar -tf "$STOCK_FROM")"
for want in BYOK.bin BYOK-Pico.bin assets.tar; do
    printf '%s\n' "$stock_listing" | grep -qx "$want" \
        || die "--stock-from ($STOCK_FROM) does not contain a member named '$want'. Listing was:
$stock_listing"
done

STOCK_DIR="$(mktempdir)"
tar -x -f "$STOCK_FROM" -C "$STOCK_DIR" BYOK.bin BYOK-Pico.bin assets.tar

# --- esptool image_info validation for an S3 app image --------------------

validate_s3_image() {
    local img="$1"
    [[ -f "$img" ]] || die "S3 image not found: $img"

    local sz
    sz="$(size_of "$img")"
    if [[ "$sz" -gt "$SLOT_SIZE" ]]; then
        die "S3 image $img is $sz bytes; exceeds the $SLOT_SIZE-byte (0x300000) OTA/factory app slot size — refusing to package it"
    fi

    local info rc=0
    info="$(python3 -m esptool image_info --version 2 "$img" 2>&1)" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        printf '%s\n' "$info" >&2
        die "esptool image_info exited $rc on $img"
    fi
    printf '%s\n' "$info" | grep -qE 'Detected image type:[[:space:]]*ESP32-S3' \
        || die "S3 image $img is not detected as chip ESP32-S3. esptool said:
$info"
    printf '%s\n' "$info" | grep -qE 'Checksum:.*\(valid\)' \
        || die "S3 image $img failed esptool's checksum check. esptool said:
$info"
    printf '%s\n' "$info" | grep -qE 'Validation hash:.*\(valid\)' \
        || die "S3 image $img failed esptool's validation-hash check. esptool said:
$info"

    printf '%s\n' "$info"
}

# --- esptool image_info validation for a Pico (ESP32 classic) app image ----
#
# Weaker guarantee than validate_s3_image: there is no verified app-slot size
# for the PICO to check against (see PICO_SANITY_MAX above), and the PICO has
# no known read-back or reflash recovery path if a bad image is installed
# (the S3-only vs PICO split -- docs/design-rationale.md D-015).
# This still catches the two cheapest, most damaging mistakes: a
# truncated/non-image file, and an image built for the wrong chip (e.g. an
# S3 image passed by accident under --pico).
validate_pico_image() {
    local img="$1"
    [[ -f "$img" ]] || die "Pico image not found: $img"

    local sz
    sz="$(size_of "$img")"
    if [[ "$sz" -eq 0 ]]; then
        die "Pico image $img is empty — refusing to package it"
    fi
    if [[ "$sz" -gt "$PICO_SANITY_MAX" ]]; then
        die "Pico image $img is $sz bytes; exceeds the $PICO_SANITY_MAX-byte sanity ceiling — refusing to package it"
    fi

    local info rc=0
    info="$(python3 -m esptool image_info --version 2 "$img" 2>&1)" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        printf '%s\n' "$info" >&2
        die "esptool image_info exited $rc on $img"
    fi
    # Anchored so plain "ESP32" doesn't also accept "ESP32-S3" as a substring
    # match — esptool prints the exact chip name and nothing else on this line.
    printf '%s\n' "$info" | grep -qE 'Detected image type:[[:space:]]*ESP32[[:space:]]*$' \
        || die "Pico image $img is not detected as chip ESP32 (classic) — the PICO's own chip
per docs/hardware.md/docs/architecture.md. esptool said:
$info"
    printf '%s\n' "$info" | grep -qE 'Checksum:.*\(valid\)' \
        || die "Pico image $img failed esptool's checksum check. esptool said:
$info"
    printf '%s\n' "$info" | grep -qE 'Validation hash:.*\(valid\)' \
        || die "Pico image $img failed esptool's validation-hash check. esptool said:
$info"

    printf '%s\n' "$info"
}

# --- minimal structural validation for the assets.tar member ---------------
#
# updateAssets (docs/flash-layout-and-updater.md) reads whichever members this tar
# happens to contain, so an empty or unreadable assets.tar wouldn't error
# loudly on-device the way a bad S3/Pico image would -- it would just quietly
# leave the stock Lua assets untouched. This is a minimal sanity check
# (readable tar, at least one member), not a validator of the assets' content.
validate_assets_tar() {
    local tarpath="$1"
    [[ -f "$tarpath" ]] || die "assets.tar not found: $tarpath"

    local sz
    sz="$(size_of "$tarpath")"
    if [[ "$sz" -eq 0 ]]; then
        die "assets.tar $tarpath is empty — refusing to package it"
    fi

    local listing rc=0
    listing="$(tar -tf "$tarpath" 2>&1)" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
        printf '%s\n' "$listing" >&2
        die "assets.tar $tarpath is not a readable tar archive (tar -tf failed)"
    fi
    if [[ -z "$listing" ]]; then
        die "assets.tar $tarpath contains no members — refusing to package it"
    fi
}

# --- resolve the three (or two, with --no-pico) source files for this run --

PICO_OMITTED=0
if [[ "$STOCK_ONLY" -eq 1 ]]; then
    MODE="stock-only (recovery rehearsal — repacks $STOCK_FROM's own members)"
    S3_SRC="$STOCK_DIR/BYOK.bin";        S3_DESC="$STOCK_FROM (member BYOK.bin, stock)"
    PICO_SRC="$STOCK_DIR/BYOK-Pico.bin"; PICO_DESC="$STOCK_FROM (member BYOK-Pico.bin, stock)"
    ASSETS_SRC="$STOCK_DIR/assets.tar";  ASSETS_DESC="$STOCK_FROM (member assets.tar, stock)"
elif [[ "$NO_PICO" -eq 1 ]]; then
    MODE="two-member (--no-pico: BYOK.bin + assets.tar only, no PICO write — see docs/flash-layout-and-updater.md §2.5)"
    S3_SRC="$S3_ARG"; S3_DESC="$S3_ARG (custom)"
    PICO_SRC=""; PICO_DESC="(omitted — --no-pico)"
    PICO_OMITTED=1

    if [[ -n "$ASSETS_ARG" ]]; then
        ASSETS_SRC="$ASSETS_ARG"; ASSETS_DESC="$ASSETS_ARG (custom)"
    else
        ASSETS_SRC="$STOCK_DIR/assets.tar"; ASSETS_DESC="$STOCK_FROM (member assets.tar, defaulted to stock)"
    fi
else
    S3_SRC="$S3_ARG"; S3_DESC="$S3_ARG (custom)"

    if [[ -n "$PICO_ARG" ]]; then
        PICO_SRC="$PICO_ARG"; PICO_DESC="$PICO_ARG (custom)"

        # A --pico that is not byte-identical to the stock member is the one
        # unrecoverable write in this whole pipeline (no PICO recovery tooling
        # exists — docs/flash-layout-and-updater.md §2.5/§2.7: it is the only
        # irreversible surface in the whole operation). Refuse
        # it outright unless the caller explicitly acknowledges that.
        [[ -f "$PICO_ARG" ]] || die "Pico image not found: $PICO_ARG"
        PICO_ARG_HASH="$(sha256_of "$PICO_ARG")"
        STOCK_PICO_HASH="$(sha256_of "$STOCK_DIR/BYOK-Pico.bin")"
        if [[ "$PICO_ARG_HASH" != "$STOCK_PICO_HASH" ]]; then
            if [[ "$I_KNOW_PICO_NOT_STOCK" -ne 1 ]]; then
                die "--pico ($PICO_ARG, sha256 $PICO_ARG_HASH) is NOT byte-identical to the stock
BYOK-Pico.bin (sha256 $STOCK_PICO_HASH) from $STOCK_FROM. The PICO write is the one
unrecoverable step in this pipeline (no PICO recovery tooling exists — see
docs/flash-layout-and-updater.md §2.5 and §2.7). Refusing to package a
non-stock PICO image without explicit acknowledgement.
  - To omit the PICO member entirely (recommended for a first install), use --no-pico instead.
  - To knowingly package a non-stock PICO image, re-run with
    --i-know-this-pico-is-not-stock in addition to --pico."
            fi
        fi
    else
        PICO_SRC="$STOCK_DIR/BYOK-Pico.bin"; PICO_DESC="$STOCK_FROM (member BYOK-Pico.bin, defaulted to stock)"
    fi

    if [[ -n "$ASSETS_ARG" ]]; then
        ASSETS_SRC="$ASSETS_ARG"; ASSETS_DESC="$ASSETS_ARG (custom)"
    else
        ASSETS_SRC="$STOCK_DIR/assets.tar"; ASSETS_DESC="$STOCK_FROM (member assets.tar, defaulted to stock)"
    fi

    if [[ "$PICO_DESC" == *"defaulted to stock"* && "$ASSETS_DESC" == *"defaulted to stock"* ]]; then
        MODE="mixed (custom S3 image + stock Pico/assets)"
    else
        MODE="custom (explicit S3, and explicit Pico and/or assets)"
    fi
fi

if [[ "$PICO_OMITTED" -ne 1 ]]; then
    [[ -f "$PICO_SRC" ]] || die "Pico image not found: $PICO_SRC"
fi
[[ -f "$ASSETS_SRC" ]] || die "assets.tar not found: $ASSETS_SRC"

hdr "Validating S3 image"
log "S3 image: $S3_DESC"
ESPTOOL_INFO="$(validate_s3_image "$S3_SRC")"
printf '%s\n' "$ESPTOOL_INFO" | sed 's/^/  /'

if [[ "$PICO_OMITTED" -eq 1 ]]; then
    hdr "Pico image"
    log "Pico image: (omitted — --no-pico). No BYOK-Pico.bin member will be written; the"
    log "PICO write is deferred as the one unrecoverable step in the pipeline."
    PICO_ESPTOOL_INFO="(omitted — --no-pico)"
else
    hdr "Validating Pico image"
    log "Pico image: $PICO_DESC"
    if [[ "$PICO_DESC" == *"(custom)"* ]]; then
        if [[ "$PICO_ARG_HASH" == "$STOCK_PICO_HASH" ]]; then
            log "  sha256 $PICO_ARG_HASH matches the stock member byte-for-byte — treated as stock."
        else
            log ""
            log "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            log "!! WARNING: --pico DOES NOT MATCH stock BYOK-Pico.bin.                       !!"
            log "!! This is the one unrecoverable write in the whole pipeline (no PICO        !!"
            log "!! recovery tooling exists on this project — see                              !!"
            log "!! docs/flash-layout-and-updater.md §2.5). Proceeding only because             !!"
            log "!! --i-know-this-pico-is-not-stock was explicitly given.                      !!"
            log "!!   ours:  $PICO_ARG_HASH"
            log "!!   stock: $STOCK_PICO_HASH"
            log "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
            log ""
        fi
    fi
    PICO_ESPTOOL_INFO="$(validate_pico_image "$PICO_SRC")"
    printf '%s\n' "$PICO_ESPTOOL_INFO" | sed 's/^/  /'
fi

hdr "Validating assets.tar"
log "assets.tar: $ASSETS_DESC"
validate_assets_tar "$ASSETS_SRC"
log "  OK (readable tar, non-empty member list)"

# --- stage the members under their exact required names --------------------

STAGE_DIR="$(mktempdir)"
cp "$S3_SRC"     "$STAGE_DIR/BYOK.bin"
if [[ "$PICO_OMITTED" -ne 1 ]]; then
    cp "$PICO_SRC" "$STAGE_DIR/BYOK-Pico.bin"
fi
cp "$ASSETS_SRC" "$STAGE_DIR/assets.tar"

if [[ "$PICO_OMITTED" -eq 1 ]]; then
    TAR_MEMBERS=(BYOK.bin assets.tar)
else
    TAR_MEMBERS=(BYOK.bin BYOK-Pico.bin assets.tar)
fi

# --- build the archive ------------------------------------------------------

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
OUT_TAR="$OUT_DIR/BYOK.tar"

# COPYFILE_DISABLE keeps macOS from smuggling AppleDouble (._*) resource-fork
# entries into the archive, which would otherwise force PAX extended headers.
COPYFILE_DISABLE=1 tar --format ustar -cf "$OUT_TAR" -C "$STAGE_DIR" "${TAR_MEMBERS[@]}"

hdr "Archive listing (tar -tvf)"
tar -tvf "$OUT_TAR"

# --- verify: pure USTAR, exact members, exact order, no extended headers ---

VERIFY_DIR="$(mktempdir)"
VERIFY_PY="$VERIFY_DIR/verify_tar.py"
cat > "$VERIFY_PY" <<'PYEOF'
import sys, tarfile

def fail(msg):
    print("VERIFY-FAIL: " + msg)
    sys.exit(1)

path = sys.argv[1]
expected = sys.argv[2:]

with open(path, 'rb') as f:
    data = f.read()

if len(data) % 512 != 0:
    fail("archive size is not a multiple of 512 bytes")

zero_block = b'\x00' * 512
offset = 0
seen_names = []
while offset < len(data):
    block = data[offset:offset + 512]
    if block == zero_block:
        offset += 512
        continue
    name = block[0:100].split(b'\x00', 1)[0].decode('ascii', 'replace')
    typeflag = block[156:157]
    magic = block[257:263]
    version = block[263:265]
    if magic != b'ustar\x00' or version != b'00':
        fail("header at offset %d for %r is not pure POSIX ustar (magic=%r version=%r)"
             % (offset, name, magic, version))
    if typeflag in (b'x', b'g', b'L', b'K'):
        fail("header at offset %d is a PAX/GNU extended header (typeflag=%r) -- not allowed"
             % (offset, typeflag))
    if typeflag not in (b'0', b'\x00'):
        fail("member %r has unexpected typeflag %r (only plain files allowed)" % (name, typeflag))
    if name.startswith('./'):
        fail("member name %r has a './' prefix" % name)
    size_field = block[124:136].split(b'\x00', 1)[0].strip()
    size = int(size_field, 8) if size_field else 0
    seen_names.append(name)
    offset += 512 + ((size + 511) // 512) * 512

if seen_names != expected:
    fail("raw header member order/names %r != expected %r" % (seen_names, expected))

with tarfile.open(path, 'r') as tf:
    members = tf.getmembers()
    names2 = [m.name for m in members]
    if names2 != expected:
        fail("tarfile member order/names %r != expected %r" % (names2, expected))
    for m in members:
        if not m.isfile():
            fail("member %s is not a regular file" % m.name)
        if m.pax_headers:
            fail("member %s carries PAX extended headers: %r" % (m.name, m.pax_headers))

print("VERIFY-OK: pure USTAR, %d members, names and order match: %s" % (len(seen_names), ", ".join(seen_names)))
PYEOF

hdr "Verifying archive format (raw ustar header walk + python3 tarfile)"
VERIFY_OUT="$(python3 "$VERIFY_PY" "$OUT_TAR" "${TAR_MEMBERS[@]}")"
log "$VERIFY_OUT"
[[ "$VERIFY_OUT" == VERIFY-OK:* ]] || die "archive verification failed"

# Cross-check: content read back out of the tar matches what was staged in.
for m in "${TAR_MEMBERS[@]}"; do
    tar -x -O -f "$OUT_TAR" "$m" > "$VERIFY_DIR/$m"
    h_staged="$(sha256_of "$STAGE_DIR/$m")"
    h_intar="$(sha256_of "$VERIFY_DIR/$m")"
    [[ "$h_staged" == "$h_intar" ]] \
        || die "member $m: staged content ($h_staged) != content read back from the tar ($h_intar)"
done
log "In-tar content hashes match staged input for all ${#TAR_MEMBERS[@]} members."

# --- checksums + manifest ---------------------------------------------------

H_TAR="$(sha256_of "$OUT_TAR")"
H_BYOK="$(sha256_of "$STAGE_DIR/BYOK.bin")"
H_ASSETS="$(sha256_of "$STAGE_DIR/assets.tar")"

HS_BYOK="$(sha256_of "$STOCK_DIR/BYOK.bin")"
HS_PICO="$(sha256_of "$STOCK_DIR/BYOK-Pico.bin")"
HS_ASSETS="$(sha256_of "$STOCK_DIR/assets.tar")"

if [[ "$PICO_OMITTED" -ne 1 ]]; then
    H_PICO="$(sha256_of "$STAGE_DIR/BYOK-Pico.bin")"
else
    H_PICO=""
fi

cmp_row() {
    local name="$1" ours="$2" stock="$3"
    if [[ "$ours" == "$stock" ]]; then printf 'MATCH'; else printf 'DIFFER'; fi
}

R_BYOK="$(cmp_row BYOK.bin "$H_BYOK" "$HS_BYOK")"
R_ASSETS="$(cmp_row assets.tar "$H_ASSETS" "$HS_ASSETS")"
if [[ "$PICO_OMITTED" -ne 1 ]]; then
    R_PICO="$(cmp_row BYOK-Pico.bin "$H_PICO" "$HS_PICO")"
else
    R_PICO="OMITTED"
fi

print_comparison_table() {
    printf '%-14s %10s %10s  %-8s %s\n' "MEMBER" "OUR-SIZE" "STK-SIZE" "RESULT" "SHA256 (ours / stock, first 16 hex chars)"
    printf '%-14s %10s %10s  %-8s %s\n' "BYOK.bin" "$(size_of "$STAGE_DIR/BYOK.bin")" "$(size_of "$STOCK_DIR/BYOK.bin")" "$R_BYOK" "${H_BYOK:0:16} / ${HS_BYOK:0:16}"
    if [[ "$PICO_OMITTED" -ne 1 ]]; then
        printf '%-14s %10s %10s  %-8s %s\n' "BYOK-Pico.bin" "$(size_of "$STAGE_DIR/BYOK-Pico.bin")" "$(size_of "$STOCK_DIR/BYOK-Pico.bin")" "$R_PICO" "${H_PICO:0:16} / ${HS_PICO:0:16}"
    else
        printf '%-14s %10s %10s  %-8s %s\n' "BYOK-Pico.bin" "-" "$(size_of "$STOCK_DIR/BYOK-Pico.bin")" "$R_PICO" "(not packaged — --no-pico)"
    fi
    printf '%-14s %10s %10s  %-8s %s\n' "assets.tar" "$(size_of "$STAGE_DIR/assets.tar")" "$(size_of "$STOCK_DIR/assets.tar")" "$R_ASSETS" "${H_ASSETS:0:16} / ${HS_ASSETS:0:16}"
}

hdr "Comparison vs. stock ($STOCK_FROM)"
print_comparison_table
if [[ "$R_PICO" == "DIFFER" ]]; then
    log ""
    log "!! NOTE: BYOK-Pico.bin DIFFER above means the PICO member is NOT stock content —"
    log "!! see the WARNING printed during Pico validation above. This is not the same"
    log "!! kind of DIFFER as a custom S3 image (which is expected and normal)."
    log ""
fi

if [[ "$STOCK_ONLY" -eq 1 ]]; then
    hdr "Stock-only verification"
    if [[ "$R_BYOK" == MATCH && "$R_PICO" == MATCH && "$R_ASSETS" == MATCH ]]; then
        log "STOCK-ONLY VERIFICATION: PASS — all three members are byte-identical to $STOCK_FROM."
        log "The packaging pipeline reproduces the stock archive's content exactly."
    else
        die "STOCK-ONLY VERIFICATION: FAIL — a repack of the stock tar's own members did not come back byte-identical (BYOK.bin=$R_BYOK BYOK-Pico.bin=$R_PICO assets.tar=$R_ASSETS). The pipeline is not trustworthy until this is fixed."
    fi
fi

{
    echo "$H_TAR  BYOK.tar"
} > "$OUT_DIR/SHA256SUMS"

{
    echo "# sha256 of each tar MEMBER's content (not separate files in this directory)."
    echo "# Verify with: tar -x -O -f BYOK.tar <name> | shasum -a 256"
    echo "$H_BYOK  BYOK.bin"
    if [[ "$PICO_OMITTED" -ne 1 ]]; then
        echo "$H_PICO  BYOK-Pico.bin"
    fi
    echo "$H_ASSETS  assets.tar"
} > "$OUT_DIR/MEMBER-SHA256SUMS"

{
    echo "BYOK.tar packaging manifest"
    echo "generated: $STAMP"
    echo "generated by: scripts/make-update-tar.sh"
    echo "mode: $MODE"
    echo "stock-from: $STOCK_FROM"
    echo "stock-from sha256: $(sha256_of "$STOCK_FROM")"
    echo
    echo "esptool: $(python3 -m esptool version 2>&1 | head -1)"
    echo "tar: $(tar --version 2>&1 | head -1)"
    echo
    echo "== Members =="
    echo "1. BYOK.bin"
    echo "   source: $S3_DESC"
    echo "   size:   $(size_of "$STAGE_DIR/BYOK.bin") bytes"
    echo "   sha256: $H_BYOK"
    echo "   vs stock ($STOCK_FROM member BYOK.bin, $(size_of "$STOCK_DIR/BYOK.bin") bytes, sha256 $HS_BYOK): $R_BYOK"
    echo
    if [[ "$PICO_OMITTED" -ne 1 ]]; then
        echo "2. BYOK-Pico.bin"
        echo "   source: $PICO_DESC"
        echo "   size:   $(size_of "$STAGE_DIR/BYOK-Pico.bin") bytes"
        echo "   sha256: $H_PICO"
        echo "   vs stock ($STOCK_FROM member BYOK-Pico.bin, $(size_of "$STOCK_DIR/BYOK-Pico.bin") bytes, sha256 $HS_PICO): $R_PICO"
        if [[ "$R_PICO" == "DIFFER" ]]; then
            echo "   !! NON-STOCK PICO — packaged only because --i-know-this-pico-is-not-stock was given."
            echo "   !! This is the one unrecoverable write in the pipeline (see docs/flash-layout-and-updater.md §2.5)."
        fi
    else
        echo "2. BYOK-Pico.bin: OMITTED (--no-pico). Two-member tar; see docs/flash-layout-and-updater.md §2.5"
        echo "   and docs/design-rationale.md D-016 (\"Stage A\")."
    fi
    echo
    echo "3. assets.tar"
    echo "   source: $ASSETS_DESC"
    echo "   size:   $(size_of "$STAGE_DIR/assets.tar") bytes"
    echo "   sha256: $H_ASSETS"
    echo "   vs stock ($STOCK_FROM member assets.tar, $(size_of "$STOCK_DIR/assets.tar") bytes, sha256 $HS_ASSETS): $R_ASSETS"
    echo
    echo "== BYOK.tar =="
    echo "path:   $OUT_TAR"
    echo "size:   $(size_of "$OUT_TAR") bytes"
    echo "sha256: $H_TAR"
    echo "format: ustar (verified: raw header magic + python3 tarfile, see $SCRIPT_PATH)"
    echo "member order in archive: ${TAR_MEMBERS[*]}"
    if [[ "$PICO_OMITTED" -ne 1 ]]; then
        echo "(NOTE: vendor release tars order their members BYOK.bin, assets.tar,"
        echo " BYOK-Pico.bin — the updater looks members up by name, not position,"
        echo " so this reordering is safe; see docs/firmware.md §6.)"
    fi
    echo
    echo "== esptool image_info --version 2 on BYOK.bin ($S3_DESC) =="
    printf '%s\n' "$ESPTOOL_INFO"
    echo
    echo "== Comparison vs. stock (member content) =="
    print_comparison_table
    if [[ "$STOCK_ONLY" -eq 1 ]]; then
        echo
        if [[ "$R_BYOK" == MATCH && "$R_PICO" == MATCH && "$R_ASSETS" == MATCH ]]; then
            echo "STOCK-ONLY VERIFICATION: PASS — member-identical to $STOCK_FROM."
        else
            echo "STOCK-ONLY VERIFICATION: FAIL"
        fi
    fi
} > "$OUT_DIR/MANIFEST.txt"

hdr "Wrote"
log "  $OUT_TAR"
log "  $OUT_DIR/SHA256SUMS"
log "  $OUT_DIR/MEMBER-SHA256SUMS"
log "  $OUT_DIR/MANIFEST.txt"

exit 0
