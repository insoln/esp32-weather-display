#include <LittleFS.h>      // must come BEFORE LovyanGFX so it sees _LITTLEFS_H_
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <time.h>
#include <math.h>
#include "driver/spi_master.h"

#include "secrets.h"
#include "ui_font.h"

// ═══════════════════════════════════════════════════════════
// Display  —  ILI9488 SPI on Freenove ESP32-S3 WROOM
//   CS→GPIO10  RST→GPIO8  DC→GPIO9
//   MOSI→GPIO11  SCK→GPIO12  MISO→GPIO13  LED→GPIO38
//   Touch: XPT2046  T_CS→GPIO5  T_IRQ→GPIO21
// ═══════════════════════════════════════════════════════════
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI       _bus;
public:
    LGFX() {
        { auto cfg = _bus.config();
          cfg.spi_host = SPI2_HOST; cfg.spi_mode = 0;
          cfg.freq_write = 40000000; cfg.freq_read = 16000000;
          cfg.spi_3wire = false; cfg.use_lock = true;
          cfg.dma_channel = SPI_DMA_CH_AUTO;
          cfg.pin_sclk = 12; cfg.pin_mosi = 11;
          cfg.pin_miso = 13; cfg.pin_dc   = 9;
          _bus.config(cfg); _panel.setBus(&_bus); }
        { auto cfg = _panel.config();
          cfg.pin_cs = 10; cfg.pin_rst = 8; cfg.pin_busy = -1;
          cfg.panel_width = 320; cfg.panel_height = 480;
          cfg.readable = false; cfg.invert = false;
          cfg.rgb_order = false; cfg.dlen_16bit = false;
          cfg.bus_shared = true;
          _panel.config(cfg); }
        setPanel(&_panel);
    }
};

static LGFX tft;
static lgfx::LGFX_Sprite scr(&tft);
static bool spriteOK = false;

static WebServer webServer(80);

// ── Backlight PWM  (LED → GPIO38, LEDC channel 7) ─────────
static const int LED_PIN     = 38;
static const int LED_CHANNEL = 7;

void initBacklight() {
    ledcSetup(LED_CHANNEL, 5000, 8);
    ledcAttachPin(LED_PIN, LED_CHANNEL);
    ledcWrite(LED_CHANNEL, 255);
}

int getTimePeriod();  // forward declaration

void applyBrightness() {
    static const uint8_t LUT[] = {60, 165, 255, 165};
    ledcWrite(LED_CHANNEL, LUT[getTimePeriod()]);
}

// ── Layout constants ──────────────────────────────────────
static const int SW = 480, SH = 320;
static const int SCRIM_H     = 100;
static const int SCRIM_ALPHA = 195;
static const int FROST_Y     = 242;
static const int FROST_H     = SH - FROST_Y;

// ── PSRAM overlay buffers ──────────────────────────────────
static lgfx::bgra8888_t* top_scrim_buf = nullptr;
static lgfx::bgra8888_t* bot_frost_buf = nullptr;
static lgfx::bgra8888_t* glyph_buf     = nullptr;
static const int GLYPH_W = 90, GLYPH_H = 135;

// ── City list  (dynamic, stored in /cities.json) ──────────
struct City { String api, name; long gmt_off; };

static const int MAX_CITIES = 24;
static City   gCities[MAX_CITIES];
static int    gNCities    = 0;
static int    g_cityIdx   = 0;
static String g_cityApi   = WEATHER_CITY;
static String g_cityDisplay;
static long   g_gmtOff    = GMT_OFFSET_SEC;

// ── Weather data ──────────────────────────────────────────
struct Weather {
    float  temp = 0, feelsLike = 0, humidity = 0, windSpeed = 0;
    String desc;
    int    id    = 800;
    bool   valid = false;
};
static Weather wx;
static unsigned long lastFetch = 0, lastClockUpd = 0;
static const unsigned long UPDATE_MS = 600000UL;

// ═══════════════════════════════════════════════════════════
//  Touch  —  T_IRQ → GPIO21 (reliable)  +  SPI coords (best-effort)
// ═══════════════════════════════════════════════════════════
static const int T_IRQ_PIN = 21;
static spi_device_handle_t xpt_handle = nullptr;

