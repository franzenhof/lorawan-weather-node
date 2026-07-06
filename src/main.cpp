#include <Arduino.h>

/**
 * LoRaWAN Weather Node – Heltec WiFi LoRa V3
 *
 * Sensors:
 *   - Davis rain sensor     (reed switch, PCNT)
 *   - Davis wind sensor     (reed switch wind speed PCNT + potentiometer direction ADC)
 *   - BME280                (temperature, humidity, barometric pressure – I2C)
 *   - DS18B20 Bus 1         (up to 3 sensors, 1-Wire)
 *   - LiPo battery          (voltage measurement via voltage divider + ADC, optional)
 *
 * Transmission: CayenneLPP via LoRaWAN EU868 → own ChirpStack server
 *
 * Configuration: Hold user button (GPIO 0) during boot to open the Wi-Fi
 * portal (SSID: WeatherNode). On first boot (no DevEUI stored) the portal
 * opens automatically. All parameters are stored in NVS.
 *
 * Button operation:
 *   Press → turn on display for 20 s
 *
 * LoRaWAN downlink commands (FPort 1, 1 byte):
 *   0x01 → reset rain accumulator (since start)
 *   0x02 → restart device
 *   0x03 → open Wi-Fi config portal (takes effect after current send cycle)
 *   0x04 → enable debug mode (persistent)
 *   0x05 → disable debug mode (persistent)
 *   0x10 → set parameter(s): [0x10, paramId, valueMSB, valueLSB, ...]
 *           paramId 1..9 (signed 16-bit values, validated)
 *
 * LPP channel mapping:
 *   Ch  1 Direction        → wind direction (°, if wind sensor enabled)
 *   Ch  1 Analog Input     → avg wind speed (m/s or km/h, if wind sensor enabled)
 *   Ch  2 Analog Input     → wind gust max (m/s or km/h, if wind sensor enabled)
 *   Ch  3 Analog Input     → rain rate (mm/h, if rain sensor enabled)
 *   Ch200 Analog Input     → free heap (KiB, debug mode only)
 *   Ch204 Analog Input     → uptime (h, debug mode only)
 *   Ch  1 Distance         → rain current cycle (m; ×1000 → mm, if rain sensor enabled)
 *   Ch  2 Distance         → rain since start (m; ×1000 → mm, if rain sensor enabled)
 *   Ch  2 Digital Input    → status byte: Bit0=BME280, Bit1=Bus1, Bit2-5=errors, Bit6=wind unit
 *   Ch200 Digital Input    → cycle counter (0–255, debug mode only)
 *   Ch202 Digital Input    → send fail counter (0-255, debug mode only)
 *   Ch  1 Voltage          → LiPo voltage (V, optional)
 *   Ch  1 Rel. Humidity    → humidity (%, BME280)
 *   Ch  1 Baro. Pressure   → barometric pressure QNH (hPa, BME280)
 *   Ch  2 Baro. Pressure   → barometric pressure absolute (hPa, BME280)
 *   Ch  1 Temperature      → temperature (°C, BME280)
 *   Ch  2–4 Temperature    → DS18B20 Bus 1 sensor 0–2 (°C)
 *   Ch201 Temperature      → CPU temperature (°C, debug mode only)
 */

// ============================================================================
// FIXED PIN ASSIGNMENT (independent of PCB revision)
// ============================================================================
#define PIN_ADC_VOLTBATTERY     1   // LiPo voltage measurement (ADC)
#define PIN_ADC_WINDDIRECTION   2   // Davis wind direction (potentiometer, ADC)
#define PIN_ADC_BATCTRL        37   // HIGH enables ADC measurement path (from heltec wifi lora v3 board rev. 3.2)
#define PIN_1WIRE_HSS          47   // High-side switch VCC_1WIRE (LP0701N3-G via VN2222L, from lorawan-weather-node pcb rev 1.3)
#define PIN_I2C_SDA_BME280     41
#define PIN_I2C_SCL_BME280     42

// ============================================================================
// PCNT CONFIGURATION (4k7 pull-up, reed switch to GND, 100 nF low-pass filter)
// ============================================================================
#define PCNT_UNIT_WINDSPEED     PCNT_UNIT_0
#define PCNT_CHANNEL_WINDSPEED  PCNT_CHANNEL_0
#define FILTER_VALUE_WINDSPEED  100   // approx. 1.25 µs at APB=80 MHz → debounce

#define PCNT_UNIT_RAIN          PCNT_UNIT_1
#define PCNT_CHANNEL_RAIN       PCNT_CHANNEL_0
#define FILTER_VALUE_RAIN       100

// ============================================================================
// I2C / BME280
// ============================================================================
#define I2C_FREQUENCY           50000
#define I2C_ADRESS_BME280       0x76
#define BME280_DEFAULT_ALTITUDE 560

// DS18B20
#define MAX_SENSORS_PER_BUS     3

// ============================================================================
// INCLUDES
// ============================================================================
#include <heltec_unofficial.h>
#include <LoRaWAN_ESP32.h>
#include <CayenneLPP.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <pins_arduino.h>
#include "driver/pcnt.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <esp_netif.h>
#include <esp_event.h>
#include "generated_version.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

// ============================================================================
// CONFIGURATION (persistent via NVS / Preferences)
// ============================================================================
struct Config {
    uint64_t joinEui           = 0;
    uint64_t devEui            = 0;
    uint8_t  appKey[16]        = {};
    char     region[8]         = "EU868"; // LoRaWAN region (default)
    int      pcbVersion        = 13;  // 12 = Rev 1.2, 13 = Rev 1.3
    int      altitudeM         = BME280_DEFAULT_ALTITUDE;
    int      uplinkIntervalSec = 300;  // target uplink interval (s)
    int      sampleSec         = 3;    // sub-sample interval (s); <= uplinkIntervalSec
    int      windDirOffsetDeg  = 0;   // north offset (added to raw value, mod 360)
    int      windUnitMs         = 1;   // 1 = transmit wind in m/s, 0 = transmit in km/h
    int      windEn            = 1;   // 1 = Davis wind sensor enabled (direction + speed)
    int      rainEn            = 1;   // 1 = Davis rain sensor enabled
    int      lipoEn            = 0;   // 1 = LiPo measurement enabled (saves 4 LPP bytes when 0)
    int      debugMode         = 0;   // 1 = include debug telemetry in payload
    int      cpuFreqMhz        = 80;  // 80 or 240 MHz; below 80 not allowed (PCNT-APB)
};

