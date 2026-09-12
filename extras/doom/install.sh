#!/usr/bin/env bash
# extras/doom/install.sh — install chocolate-doom + fetch the shareware DOOM1.WAD.
#
# Idempotent: safe to re-run. Installs the Homebrew formula (bottled, no sudo)
# if missing, and (re-)acquires extras/doom/wad/DOOM1.WAD if it isn't already
# present and hash-verified.
#
# Per docs/sample-projects/doom.md §3: only the shareware DOOM1.WAD (freely redistributable
# under id Software's original 1993 shareware license) is ever fetched here.
# The full/registered DOOM.WAD is out of scope and this script will never
# fetch it.
#
# Expected identity of the file this script must produce:
#   size   4196020 bytes
#   sha1   5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
#   md5    f0cefca49926d00903cf57551d901abe
#
# NOTE on the widely-published "MD5" 5b2e249b9c5133ec987b3ea77596381 that
# docs/sample-projects/doom.md §3 warns about: that value
# is 32 hex chars, the length of an MD5 -- but it is actually the first 32
# hex characters of the file's real SHA1, 5b2e249b9c5133ec987b3ea77596381
# dc0d6bc1d (40 hex chars). Cross-checked against two independent public
# sources (github.com/rommapp/romm backend/models/fixtures/
# known_bios_files.json, and github.com/sysprog21/rv32emu mk/external.mk,
# both explicitly labeling it "sha1"), and it matches this file exactly. The
# real MD5 (computed directly from the file; I could not find it published
# anywhere else) is f0cefca49926d00903cf57551d901abe. This script verifies
# against the SHA1, which is the value actually confirmed correct.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAD_DIR="$HERE/wad"
WAD_PATH="$WAD_DIR/DOOM1.WAD"
SHA_RECORD="$HERE/WAD.sha256"

EXPECT_SIZE=4196020
EXPECT_SHA1="5b2e249b9c5133ec987b3ea77596381dc0d6bc1d"
EXPECT_MD5="f0cefca49926d00903cf57551d901abe"

log()  { printf '[install.sh] %s\n' "$*"; }
fail() { printf '[install.sh] ERROR: %s\n' "$*" >&2; exit 1; }

# --- 1. chocolate-doom via Homebrew (bottled, no sudo) ----------------------
if ! command -v brew >/dev/null 2>&1; then
  fail "Homebrew not found on PATH. Install Homebrew first (https://brew.sh), no sudo needed for this formula."
fi

if brew list --versions chocolate-doom >/dev/null 2>&1; then
  log "chocolate-doom already installed: $(brew list --versions chocolate-doom)"
else
  log "installing chocolate-doom via Homebrew..."
  brew install chocolate-doom
fi
log "chocolate-doom version: $(brew list --versions chocolate-doom)"
command -v chocolate-doom >/dev/null 2>&1 && log "chocolate-doom binary: $(command -v chocolate-doom)"

# --- 2. DOOM1.WAD (shareware) -----------------------------------------------
mkdir -p "$WAD_DIR"

verify_wad() {
  # $1 = path. Returns 0 if size and SHA1 match the known-good shareware
  # DOOM1.WAD v1.9 identity above, 1 (with a logged reason) otherwise.
  local path="$1"
  [ -f "$path" ] || return 1
  local size
  size=$(stat -f%z "$path" 2>/dev/null || stat -c%s "$path" 2>/dev/null) || return 1
  if [ "$size" != "$EXPECT_SIZE" ]; then
    log "size mismatch: got $size, expected $EXPECT_SIZE"
    return 1
  fi
  local sha1
  sha1=$(shasum -a 1 "$path" 2>/dev/null | awk '{print $1}')
  if [ "$sha1" != "$EXPECT_SHA1" ]; then
    log "SHA1 mismatch: got $sha1, expected $EXPECT_SHA1"
    return 1
  fi
  return 0
}

