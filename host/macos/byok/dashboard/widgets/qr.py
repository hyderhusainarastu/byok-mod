"""qr widget -- a minimal, dependency-free byte-mode QR Code encoder.

Implements ISO/IEC 18004 byte mode, error-correction level L, symbol
versions 1-10 only (up to 271 bytes of payload) -- enough for a URL, a
Wi-Fi join string, or a short message on a small display, without pulling
in an external QR library.

This is a from-scratch implementation of the standard algorithm (GF(256)
Reed-Solomon error correction, finder/timing/alignment/format/version
pattern placement, the zigzag data-bit scan, and mask-penalty scoring to
pick the best of the 8 standard masks) -- there is no single canonical
"the" MIT QR implementation vendored here, but the placement/scan
structure below is the standard technique used by (and independently
verified against the public-domain description in) well-known open-source
encoders such as Project Nayuki's "QR Code generator library" (MIT) and
Thonky's QR Code tutorial; credit to both as prior art for that structure.

Verification: besides the ISO/IEC 18004 algorithm itself, this has been
checked with an independent decode round-trip (read back the format info
via its BCH check, re-derive the version info via its own BCH check for
v>=7, re-walk the same zigzag scan to recover codewords, split them back
into per-block Reed-Solomon codewords and confirm each has a zero
syndrome -- i.e. re-encoding the recovered data reproduces exactly the EC
bytes that were placed -- then decode the byte-mode payload and diff it
against the input) across versions 1, 2, 8 and 10 and several payloads,
all passing; see tests/host/test_dashboard.py for the structural subset
of that kept in the test suite. It has *not* been verified by scanning a
printed/rendered code with a real phone camera, though. If you depend on
a generated code being scannable, test it once with your phone before
relying on it unattended.
"""

from __future__ import annotations

from typing import List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw

from .base import RenderContext, Widget, register

# ---------------------------------------------------------------------------
# GF(256) arithmetic (primitive polynomial 0x11D, generator 2) and
# Reed-Solomon encoding
# ---------------------------------------------------------------------------

_GF_EXP = [0] * 512
_GF_LOG = [0] * 256