// ============================================================================
// PIN ASSIGNMENT BY PCB REVISION (runtime)
// Rev 1.2: Wind=GPIO3, Rain=GPIO4, 1-Wire=GPIO5
// Rev 1.3: Wind=GPIO4, Rain=GPIO5, 1-Wire=GPIO6
// ============================================================================
struct PinSet { int windSpeed, rain, oneWire; };

static PinSet getPins(int pcbVersion)
{
    if (pcbVersion == 13) return { 4, 5, 6 };
    return { 3, 4, 5 };
}

// ============================================================================
// NVS UTILITY FUNCTIONS
// ============================================================================
static uint64_t hexToUint64(const char* s) { return strtoull(s, nullptr, 16); }

static void hexToBytes(const char* s, uint8_t* out, size_t maxLen)
{
    for (size_t i = 0; i < strlen(s) && (i / 2) < maxLen; i += 2) {
        char b[] = { s[i], s[i + 1], '\0' };
        out[i / 2] = (uint8_t)strtol(b, nullptr, 16);
    }
}

static void uint64ToHex(uint64_t value, char* out, size_t outSize)
{
    if (outSize > 0) {
        snprintf(out, outSize, "%016llX", (unsigned long long)value);
    }
}

static void bytesToHex(const uint8_t* bytes, size_t byteCount, char* out, size_t outSize)
{
    size_t pos = 0;
    if (outSize == 0) return;

    for (size_t i = 0; i < byteCount && (pos + 2) < outSize; ++i) {
        pos += snprintf(out + pos, outSize - pos, "%02X", bytes[i]);
    }
}

static bool isSupportedRegion(const String& region)
{
    static const char* const kRegions[] = {
        "EU868", "US915", "AU915", "AS923", "IN865", "KR920", "CN470", "RU864"
    };
    for (size_t i = 0; i < (sizeof(kRegions) / sizeof(kRegions[0])); ++i) {
        if (region == kRegions[i]) return true;
    }
    return false;
}

static void normalizeRegionInput(const char* raw, char* out, size_t outSize)
{
    String region = raw ? String(raw) : String("");
    region.trim();
    region.toUpperCase();
    region.replace(" ", "");

    if (!isSupportedRegion(region)) {
        region = "EU868";
    }
    strlcpy(out, region.c_str(), outSize);
}

static Config loadConfig()
{
    Preferences prefs;
    Config c;
    // On first boot the "config" namespace doesn't exist in NVS yet, so
    // begin() returns false.  Return the all-defaults Config (devEui == 0),
    // which causes setup() to open the Wi-Fi configuration portal.
    if (!prefs.begin("config", true)) {
        return c;
    }
    c.pcbVersion       = prefs.getInt    ("pcbver",   c.pcbVersion);
    c.altitudeM        = prefs.getInt    ("altitude", c.altitudeM);
    c.uplinkIntervalSec = prefs.getInt   ("uplinksec", prefs.getInt("delay", c.uplinkIntervalSec));
    c.sampleSec        = prefs.getInt    ("samplesec", c.sampleSec);
    c.windDirOffsetDeg = prefs.getInt    ("windoff",  c.windDirOffsetDeg);
    c.windUnitMs       = prefs.getInt    ("windunit", c.windUnitMs);
    c.windEn           = prefs.getInt    ("winden",   c.windEn);
    c.rainEn           = prefs.getInt    ("rainen",   c.rainEn);
    c.lipoEn           = prefs.getInt    ("lipoen",   c.lipoEn);
    c.debugMode        = prefs.getInt    ("debugmode", c.debugMode);
    c.cpuFreqMhz       = prefs.getInt    ("cpufreq",  c.cpuFreqMhz);
    c.devEui           = prefs.getULong64("deveui",   0);
    c.joinEui          = prefs.getULong64("joineui",  0);
    prefs.getBytes("appkey", c.appKey, 16);
    String region      = prefs.getString("region", c.region);
    normalizeRegionInput(region.c_str(), c.region, sizeof(c.region));
    prefs.end();
    return c;
}

static void saveConfig(const Config& c)
{
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putInt    ("pcbver",   c.pcbVersion);
    prefs.putInt    ("altitude", c.altitudeM);
    prefs.putInt    ("uplinksec", c.uplinkIntervalSec);
    prefs.putInt    ("samplesec", c.sampleSec);
    prefs.putInt    ("windoff",  c.windDirOffsetDeg);
    prefs.putInt    ("windunit", c.windUnitMs);
    prefs.putInt    ("winden",   c.windEn);
    prefs.putInt    ("rainen",   c.rainEn);
    prefs.putInt    ("lipoen",   c.lipoEn);
    prefs.putInt    ("debugmode", c.debugMode);
    prefs.putInt    ("cpufreq",  c.cpuFreqMhz);
    prefs.putULong64("deveui",   c.devEui);
    prefs.putULong64("joineui",  c.joinEui);
    prefs.putBytes  ("appkey",   c.appKey, 16);
    prefs.putString ("region",   c.region);
    prefs.end();
}

// ============================================================================
// TIME SYNCHRONIZATION
// (Placeholder – no time source available yet. Hook in here for future extension,
//  e.g. via LoRaWAN DeviceTimeReq or GPS.)
// ============================================================================

