#!/usr/bin/env python3
"""
Rasterize Segoe UI Light glyphs into anti-aliased 8-bit alpha bitmaps.
Emits include/ui_font.h for use on the ESP32 with LovyanGFX pushAlphaImage().

Font sizes:
  XL (~110 px) — charset '0'-'9', '-', '°'   (big temperature numeral)
  M  (~32 px)  — ASCII + Cyrillic + '°'       (city, time, condition)
  S  (~20 px)  — ASCII + Cyrillic + '°'       (labels, detail values)

Usage:  python tools/generate_font.py
"""

import struct
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# ── Config ────────────────────────────────────────────────
FONT_PATH_LIGHT    = r"C:\Windows\Fonts\segoeuil.ttf"   # Segoe UI Light
FONT_PATH_SEMI     = r"C:\Windows\Fonts\segoeuisl.ttf"  # Segoe UI Semilight

XL_SIZE = 110
M_SIZE  = 32
S_SIZE  = 20

XL_CHARS   = "0123456789-°"
ASCII_CHARS = "".join(chr(c) for c in range(32, 127))
CYRILLIC    = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя"
FULL_CHARS  = ASCII_CHARS + "°" + CYRILLIC   # font_M and font_S

OUT = Path("include/ui_font.h")

RENDER_PADDING = 4   # extra pixels around each glyph to avoid clipping


# ── Helpers ───────────────────────────────────────────────
def render_glyph(font: ImageFont.FreeTypeFont, char: str):
    """
    Returns (alpha_bytes, width, height, xoff, yoff, advance).
    alpha_bytes: raw bytes, one uint8 per pixel, row-major.
    xoff/yoff: pixel offsets from the pen position to the top-left of the glyph data.
    advance: horizontal advance in pixels.
    """
    try:
        bbox = font.getbbox(char)          # (left, top, right, bottom) from origin
        adv  = int(font.getlength(char))
    except Exception:
        return None

    if bbox is None:
        return None

    l, t, r, b = bbox
    gw = max(r - l, 1)
    gh = max(b - t, 1)

    # Render onto a slightly larger canvas to avoid clipping edge pixels
    cw = gw + RENDER_PADDING * 2
    ch = gh + RENDER_PADDING * 2
    img = Image.new("L", (cw, ch), 0)
    draw = ImageDraw.Draw(img)
    # Draw white glyph; offset so bbox top-left lands at (PADDING, PADDING)
    draw.text((-l + RENDER_PADDING, -t + RENDER_PADDING), char, font=font, fill=255)

    # Crop to tight content bounds
    img = img.crop((RENDER_PADDING, RENDER_PADDING, RENDER_PADDING + gw, RENDER_PADDING + gh))

    alpha = bytes(img.tobytes())
    return alpha, gw, gh, l, t, adv


def make_font_table(font_path: str, size: int, charset: str, name: str, lines: list):
    try:
        font = ImageFont.truetype(font_path, size)
    except Exception as e:
        print(f"  ✗ Could not load {font_path}: {e}")
        sys.exit(1)

    print(f"  Building font_{name} ({size}px, {len(charset)} chars)...", flush=True)

    # Collect metrics & alpha data
    glyphs = {}
    for ch in charset:
        result = render_glyph(font, ch)
        if result is None:
            continue
        alpha, gw, gh, xoff, yoff, adv = result
        if gw == 0 or gh == 0:
            continue
        glyphs[ch] = (alpha, gw, gh, xoff, yoff, adv)

    # Compute baseline (ascent) from 'A' or first available uppercase
    ref_ch = 'A' if 'A' in glyphs else list(glyphs.keys())[0]
    ascent = glyphs[ref_ch][4] if ref_ch in glyphs else 0  # yoff of ref glyph (negative = above baseline)
    # Font height (cap-height proxy)
    font_height = size

    # --- Emit data array ---
    data_name = f"font_{name}_data"
    lines.append(f"// ── Font: {name}  {size}px ──────────────────────────────────────")
    lines.append(f"static const uint8_t {data_name}[] = {{")

    offsets = {}
    byte_offset = 0
    for ch, (alpha, gw, gh, xoff, yoff, adv) in glyphs.items():
        offsets[ch] = byte_offset
        hex_vals = ", ".join(f"0x{b:02X}" for b in alpha)
        cp = ord(ch)
        comment_ch = ch if (ch.isprintable() and ch != '/' and cp < 0x300) else hex(cp)
        lines.append(f"  // '{comment_ch}'  {gw}x{gh}")
        lines.append(f"  {hex_vals},")
        byte_offset += len(alpha)
    lines.append("};")
    lines.append("")

    # --- Glyph info struct entries ---
    info_name = f"font_{name}_info"
    lines.append(f"static const UIGlyphInfo {info_name}[] = {{")
    for ch, (alpha, gw, gh, xoff, yoff, adv) in glyphs.items():
        off = offsets[ch]
        codepoint = ord(ch)
        cp = ord(ch)
        comment_ch = ch if (ch.isprintable() and cp < 0x300) else hex(cp)
        lines.append(f"  {{0x{codepoint:04X}U, {gw}, {gh}, {xoff}, {yoff}, {adv}, {off}U}},  // '{comment_ch}'")
    lines.append("};")
    lines.append("")

    # --- UIFont descriptor ---
    font_desc = f"font_{name}"
    lines.append(f"static const UIFont {font_desc} = {{")
    lines.append(f"  {data_name},")
    lines.append(f"  {info_name},")
    lines.append(f"  {len(glyphs)},")
    lines.append(f"  {font_height},")
    lines.append("};")
    lines.append("")

    print(f"    → {len(glyphs)} glyphs, {byte_offset} bytes alpha data", flush=True)
    return font_height


def main():
    lines = [
        "// Auto-generated by tools/generate_font.py — do not edit",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "// Per-glyph metrics + data offset",
        "struct UIGlyphInfo {",
        "  uint16_t ch;",       # Unicode codepoint (supports Cyrillic U+0400+)
        "  uint16_t w;",
        "  uint16_t h;",
        "  int16_t  xoff;",
        "  int16_t  yoff;",
        "  uint16_t advance;",
        "  uint32_t data_offset;",
        "};",
        "",
        "struct UIFont {",
        "  const uint8_t*      data;",
        "  const UIGlyphInfo*  glyphs;",
        "  uint16_t            count;",
        "  uint16_t            height;",
        "};",
        "",
    ]

    make_font_table(FONT_PATH_LIGHT, XL_SIZE, XL_CHARS,   "XL", lines)
    make_font_table(FONT_PATH_SEMI,   M_SIZE, FULL_CHARS, "M",  lines)
    make_font_table(FONT_PATH_SEMI,   S_SIZE, FULL_CHARS, "S",  lines)

    OUT.write_text("\n".join(lines), encoding="utf-8")
    size_kb = OUT.stat().st_size // 1024
    print(f"\n✓ {OUT}  ({size_kb} KB)")


if __name__ == "__main__":
    main()