void initTouch() {
    // T_IRQ: active-low interrupt — tells us IS screen touched
    pinMode(T_IRQ_PIN, INPUT_PULLUP);

    // Try adding XPT2046 to the same SPI2 bus for coordinate reads
    spi_device_interface_config_t cfg = {};
    cfg.clock_speed_hz = 2000000;
    cfg.mode           = 0;
    cfg.spics_io_num   = 5;
    cfg.queue_size     = 1;
    if (spi_bus_add_device(SPI2_HOST, &cfg, &xpt_handle) != ESP_OK) {
        xpt_handle = nullptr;
        Serial.println("[Touch] coords: SPI unavailable, using left/right split");
    } else {
        Serial.println("[Touch] coords: SPI OK");
    }
}

// Is the screen currently being touched? (fast GPIO check, no SPI)
bool isTouching() {
    return digitalRead(T_IRQ_PIN) == LOW;
}

// Read X coordinate via SPI when touching. Returns -1 if unavailable.
static int readTouchX() {
    if (!xpt_handle) return -1;
    uint8_t tx[3] = {0xD0, 0, 0}, rx[3] = {0, 0, 0};
    spi_transaction_t t = {};
    t.length = 24; t.tx_buffer = tx; t.rx_buffer = rx;
    if (spi_device_transmit(xpt_handle, &t) != ESP_OK) return -1;
    int raw = ((rx[1] << 5) | (rx[2] >> 3));
    if (raw < 100 || raw > 3950) return -1;
    return constrain(map(raw, 300, 3900, 0, SW - 1), 0, SW - 1);
}

// ═══════════════════════════════════════════════════════════
//  City persistence  —  /cities.json  +  /config.json
// ═══════════════════════════════════════════════════════════
static void applyCityIdx(int idx) {
    if (idx < 0 || idx >= gNCities) idx = 0;
    g_cityIdx     = idx;
    g_cityApi     = gCities[idx].api;
    g_cityDisplay = gCities[idx].name;
    g_gmtOff      = gCities[idx].gmt_off;
}

static void addDefaultCities() {
    struct { const char* api; const char* name; long gmt; } D[] = {
        {"Missaglia,IT",  "Missaglia (LC)",  7200},
        {"Moscow,RU",     "Москва",         10800},
        {"Singapore,SG",  "Singapore",      28800},
        {"London,GB",     "London",             0},
        {"Dubai,AE",      "Dubai",          14400},
        {"Murmansk,RU",   "Мурманск",       10800},
        {"Bangkok,TH",    "Bangkok",        25200},
        {"Reykjavik,IS",  "Reykjavik",          0},
    };
    for (auto& d : D) {
        if (gNCities >= MAX_CITIES) break;
        gCities[gNCities++] = {d.api, d.name, d.gmt};
    }
}

void saveCities() {
    File f = LittleFS.open("/cities.json", "w");
    if (!f) return;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < gNCities; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["api"]  = gCities[i].api;
        o["name"] = gCities[i].name;
        o["gmt"]  = gCities[i].gmt_off;
    }
    serializeJson(doc, f);
    f.close();
}

void saveCityConfig() {
    File f = LittleFS.open("/config.json", "w");
    if (!f) return;
    JsonDocument doc;
    doc["idx"] = g_cityIdx;
    serializeJson(doc, f);
    f.close();
}

void loadCityConfig() {
    gNCities = 0;
    // Load city list
    if (LittleFS.exists("/cities.json")) {
        File f = LittleFS.open("/cities.json", "r");
        if (f) {
            JsonDocument doc;
            if (!deserializeJson(doc, f)) {
                for (JsonObject o : doc.as<JsonArray>()) {
                    if (gNCities >= MAX_CITIES) break;
                    gCities[gNCities++] = {
                        o["api"].as<const char*>(),
                        o["name"].as<const char*>(),
                        (long)(o["gmt"] | 0)
                    };
                }
            }
            f.close();
        }
    }
    if (gNCities == 0) { addDefaultCities(); saveCities(); }

    // Load current city selection
    int idx = 0;
    if (LittleFS.exists("/config.json")) {
        File f = LittleFS.open("/config.json", "r");
        if (f) {
            JsonDocument doc;
            if (!deserializeJson(doc, f)) idx = doc["idx"] | 0;
            f.close();
        }
    }
    applyCityIdx(constrain(idx, 0, gNCities - 1));
    Serial.printf("[City] %s  gmt=%ld\n", g_cityDisplay.c_str(), g_gmtOff);
}