// ============================================================================
// WI-FI CONFIGURATION PORTAL
// ============================================================================
static Config runConfigPortal(const Config& cur)
{
    setCpuFrequencyMhz(240);
    display.displayOn();
    display.clear();
    display.drawString(0, 0, "Starting config portal...");
    display.display();

    char b_dev[17], b_join[17], b_key[33], b_region[8], b_pcb[4], b_alt[8], b_uplink[8], b_sample[4], b_windoff[6], b_windunit[2], b_debug[2], b_cpu[4];
    uint64ToHex(cur.devEui, b_dev, sizeof(b_dev));
    uint64ToHex(cur.joinEui, b_join, sizeof(b_join));
    bytesToHex(cur.appKey, 16, b_key, sizeof(b_key));
    strlcpy(b_region, cur.region, sizeof(b_region));
    itoa(cur.pcbVersion,       b_pcb,     10);
    itoa(cur.altitudeM,        b_alt,     10);
    itoa(cur.uplinkIntervalSec, b_uplink,  10);
    itoa(cur.sampleSec,        b_sample,  10);
    itoa(cur.windDirOffsetDeg, b_windoff, 10);
    itoa(cur.windUnitMs,       b_windunit, 10);
    itoa(cur.debugMode,        b_debug,   10);
    itoa(cur.cpuFreqMhz,       b_cpu,     10);

    WiFiManagerParameter p_dev    ("dev",      "DevEUI (16 hex chars)",                        b_dev,     17);
    WiFiManagerParameter p_join   ("join",     "JoinEUI (16 hex chars)",                       b_join,    17);
    WiFiManagerParameter p_key    ("key",      "AppKey (32 hex chars)",                        b_key,     33);
    WiFiManagerParameter p_region ("region",   "LoRaWAN region (EU868, US915, AU915, AS923, IN865, KR920, CN470, RU864)", b_region, 8);
    WiFiManagerParameter p_pcb    ("pcbver",   "PCB revision (12=Rev1.2, 13=Rev1.3)",          b_pcb,     3);
    WiFiManagerParameter p_alt    ("altitude", "Altitude above sea level in m (0-4000)",       b_alt,     6);
    WiFiManagerParameter p_uplink ("uplinksec","Uplink interval in s (5-3600 typical)",         b_uplink, 7);
    WiFiManagerParameter p_sample ("samplesec","Sample interval in s (1 to uplink interval)",  b_sample, 4);
    WiFiManagerParameter p_windoff("windoff",  "Wind direction offset in degrees (-180 to +180)", b_windoff, 5);
    WiFiManagerParameter p_windunit("windunit", "Wind speed unit (0=km/h, 1=m/s)",             b_windunit, 2);
    WiFiManagerParameter p_winden ("winden",   "Wind sensor enabled (0=No, 1=Yes)",            cur.windEn ? "1" : "0", 2);
    WiFiManagerParameter p_rainen ("rainen",   "Rain sensor enabled (0=No, 1=Yes)",            cur.rainEn ? "1" : "0", 2);
    WiFiManagerParameter p_lipo   ("lipoen",   "LiPo measurement enabled (0=No, 1=Yes)",       cur.lipoEn ? "1" : "0", 2);
    WiFiManagerParameter p_debug  ("debugmode","Debug mode (0=No, 1=Yes, sends extra telemetry)", b_debug, 2);
    WiFiManagerParameter p_cpu    ("cpufreq",  "CPU frequency in MHz (80 or 240)",             b_cpu,     4);
    WiFiManagerParameter p_section("<div style='margin:0.5rem 0 0.75rem 0;'><h3>LoRa-Weather-Node Parameter</h3><p style='margin:0 0 0.35rem 0;'>Firmware version: " FIRMWARE_VERSION "</p><p style='margin:0;'>Device settings are shown here on a separate page.</p></div>");

    WiFiManager wm;
    const char* menu[] = { "wifi", "info", "custom", "sep", "exit" };
    wm.setMenu(menu, 5);
    wm.setCustomMenuHTML("<div style='margin:0.75rem 0 1rem 0;'><div style='font-size:0.85rem;opacity:0.75;margin:0 0 0.25rem 0;'>Firmware version: " FIRMWARE_VERSION "</div><form action='/param' method='get'><button>LoRa-Weather-Node Parameter</button></form><br/><form action='/update' method='get'><button>Firmwareupdate</button></form></div>");

    wm.addParameter(&p_section);
    wm.addParameter(&p_dev);    wm.addParameter(&p_join);   wm.addParameter(&p_key); wm.addParameter(&p_region);
    wm.addParameter(&p_pcb);    wm.addParameter(&p_alt);    wm.addParameter(&p_uplink);
    wm.addParameter(&p_sample); wm.addParameter(&p_windoff);wm.addParameter(&p_windunit);
    wm.addParameter(&p_winden);
    wm.addParameter(&p_rainen); wm.addParameter(&p_lipo);   wm.addParameter(&p_debug); wm.addParameter(&p_cpu);

    // Update display once the AP is up and the IP address is known
    wm.setAPCallback([](WiFiManager*) {
        String ip = WiFi.softAPIP().toString();
        display.clear();
        display.drawString(0,  0, "Config Portal");
        display.drawString(0, 11, "SSID: WeatherNode");
        display.drawString(0, 22, "http://" + ip);
        display.drawString(0, 33, "OTA:  weather-node");
        display.drawString(0, 44, "1. Connect to SSID");
        display.drawString(0, 55, "2. Open URL in browser");
        display.display();
    });

    // ArduinoOTA running in parallel to the portal in its own task (firmware update without USB)
    ArduinoOTA.setHostname("weather-node");
    ArduinoOTA.onStart([]()  { Serial.println("OTA: Start"); });
    ArduinoOTA.onEnd([]()    { Serial.println("OTA: Done"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA Error %u\n", e); });
    ArduinoOTA.begin();
    TaskHandle_t otaHandle = nullptr;
    xTaskCreate([](void*) {
        for (;;) { ArduinoOTA.handle(); vTaskDelay(10 / portTICK_PERIOD_MS); }
    }, "OTA", 4096, nullptr, 1, &otaHandle);

    wm.startConfigPortal("WeatherNode");

    if (otaHandle) { vTaskDelete(otaHandle); ArduinoOTA.end(); }

    Config c = cur;
    if (strlen(p_dev.getValue())  == 16) c.devEui  = hexToUint64(p_dev.getValue());
    if (strlen(p_join.getValue()) == 16) c.joinEui = hexToUint64(p_join.getValue());
    if (strlen(p_key.getValue())  == 32) hexToBytes(p_key.getValue(), c.appKey, 16);
    normalizeRegionInput(p_region.getValue(), c.region, sizeof(c.region));
    int pcb = atoi(p_pcb.getValue());
    c.pcbVersion       = (pcb == 12) ? 12 : 13;
    if (atoi(p_alt.getValue()) > 0) c.altitudeM = atoi(p_alt.getValue());
    if (atoi(p_uplink.getValue()) > 0) c.uplinkIntervalSec = atoi(p_uplink.getValue());
    int ss = atoi(p_sample.getValue());
    c.sampleSec        = (ss > 0) ? min(ss, c.uplinkIntervalSec) : c.sampleSec;
    c.windDirOffsetDeg = atoi(p_windoff.getValue());
    c.windUnitMs       = (atoi(p_windunit.getValue()) == 0) ? 0 : 1;
    c.windEn           = atoi(p_winden.getValue());
    c.rainEn           = atoi(p_rainen.getValue());
    c.lipoEn           = atoi(p_lipo.getValue());
    c.debugMode        = (atoi(p_debug.getValue()) == 1) ? 1 : 0;
    c.cpuFreqMhz       = (atoi(p_cpu.getValue()) == 240) ? 240 : 80;
    return c;
}

// ============================================================================
// DISPLAY MANAGEMENT
// ============================================================================
static unsigned long displayUntilMs = 0;

static void activateDisplay(unsigned long durationMs = 20000)
{
    displayUntilMs = millis() + durationMs;
    display.displayOn();
}

static void updateDisplayState()
{
    if (displayUntilMs > 0 && millis() >= displayUntilMs) {
        displayUntilMs = 0;
        display.displayOff();
    }
}

// ============================================================================
// BUTTON HANDLER (GPIO 0, active LOW)
//   Short press (< 3 s, on release) → turn display on for 20 s
//   Long press  (≥ 3 s)            → open config portal, then restart
//
// Note: GPIO0 must NOT be held during power-on/reset (triggers bootloader).
//       The portal is therefore only opened via long press during operation,
//       or automatically on first boot (no DevEUI stored).
//
// An ISR on CHANGE captures press and release timestamps so that button
// events are never missed, even while sendLoRa() blocks for several seconds.
// The ISR only writes volatile variables; all actual actions happen in the
// main context via handleButton().
// ============================================================================
static Config gCfg;

static volatile unsigned long isrPressedAtMs  = 0;
static volatile unsigned long isrReleasedAtMs = 0;
static volatile bool          isrPressEvent   = false;
static volatile bool          isrReleaseEvent = false;

void IRAM_ATTR buttonISR()
{
    // Simple software debounce: ignore edges < 50 ms after the previous one
    static unsigned long lastEdgeMs = 0;
    unsigned long now = millis();
    if (now - lastEdgeMs < 50) return;
    lastEdgeMs = now;

    if (digitalRead(BUTTON) == LOW) {
        isrPressedAtMs = now;
        isrPressEvent  = true;
    } else {
        isrReleasedAtMs = now;
        isrReleaseEvent = true;
    }
}

static void handleButton()
{
    static unsigned long pressedAtMs = 0;
    static bool          longFired   = false;

    // Consume ISR press event (falling edge)
    if (isrPressEvent) {
        isrPressEvent = false;
        pressedAtMs   = isrPressedAtMs;
        longFired     = false;
    }

    // Consume ISR release event (rising edge)
    if (isrReleaseEvent) {
        isrReleaseEvent = false;
        if (!longFired && (isrReleasedAtMs - pressedAtMs) < 3000) {
            activateDisplay(20000);
        }
    }

    // Long press: button currently held for ≥ 3 s
    if (!longFired && pressedAtMs > 0 && (digitalRead(BUTTON) == LOW)
            && (millis() - pressedAtMs >= 3000)) {
        longFired = true;
        activateDisplay(5000);
        display.clear();
        display.drawString(0,  0, "Config Portal");
        display.drawString(0, 16, "starting...");
        display.display();
        delay(1000);
        gCfg = runConfigPortal(gCfg);
        saveConfig(gCfg);
        display.clear();
        display.drawString(0,  0, "Config saved!");
        display.drawString(0, 16, "Restarting...");
        display.display();
        delay(2000);
        ESP.restart();
    }
}

// Waits 'ms' milliseconds, polling the button every 50 ms.
// This ensures the button stays responsive during blocking setup loops.
static void waitWithButtonPoll(unsigned long ms)
{
    unsigned long start = millis();
    while (millis() - start < ms) {
        handleButton();
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// HARDWARE OBJECTS
// ============================================================================
LoRaWANNode*       loRaNode    = nullptr;
CayenneLPP         lpp(200);
TwoWire            Wire2(1);
Adafruit_BME280    bme;

OneWire*           oneWireBus1 = nullptr;
DallasTemperature* sensorsBus1 = nullptr;

// ============================================================================
// MEASUREMENT DATA
// ============================================================================
RTC_DATA_ATTR uint8_t loopCount         = 0;
RTC_DATA_ATTR float   rainMMSinceStart  = 0.0f;

static int16_t       pulsesLastCountWindspeed = 0;
static int16_t       pulsesLastCountRain      = 0;
static unsigned long previousMillis           = 0;
static int           sendFailCount            = 0;
static bool          portalRequestedViaDownlink = false;

static PinSet        gPins = { 3, 4, 5 };  // set in setup()

bool     bme280Present  = false;
float    bme280TempC    = 0.0f;
float    bme280PressAbs = 0.0f;
float    bme280PressSea = 0.0f;
float    bme280Humidity = 0.0f;

float    bus1Temps[MAX_SENSORS_PER_BUS];
int      bus1Count = 0;

float    windSpeedMs   = 0.0f;
float    windGustMs    = 0.0f;
float    rainMM        = 0.0f;
float    rainRateMmH   = 0.0f;
uint16_t windDirection = 0;
float    batteryVolt   = 0.0f;
float    cpuTemperature = 0.0f;

// ============================================================================
// SAMPLING ACCUMULATORS
// A sub-sample is taken every sampleSec seconds (max. uplinkIntervalSec).
// At the end of the uplink interval accumulators are reduced to final values.
// Wind direction: vector averaging (sin/cos) so that 350°+10° → 0° is correct.
// ============================================================================
static float         windDirSinSum    = 0.0f;
static float         windDirCosSum    = 0.0f;
static float         windSpeedSum     = 0.0f;
static float         bme280TempSum    = 0.0f;
static float         bme280PressAbsSum = 0.0f;
static float         bme280PressSeaSum = 0.0f;
static float         bme280HumSum     = 0.0f;
static float         rainCycleMm      = 0.0f;
static int           subSampleCount   = 0;
static unsigned long lastSampleMs     = 0;
static unsigned long lastSendMs       = 0;

// ============================================================================
// SENSOR READ FUNCTIONS
// ============================================================================
static int adcAverage(int pin, int samples = 8)
{
    int sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += analogRead(pin);
        delayMicroseconds(200);   // ESP32 ADC: ~200 µs settling time between samples
    }
    return sum / samples;
}

static float readBatteryVoltage()
{
    constexpr int   ADC_BITS   = 12;
    constexpr float ADC_MAX    = (1 << ADC_BITS) - 1;
    constexpr float ADC_VREF   = 3.3f;
    constexpr int   R_UPPER    = 390;   // R13
    constexpr int   R_LOWER    = 100;   // R14
    // Calibration measurement: 4.095 V measured when 4.2 V was applied
    constexpr float CAL_FACTOR = (ADC_VREF / ADC_MAX)
                                 * ((R_UPPER + R_LOWER) / (float)R_LOWER)
                                 * (4.2f / 4.095f);

    digitalWrite(PIN_ADC_BATCTRL, HIGH);
    delay(100);
    int raw = adcAverage(PIN_ADC_VOLTBATTERY);
    digitalWrite(PIN_ADC_BATCTRL, LOW);
    return CAL_FACTOR * raw;
}

static uint16_t readWindDirection()
{
    // Potentiometer powered by full VCC3.3, linear 0–4095 → 0–359°.
    // windDirOffsetDeg corrects the north alignment during installation.
    int raw = map(adcAverage(PIN_ADC_WINDDIRECTION), 0, 4095, 0, 359);
    return (uint16_t)((raw + gCfg.windDirOffsetDeg + 360) % 360);
}

static void readPulseCounters()
{
    constexpr float DAVIS_MS_PER_PPS = 1.00584f; // 2.25 mph per Hz converted to m/s
    int16_t       curWind = 0, curRain = 0;
    unsigned long now     = millis();
    unsigned long elapsed = now - previousMillis;
    previousMillis = now;

    if (gCfg.windEn) {
        pcnt_get_counter_value(PCNT_UNIT_WINDSPEED, &curWind);
        int16_t pulsesWind = curWind - pulsesLastCountWindspeed;
        pulsesLastCountWindspeed = curWind;
        float pps    = (float)pulsesWind / ((float)elapsed / 1000.0f);
        windSpeedMs  = pps * DAVIS_MS_PER_PPS;
        if (windSpeedMs > windGustMs) windGustMs = windSpeedMs;
        Serial.printf("Wind: %d pulses → %.1f m/s (gust %.1f)\n", pulsesWind, windSpeedMs, windGustMs);
    }

    if (gCfg.rainEn) {
        pcnt_get_counter_value(PCNT_UNIT_RAIN, &curRain);
        int16_t pulsesRain = curRain - pulsesLastCountRain;
        pulsesLastCountRain = curRain;
        // Davis spec: 1 pulse = 0.2 mm rain
        rainMM           = (float)pulsesRain * 0.2f;
        rainMMSinceStart += rainMM;
        rainRateMmH      = rainMM / ((float)elapsed / 3600000.0f);
        Serial.printf("Rain: %d pulses → %.1f mm @ %.1f mm/h\n",
                      pulsesRain, rainMM, rainRateMmH);
    }
}

static void readBme280()
{
    // Forced mode: trigger a single measurement, then wait for completion
    if (!bme.takeForcedMeasurement()) {
        Serial.println("BME280: forced measurement failed.");
        return;
    }
    bme280TempC    = bme.readTemperature();
    bme280PressAbs = bme.readPressure() / 100.0f;
    bme280PressSea = bme.seaLevelForAltitude(gCfg.altitudeM, bme280PressAbs);
    bme280Humidity = bme.readHumidity();

    Serial.printf("BME280: %.1f °C, %.1f %%rH, %.1f hPa (abs), %.1f hPa (QNH)\n",
                  bme280TempC, bme280Humidity, bme280PressAbs, bme280PressSea);
}

static void readOneWire()
{
    // Rev 1.3+: power up VCC_1WIRE, then switch off again
    const bool hssPresent = (gCfg.pcbVersion >= 13);
    if (hssPresent) {
        digitalWrite(PIN_1WIRE_HSS, HIGH);
        delay(5);   // DS18B20 needs ≥1 ms after VDD power-on before first reset
    }

    bus1Count = min((int)sensorsBus1->getDeviceCount(), MAX_SENSORS_PER_BUS);
    if (bus1Count > 0) {
        sensorsBus1->requestTemperatures();
        for (int i = 0; i < bus1Count; i++) {
            bus1Temps[i] = sensorsBus1->getTempCByIndex(i);
            if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
                Serial.printf("1-Wire sensor %d: %.1f °C\n", i, bus1Temps[i]);
        }
    } else {
        Serial.println("1-Wire: no sensor found");
    }

    if (hssPresent) {
        digitalWrite(PIN_1WIRE_HSS, LOW);
        // DQ line high-impedance → pull-up to VCC_1WIRE=0V, no parasitic power
        pinMode(gPins.oneWire, INPUT);
    }
}

// ============================================================================
// SAMPLING FUNCTIONS
// ============================================================================

// Reset accumulators (call after each transmission)
static void resetAccumulators()
{
    windDirSinSum     = 0.0f;
    windDirCosSum     = 0.0f;
    windSpeedSum      = 0.0f;
    bme280TempSum     = 0.0f;
    bme280PressAbsSum = 0.0f;
    bme280PressSeaSum = 0.0f;
    bme280HumSum      = 0.0f;
    rainCycleMm       = 0.0f;
    windGustMs        = 0.0f;
    subSampleCount    = 0;
    loopCount++;   // increment cycle counter after transmission
}

// Take one sub-sample (called every sampleMs)
static void takeSample()
{
    readPulseCounters();   // windSpeedMs, windGustMs, rainMM, rainMMSinceStart

    if (gCfg.windEn) {
        uint16_t dir = readWindDirection();
        float rad     = dir * DEG_TO_RAD;
        windDirSinSum += sinf(rad);
        windDirCosSum += cosf(rad);
        windSpeedSum  += windSpeedMs;
    }
    if (gCfg.rainEn)
        rainCycleMm += rainMM;

    if (bme280Present) {
        readBme280();
        bme280TempSum     += bme280TempC;
        bme280PressAbsSum += bme280PressAbs;
        bme280PressSeaSum += bme280PressSea;
        bme280HumSum      += bme280Humidity;
    }
    subSampleCount++;
}

// Calculate final values from accumulators (once per send cycle)
static void computeFinalValues(unsigned long elapsedMs)
{
    if (subSampleCount == 0) return;

    if (gCfg.windEn) {
        // Vector-average wind direction: circular arithmetic correct (350°+10° → 0°)
        float avgSin  = windDirSinSum / subSampleCount;
        float avgCos  = windDirCosSum / subSampleCount;
        int   angleDeg = (int)roundf(atan2f(avgSin, avgCos) * RAD_TO_DEG);
        windDirection = (uint16_t)((angleDeg % 360 + 360) % 360);
        windSpeedMs   = windSpeedSum / subSampleCount;
        // Gust should not be lower than the cycle average, otherwise some
        // dashboard queries may treat gust as missing at very short sample windows.
        if (windGustMs < windSpeedMs) windGustMs = windSpeedMs;
    }
    if (gCfg.rainEn) {
        rainMM      = rainCycleMm;
        // Guard: avoid astronomically high rate on very short first cycle
        rainRateMmH = (elapsedMs >= 1000)
                      ? rainCycleMm / ((float)elapsedMs / 3600000.0f)
                      : 0.0f;
    }
    if (bme280Present) {
        bme280TempC    = bme280TempSum    / subSampleCount;
        bme280PressAbs = bme280PressAbsSum / subSampleCount;
        bme280PressSea = bme280PressSeaSum / subSampleCount;
        bme280Humidity = bme280HumSum     / subSampleCount;
    }

    cpuTemperature = heltec_temperature();
    batteryVolt    = gCfg.lipoEn ? readBatteryVoltage() : 0.0f;

    Serial.printf("[Cycle %d samples] Wind: %d°, %.1f m/s (gust %.1f) | "
                  "Rain: %.1f mm @ %.1f mm/h | Batt: %.2fV | CPU: %.1f°C\n",
                  subSampleCount, windDirection, windSpeedMs, windGustMs,
                  rainMM, rainRateMmH, batteryVolt, cpuTemperature);
}

// ============================================================================
// BUILD AND SEND LPP PACKET
// ============================================================================
static void buildLppPacket()
{
    const bool  txWindAsMs   = (gCfg.windUnitMs != 0);
    const float txWindFactor = txWindAsMs ? 1.0f : 3.6f;

    lpp.reset();
    if (gCfg.windEn) {
        lpp.addDirection  (1, windDirection);
        lpp.addAnalogInput(1, windSpeedMs * txWindFactor);
        lpp.addAnalogInput(2, windGustMs  * txWindFactor);
    }
    if (gCfg.rainEn) {
        lpp.addAnalogInput(3, rainRateMmH);
        lpp.addDistance   (1, rainMM           / 1000.0f);   // mm → m (LPP unit)
        lpp.addDistance   (2, rainMMSinceStart / 1000.0f);
    }

    // Status byte: BME280 Bit0, Bus1 Bit1, errors Bit2-5, wind unit Bit6 (0=km/h, 1=m/s)
    uint8_t status = 0;
    if (bme280Present)  status |= (1 << 0);
    if (bus1Count > 0)  status |= (1 << 1);
    status |= (uint8_t)(min(sendFailCount, 15) << 2);
    if (txWindAsMs)     status |= (1 << 6);
    lpp.addDigitalInput   (2, status);
    if (gCfg.lipoEn) lpp.addVoltage(1, batteryVolt);
    if (bme280Present) {
        lpp.addRelativeHumidity  (1, bme280Humidity);
        lpp.addBarometricPressure(1, bme280PressSea);
        lpp.addBarometricPressure(2, bme280PressAbs);
        lpp.addTemperature       (1, bme280TempC);
    }
    for (int i = 0; i < bus1Count; i++)
        if (bus1Temps[i] != DEVICE_DISCONNECTED_C) lpp.addTemperature(2 + i, bus1Temps[i]);

    if (gCfg.debugMode) {
        // Keep debug channels high so production channels stay stable and compact.
        lpp.addDigitalInput(200, loopCount);
        lpp.addTemperature (201, cpuTemperature);
        lpp.addDigitalInput(202, (uint8_t)min(sendFailCount, 255));
        lpp.addAnalogInput (203, (float)ESP.getFreeHeap() / 1024.0f);
        lpp.addAnalogInput (204, (float)millis() / 3600000.0f);
    }

    // EU868: DR0/SF12 = max 51 bytes, DR5/SF7 = max 222 bytes payload
    if (lpp.getSize() > 51)
        Serial.printf("Info: LPP packet %d bytes (> DR0 limit 51, OK from DR3)\n", lpp.getSize());
}

static void processDownlink(const uint8_t* data, size_t len)
{
    if (len < 1) return;

    auto applyDownlinkParam = [](uint8_t paramId, int16_t value) -> bool {
        switch (paramId) {
            case 1: // uplinkIntervalSec
                if (value < 5 || value > 3600) return false;
                gCfg.uplinkIntervalSec = value;
                if (gCfg.sampleSec > gCfg.uplinkIntervalSec) {
                    gCfg.sampleSec = gCfg.uplinkIntervalSec;
                }
                return true;
            case 2: // sampleSec
                if (value < 1 || value > gCfg.uplinkIntervalSec) return false;
                gCfg.sampleSec = value;
                return true;
            case 3: // windEn
                if (value != 0 && value != 1) return false;
                gCfg.windEn = value;
                return true;
            case 4: // rainEn
                if (value != 0 && value != 1) return false;
                gCfg.rainEn = value;
                return true;
            case 5: // lipoEn
                if (value != 0 && value != 1) return false;
                gCfg.lipoEn = value;
                return true;
            case 6: // debugMode
                if (value != 0 && value != 1) return false;
                gCfg.debugMode = value;
                return true;
            case 7: // windDirOffsetDeg
                if (value < -180 || value > 180) return false;
                gCfg.windDirOffsetDeg = value;
                return true;
            case 8: // windUnitMs
                if (value != 0 && value != 1) return false;
                gCfg.windUnitMs = value;
                return true;
            case 9: // cpuFreqMhz
                if (value != 80 && value != 240) return false;
                gCfg.cpuFreqMhz = value;
                setCpuFrequencyMhz(gCfg.cpuFreqMhz);
                return true;
            default:
                return false;
        }
    };

    switch (data[0]) {
        case 0x01:
            rainMMSinceStart = 0.0f;
            Serial.println("Downlink: rain accumulator reset.");
            break;
        case 0x02:
            Serial.println("Downlink: restarting...");
            delay(500);
            ESP.restart();
            break;
        case 0x03:
            Serial.println("Downlink: config portal requested.");
            portalRequestedViaDownlink = true;
            break;
        case 0x04:
            gCfg.debugMode = 1;
            saveConfig(gCfg);
            Serial.println("Downlink: debug mode enabled.");
            break;
        case 0x05:
            gCfg.debugMode = 0;
            saveConfig(gCfg);
            Serial.println("Downlink: debug mode disabled.");
            break;
        case 0x10: {
            // Format: [0x10, paramId, valueMSB, valueLSB, ...] (1..9)
            if (len < 4 || ((len - 1) % 3) != 0) {
                Serial.println("Downlink: invalid parameter payload length.");
                break;
            }
            bool anyApplied = false;
            for (size_t i = 1; i + 2 < len; i += 3) {
                uint8_t paramId = data[i];
                int16_t value = (int16_t)(((uint16_t)data[i + 1] << 8) | data[i + 2]);
                bool ok = applyDownlinkParam(paramId, value);
                Serial.printf("Downlink: set param %u = %d -> %s\n",
                              paramId, value, ok ? "OK" : "INVALID");
                anyApplied = anyApplied || ok;
            }
            if (anyApplied) {
                saveConfig(gCfg);
            }
            break;
        }
        default:
            Serial.printf("Downlink: unknown command 0x%02X\n", data[0]);
            break;
    }
}

static void sendLoRa()
{
    uint8_t downlinkData[256] = {};
    size_t  lenDown = sizeof(downlinkData) - 1;

    int16_t state = loRaNode->sendReceive(
        (const uint8_t*)lpp.getBuffer(), lpp.getSize(), 1, downlinkData, &lenDown);

    char buf[64];
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("Uplink sent, no downlink.");
        sendFailCount = 0;
        display.drawString(0, 44, "Uplink sent");
        display.drawString(0, 55, "no downlink");
    } else if (state > 0) {
        Serial.printf("Uplink + Downlink (%d bytes), SNR: %.1f dB, RSSI: %.1f dBm\n",
                      lenDown, radio.getSNR(), radio.getRSSI());
        sendFailCount = 0;
        processDownlink(downlinkData, lenDown);
        display.drawString(0, 44, "Uplink + Downlink");
        snprintf(buf, sizeof(buf), "SNR %.1f  RSSI %.1f", radio.getSNR(), radio.getRSSI());
        display.drawString(0, 55, buf);
    } else {
        Serial.printf("Send failed (code %d), attempt %d/10\n", state, ++sendFailCount);
        display.drawString(0, 44, "Send error");
        snprintf(buf, sizeof(buf), "Code %d  Attempt %d/10", state, sendFailCount);
        display.drawString(0, 55, buf);
        if (sendFailCount >= 10) {
            Serial.println("Too many send failures – restarting for new join.");
            delay(1000);
            ESP.restart();
        }
    }
}

// ============================================================================
// DRAW DISPLAY CONTENT (only call when display is active)
// ============================================================================
static void drawDisplay()
{
    char buf[64];
    display.clear();

    if (gCfg.windEn) {
        const bool showWindAsMs      = (gCfg.windUnitMs != 0);
        const float displayWindFactor = showWindAsMs ? 1.0f : 3.6f;
        const char* windUnitLabel     = showWindAsMs ? "m/s" : "km/h";
        snprintf(buf, sizeof(buf), "D%03d S%.1f G%.1f %s",
                 windDirection,
                 windSpeedMs * displayWindFactor,
                 windGustMs  * displayWindFactor,
                 windUnitLabel);
        display.drawString(0,  0, buf);
    }

    if (gCfg.rainEn)
        snprintf(buf, sizeof(buf), "#%d  Rain: %.1fmm", loopCount, rainMMSinceStart);
    else
        snprintf(buf, sizeof(buf), "#%d", loopCount);
    display.drawString(0, 11, buf);

    if (bme280Present)
        snprintf(buf, sizeof(buf), "%.1f°C  %.1f%%rH  %.0fhPa",
                 bme280TempC, bme280Humidity, bme280PressAbs);
    else
        snprintf(buf, sizeof(buf), "no BME280");
    display.drawString(0, 22, buf);

    snprintf(buf, sizeof(buf), "Batt: %.3fV", batteryVolt);
    display.drawString(0, 33, buf);
}

// ============================================================================
// SETUP
// ============================================================================
void setup()
{
    // Watchdog: 120 s timeout – catches hanging joins and blocked sends
    esp_task_wdt_init(120, true);

    // Enable VEXT (GPIO36, active-LOW) before heltec_setup() so the display
    // already has power when the RST pulse is issued.
    heltec_ve(true);

    heltec_setup();

    // Button interrupt – must be registered early so no press is missed
    pinMode(BUTTON, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON), buttonISR, CHANGE);

    // Initialize sensor arrays
    for (int i = 0; i < MAX_SENSORS_PER_BUS; i++)
        bus1Temps[i] = DEVICE_DISCONNECTED_C;

    display.init();
    display.setContrast(200);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    display.clear();
    display.displayOff();

    // Load config and set CPU frequency
    gCfg = loadConfig();
    setCpuFrequencyMhz(gCfg.cpuFreqMhz);

    // Boot-Anzeige: kurzer Hinweis, dann direkt weiter
    display.displayOn();
    display.clear();
    display.drawString(0,  0, "WeatherNode");
    display.drawString(0, 16, "Hold 3s: Config");
    display.display();
    delay(1500);

    // Ensure the TCP/IP stack and default event loop are initialized before
    // WiFiManager starts.  These calls are safe to make even if the Arduino
    // framework already called them – they return ESP_ERR_INVALID_STATE in
    // that case, which we intentionally ignore.
    esp_netif_init();
    esp_event_loop_create_default();

    // First boot (no DevEUI stored) → open portal automatically
    if (gCfg.devEui == 0) {
        gCfg = runConfigPortal(gCfg);
        saveConfig(gCfg);
        display.clear();
        display.drawString(0,  0, "Config saved!");
        display.drawString(0, 16, "Restarting...");
        display.display();
        delay(2000);
        ESP.restart();
    }

    // Revision-dependent pins
    PinSet pins = getPins(gCfg.pcbVersion);
    gPins = pins;
    Serial.printf("PCB rev %d: Wind=GPIO%d, Rain=GPIO%d, 1-Wire=GPIO%d\n",
                  gCfg.pcbVersion, pins.windSpeed, pins.rain, pins.oneWire);

    // Initialize radio
    int16_t radioState = RADIOLIB_ERR_UNKNOWN;
    display.clear();
    display.drawString(0, 0, "Radio init...");
    display.display();
    while (radioState != RADIOLIB_ERR_NONE) {
        radioState = radio.begin();
        if (radioState != RADIOLIB_ERR_NONE) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Error code: %d", radioState);
            display.clear();
            display.drawString(0,  0, "Radio Error");
            display.drawString(0, 11, buf);
            display.drawString(0, 22, "Retry in 2 s...");
            display.display();
            Serial.printf("Radio init failed (code %d)\n", radioState);
            waitWithButtonPoll(2000);
        }
    }

    // LoRaWAN join
    display.clear();
    display.drawString(0, 0, "LoRaWAN Join...");
    display.display();
    int joinAttempts = 0;
    do {
        persist.provision(gCfg.region, 0x00, gCfg.joinEui, gCfg.devEui, gCfg.appKey, gCfg.appKey);
        persist.isProvisioned();
        loRaNode = persist.manage(&radio);

        if (!loRaNode || !loRaNode->isActivated()) {
            ++joinAttempts;
            Serial.printf("Join failed (attempt %d), retry in 2 s...\n", joinAttempts);
            char buf[32];
            snprintf(buf, sizeof(buf), "Attempt %d – retry in 2 s", joinAttempts);
            display.clear();
            display.drawString(0,  0, "Join failed");
            display.drawString(0, 11, buf);
            display.drawString(0, 22, "Possible causes:");
            display.drawString(0, 33, "- Wrong keys");
            display.drawString(0, 44, "- No gateway");
            display.drawString(0, 55, "Hold 3s: Config portal");
            display.display();
            waitWithButtonPoll(2000);
        }
    } while (!loRaNode || !loRaNode->isActivated());

    // Configure ADC
    pinMode(PIN_ADC_BATCTRL,       OUTPUT);
    pinMode(PIN_ADC_VOLTBATTERY,   INPUT);
    pinMode(PIN_ADC_WINDDIRECTION, INPUT);
    adcAttachPin(PIN_ADC_VOLTBATTERY);
    adcAttachPin(PIN_ADC_WINDDIRECTION);
    analogReadResolution(12);

    // High-side switch for VCC_1WIRE (Rev 1.3+ only)
    if (gCfg.pcbVersion >= 13) {
        pinMode(PIN_1WIRE_HSS, OUTPUT);
        digitalWrite(PIN_1WIRE_HSS, LOW);   // sensors off at startup
        Serial.println("1-Wire HSS (GPIO47) initialized – sensors off.");
    }

    // BME280
    Wire2.begin(PIN_I2C_SDA_BME280, PIN_I2C_SCL_BME280, I2C_FREQUENCY);
    Wire2.setTimeOut(50);
    bme280Present = bme.begin(I2C_ADRESS_BME280, &Wire2);
    if (bme280Present) {
        // Forced mode: sensor only measures on request (takeForcedMeasurement) → ~0 µA idle current
        bme.setSampling(Adafruit_BME280::MODE_FORCED,
                        Adafruit_BME280::SAMPLING_X1,   // Temperatur
                        Adafruit_BME280::SAMPLING_X1,   // Luftdruck
                        Adafruit_BME280::SAMPLING_X1,   // Luftfeuchte
                        Adafruit_BME280::FILTER_OFF,
                        Adafruit_BME280::STANDBY_MS_0_5);
    } else {
        Serial.println("BME280 not found – check wiring.");
    }

    // 1-Wire (pin configured at runtime)
    oneWireBus1 = new OneWire(pins.oneWire);
    sensorsBus1 = new DallasTemperature(oneWireBus1);
    sensorsBus1->begin();

    // PCNT wind speed
    if (gCfg.windEn) {
        pcnt_config_t pcnt_wind = {
            .pulse_gpio_num = pins.windSpeed,
            .ctrl_gpio_num  = PCNT_PIN_NOT_USED,
            .pos_mode       = PCNT_COUNT_INC,
            .neg_mode       = PCNT_COUNT_DIS,
            .counter_h_lim  = 32767,
            .counter_l_lim  = -32768,
            .unit           = PCNT_UNIT_WINDSPEED,
            .channel        = PCNT_CHANNEL_WINDSPEED,
        };
        pcnt_unit_config(&pcnt_wind);
        pcnt_set_filter_value(PCNT_UNIT_WINDSPEED, FILTER_VALUE_WINDSPEED);
        pcnt_filter_enable(PCNT_UNIT_WINDSPEED);
        pcnt_counter_pause(PCNT_UNIT_WINDSPEED);
        pcnt_counter_clear(PCNT_UNIT_WINDSPEED);
        pcnt_counter_resume(PCNT_UNIT_WINDSPEED);
    }

    // PCNT rain sensor
    if (gCfg.rainEn) {
        pcnt_config_t pcnt_rain = {
            .pulse_gpio_num = pins.rain,
            .ctrl_gpio_num  = PCNT_PIN_NOT_USED,
            .pos_mode       = PCNT_COUNT_INC,
            .neg_mode       = PCNT_COUNT_DIS,
            .counter_h_lim  = 32767,
            .counter_l_lim  = -32768,
            .unit           = PCNT_UNIT_RAIN,
            .channel        = PCNT_CHANNEL_RAIN,
        };
        pcnt_unit_config(&pcnt_rain);
        pcnt_set_filter_value(PCNT_UNIT_RAIN, FILTER_VALUE_RAIN);
        pcnt_filter_enable(PCNT_UNIT_RAIN);
        pcnt_counter_pause(PCNT_UNIT_RAIN);
        pcnt_counter_clear(PCNT_UNIT_RAIN);
        pcnt_counter_resume(PCNT_UNIT_RAIN);
    }

    previousMillis = millis();
    lastSampleMs   = millis();
    lastSendMs     = millis();

    // Setup abgeschlossen – jetzt normaler Messbetrieb. Display noch 20 s
    // anzeigen, damit der erste Messwert sichtbar ist, danach Timeout.
    activateDisplay(20000);

    // Enable WDT for the loop task from here
    esp_task_wdt_add(nullptr);
}

// ============================================================================
// LOOP
// ============================================================================
void loop()
{
    esp_task_wdt_reset();
    handleButton();
    updateDisplayState();

    const unsigned long now           = millis();
    const unsigned long sendTargetMs  = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    // Sample interval from config; never larger than uplink interval
    const unsigned long sampleMs      = min((unsigned long)gCfg.sampleSec * 1000UL, sendTargetMs);

    // ── Take sub-sample ────────────────────────────────────────────────────
    if (now - lastSampleMs >= sampleMs) {
        lastSampleMs = now;
        takeSample();
    }

    // ── Uplink when target interval reached ───────────────────────────────
    if (now - lastSendMs >= sendTargetMs) {
        const unsigned long elapsedMs = now - lastSendMs;
        lastSendMs = now;

        computeFinalValues(elapsedMs);
        readOneWire();   // DS18B20: once per send cycle (~750 ms conversion time)
        buildLppPacket();

        if (displayUntilMs > 0) {
            drawDisplay();
            sendLoRa();
            display.display();
        } else {
            sendLoRa();
        }

        resetAccumulators();

        if (portalRequestedViaDownlink) {
            portalRequestedViaDownlink = false;
            activateDisplay(5000);
            display.clear();
            display.drawString(0,  0, "Config Portal");
            display.drawString(0, 16, "starting...");
            display.display();
            delay(1000);
            gCfg = runConfigPortal(gCfg);
            saveConfig(gCfg);
            display.clear();
            display.drawString(0,  0, "Config saved!");
            display.drawString(0, 16, "Restarting...");
            display.display();
            delay(2000);
            ESP.restart();
        }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);   // short yield, no busy-wait
}
