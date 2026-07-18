#include "json_payload.h"
#include "globals.h"
#include "units.h"
#include <Arduino.h>
#include <DallasTemperature.h>

// Unit-neutral field names (temp, pressureAbs, ...) — the actual unit used
// depends on gCfg.unitSystem and is always given by a sibling "*Unit" field
// (same pattern as the pre-existing windUnit), so consumers don't need to
// hardcode an assumption about which system is active.
void buildJsonPayload(JsonDocument& doc)
{
    doc.clear();

    if (gCfg.windEn) {
        doc["windDirDeg"] = windDirection;
        doc["windSpeed"]  = windSpeedForOutput(windSpeedMs, gCfg);
        doc["windGust"]   = windSpeedForOutput(windGustMs,  gCfg);
        doc["windUnit"]   = windSpeedUnitLabel(gCfg);
    }
    if (gCfg.rainEn) {
        doc["rainRate"]        = rainRateForOutput(rainRateMmH, gCfg);
        doc["rainRateUnit"]    = rainRateUnitLabel(gCfg);
        doc["rain"]            = rainForOutput(rainMM, gCfg);
        doc["rainSinceStart"]  = rainForOutput(rainMMSinceStart, gCfg);
        doc["rainUnit"]        = rainUnitLabel(gCfg);
    }
    if (bme280Present) {
        doc["temp"]         = temperatureForOutput(bme280TempC, gCfg);
        doc["tempUnit"]     = temperatureUnitLabel(gCfg);
        doc["humidityPct"]  = bme280Humidity;
        doc["pressureAbs"]  = pressureForOutput(bme280PressAbs, gCfg);
        doc["pressureQnh"]  = pressureForOutput(bme280PressSea, gCfg);
        doc["pressureUnit"] = pressureUnitLabel(gCfg);
    }
    if (bus1Count > 0) {
        JsonArray ds18b20 = doc["ds18b20"].to<JsonArray>();
        for (int i = 0; i < bus1Count; i++)
            if (bus1Temps[i] != DEVICE_DISCONNECTED_C)
                ds18b20.add(temperatureForOutput(bus1Temps[i], gCfg));
        doc["ds18b20Unit"] = temperatureUnitLabel(gCfg);
    }
    if (gCfg.lipoEn) {
        doc["batteryVolt"] = batteryVolt;
    }

    JsonObject status       = doc["status"].to<JsonObject>();
    status["bme280Present"] = bme280Present;
    status["bus1Present"]   = bus1Count > 0;
    status["sendFailCount"] = sendFailCount;

    // Time left until the current measurement cycle completes and the next
    // transmit/publish happens (clamped to 0 — a cycle boundary can be
    // crossed between millis() here and the next loop() iteration).
    unsigned long sendTargetMs = (unsigned long)gCfg.uplinkIntervalSec * 1000UL;
    unsigned long elapsedMs    = millis() - lastSendMs;
    doc["nextCycleInSec"] = (elapsedMs < sendTargetMs) ? (sendTargetMs - elapsedMs) / 1000 : 0;

    if (gCfg.debugMode) {
        doc["cycle"]       = loopCount;
        doc["cpuTemp"]     = temperatureForOutput(cpuTemperature, gCfg);
        doc["cpuTempUnit"] = temperatureUnitLabel(gCfg);
        doc["freeHeapKiB"] = (float)ESP.getFreeHeap() / 1024.0f;
        doc["uptimeH"]     = (float)millis() / 3600000.0f;
    }
}
