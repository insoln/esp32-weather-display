# ESP32-S3 Weather Station

A premium Wi-Fi weather station built around the **Freenove ESP32-S3 WROOM** and a **3.5" ILI9488 SPI TFT display** (480×320).  
Features cinematic AI-generated weather backdrops, full Cyrillic/Russian support, temperature-aware scenes, and a built-in web UI for city management.

---

## Features

- **16 cinematic backdrops** — AI-generated via GPT Image 2, selected by weather condition, time of day, and temperature
- **Temperature-aware scenes** — cold/hot variants for clear-sky days and nights
- **Russian / Cyrillic UI** — weather descriptions and city names rendered in a custom bitmap font
- **Dynamic city management** — add and remove cities at runtime via the on-device web UI (`http://<device-ip>/`)
- **Touch city picker** — tap to cycle cities, hold 700 ms to confirm; no SPI coordinates needed
- **Transparent frost bar** — bottom stats strip (humidity · feels-like · wind) blends into the backdrop
- **LittleFS persistence** — city list and current selection survive reboots

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | Freenove ESP32-S3 WROOM (8 MB PSRAM, 16 MB Flash) |
| Display | 3.5" ILI9488 SPI TFT, 480×320, with XPT2046 touch controller |
| Backlight | GPIO 38 via LEDC PWM |
| Touch IRQ | GPIO 21 (`T_IRQ`) — used for touch detection only |

### Wiring (SPI)

| TFT Pin | ESP32-S3 GPIO |
|---------|--------------|
| CLK / SCK | 12 |
| MOSI / SDA | 11 |
| MISO | 13 |
| CS (display) | 10 |
| DC / RS | 9 |
| RST | 8 |
| BL | 38 |
| T_IRQ | 21 |
| T_CS | 47 |

> **Note:** `Touch_XPT2046` inside the LGFX class causes a black screen on this hardware. Touch IRQ detection uses `digitalRead(T_IRQ_PIN)` directly instead.

---

## Setup

### 1. Clone and install dependencies

```bash
git clone https://github.com/insoln/esp32-weather-display.git
cd esp32-weather-display
```

[PlatformIO](https://platformio.org/) handles all library dependencies automatically (`LovyanGFX`, `ArduinoJson`).

### 2. Create your secrets file

```bash
cp include/secrets.h.example include/secrets.h
```

Edit `include/secrets.h` and fill in:

- `WIFI_SSID` / `WIFI_PASS` — your Wi-Fi network
- `OWM_API_KEY` — free API key from [openweathermap.org](https://openweathermap.org/api)
- `WEATHER_CITY` / `WEATHER_CITY_DISPLAY` — default boot city (e.g. `"London,GB"` / `"London"`)
- `GMT_OFFSET_SEC` — UTC offset in seconds for the default city

### 3. Generate backdrop images

```bash
pip install openai pillow
export OPENAI_API_KEY=your-openai-key-here
python tools/generate_backgrounds.py
```

This creates 16 JPEG files in `data/bg/`. Generation costs roughly $0.50–$1.00 in OpenAI credits.  
Images are cached — delete a file to regenerate that scene only.

### 4. Flash firmware and filesystem

```bash
pio run --target uploadfs   # Upload LittleFS image (cities + backdrops)
pio run --target upload     # Upload firmware
```

---

## Web UI

Once connected to Wi-Fi, visit `http://<device-ip>/` in a browser to:

- **Switch** the active city instantly
- **Add** any city by OWM query string (e.g. `Tokyo,JP`) — timezone is resolved automatically
- **Delete** cities you no longer need

Up to 24 cities can be stored.

---

## Backdrop Scenes

| File | Condition |
|------|-----------|
| `clear_day.jpg` | Clear sky, day, mild temperature |
| `clear_day_cold.jpg` | Clear sky, day, < 5 °C |
| `clear_day_hot.jpg` | Clear sky, day, > 27 °C |
| `clear_night.jpg` | Clear sky, night |
| `clear_night_cold.jpg` | Clear sky, night, < 3 °C |
| `clear_dawn.jpg` | Sunrise / dawn |
| `clear_sunset.jpg` | Sunset |
| `cloudy_day.jpg` | Overcast, day |
| `cloudy_night.jpg` | Overcast, night |
| `rain_light.jpg` | Light rain / drizzle (id 500–501, 520) |
| `rain.jpg` | Heavy rain |
| `drizzle.jpg` | Drizzle (id 300–399) |
| `snow.jpg` | Snow |
| `thunderstorm.jpg` | Thunderstorm |
| `fog.jpg` | Fog |
| `haze.jpg` | Haze / dust / smoke |

---

## Default Cities

Eight cities are pre-loaded on first boot to showcase the full range of backdrops:

| City | Time zone |
|------|-----------|
| Missaglia (LC), Italy | UTC+2 |
| Moscow, Russia | UTC+3 |
| Singapore | UTC+8 |
| London, UK | UTC+0 |
| Dubai, UAE | UTC+4 |
| Murmansk, Russia | UTC+3 |
| Bangkok, Thailand | UTC+7 |
| Reykjavik, Iceland | UTC+0 |

You can add or remove cities at any time via the web UI.

---

## Project Structure

```
├── src/
│   └── main.cpp            # All firmware — display, weather fetch, web server, touch
├── include/
│   ├── secrets.h.example   # Template — copy to secrets.h and fill in
│   └── ui_font.h           # Auto-generated bitmap font (ASCII + Cyrillic + °)
├── tools/
│   ├── generate_font.py    # Regenerates ui_font.h from a TTF file
│   └── generate_backgrounds.py  # Generates backdrop JPEGs via GPT Image 2
├── data/
│   └── bg/                 # 16 backdrop JPEG files (generated, not tracked in git)
├── partitions.csv          # Custom partition table: 3 MB app + 4.9 MB LittleFS
└── platformio.ini
```

---

## License

MIT
