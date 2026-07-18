#pragma once

#include "config.h"

// Unit conversion for output only (LoRaWAN uplink, MQTT/REST JSON, OLED
// display, Home Assistant discovery). Internal computation always stays in
// the existing base units (m/s, mm, °C, hPa) regardless of gCfg.unitSystem —
// only these functions convert, at the point a value is about to be
// serialized/displayed.
//
// Wind speed keeps its own existing windUnitMs choice (km/h vs m/s) when
// unitSystem is Metric; unitSystem = Imperial overrides wind to mph
// regardless of windUnitMs.

float       windSpeedForOutput(float ms, const Config& c);
const char* windSpeedUnitLabel(const Config& c);

float       temperatureForOutput(float celsius, const Config& c);
const char* temperatureUnitLabel(const Config& c);

float       rainForOutput(float mm, const Config& c);
const char* rainUnitLabel(const Config& c);

float       rainRateForOutput(float mmPerHour, const Config& c);
const char* rainRateUnitLabel(const Config& c);

float       pressureForOutput(float hPa, const Config& c);
const char* pressureUnitLabel(const Config& c);