// ═══════════════════════════════════════════════════════════
//  Network helpers
// ═══════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) delay(300);
    Serial.printf("[WiFi] %s\n",
        WiFi.status() == WL_CONNECTED ? "Connected" : "Failed");
}

bool fetchWeather() {
    if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(3000); }
    if (WiFi.status() != WL_CONNECTED) return false;

    String url = "http://api.openweathermap.org/data/2.5/weather?q=";
    url += g_cityApi;
    url += "&units=metric&lang=ru&appid=";
    url += OWM_API_KEY;

    HTTPClient http;
    http.begin(url); http.setTimeout(10000);
    if (http.GET() != 200) { http.end(); return false; }

    JsonDocument doc;
    bool ok = !deserializeJson(doc, http.getStream());
    http.end();
    if (!ok) return false;

    wx.temp      = doc["main"]["temp"];
    wx.feelsLike = doc["main"]["feels_like"];
    wx.humidity  = doc["main"]["humidity"];
    wx.windSpeed = doc["wind"]["speed"];
    wx.desc      = doc["weather"][0]["description"].as<const char*>();
    wx.id        = doc["weather"][0]["id"];
    wx.valid     = true;
    // Capitalize first character — handles UTF-8 Cyrillic (2-byte) and ASCII
    if (wx.desc.length() >= 1) {
        uint8_t b0 = (uint8_t)wx.desc[0];
        if (b0 < 0x80) {
            wx.desc[0] = (char)toupper(b0);
        } else if (wx.desc.length() >= 2) {
            uint8_t b1 = (uint8_t)wx.desc[1];
            if (b0 == 0xD0 && b1 >= 0xB0 && b1 <= 0xBF)       // а-п → А-П
                wx.desc[1] = (char)(b1 - 0x20);
            else if (b0 == 0xD1 && b1 >= 0x80 && b1 <= 0x8F)  // р-я → Р-Я
                { wx.desc[0] = (char)0xD0; wx.desc[1] = (char)(b1 + 0x20); }
            else if (b0 == 0xD1 && b1 == 0x91)                 // ё → Ё
                { wx.desc[0] = (char)0xD0; wx.desc[1] = (char)0x81; }
        }
    }
    Serial.printf("[WX] %.1f C  %s  id=%d\n", wx.temp, wx.desc.c_str(), wx.id);
    return true;
}

int getTimePeriod() {
    struct tm t;
    if (!getLocalTime(&t, 60)) return 2;
    int h = t.tm_hour;
    if (h < 5)  return 0;
    if (h < 8)  return 1;
    if (h < 18) return 2;
    if (h < 21) return 3;
    return 0;
}

const char* selectBackdrop(int id, int period, float temp) {
    if (id == 800) {
        if (period == 0)
            return (temp < 3.0f) ? "/bg/clear_night_cold.jpg" : "/bg/clear_night.jpg";
        if (period == 1) return "/bg/clear_dawn.jpg";
        if (period == 3) return "/bg/clear_sunset.jpg";
        // Day: temperature-aware
        if (temp < 5.0f)  return "/bg/clear_day_cold.jpg";
        if (temp > 27.0f) return "/bg/clear_day_hot.jpg";
        return "/bg/clear_day.jpg";
    }
    if (id >= 801 && id <= 804) return (period <= 1) ? "/bg/cloudy_night.jpg" : "/bg/cloudy_day.jpg";
    if (id >= 300 && id < 400)                 return "/bg/drizzle.jpg";
    if (id == 500 || id == 501 || id == 520)   return "/bg/rain_light.jpg";
    if (id >= 500 && id < 600)                 return "/bg/rain.jpg";
    if (id >= 600 && id < 700)  return "/bg/snow.jpg";
    if (id >= 200 && id < 300)  return "/bg/thunderstorm.jpg";
    if (id == 741)               return "/bg/fog.jpg";
    if (id >= 700 && id < 800)  return "/bg/haze.jpg";
    return "/bg/cloudy_day.jpg";
}

