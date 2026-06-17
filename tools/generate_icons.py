#!/usr/bin/env python3
"""
Generate time-of-day aware weather icons via GPT Image 2,
convert to RGB565 C arrays, write include/weather_icons.h

Usage:  python tools/generate_icons.py
Cached PNGs are saved to tools/icons_png/ — delete to regenerate.
"""

import base64
import io
import os
import sys
import urllib.request
from pathlib import Path

from openai import OpenAI
from PIL import Image

# ── Config ────────────────────────────────────────────────
ICON_SIZE  = 128          # pixels (square)
API_KEY    = os.environ.get("OPENAI_API_KEY")
if not API_KEY:
    sys.exit("Error: set the OPENAI_API_KEY environment variable before running this script.")

# Background colour (C_BG = 0x0821 RGB565 → ~RGB(8,4,8))
BG_R, BG_G, BG_B = 8, 4, 8

# ── Icon prompts ──────────────────────────────────────────
# 4 sky variants × clear sky  +  2 for clouds  +  4 static
ICONS = {
    "clear_day": (
        "Minimalist weather icon, vivid golden sun with 8 thick radiating rays, "
        "flat bold design, deep navy blue background, centered, no text, no border"
    ),
    "clear_dawn": (
        "Minimalist weather icon, pastel orange-pink sunrise, sun just cresting the horizon "
        "with soft warm rays, flat bold design, deep navy blue background, centered, no text"
    ),
    "clear_sunset": (
        "Minimalist weather icon, dramatic golden-orange sunset sun half below the horizon, "
        "warm red-orange radial glow, flat bold design, deep navy blue background, centered, no text"
    ),
    "clear_night": (
        "Minimalist weather icon, bright crescent moon with 5 small glowing white stars, "
        "flat bold design, deep navy blue background, centered, no text"
    ),
    "cloudy_day": (
        "Minimalist weather icon, three overlapping puffy white clouds with soft grey shadows, "
        "flat bold design, deep navy blue background, centered, no text"
    ),
    "cloudy_night": (
        "Minimalist weather icon, dark blue-grey storm cloud with a crescent moon peeking "
        "behind, flat bold design, deep navy blue background, centered, no text"
    ),
    "rain": (
        "Minimalist weather icon, dark grey rain cloud with 5 vivid blue diagonal rain drops, "
        "flat bold design, deep navy blue background, centered, no text"
    ),
    "snow": (
        "Minimalist weather icon, light grey cloud with 4 crisp white six-pointed snowflakes, "
        "flat bold design, deep navy blue background, centered, no text"
    ),
    "thunderstorm": (
        "Minimalist weather icon, dark purple-grey storm cloud with one thick bright yellow "
        "lightning bolt, flat bold design, deep navy blue background, centered, no text"
    ),
    "fog": (
        "Minimalist weather icon, five horizontal wavy fog bands in light blue-grey, "
        "softly layered, flat bold design, deep navy blue background, centered, no text"
    ),
}


# ── Helpers ───────────────────────────────────────────────
def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def blend(r, g, b, a):
    alpha = a / 255.0
    return (
        int(r * alpha + BG_R * (1 - alpha)),
        int(g * alpha + BG_G * (1 - alpha)),
        int(b * alpha + BG_B * (1 - alpha)),
    )


def img_to_rgb565(img: Image.Image) -> list[int]:
    img = img.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS).convert("RGBA")
    pixels = []
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            r, g, b, a = img.getpixel((x, y))
            r, g, b = blend(r, g, b, a)
            pixels.append(rgb_to_rgb565(r, g, b))
    return pixels


def call_api(client: OpenAI, name: str, prompt: str) -> bytes | None:
    print(f"  Calling API for '{name}'...", flush=True)

    # gpt-image-2: always returns b64_json, no response_format needed
    try:
        resp = client.images.generate(
            model="gpt-image-2",
            prompt=prompt,
            n=1,
            size="1024x1024",
            quality="medium",
        )
        item = resp.data[0]
        if item.b64_json:
            print("    ✓ gpt-image-2", flush=True)
            return base64.b64decode(item.b64_json)
        if item.url:
            print("    ✓ gpt-image-2 (url)", flush=True)
            with urllib.request.urlopen(item.url) as r:
                return r.read()
    except Exception as e:
        print(f"    gpt-image-2 failed: {e}", flush=True)

    # dall-e-3 fallback: response_format="url" then download
    try:
        resp = client.images.generate(
            model="dall-e-3",
            prompt=prompt,
            n=1,
            size="1024x1024",
            response_format="url",
        )
        url = resp.data[0].url
        print("    ✓ dall-e-3 (url)", flush=True)
        with urllib.request.urlopen(url) as r:
            return r.read()
    except Exception as e:
        print(f"    dall-e-3 failed: {e}", flush=True)

    return None


def write_header(icons: dict[str, list[int]], path: Path) -> None:
    lines = [
        "// Auto-generated by tools/generate_icons.py — do not edit",
        f"// {len(icons)} weather icons, {ICON_SIZE}x{ICON_SIZE} px, RGB565",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for name, pixels in icons.items():
        lines.append(f"// {name}")
        lines.append(f"static const uint16_t icon_{name}[{len(pixels)}] = {{")
        for i in range(0, len(pixels), 16):
            chunk = pixels[i:i + 16]
            lines.append("  " + ", ".join(f"0x{p:04X}" for p in chunk) + ",")
        lines.append("};")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main():
    client = OpenAI(api_key=API_KEY)
    png_dir = Path("tools/icons_png")
    png_dir.mkdir(parents=True, exist_ok=True)
    out    = Path("include/weather_icons.h")

    icons = {}
    for name, prompt in ICONS.items():
        png = png_dir / f"{name}.png"
        if png.exists():
            print(f"  Using cached '{name}'", flush=True)
            raw = png.read_bytes()
        else:
            raw = call_api(client, name, prompt)
            if not raw:
                print(f"  Skipping '{name}'", flush=True)
                continue
            png.write_bytes(raw)

        img = Image.open(io.BytesIO(raw))
        icons[name] = img_to_rgb565(img)
        print(f"  ✓ converted '{name}'", flush=True)

    if not icons:
        print("No icons generated."); sys.exit(1)

    write_header(icons, out)
    kb = out.stat().st_size // 1024
    print(f"\n✓ {out}  ({kb} KB, {len(icons)} icons)")


if __name__ == "__main__":
    main()