if verify_wad "$WAD_PATH"; then
  log "DOOM1.WAD already present and hash-verified at $WAD_PATH"
else
  rm -f "$WAD_PATH"
  log "fetching shareware DOOM1.WAD..."

  fetched=0

  # (a) Debian/Ubuntu doom-wad-shareware .deb — extract doom1.wad from it.
  #     This package repackages exactly the shareware WAD id Software's
  #     license permits redistributing.
  if [ "$fetched" -eq 0 ]; then
    DEB_URL="http://archive.ubuntu.com/ubuntu/pool/multiverse/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed-5_all.deb"
    TMPDIR_DEB="$(mktemp -d)"
    log "trying (a) Debian/Ubuntu doom-wad-shareware .deb: $DEB_URL"
    if curl -fsSL -o "$TMPDIR_DEB/pkg.deb" "$DEB_URL" 2>/dev/null; then
      if (cd "$TMPDIR_DEB" && ar x pkg.deb 2>/dev/null); then
        DATA_TAR=$(ls "$TMPDIR_DEB"/data.tar.* 2>/dev/null | head -1)
        if [ -n "${DATA_TAR:-}" ]; then
          (cd "$TMPDIR_DEB" && tar xf "$DATA_TAR") 2>/dev/null || true
          FOUND=$(find "$TMPDIR_DEB" -iname 'doom1.wad' | head -1)
          if [ -n "${FOUND:-}" ]; then
            cp "$FOUND" "$WAD_PATH"
            if verify_wad "$WAD_PATH"; then
              fetched=1
              log "(a) succeeded via doom-wad-shareware .deb"
            else
              log "(a) extracted a file but it failed verification; discarding"
              rm -f "$WAD_PATH"
            fi
          else
            log "(a) .deb did not contain doom1.wad"
          fi
        else
          log "(a) no data.tar.* found inside .deb"
        fi
      else
        log "(a) 'ar x' failed (ar not available or not a valid .deb)"
      fi
    else
      log "(a) download failed"
    fi
    rm -rf "$TMPDIR_DEB"
  fi

  # (b) archive.org mirror -- same doom-wad-shareware source tarball as (a),
  #     hosted independently on archive.org (identifier verified:
  #     item "doom-wad-shareware_1.9_11apr2025-mirror"), as a fallback in
  #     case the Ubuntu archive mirror in (a) is unreachable.
  if [ "$fetched" -eq 0 ]; then
    ARCHIVE_URL="https://archive.org/download/doom-wad-shareware_1.9_11apr2025-mirror/doom-wad-shareware_1.9.fixed.orig.tar.gz"
    TMPDIR_AO="$(mktemp -d)"
    log "trying (b) archive.org: $ARCHIVE_URL"
    if curl -fsSL -o "$TMPDIR_AO/orig.tar.gz" "$ARCHIVE_URL" 2>/dev/null; then
      (cd "$TMPDIR_AO" && tar xzf orig.tar.gz) 2>/dev/null || true
      FOUND=$(find "$TMPDIR_AO" -iname 'doom1.wad' | head -1)
      if [ -n "${FOUND:-}" ]; then
        cp "$FOUND" "$WAD_PATH"
        if verify_wad "$WAD_PATH"; then
          fetched=1
          log "(b) succeeded via archive.org"
        else
          log "(b) extracted a file but it failed verification; discarding"
          rm -f "$WAD_PATH"
        fi
      else
        log "(b) tarball did not contain doom1.wad"
      fi
    else
      log "(b) download failed"
    fi
    rm -rf "$TMPDIR_AO"
  fi

  # (c) idgames doom19s.zip (DEICE self-extracting DOS archive) -- only if
  #     extraction tools are already present; this script will NOT install
  #     new extraction tools (e.g. a DEICE decompressor or DOSBox) to do this.
  if [ "$fetched" -eq 0 ]; then
    log "trying (c) idgames doom19s.zip (DEICE archive)"
    if command -v unzip >/dev/null 2>&1 && (command -v dice >/dev/null 2>&1 || command -v deice >/dev/null 2>&1); then
      TMPDIR_ID="$(mktemp -d)"
      IDGAMES_URL="https://youfailit.net/pub/idgames/idstuff/doom/doom19s.zip"
      if curl -fsSL -o "$TMPDIR_ID/doom19s.zip" "$IDGAMES_URL" 2>/dev/null; then
        (cd "$TMPDIR_ID" && unzip -q doom19s.zip) 2>/dev/null || true
        DEICE_BIN=$(command -v dice || command -v deice)
        # DOOMS_19.1 + DOOMS_19.2 -> DEICE -> DOOM1.WAD among other files.
        if [ -f "$TMPDIR_ID/DOOMS_19.1" ]; then
          (cd "$TMPDIR_ID" && "$DEICE_BIN" DOOMS_19.1) 2>/dev/null || true
          FOUND=$(find "$TMPDIR_ID" -iname 'doom1.wad' | head -1)
          if [ -n "${FOUND:-}" ]; then
            cp "$FOUND" "$WAD_PATH"
            if verify_wad "$WAD_PATH"; then
              fetched=1
              log "(c) succeeded via idgames doom19s.zip"
            else
              rm -f "$WAD_PATH"
            fi
          fi
        fi
      else
        log "(c) download of doom19s.zip failed"
      fi
      rm -rf "$TMPDIR_ID"
    else
      log "(c) skipped: no DEICE decompressor (dice/deice) present on this Mac, and this script" \
          "will not install one -- doom19s.zip is a 1995 DOS self-extracting archive that needs" \
          "DEICE.EXE (via DOSBox or a native DEICE port) to unpack. See docs/sample-projects/doom.md §3."
    fi
  fi

  if [ "$fetched" -eq 0 ]; then
    fail "could not obtain a hash-verified DOOM1.WAD from any source (a/b/c). Refusing to proceed with an unverified file."
  fi