// ═══════════════════════════════════════════════════════════
//  Font rendering  (UTF-8 aware, supports Cyrillic U+0400+)
// ═══════════════════════════════════════════════════════════

// Decode next Unicode codepoint from a UTF-8 string; advances *pp
static uint16_t nextCP(const char** pp) {
    uint8_t c = (uint8_t)**pp; (*pp)++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) {          // 2-byte (U+0080..U+07FF, includes Cyrillic)
        uint16_t cp = (c & 0x1F) << 6;
        if (**pp) { cp |= ((uint8_t)**pp & 0x3F); (*pp)++; }
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {          // 3-byte (U+0800..U+FFFF)
        uint16_t cp = (c & 0x0F) << 12;
        if (**pp) { cp |= ((uint8_t)**pp & 0x3F) << 6; (*pp)++; }
        if (**pp) { cp |= ((uint8_t)**pp & 0x3F);       (*pp)++; }
        return cp;
    }
    return (uint16_t)'?';
}

const UIGlyphInfo* getGlyph(const UIFont& f, uint16_t cp) {
    for (int i = 0; i < f.count; i++)
        if (f.glyphs[i].ch == cp) return &f.glyphs[i];
    return nullptr;
}

int textWidth(const UIFont& f, const char* s) {
    int w = 0;
    while (*s) {
        const UIGlyphInfo* g = getGlyph(f, nextCP(&s));
        w += g ? g->advance : (f.height / 3);
    }
    return w;
}

static bool fillGlyphBuf(const UIGlyphInfo* g, const uint8_t* data,
                         uint8_t r, uint8_t gr, uint8_t b, uint8_t aMul) {
    if (!glyph_buf || g->w > GLYPH_W || g->h > GLYPH_H) return false;
    const uint8_t* src = data + g->data_offset;
    lgfx::bgra8888_t* dst = glyph_buf;
    int n = (int)g->w * (int)g->h;
    for (int i = 0; i < n; i++, src++, dst++) {
        uint8_t a = (uint8_t)(((uint16_t)*src * (uint16_t)aMul) >> 8);
        dst->r = r; dst->g = gr; dst->b = b; dst->a = a;
    }
    return true;
}

int renderChar(lgfx::LGFX_Sprite& spr, const UIFont& f, uint16_t cp,
               int px, int py, uint8_t r, uint8_t g, uint8_t b,
               bool shadow = true) {
    const UIGlyphInfo* gi = getGlyph(f, cp);
    if (!gi) return f.height / 3;
    if (shadow && fillGlyphBuf(gi, f.data, 0, 0, 0, 150))
        spr.pushAlphaImage(px + gi->xoff + 2, py + gi->yoff + 3, gi->w, gi->h, glyph_buf);
    if (fillGlyphBuf(gi, f.data, r, g, b, 255))
        spr.pushAlphaImage(px + gi->xoff, py + gi->yoff, gi->w, gi->h, glyph_buf);
    return gi->advance;
}

int drawStr(lgfx::LGFX_Sprite& spr, const UIFont& f, const char* s,
            int x, int y,
            uint8_t r = 255, uint8_t g = 255, uint8_t b = 255,
            bool shadow = true) {
    int pen = x;
    while (*s) pen += renderChar(spr, f, nextCP(&s), pen, y, r, g, b, shadow);
    return pen - x;
}

void drawStrCentered(lgfx::LGFX_Sprite& spr, const UIFont& f, const char* s,
                     int cx, int y,
                     uint8_t r = 255, uint8_t g = 255, uint8_t b = 255,
                     bool shadow = false) {
    drawStr(spr, f, s, cx - textWidth(f, s) / 2, y, r, g, b, shadow);
}

