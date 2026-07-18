#include "params.h"
#include "globals.h"
#include "sensors.h"
#include "serial_log.h"
#include <Arduino.h>
#include <Preferences.h>

bool applyDownlinkParam(uint8_t paramId, int16_t value)
{
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
            if (gCfg.windEn == value) return true;
            gCfg.windEn = value;
            if (gCfg.windEn) {
                configureWindPcnt();
            } else {
                stopWindPcnt();
                windSpeedMs = 0.0f;
                windGustMs = 0.0f;
            }
            return true;
        case 4: // rainEn
            if (value != 0 && value != 1) return false;
            if (gCfg.rainEn == value) return true;
            gCfg.rainEn = value;
            if (gCfg.rainEn) {
                configureRainPcnt();
            } else {
                stopRainPcnt();
                rainMM = 0.0f;
                rainRateMmH = 0.0f;
            }
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
        case 10: // payloadFormat
            if (value != 0 && value != 1) return false;
            gCfg.payloadFormat = value;
            return true;
        case 11: // unitSystem
            if (value != 0 && value != 1) return false;
            gCfg.unitSystem = value;
            return true;
        case 12: // rainMmPerTip, value = mm/tip x1000 (e.g. 200 = 0.2mm)
            if (value < 1 || value > 5000) return false;
            gCfg.rainMmPerTip = value / 1000.0f;
            return true;
        case 13: // windMsPerPps, value = (m/s per pulse/s) x1000
            if (value < 1 || value > 10000) return false;
            gCfg.windMsPerPps = value / 1000.0f;
            return true;
        case 14: // windDirSensorType
            if (value != 0 && value != 1) return false;
            gCfg.windDirSensorType = value;
            return true;
        default:
            return false;
    }
}

void cmdResetRainAccumulator()
{
    rainMMSinceStart = 0.0f;
    dbgSerial.println("Command: rain accumulator reset.");
}

void cmdRestart()
{
    dbgSerial.println("Command: restarting...");
    delay(500);
    ESP.restart();
}

void cmdSetDebugMode(bool enabled)
{
    gCfg.debugMode = enabled ? 1 : 0;
    saveConfig(gCfg);
    dbgSerial.printf("Command: debug mode %s.\n", enabled ? "enabled" : "disabled");
}

void cmdRequestConfigPortal()
{
    dbgSerial.println("Command: config portal requested.");
    portalRequestedViaDownlink = true;
}

void cmdFactoryReset()
{
    dbgSerial.println("Command: factory reset — wiping config, restarting...");
    Preferences prefs;
    if (prefs.begin("config", false, "cfgdata")) {
        prefs.clear();
        prefs.end();
    }
    delay(500);
    ESP.restart();
}
