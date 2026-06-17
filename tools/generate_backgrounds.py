#!/usr/bin/env python3
"""
Generate full-screen cinematic weather backdrop images via GPT Image 2.
Output: data/bg/<name>.jpg  (480×320, JPEG q=90)
Cached — delete a file to regenerate it.

Usage:  python tools/generate_backgrounds.py
"""

import base64
import io
import os
import sys
from pathlib import Path

from openai import OpenAI
from PIL import Image

API_KEY = os.environ.get("OPENAI_API_KEY")
if not API_KEY:
    sys.exit("Error: set the OPENAI_API_KEY environment variable before running this script.")

TARGET_W, TARGET_H = 480, 320
JPEG_QUALITY = 90

# ── Composition rule injected into every prompt ─────────────────────────────
COMPOSE = (
    "CRITICAL COMPOSITION RULE: the horizon line must sit at the exact vertical "
    "center of the image — the top half is entirely sky, the bottom half is entirely "
    "landscape (hills, treetops, rooftops, water, or ground). No sky below the midpoint, "
    "no landscape above the midpoint. Ultra-wide 3:2 cinematic crop."
)

# ── Prompts ────────────────────────────────────────────────────────────────────
SCENES = {
    "clear_day": (
        f"{COMPOSE} "
        "Bright midday sun, vast pale blue sky with soft wispy cirrus clouds filling the top half, "
        "rolling green hills and meadows filling the bottom half, airy and minimal, warm light. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_day_cold": (
        f"{COMPOSE} "
        "Cold clear winter day, pale crisp blue sky with bright but low-angled winter sun filling "
        "the top half, bare frost-covered trees and lightly snow-dusted ground filling the bottom half, "
        "sharp wintry air, cool blue-white tones, minimal and serene. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_day_hot": (
        f"{COMPOSE} "
        "Hot sunny day, deep vivid blue sky with blazing high sun filling the top half, "
        "sun-baked lush landscape with palm trees or Mediterranean vegetation and subtle heat shimmer "
        "on the horizon filling the bottom half, vivid saturated warm tones, tropical mood. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_night_cold": (
        f"{COMPOSE} "
        "Clear cold winter night, deep blue-black starry sky with Milky Way filling the top half, "
        "moonlit snow-covered ground and frost-encrusted bare tree silhouettes filling the bottom half, "
        "cool blue-white ambient moonlight, crystalline and serene. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_dawn": (
        f"{COMPOSE} "
        "Delicate pink-orange sunrise gradient sky filling the top half, thin morning mist and "
        "dark silhouetted treetops filling the bottom half, pastel warm tones, serene and dreamlike. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_sunset": (
        f"{COMPOSE} "
        "Dramatic golden-amber sunset sky with rich orange-red gradients filling the top half, "
        "silhouetted rolling hills and trees filling the bottom half, long warm light rays. "
        "Photorealistic, no text, no people, no logos."
    ),
    "clear_night": (
        f"{COMPOSE} "
        "Clear deep blue-black night sky blazing with stars and the Milky Way filling the top half, "
        "faint silhouetted hills and trees filling the bottom half, cool moonlight ambient glow. "
        "Photorealistic, no text, no people, no logos."
    ),
    "cloudy_day": (
        f"{COMPOSE} "
        "Overcast sky with dramatic layered grey-white clouds filling the top half, soft diffused "
        "even lighting, muted green fields and treetops filling the bottom half, calm and melancholic. "
        "Photorealistic, no text, no people, no logos."
    ),
    "cloudy_night": (
        f"{COMPOSE} "
        "Dark overcast night sky with thick moving grey clouds lit faintly from below filling the top half, "
        "moody blue-grey atmosphere, dark silhouetted treetops filling the bottom half. "
        "Photorealistic, no text, no people, no logos."
    ),
    "rain_light": (
        "View through a mostly clear window glass with sparse light raindrops and thin water "
        "trickles on the surface, glass is mostly transparent; beyond: soft even grey overcast sky "
        "in the top half, blurred green trees and rooftops visible in the bottom half through "
        "the gentle steady drizzle, quiet melancholic rainy afternoon mood, cool grey-green tones. "
        "Photorealistic, no text, no people, no logos."
    ),
    "rain": (
        "View through a rain-covered window glass, large photorealistic water droplets and "
        "rivulets on the glass surface in sharp focus in the foreground covering the full frame, "
        "beyond the glass: dark brooding rain clouds in the upper half, heavy rainfall, "
        "wet rooftop silhouettes barely visible at the bottom half. "
        "Shallow depth of field, cinematic, moody dark blue-grey tones. "
        "Photorealistic, no text, no people, no logos."
    ),
    "drizzle": (
        "View through a window glass with fine misty condensation and tiny water droplets "
        "covering the full frame, beyond the glass in the top half: soft grey overcast sky, light drizzle, "
        "in the bottom half: dark wet foliage silhouette, cool melancholic tones. "
        "Photorealistic, no text, no people, no logos."
    ),
    "snow": (
        f"{COMPOSE} "
        "Gentle snowfall with soft white-grey sky and falling snowflakes filling the top half, "
        "snow-covered tree tops and white ground filling the bottom half, serene quiet palette. "
        "Photorealistic, no text, no people, no logos."
    ),
    "thunderstorm": (
        "View through a window glass covered in large heavy raindrops and water streams "
        "filling the full frame in sharp focus, beyond the glass: top half shows violent thunderstorm "
        "with dark purple-black storm clouds dramatically lit by lightning from within, "
        "bottom half shows rooftop silhouettes in driving rain. Intense and cinematic. "
        "Photorealistic, no text, no people, no logos."
    ),
    "fog": (
        f"{COMPOSE} "
        "Thick morning fog bank: top half shows misty pale grey sky barely distinct from fog, "
        "bottom half shows only tree tops and hilltops peeking just above the fog layer, "
        "mystical soft blue-grey tones, dreamy and atmospheric. "
        "Photorealistic, no text, no people, no logos."
    ),
    "haze": (
        f"{COMPOSE} "
        "Atmospheric heat haze and golden dust: top half shows warm amber-gold sky, "
        "bottom half shows distant mountains and flat landscape barely visible through the haze. "
        "Calm and dreamlike, warm tones. "
        "Photorealistic, no text, no people, no logos."
    ),
}


def call_api(client: OpenAI, name: str, prompt: str) -> bytes | None:
    print(f"  Generating '{name}'...", flush=True)
    try:
        resp = client.images.generate(
            model="gpt-image-2",
            prompt=prompt,
            n=1,
            size="1536x1024",
            quality="high",
        )
        item = resp.data[0]
        if item.b64_json:
            print("    ✓ gpt-image-2", flush=True)
            return base64.b64decode(item.b64_json)
        if item.url:
            import urllib.request
            print("    ✓ gpt-image-2 (url)", flush=True)
            with urllib.request.urlopen(item.url) as r:
                return r.read()
    except Exception as e:
        print(f"    ✗ failed: {e}", flush=True)
    return None


def process_image(raw: bytes) -> bytes:
    """Resize to 480×320 and encode as JPEG."""
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    img = img.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=JPEG_QUALITY, optimize=True, subsampling=0)
    return buf.getvalue()


def main():
    client = OpenAI(api_key=API_KEY)
    out_dir = Path("data/bg")
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = Path("tools/bg_raw")
    raw_dir.mkdir(parents=True, exist_ok=True)

    generated = 0
    for name, prompt in SCENES.items():
        jpg_path = out_dir / f"{name}.jpg"
        raw_path = raw_dir / f"{name}.png"

        if jpg_path.exists():
            print(f"  Using cached '{name}'", flush=True)
            generated += 1
            continue

        raw = None
        if raw_path.exists():
            print(f"  Using cached raw '{name}'", flush=True)
            raw = raw_path.read_bytes()
        else:
            raw = call_api(client, name, prompt)
            if raw is None:
                print(f"  Skipping '{name}'", flush=True)
                continue
            raw_path.write_bytes(raw)

        jpg_bytes = process_image(raw)
        jpg_path.write_bytes(jpg_bytes)
        kb = len(jpg_bytes) // 1024
        print(f"  ✓ '{name}' → {kb} KB", flush=True)
        generated += 1

    print(f"\n✓ {generated}/{len(SCENES)} backdrops ready in {out_dir}/")


if __name__ == "__main__":
    main()