void drawStrRight(lgfx::LGFX_Sprite& spr, const UIFont& f, const char* s,
                  int rx, int y,
                  uint8_t r = 255, uint8_t g = 255, uint8_t b = 255,
                  bool shadow = true) {
    drawStr(spr, f, s, rx - textWidth(f, s), y, r, g, b, shadow);
}

// ═══════════════════════════════════════════════════════════
//  Screen composition
// ═══════════════════════════════════════════════════════════
void composeScreen() {
    const char* bgPath = selectBackdrop(wx.valid ? wx.id : 800, getTimePeriod(), wx.temp);
    if (LittleFS.exists(bgPath))
        scr.drawJpgFile(LittleFS, bgPath, 0, 0, SW, SH);
    else {
        Serial.printf("[BG] missing %s\n", bgPath);
        scr.fillScreen(lgfx::color888(6, 14, 42));
    }

    if (top_scrim_buf) scr.pushAlphaImage(0, 0, SW, SCRIM_H, top_scrim_buf);
    if (bot_frost_buf) scr.pushAlphaImage(0, FROST_Y, SW, FROST_H, bot_frost_buf);

    struct tm t;
    bool hasTime = getLocalTime(&t, 60);

    drawStr(scr, font_M, g_cityDisplay.c_str(), 18, 12, 255,255,255, true);

    if (hasTime) {
        char timeBuf[6];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
        drawStrRight(scr, font_M, timeBuf, SW - 16, 12, 255,255,255, true);

        static const char* mons[] = {
            "янв","фев","мар","апр","май","июн",
            "июл","авг","сен","окт","ноя","дек"};
        char dateBuf[12];
        snprintf(dateBuf, sizeof(dateBuf), "%d %s", t.tm_mday, mons[t.tm_mon]);
        drawStr(scr, font_S, dateBuf, 18, 12 + font_M.height + 5, 200,210,220, false);
    }

    if (wx.valid) {
        int ty = 88;
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%d°", (int)roundf(wx.temp));
        drawStr(scr, font_XL, tbuf, 18, ty, 255,255,255, true);

        int condY = ty + font_XL.height + 2;
        drawStr(scr, font_M, wx.desc.c_str(), 18, condY, 210,215,225, true);

        int labY = FROST_Y + 8, valY = labY + font_S.height + 4;
        int c1 = SW/6, c2 = SW/2, c3 = SW*5/6;
        drawStrCentered(scr, font_S, "ВЛАЖНОСТЬ", c1, labY, 160,210,255, false);
        drawStrCentered(scr, font_S, "ОЩУЩЕНИЕ",  c2, labY, 160,210,255, false);
        drawStrCentered(scr, font_S, "ВЕТЕР",     c3, labY, 160,210,255, false);

        char hbuf[8], fbuf[12], wbuf[16];
        snprintf(hbuf, sizeof(hbuf), "%d%%",     (int)wx.humidity);
        snprintf(fbuf, sizeof(fbuf), "%d°",      (int)roundf(wx.feelsLike));  // UTF-8 degree
        snprintf(wbuf, sizeof(wbuf), "%.1f м/с", wx.windSpeed);
        drawStrCentered(scr, font_M, hbuf, c1, valY, 255,255,255, false);
        drawStrCentered(scr, font_M, fbuf, c2, valY, 255,255,255, false);
        drawStrCentered(scr, font_M, wbuf, c3, valY, 255,255,255, false);
    }

    if (spriteOK) scr.pushSprite(0, 0);
    applyBrightness();
}

void splashLine(const char* msg) {
    if (spriteOK) {
        scr.fillRoundRect(40, 142, 400, 44, 10, lgfx::color888(8,12,30));
        drawStrCentered(scr, font_M, msg, SW/2, 148, 200,210,230, false);
        scr.pushSprite(0, 0);
    }
}

// ═══════════════════════════════════════════════════════════
//  City picker  —  long press (1.5 s) triggers this
//
//  Touch detection: T_IRQ (GPIO21) — reliable, no SPI needed
//  Coordinates:     SPI via xpt_handle — best-effort
//    • If SPI works: left tile = Missaglia, right tile = Moscow
//    • If SPI fails: any tap toggles to the other city
// ═══════════════════════════════════════════════════════════
static void applyCity(int idx) {
    applyCityIdx(idx);
    saveCityConfig();
    char msg[64];
    snprintf(msg, sizeof(msg), "Переключаю на %s...", gCities[idx].name.c_str());
    splashLine(msg);
    configTime(g_gmtOff, 0, "pool.ntp.org");
    struct tm t;
    for (int i = 0; i < 5 && !getLocalTime(&t, 500); i++) delay(600);
    fetchWeather();
}