def _init_gf() -> None:
    x = 1
    for i in range(255):
        _GF_EXP[i] = x
        _GF_LOG[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
    for i in range(255, 512):
        _GF_EXP[i] = _GF_EXP[i - 255]


_init_gf()


def _gf_mul(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    return _GF_EXP[_GF_LOG[a] + _GF_LOG[b]]


def _poly_mul(p: Sequence[int], q: Sequence[int]) -> List[int]:
    out = [0] * (len(p) + len(q) - 1)
    for i, a in enumerate(p):
        if a == 0:
            continue
        for j, b in enumerate(q):
            out[i + j] ^= _gf_mul(a, b)
    return out


def _rs_generator_poly(degree: int) -> List[int]:
    """Monic generator polynomial of the given degree, coefficients
    highest-degree-first, as used for RS error-correction codewords."""
    poly = [1]
    for i in range(degree):
        poly = _poly_mul(poly, [1, _GF_EXP[i]])
    return poly


def rs_encode(data: Sequence[int], ec_len: int) -> List[int]:
    """Reed-Solomon error-correction codewords for `data` (a byte sequence),
    `ec_len` codewords long -- the standard "polynomial long division in
    GF(256)" algorithm."""
    generator = _rs_generator_poly(ec_len)
    remainder = list(data) + [0] * ec_len
    for i in range(len(data)):
        factor = remainder[i]
        if factor == 0:
            continue
        for j, g in enumerate(generator):
            remainder[i + j] ^= _gf_mul(g, factor)
    return remainder[len(data):]


# ---------------------------------------------------------------------------
# Version / error-correction-level L capacity table, versions 1-10
#   version -> (total_codewords, ec_codewords_per_block, [(num_blocks, data_codewords_per_block), ...])
# from ISO/IEC 18004 Annex, EC level L column.
# ---------------------------------------------------------------------------

_EC_L_TABLE = {
    1: (26, 7, [(1, 19)]),
    2: (44, 10, [(1, 34)]),
    3: (70, 15, [(1, 55)]),
    4: (100, 20, [(1, 80)]),
    5: (134, 26, [(1, 108)]),
    6: (172, 18, [(2, 68)]),
    7: (196, 20, [(2, 78)]),
    8: (242, 24, [(2, 97)]),
    9: (292, 30, [(2, 116)]),
    10: (346, 18, [(2, 68), (2, 69)]),
}

# Alignment pattern center coordinates per version (empty/[6,*] as applicable).
_ALIGNMENT_COORDS = {
    1: [],
    2: [6, 18],
    3: [6, 22],
    4: [6, 26],
    5: [6, 30],
    6: [6, 34],
    7: [6, 22, 38],
    8: [6, 24, 42],
    9: [6, 26, 46],
    10: [6, 28, 50],
}


def _data_codewords_total(version: int) -> int:
    _, _, groups = _EC_L_TABLE[version]
    return sum(n * c for n, c in groups)


def capacity_bytes(version: int) -> int:
    """Max byte-mode payload length that fits in `version` at EC level L."""
    count_bits = 8 if version <= 9 else 16
    overhead = -(-(4 + count_bits) // 8)  # ceil((mode + count) / 8)
    return _data_codewords_total(version) - overhead


def choose_version(data_len: int) -> int:
    for v in range(1, 11):
        if data_len <= capacity_bytes(v):
            return v
    raise ValueError(
        f"data too long ({data_len} bytes) for QR versions 1-10 at EC level L "
        f"(max {capacity_bytes(10)} bytes); shorten the text"
    )


# ---------------------------------------------------------------------------
# Bit-level data codeword encoding (mode + count + payload + terminator + pad)
# ---------------------------------------------------------------------------


class _BitWriter:
    def __init__(self) -> None:
        self._bits: List[int] = []

    def write(self, value: int, nbits: int) -> None:
        for i in range(nbits - 1, -1, -1):
            self._bits.append((value >> i) & 1)

    def __len__(self) -> int:
        return len(self._bits)

    def to_bytes(self) -> bytes:
        out = bytearray()
        for i in range(0, len(self._bits), 8):
            chunk = self._bits[i:i + 8]
            byte = 0
            for bit in chunk:
                byte = (byte << 1) | bit
            byte <<= 8 - len(chunk)
            out.append(byte)
        return bytes(out)


def _encode_data_codewords(data: bytes, version: int) -> bytes:
    data_codewords_total = _data_codewords_total(version)
    count_bits = 8 if version <= 9 else 16
    capacity_bits = data_codewords_total * 8

    writer = _BitWriter()
    writer.write(0b0100, 4)  # byte mode indicator
    writer.write(len(data), count_bits)
    for byte in data:
        writer.write(byte, 8)

    term_len = min(4, capacity_bits - len(writer))
    if term_len > 0:
        writer.write(0, term_len)
    while len(writer) % 8 != 0:
        writer.write(0, 1)

    pad_bytes = (0xEC, 0x11)
    i = 0
    while len(writer) < capacity_bits:
        writer.write(pad_bytes[i % 2], 8)
        i += 1

    return writer.to_bytes()


def _interleave_codewords(data: bytes, version: int) -> bytes:
    total_cw, ec_per_block, groups = _EC_L_TABLE[version]
    blocks: List[List[int]] = []
    idx = 0
    for num_blocks, cw_per_block in groups:
        for _ in range(num_blocks):
            blocks.append(list(data[idx:idx + cw_per_block]))
            idx += cw_per_block
    ec_blocks = [rs_encode(block, ec_per_block) for block in blocks]

    out: List[int] = []
    max_len = max(len(b) for b in blocks)
    for i in range(max_len):
        for b in blocks:
            if i < len(b):
                out.append(b[i])
    for i in range(ec_per_block):
        for eb in ec_blocks:
            out.append(eb[i])

    assert len(out) == total_cw
    return bytes(out)


# ---------------------------------------------------------------------------
# Module matrix
# ---------------------------------------------------------------------------


class QRMatrix:
    def __init__(self, version: int):
        self.version = version
        self.size = version * 4 + 17
        self.modules = [[False] * self.size for _ in range(self.size)]
        self.is_function = [[False] * self.size for _ in range(self.size)]

    def _set(self, col: int, row: int, dark: bool, function: bool = False) -> None:
        if 0 <= col < self.size and 0 <= row < self.size:
            self.modules[row][col] = dark
            if function:
                self.is_function[row][col] = True

    def _draw_finder(self, cx: int, cy: int) -> None:
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                dist = max(abs(dx), abs(dy))
                self._set(cx + dx, cy + dy, dist not in (2, 4), function=True)

    def _draw_alignment(self, cx: int, cy: int) -> None:
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                self._set(cx + dx, cy + dy, max(abs(dx), abs(dy)) != 1, function=True)

    def _draw_timing(self) -> None:
        for i in range(8, self.size - 8):
            dark = i % 2 == 0
            self._set(6, i, dark, function=True)
            self._set(i, 6, dark, function=True)

    def _reserve_format_areas(self) -> None:
        for i in range(9):
            self._set(8, i, False, function=True)
            self._set(i, 8, False, function=True)
        for i in range(8):
            self._set(self.size - 1 - i, 8, False, function=True)
        for i in range(7):
            self._set(8, self.size - 1 - i, False, function=True)
        self._set(8, self.size - 8, True, function=True)  # the fixed dark module

    def _draw_version_info(self) -> None:
        if self.version < 7:
            return
        rem = self.version
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        bits = (self.version << 12) | rem
        for i in range(18):
            bit = ((bits >> i) & 1) == 1
            a = self.size - 11 + i % 3
            b = i // 3
            self._set(a, b, bit, function=True)
            self._set(b, a, bit, function=True)

    def draw_function_patterns(self) -> None:
        self._draw_finder(3, 3)
        self._draw_finder(self.size - 4, 3)
        self._draw_finder(3, self.size - 4)
        self._draw_timing()
        self._reserve_format_areas()
        self._draw_version_info()

        coords = _ALIGNMENT_COORDS[self.version]
        last = len(coords) - 1
        for i, cx in enumerate(coords):
            for j, cy in enumerate(coords):
                # Skip the three combos that overlap a finder pattern.
                if (i == 0 and j == 0) or (i == 0 and j == last) or (i == last and j == 0):
                    continue
                self._draw_alignment(cx, cy)

    def draw_codewords(self, codewords: bytes) -> None:
        total_bits = len(codewords) * 8
        bit_index = 0
        col = self.size - 1
        while col >= 1:
            if col == 6:  # timing column, skip
                col -= 1
            upward = ((col + 1) & 2) == 0
            rows = range(self.size - 1, -1, -1) if upward else range(self.size)
            for row in rows:
                for dc in (0, 1):
                    c = col - dc
                    if self.is_function[row][c]:
                        continue
                    if bit_index < total_bits:
                        bit = (codewords[bit_index >> 3] >> (7 - (bit_index & 7))) & 1
                        self.modules[row][c] = bool(bit)
                        bit_index += 1
                    # else: remainder bit, left at its initialized (light) value
            col -= 2

    @staticmethod
    def _mask_fn(mask: int, row: int, col: int) -> bool:
        if mask == 0:
            return (row + col) % 2 == 0
        if mask == 1:
            return row % 2 == 0
        if mask == 2:
            return col % 3 == 0
        if mask == 3:
            return (row + col) % 3 == 0
        if mask == 4:
            return (row // 2 + col // 3) % 2 == 0
        if mask == 5:
            return (row * col) % 2 + (row * col) % 3 == 0
        if mask == 6:
            return ((row * col) % 2 + (row * col) % 3) % 2 == 0
        if mask == 7:
            return ((row + col) % 2 + (row * col) % 3) % 2 == 0
        raise ValueError(f"mask must be 0-7, got {mask}")

    def masked_copy(self, mask: int) -> "QRMatrix":
        clone = QRMatrix(self.version)
        clone.is_function = self.is_function
        for row in range(self.size):
            for col in range(self.size):
                v = self.modules[row][col]
                if not self.is_function[row][col] and self._mask_fn(mask, row, col):
                    v = not v
                clone.modules[row][col] = v
        return clone

    def draw_format_info(self, mask: int) -> None:
        ecl_bits = 0b01  # EC level L
        data = (ecl_bits << 3) | mask
        rem = data
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        bits = ((data << 10) | rem) ^ 0x5412

        def bit(i: int) -> bool:
            return ((bits >> i) & 1) == 1

        for i in range(6):
            self._set(8, i, bit(i), function=True)
        self._set(8, 7, bit(6), function=True)
        self._set(8, 8, bit(7), function=True)
        self._set(7, 8, bit(8), function=True)
        for i in range(9, 15):
            self._set(14 - i, 8, bit(i), function=True)

        for i in range(8):
            self._set(self.size - 1 - i, 8, bit(i), function=True)
        for i in range(8, 15):
            self._set(8, self.size - 15 + i, bit(i), function=True)

    def penalty_score(self) -> int:
        size = self.size
        m = self.modules
        score = 0

        # Rule 1: runs of 5+ same-color modules, per row and per column.
        for row in range(size):
            score += _run_penalty(m[row])
        for col in range(size):
            score += _run_penalty([m[row][col] for row in range(size)])

        # Rule 2: 2x2 blocks of the same color.
        for row in range(size - 1):
            for col in range(size - 1):
                v = m[row][col]
                if v == m[row][col + 1] == m[row + 1][col] == m[row + 1][col + 1]:
                    score += 3

        # Rule 3: finder-like 1:1:3:1:1 patterns with 4 light modules attached.
        pattern_dark = [True, False, True, True, True, False, True]
        pattern_light_pad = [False, False, False, False]

        def has_pattern(seq: List[bool]) -> bool:
            target_a = pattern_dark + pattern_light_pad
            target_b = pattern_light_pad + pattern_dark
            n = len(target_a)
            return any(seq[i:i + n] == target_a or seq[i:i + n] == target_b for i in range(len(seq) - n + 1))

        for row in range(size):
            if has_pattern(m[row]):
                score += 40
        for col in range(size):
            if has_pattern([m[row][col] for row in range(size)]):
                score += 40

        # Rule 4: overall dark/light balance.
        dark_count = sum(1 for row in m for v in row if v)
        percent = (dark_count * 100) // (size * size)
        prev = percent - (percent % 5)
        nxt = prev + 5
        score += min(abs(prev - 50) // 5, abs(nxt - 50) // 5) * 10

        return score

    def to_image(self, scale: int = 4, border: int = 4, dark: int = 0, light: int = 255) -> Image.Image:
        total = self.size + 2 * border
        img = Image.new("L", (total * scale, total * scale), color=light)
        draw = ImageDraw.Draw(img)
        for row in range(self.size):
            for col in range(self.size):
                if self.modules[row][col]:
                    x0 = (col + border) * scale
                    y0 = (row + border) * scale
                    draw.rectangle([x0, y0, x0 + scale - 1, y0 + scale - 1], fill=dark)
        return img


def _run_penalty(line: Sequence[bool]) -> int:
    score = 0
    run_color = line[0]
    run_len = 1
    for v in line[1:]:
        if v == run_color:
            run_len += 1
        else:
            if run_len >= 5:
                score += 3 + (run_len - 5)
            run_color = v
            run_len = 1
    if run_len >= 5:
        score += 3 + (run_len - 5)
    return score


def encode(data: bytes, version: Optional[int] = None) -> QRMatrix:
    """Encode `data` (bytes) as a byte-mode, EC-level-L QR code and return
    the finished (masked, all patterns drawn) `QRMatrix`."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    if version is None:
        version = choose_version(len(data))
    elif len(data) > capacity_bytes(version):
        raise ValueError(f"data ({len(data)} bytes) does not fit in version {version} at EC level L")

    codewords = _interleave_codewords(_encode_data_codewords(data, version), version)

    template = QRMatrix(version)
    template.draw_function_patterns()
    template.draw_codewords(codewords)

    best: Optional[QRMatrix] = None
    best_score: Optional[int] = None
    for mask in range(8):
        candidate = template.masked_copy(mask)
        candidate.draw_format_info(mask)
        score = candidate.penalty_score()
        if best_score is None or score < best_score:
            best, best_score = candidate, score

    assert best is not None
    return best


def encode_to_image(text: str, scale: int = 4, border: int = 4) -> Image.Image:
    return encode(text).to_image(scale=scale, border=border)


# ---------------------------------------------------------------------------
# Dashboard widget
# ---------------------------------------------------------------------------


@register("qr")
class QRWidget(Widget):
    """options:
        data: text to encode (required; falls back to a placeholder message
              if missing/too long for version 10 EC-L, i.e. > 271 bytes)
        border: quiet-zone modules, default 2 (small tiles can't spare 4)
        title: optional header
    """

    def render(self, ctx: RenderContext) -> Image.Image:
        data = str(ctx.options.get("data", "") or "")
        title = ctx.options.get("title", "")
        border = int(ctx.options.get("border", 2))

        image = self.blank_tile(ctx)
        draw = ImageDraw.Draw(image)
        top = self.draw_title(draw, ctx, title) if title else 0
        avail_h = max(1, ctx.height - top)

        if not data:
            self.draw_border(image, ctx.options)
            return image

        try:
            matrix = encode(data)
        except Exception:
            font = ctx.font(max(9, ctx.height // 10))
            draw.text((2, top + 2), "QR data too long", font=font, fill=0)
            self.draw_border(image, ctx.options)
            return image

        modules_across = matrix.size + 2 * border
        scale = max(1, min(ctx.width // modules_across, avail_h // modules_across))
        qr_img = matrix.to_image(scale=scale, border=border)
        x = (ctx.width - qr_img.width) // 2
        y = top + (avail_h - qr_img.height) // 2
        image.paste(qr_img, (max(0, x), max(top, y)))

        self.draw_border(image, ctx.options)
        return image