fi

# --- 3. record the sha256 --------------------------------------------------
SHA256=$(shasum -a 256 "$WAD_PATH" | awk '{print $1}')
SHA1=$(shasum -a 1 "$WAD_PATH" | awk '{print $1}')
MD5=$(md5 -q "$WAD_PATH" 2>/dev/null || md5sum "$WAD_PATH" | awk '{print $1}')
SIZE=$(stat -f%z "$WAD_PATH" 2>/dev/null || stat -c%s "$WAD_PATH")

cat > "$SHA_RECORD" <<EOF
# extras/doom/WAD.sha256 — identity record for extras/doom/wad/DOOM1.WAD
# (the WAD itself is gitignored; this record is tracked)
#
# file:   DOOM1.WAD (shareware, v1.9)
# size:   $SIZE bytes
# sha1:   $SHA1
# md5:    $MD5
# sha256: $SHA256
#
# NOTE: the value 5b2e249b9c5133ec987b3ea77596381, published across many
# Doom-community sources as this file's "MD5" (docs/sample-projects/doom.md
# §3 covers the mix-up), is actually the first 32 hex chars of the SHA1 above
# (5b2e249b9c5133ec987b3ea77596381dc0d6bc1d), not a real MD5. Confirmed
# against two independent public sources (github.com/rommapp/romm and
# github.com/sysprog21/rv32emu, both explicitly labeling it "sha1"). The
# actual MD5 is the value recorded above.
#
# Verify a local copy with:
#   shasum -a 256 -c <(echo "$SHA256  extras/doom/wad/DOOM1.WAD")
$SHA256  DOOM1.WAD
EOF

log "DOOM1.WAD verified: size=$SIZE md5=$MD5"
log "sha256=$SHA256 recorded in $SHA_RECORD"
log "done. Run extras/doom/byok-doom.sh to play."