void showCityPicker() {
    int highlighted = g_cityIdx;

    char ipLine[48];
    {
        String ip = WiFi.localIP().toString();
        snprintf(ipLine, sizeof(ipLine), "http://%s/", ip.c_str());
    }

    auto drawPicker = [&]() {
        scr.fillScreen(lgfx::color888(8, 14, 38));
        drawStrCentered(scr, font_S, "тап - дальше,  держать - выбрать", SW/2, 14, 90,110,140, false);
        drawStrCentered(scr, font_S, ipLine, SW/2, 14 + font_S.height + 4, 60, 90, 130, false);

        // City name + region centered
        drawStrCentered(scr, font_M, gCities[highlighted].name.c_str(),
                        SW/2, SH/2 - font_M.height - 6, 255,255,255, false);

        // Page-indicator dots
        int dotY = SH - 22;
        int showDots = min(gNCities, 12);
        for (int i = 0; i < showDots; i++) {
            int dotX = SW/2 + (i - showDots/2) * 18 + 9;
            uint32_t col = (i == highlighted) ? lgfx::color888(100,150,255) : lgfx::color888(40,50,80);
            scr.fillCircle(dotX, dotY, (i == highlighted) ? 5 : 3, col);
        }
        scr.pushSprite(0, 0);
    };

    // Let go of the long-press first
    while (isTouching()) delay(20);
    drawPicker();

    const uint32_t HOLD_SELECT_MS = 700;   // hold this long to confirm
    const uint32_t CANCEL_IDLE_MS = 10000; // no touch → cancel

    uint32_t lastActivity = millis();
    while (millis() - lastActivity < CANCEL_IDLE_MS) {
        webServer.handleClient();
        if (!isTouching()) { delay(30); continue; }

        uint32_t holdStart = millis();
        while (isTouching()) delay(20);
        uint32_t held = millis() - holdStart;
        lastActivity = millis();

        if (held >= HOLD_SELECT_MS) {
            // Long hold → select highlighted city
            applyCity(highlighted);
            composeScreen();
            return;
        }
        // Short tap → advance to next city
        highlighted = (highlighted + 1) % gNCities;
        drawPicker();
    }

    composeScreen();  // timeout → cancel
}

// ═══════════════════════════════════════════════════════════
//  PSRAM buffer init
// ═══════════════════════════════════════════════════════════
void initPsramBuffers() {
    size_t sz = (size_t)SW * SCRIM_H * sizeof(lgfx::bgra8888_t);
    top_scrim_buf = (lgfx::bgra8888_t*)ps_malloc(sz);
    if (top_scrim_buf) {
        for (int y = 0; y < SCRIM_H; y++) {
            uint8_t a = (uint8_t)(SCRIM_ALPHA * (1.0f - (float)y / SCRIM_H));
            for (int x = 0; x < SW; x++) {
                auto& px = top_scrim_buf[y*SW+x];
                px.b = 0; px.g = 0; px.r = 0; px.a = a;
            }
        }
        Serial.println("[PSRAM] top_scrim OK");
    } else Serial.println("[PSRAM] top_scrim FAIL");

    sz = (size_t)SW * FROST_H * sizeof(lgfx::bgra8888_t);
    bot_frost_buf = (lgfx::bgra8888_t*)ps_malloc(sz);
    if (bot_frost_buf) {
        for (int i = 0; i < SW*FROST_H; i++) {
            auto& px = bot_frost_buf[i];
            px.b = 6; px.g = 8; px.r = 12; px.a = 80;   // translucent — backdrop visible through
        }
        Serial.println("[PSRAM] bot_frost OK");
    } else Serial.println("[PSRAM] bot_frost FAIL");

    sz = (size_t)GLYPH_W * GLYPH_H * sizeof(lgfx::bgra8888_t);
    glyph_buf = (lgfx::bgra8888_t*)ps_malloc(sz);
    if (glyph_buf) Serial.println("[PSRAM] glyph_buf OK");
    else           Serial.println("[PSRAM] glyph_buf FAIL");
}

