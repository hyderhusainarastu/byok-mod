#!/usr/bin/env python3
"""
Test Pattern Generator for BYOK Display Testing

Generates 1-bpp and 2-bpp display test patterns for verification.
Outputs PNG (if Pillow available) and PBM (always available) formats.

Usage:
    python3 testpatterns.py [--width 240] [--height 80] [--output-dir ../tests/patterns/]

Default: 240x80 (BYOK device size from hw_config.h)
"""

import sys
import struct
import os
import zlib
from pathlib import Path
from typing import Tuple, List

try:
    from PIL import Image, ImageDraw, ImageFont
    PILLOW_AVAILABLE = True
except ImportError:
    PILLOW_AVAILABLE = False


class PatternGenerator:
    """Generate 1-bpp and 2-bpp display test patterns."""

    def __init__(self, width: int = 240, height: int = 80):
        """
        Initialize pattern generator.

        Args:
            width: Display width in pixels
            height: Display height in pixels
        """
        self.width = width
        self.height = height
        self.stride_1bpp = (width + 7) // 8  # bytes per row for 1-bpp
        self.stride_2bpp = (width + 3) // 4  # bytes per row for 2-bpp

    def _pack_1bpp(self, pixels: List[int]) -> bytes:
        """Pack a list of 1-bpp pixel values (0/1) into bytes, row by row.

        Each row is padded to a byte boundary independently (stride_1bpp
        bytes per row), matching the PBM P4 format and the device's own
        1-bpp row layout — NOT a flat contiguous pack, which shears rows
        whenever width is not a multiple of 8.
        """
        result = bytearray()
        width = self.width
        for row_start in range(0, len(pixels), width):
            row = pixels[row_start:row_start + width]
            for i in range(0, len(row), 8):
                byte = 0
                for j in range(min(8, len(row) - i)):
                    if row[i + j]:
                        byte |= (0x80 >> j)
                result.append(byte)
        assert len(result) == self.stride_1bpp * self.height, \
            f"1-bpp pack size {len(result)} != stride*height {self.stride_1bpp * self.height}"
        return bytes(result)

    def _pack_2bpp(self, pixels: List[int]) -> bytes:
        """Pack a list of 2-bpp pixel values (0-3) into bytes, row by row.

        Each row is padded to a byte boundary independently (stride_2bpp
        bytes per row) — see _pack_1bpp's docstring for why this matters.
        """
        result = bytearray()
        width = self.width
        for row_start in range(0, len(pixels), width):
            row = pixels[row_start:row_start + width]
            for i in range(0, len(row), 4):
                byte = 0
                for j in range(min(4, len(row) - i)):
                    val = row[i + j] & 0x3
                    byte |= (val << (6 - j * 2))
                result.append(byte)
        assert len(result) == self.stride_2bpp * self.height, \
            f"2-bpp pack size {len(result)} != stride*height {self.stride_2bpp * self.height}"
        return bytes(result)

    def pattern_solid(self, value: int, bpp: int = 1) -> bytes:
        """Generate solid fill pattern.

        Args:
            value: pixel value (0-1 for 1-bpp, 0-3 for 2-bpp)
            bpp: bits per pixel

        Returns:
            Packed pixel data
        """
        if bpp == 1:
            stride = self.stride_1bpp
            byte_val = 0xFF if value else 0x00
        else:  # 2-bpp
            stride = self.stride_2bpp
            # For 2-bpp: value repeated in every 2-bit pair
            if value == 0:
                byte_val = 0x00
            elif value == 1:
                byte_val = 0x55  # 01010101
            elif value == 2:
                byte_val = 0xAA  # 10101010
            else:  # 3
                byte_val = 0xFF

        return bytes([byte_val] * (stride * self.height))

    def pattern_checkerboard(self, square_size: int = 8, bpp: int = 1) -> bytes:
        """Generate checkerboard pattern.

        Args:
            square_size: pixel size of each square (1, 2, 4, 8, ...)
            bpp: bits per pixel

        Returns:
            Packed pixel data
        """
        pixels = []
        for y in range(self.height):
            for x in range(self.width):
                # Determine if this pixel is in a "dark" square
                square_x = x // square_size
                square_y = y // square_size
                if (square_x + square_y) % 2 == 0:
                    pixels.append(1)  # dark
                else:
                    pixels.append(0)  # light

        if bpp == 1:
            return self._pack_1bpp(pixels)
        else:
            return self._pack_2bpp(pixels)

    def pattern_hlines(self, spacing: int = 1, bpp: int = 1) -> bytes:
        """Generate horizontal lines pattern.

        Args:
            spacing: line thickness in pixels; lines alternate with equally
                thick gaps every 2*spacing rows (spacing=1 -> alternating
                single-pixel rows: line, gap, line, gap, ...)
            bpp: bits per pixel

        Returns:
            Packed pixel data
        """
        pixels = []
        for y in range(self.height):
            for x in range(self.width):
                if (y % (2 * spacing)) < spacing:
                    pixels.append(1)  # line
                else:
                    pixels.append(0)  # space

        if bpp == 1:
            return self._pack_1bpp(pixels)
        else:
            return self._pack_2bpp(pixels)

    def pattern_vlines(self, spacing: int = 1, bpp: int = 1) -> bytes:
        """Generate vertical lines pattern.

        Args:
            spacing: line thickness in pixels; lines alternate with equally
                thick gaps every 2*spacing columns (spacing=1 -> alternating
                single-pixel columns: line, gap, line, gap, ...)
            bpp: bits per pixel

        Returns:
            Packed pixel data
        """
        pixels = []
        for y in range(self.height):
            for x in range(self.width):
                if (x % (2 * spacing)) < spacing:
                    pixels.append(1)  # line
                else:
                    pixels.append(0)  # space

        if bpp == 1:
            return self._pack_1bpp(pixels)
        else:
            return self._pack_2bpp(pixels)

    def pattern_gradient_2bpp(self) -> bytes:
        """Generate 2-bit greyscale gradient (left to right).

        Returns:
            Packed pixel data (2-bpp)
        """
        pixels = []
        for y in range(self.height):
            for x in range(self.width):
                # Map x to 0-3 (2-bit value), reaching the full range including 3
                val = min(3, (x * 4) // self.width)
                pixels.append(val)

        return self._pack_2bpp(pixels)

    def pattern_text_demo(self) -> bytes:
        """Generate a pattern with text (requires Pillow).

        Falls back to hlines if Pillow unavailable.

        Returns:
            Packed pixel data (1-bpp)
        """
        if not PILLOW_AVAILABLE:
            print("  (Pillow unavailable; using checkerboard fallback for text demo — "
                  "not solid, so it stays visually distinct from other patterns)", file=sys.stderr)
            return self.pattern_checkerboard(square_size=1)

        # Create a 1-bit white image
        img = Image.new('1', (self.width, self.height), 1)  # 1 = white
        draw = ImageDraw.Draw(img)

        # Use default font (or attempt to load a small bitmap font)
        try:
            font = ImageFont.load_default()
        except Exception:
            font = None

        # Draw text at several positions
        text_samples = ["BYOK", "Test", "240x80"]
        y_pos = 5
        for text in text_samples:
            draw.text((5, y_pos), text, fill=0, font=font)  # 0 = black
            y_pos += 15

        # Convert to 1-bpp packed bytes
        pixels = [1 - p for p in img.getdata()]  # Invert: PIL 1=white, we want 1=dark
        return self._pack_1bpp(pixels)

    def save_pbm(self, data: bytes, filename: str, bpp: int = 1):
        """Save pattern as PBM (Portable Bitmap) file.

        Args:
            data: Packed pixel data
            filename: output file path
            bpp: bits per pixel (only 1-bpp supported for PBM)
        """
        if bpp != 1:
            print(f"Warning: PBM format only supports 1-bpp; skipping {filename}", file=sys.stderr)
            return

        with open(filename, 'wb') as f:
            # PBM header (P4 = binary)
            f.write(f"P4\n{self.width} {self.height}\n".encode('ascii'))
            # Binary data
            f.write(data)

    def save_png(self, data: bytes, filename: str, bpp: int = 1):
        """Save pattern as PNG file (requires Pillow).

        Args:
            data: Packed pixel data
            filename: output file path
            bpp: bits per pixel
        """
        if not PILLOW_AVAILABLE:
            print(f"Warning: Pillow unavailable; skipping PNG save: {filename}", file=sys.stderr)
            return

        if bpp == 1:
            mode = '1'  # 1-bit
            pixels = self._unpack_1bpp(data)
            # Convert our encoding (1=dark) to PIL encoding (0=black, 1=white)
            pixels = [1 - p for p in pixels]
        else:  # 2-bpp
            mode = 'L'  # 8-bit grayscale (PIL will expand 2-bpp)
            pixels = self._unpack_2bpp(data)
            # Map 0-3 to 0-255
            pixels = [p * 85 for p in pixels]

        img = Image.new(mode, (self.width, self.height))
        img.putdata(pixels)
        img.save(filename)

    def _unpack_1bpp(self, data: bytes) -> List[int]:
        """Unpack 1-bpp packed bytes back to pixel list."""
        pixels = []
        for byte in data:
            for i in range(8):
                pixels.append((byte >> (7 - i)) & 1)
        return pixels[:self.width * self.height]

    def _unpack_2bpp(self, data: bytes) -> List[int]:
        """Unpack 2-bpp packed bytes back to pixel list."""
        pixels = []
        for byte in data:
            for i in range(4):
                pixels.append((byte >> (6 - i * 2)) & 0x3)
        return pixels[:self.width * self.height]


def main():
    """Generate all test patterns."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate BYOK display test patterns"
    )
    parser.add_argument('--width', type=int, default=240, help='Display width (default 240)')
    parser.add_argument('--height', type=int, default=80, help='Display height (default 80)')
    default_output_dir = str(Path(__file__).resolve().parent.parent.parent / 'tests' / 'patterns')
    parser.add_argument('--output-dir', type=str, default=default_output_dir,
                        help='Output directory for pattern files (default: '
                             '<repo>/tests/patterns, resolved from this script\'s '
                             'own location, not the CWD)')
    parser.add_argument('--format', choices=['pbm', 'png', 'both'], default='both',
                        help='Output format')

    args = parser.parse_args()

    # Create output directory
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    gen = PatternGenerator(args.width, args.height)

    patterns = [
        # (name, bpp, method_call)
        ('black', 1, lambda g: g.pattern_solid(1, bpp=1)),
        ('white', 1, lambda g: g.pattern_solid(0, bpp=1)),
        ('checkerboard-1px', 1, lambda g: g.pattern_checkerboard(square_size=1, bpp=1)),
        ('checkerboard-8px', 1, lambda g: g.pattern_checkerboard(square_size=8, bpp=1)),
        ('hlines-1px', 1, lambda g: g.pattern_hlines(spacing=1, bpp=1)),
        ('hlines-2px', 1, lambda g: g.pattern_hlines(spacing=2, bpp=1)),
        ('vlines-1px', 1, lambda g: g.pattern_vlines(spacing=1, bpp=1)),
        ('vlines-2px', 1, lambda g: g.pattern_vlines(spacing=2, bpp=1)),
        ('text-demo', 1, lambda g: g.pattern_text_demo()),
        ('gradient-2bit', 2, lambda g: g.pattern_gradient_2bpp()),
    ]

    print(f"Generating test patterns for {args.width}×{args.height} display...")
    print(f"Output directory: {output_dir}")
    print()

    generated_files = []
    pattern_crcs = []  # (name, bpp, crc32 of packed payload) — computed, never hardcoded

    for name, bpp, pattern_func in patterns:
        data = pattern_func(gen)
        pattern_crcs.append((name, bpp, zlib.crc32(data) & 0xFFFFFFFF))

        base_filename = f"{name}-{args.width}x{args.height}"

        if args.format in ('pbm', 'both'):
            pbm_file = output_dir / f"{base_filename}.pbm"
            gen.save_pbm(data, str(pbm_file), bpp=bpp)
            print(f"✓ {pbm_file.name} ({len(data)} bytes, {bpp}-bpp)")
            generated_files.append(pbm_file)

        if args.format in ('png', 'both'):
            png_file = output_dir / f"{base_filename}.png"
            gen.save_png(data, str(png_file), bpp=bpp)
            if PILLOW_AVAILABLE:
                print(f"✓ {png_file.name}")
                generated_files.append(png_file)

    print()
    print(f"Generated {len(generated_files)} pattern files.")
    print()
    print("Reference CRCs (computed at generation time from the actual packed payload —")
    print("never hardcoded, so this list cannot drift from the files it describes):")
    for name, bpp, crc in pattern_crcs:
        print(f"  {name:<18} ({bpp}-bpp): 0x{crc:08X}")
    print()
    print("To verify in Python:")
    print("  import zlib")
    print("  with open('tests/patterns/black-240x80.pbm', 'rb') as f:")
    print("      f.readline()  # 'P4'")
    print("      f.readline()  # '<width> <height>'")
    print("      data = f.read()")
    print("      crc = zlib.crc32(data) & 0xFFFFFFFF")
    print("      print(f'CRC: 0x{crc:08X}')")
    print()
    print("Pillow status:", "Available ✓" if PILLOW_AVAILABLE else "Unavailable (PNG export disabled)")


if __name__ == '__main__':
    main()
