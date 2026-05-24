#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


ASCII_FONT_CANDIDATES = [
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
]

HAN_FONT_CANDIDATES = [
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
]

ASCII_WIDTH = 8
ASCII_HEIGHT = 16
GB2312_WIDTH = 16
GB2312_HEIGHT = 16
ASCII_RANGE = range(32, 127)
OUTPUT_PATH = Path(__file__).resolve().parents[1] / "nas_notice_font.h"


@dataclass(frozen=True)
class GlyphSet:
    codepoints: list[int]
    glyphs: list[bytes]


def pick_font(candidates: Iterable[str], size: int) -> ImageFont.FreeTypeFont:
    for candidate in candidates:
        path = Path(candidate)
        if not path.exists():
            continue
        try:
            return ImageFont.truetype(str(path), size=size)
        except OSError:
            continue
    raise SystemExit(f"No usable font found in: {', '.join(candidates)}")


def pack_bitmap_rows(image: Image.Image, width: int, height: int) -> bytes:
    pixels = image.load()
    row_bytes = (width + 7) // 8
    packed = bytearray()
    for y in range(height):
        for byte_index in range(row_bytes):
            value = 0
            for bit in range(8):
                x = byte_index * 8 + bit
                if x >= width:
                    continue
                if pixels[x, y] >= 128:
                    value |= 0x80 >> bit
            packed.append(value)
    return bytes(packed)


def render_glyph(char: str, font: ImageFont.FreeTypeFont, width: int, height: int, y_tweak: int = 0) -> bytes:
    canvas = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(canvas)
    bbox = draw.textbbox((0, 0), char, font=font)
    glyph_width = bbox[2] - bbox[0]
    glyph_height = bbox[3] - bbox[1]
    x = (width - glyph_width) // 2 - bbox[0]
    y = (height - glyph_height) // 2 - bbox[1] + y_tweak
    draw.text((x, y), char, fill=255, font=font)
    return pack_bitmap_rows(canvas, width, height)


def collect_gb2312_chars() -> list[str]:
    chars: dict[int, str] = {}
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                char = bytes((hi, lo)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.setdefault(ord(char), char)
    return [chars[codepoint] for codepoint in sorted(chars)]


def generate_ascii_glyphs(font: ImageFont.FreeTypeFont) -> GlyphSet:
    codepoints: list[int] = []
    glyphs: list[bytes] = []
    for codepoint in ASCII_RANGE:
        codepoints.append(codepoint)
        glyphs.append(render_glyph(chr(codepoint), font, ASCII_WIDTH, ASCII_HEIGHT, y_tweak=-1))
    return GlyphSet(codepoints, glyphs)


def generate_gb2312_glyphs(font: ImageFont.FreeTypeFont) -> GlyphSet:
    codepoints: list[int] = []
    glyphs: list[bytes] = []
    for char in collect_gb2312_chars():
        codepoints.append(ord(char))
        glyphs.append(render_glyph(char, font, GB2312_WIDTH, GB2312_HEIGHT, y_tweak=-1))
    return GlyphSet(codepoints, glyphs)


def format_byte_array(name: str, values: bytes | bytearray | list[int], *, bytes_per_line: int = 16) -> str:
    lines = [f"const uint8_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(values), bytes_per_line):
        chunk = values[offset : offset + bytes_per_line]
        lines.append("  " + ", ".join(f"0x{value:02X}" for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def format_word_array(name: str, values: list[int], *, words_per_line: int = 12) -> str:
    lines = [f"const uint16_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(values), words_per_line):
        chunk = values[offset : offset + words_per_line]
        lines.append("  " + ", ".join(f"0x{value:04X}" for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def flatten(glyphs: list[bytes]) -> bytes:
    blob = bytearray()
    for glyph in glyphs:
        blob.extend(glyph)
    return bytes(blob)


def build_header(ascii_set: GlyphSet, gb2312_set: GlyphSet) -> str:
    ascii_blob = flatten(ascii_set.glyphs)
    gb2312_blob = flatten(gb2312_set.glyphs)
    header_lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        f"constexpr uint8_t kNasNoticeAsciiFirst = {ASCII_RANGE.start};",
        f"constexpr uint8_t kNasNoticeAsciiLast = {ASCII_RANGE.stop - 1};",
        f"constexpr uint8_t kNasNoticeAsciiWidth = {ASCII_WIDTH};",
        f"constexpr uint8_t kNasNoticeAsciiHeight = {ASCII_HEIGHT};",
        f"constexpr uint8_t kNasNoticeAsciiBytesPerGlyph = {ASCII_HEIGHT};",
        f"constexpr uint8_t kNasNoticeGb2312Width = {GB2312_WIDTH};",
        f"constexpr uint8_t kNasNoticeGb2312Height = {GB2312_HEIGHT};",
        f"constexpr uint8_t kNasNoticeGb2312BytesPerGlyph = {GB2312_HEIGHT * 2};",
        f"constexpr uint16_t kNasNoticeGb2312Count = {len(gb2312_set.codepoints)};",
        "",
        "// Auto-generated bitmap notice font.",
        "// ASCII uses 8x16 monospace glyphs.",
        "// Chinese and common symbols use 16x16 GB2312 coverage glyphs.",
        "",
        format_byte_array("NasNoticeAsciiGlyphs", ascii_blob),
        "",
        format_word_array("NasNoticeGb2312Codepoints", gb2312_set.codepoints),
        "",
        format_byte_array("NasNoticeGb2312Glyphs", gb2312_blob),
        "",
    ]
    return "\n".join(header_lines)


def main() -> None:
    ascii_font = pick_font(ASCII_FONT_CANDIDATES, size=14)
    han_font = pick_font(HAN_FONT_CANDIDATES, size=15)
    ascii_set = generate_ascii_glyphs(ascii_font)
    gb2312_set = generate_gb2312_glyphs(han_font)
    OUTPUT_PATH.write_text(build_header(ascii_set, gb2312_set), encoding="utf-8")
    print(f"Wrote {OUTPUT_PATH}")
    print(f"ASCII glyphs: {len(ascii_set.codepoints)}")
    print(f"GB2312 glyphs: {len(gb2312_set.codepoints)}")


if __name__ == "__main__":
    main()