// ═══════════════════════════════════════════════════════════
//  Web UI  — http://<ip>/   city picker in the browser
// ═══════════════════════════════════════════════════════════
static const char* WEB_CSS =
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
    "background:#070e24;color:#ccd;padding:24px 16px;max-width:460px;margin:auto}"
    "h1{color:#9ac4ff;font-size:20px;margin-bottom:18px}"
    ".row{display:flex;align-items:center;gap:10px;margin-bottom:10px}"
    "a.city{flex:1;padding:14px 16px;border-radius:12px;background:#0f1830;"
    "color:#dde;text-decoration:none;border:1.5px solid #1a2540}"
    "a.city.sel{background:#192d6a;border-color:#4878cc}"
    ".nm{font-size:17px;font-weight:600}"
    ".gmt{font-size:11px;color:#4a70a0;margin-top:3px}"
    "a.del{padding:10px 14px;border-radius:10px;background:#2a1020;"
    "color:#f08080;text-decoration:none;font-size:18px;line-height:1}"
    "hr{border:none;border-top:1px solid #1a2540;margin:20px 0}"
    "form{display:flex;flex-direction:column;gap:10px}"
    "input{padding:12px;border-radius:10px;border:1.5px solid #1a2540;"
    "background:#0a1228;color:#dde;font-size:14px}"
    "button{padding:12px;border-radius:10px;background:#1a3a80;"
    "color:#9ac4ff;border:none;font-size:15px;font-weight:600;cursor:pointer}"
    ".err{color:#f08080;font-size:13px;margin-top:-4px}";

static void handleWebRoot() {
    bool hasErr = webServer.hasArg("err");
    String html;
    html.reserve(2400 + gNCities * 160);
    html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Погода</title><style>";
    html += WEB_CSS;
    html += "</style></head><body><h1>Погода</h1>";
    for (int i = 0; i < gNCities; i++) {
        long h = gCities[i].gmt_off / 3600;
        char gmt[12];
        snprintf(gmt, sizeof(gmt), "UTC%+ld", h);
        html += "<div class='row'><a class='city";
        if (i == g_cityIdx) html += " sel";
        html += "' href='/set?idx="; html += i; html += "'>";
        html += "<div class='nm'>"; html += gCities[i].name; html += "</div>";
        html += "<div class='gmt'>"; html += gCities[i].api; html += " &nbsp;"; html += gmt; html += "</div>";
        html += "</a><a class='del' href='/del?idx="; html += i; html += "'>&#x2715;</a></div>";
    }
    html += "<hr><form action='/add' method='post'>";
    if (hasErr) html += "<div class='err'>Город не найден. Проверь формат: London,GB</div>";
    html += "<input name='api' placeholder='OWM запрос: London,GB' required>"
            "<input name='name' placeholder='Название (необязательно)'>"
            "<button type='submit'>+ Добавить город</button>"
            "</form></body></html>";
    webServer.send(200, "text/html; charset=utf-8", html);
}

static void handleWebSet() {
    int idx = webServer.hasArg("idx") ? webServer.arg("idx").toInt() : -1;
    if (idx < 0 || idx >= gNCities) { webServer.send(400); return; }
    if (idx != g_cityIdx) { applyCity(idx); composeScreen(); }
    webServer.sendHeader("Location", "/");
    webServer.send(302, "text/plain", "");
}

static void handleWebDel() {
    int idx = webServer.hasArg("idx") ? webServer.arg("idx").toInt() : -1;
    if (idx < 0 || idx >= gNCities || gNCities <= 1) {
        webServer.sendHeader("Location", "/"); webServer.send(302); return;
    }
    for (int i = idx; i < gNCities - 1; i++) gCities[i] = gCities[i+1];
    gNCities--;
    if (g_cityIdx >= gNCities) g_cityIdx = gNCities - 1;
    applyCityIdx(g_cityIdx);
    saveCities(); saveCityConfig();
    composeScreen();
    webServer.sendHeader("Location", "/");
    webServer.send(302, "text/plain", "");
}

static void handleWebAdd() {
    if (!webServer.hasArg("api") || webServer.arg("api").length() < 3) {
        webServer.sendHeader("Location", "/?err=1"); webServer.send(302); return;
    }
    String api  = webServer.arg("api"); api.trim();
    String name = webServer.arg("name"); name.trim();

    // Verify city with OWM and get timezone
    String url = "http://api.openweathermap.org/data/2.5/weather?q=";
    url += api; url += "&appid="; url += OWM_API_KEY;
    HTTPClient http; http.begin(url); http.setTimeout(8000);
    int code = http.GET();
    if (code != 200) { http.end(); webServer.sendHeader("Location","/?err=2"); webServer.send(302); return; }

    long gmt = 0;
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
        gmt = (long)(doc["timezone"] | 0);
        if (name.length() == 0) name = doc["name"].as<const char*>();
    }
    http.end();

    if (name.length() == 0) name = api;
    if (gNCities < MAX_CITIES) {
        gCities[gNCities++] = {api, name, gmt};
        saveCities();
    }
    webServer.sendHeader("Location", "/");
    webServer.send(302, "text/plain", "");
}

static void initWebServer() {
    webServer.on("/",    HTTP_GET,  handleWebRoot);
    webServer.on("/set", HTTP_GET,  handleWebSet);
    webServer.on("/del", HTTP_GET,  handleWebDel);
    webServer.on("/add", HTTP_POST, handleWebAdd);
    webServer.begin();
    Serial.printf("[Web] http://%s/\n", WiFi.localIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════
//  Setup & Loop
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n[Boot] Weather Display Premium v2");

    initBacklight();

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(0x0000);

    scr.setPsram(true);
    spriteOK = (scr.createSprite(SW, SH) != nullptr);
    Serial.printf("[Sprite] %s\n", spriteOK ? "OK (PSRAM)" : "FAIL");

    initPsramBuffers();

    if (spriteOK) {
        scr.fillScreen(lgfx::color888(8,14,38));
        drawStrCentered(scr, font_M, "Погода", SW/2, 140, 255,255,255, false);
        scr.pushSprite(0, 0);
    }

    if (!LittleFS.begin(false))
        Serial.println("[FS] LittleFS FAIL");
    else
        Serial.println("[FS] LittleFS OK");

    loadCityConfig();
    initTouch();

    splashLine("Подключение...");
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        splashLine("Синхронизация...");
        configTime(g_gmtOff, DST_OFFSET_SEC, "pool.ntp.org");
        struct tm t;
        for (int i = 0; i < 10 && !getLocalTime(&t, 500); i++) delay(600);

        splashLine("Загрузка погоды...");
        fetchWeather();

        initWebServer();
        // Show IP for 2 s so user can bookmark it
        char ipBuf[40];
        snprintf(ipBuf, sizeof(ipBuf), "http://%s/", WiFi.localIP().toString().c_str());
        splashLine(ipBuf);
        delay(2000);
    }

    composeScreen();
    lastFetch    = millis();
    lastClockUpd = millis();
}

static uint32_t  touchStartMs     = 0;
static bool      touchActive      = false;
static bool      longPressHandled = false;
static const uint32_t LONG_PRESS_MS = 1500;

void loop() {
    unsigned long now = millis();

    bool touching = isTouching();
    if (touching) {
        if (!touchActive) {
            touchActive      = true;
            touchStartMs     = now;
            longPressHandled = false;
        } else if (!longPressHandled && now - touchStartMs > LONG_PRESS_MS) {
            longPressHandled = true;
            showCityPicker();
            lastClockUpd = millis();
        }
    } else {
        touchActive = false;
    }

    webServer.handleClient();

    if (now - lastClockUpd >= 30000UL) { composeScreen(); lastClockUpd = now; }
    if (now - lastFetch    >= UPDATE_MS) {
        if (fetchWeather()) composeScreen();
        lastFetch = now;
    }

    delay(100);
}
