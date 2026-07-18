#include "units.h"

float windSpeedForOutput(float ms, const Config& c)
{
    if (c.unitSystem == 1) return ms * 2.23694f; // mph
    return (c.windUnitMs != 0) ? ms : ms * 3.6f; // m/s or km/h
}

const char* windSpeedUnitLabel(const Config& c)
{
    if (c.unitSystem == 1) return "mph";
    return (c.windUnitMs != 0) ? "m/s" : "km/h";
}

float temperatureForOutput(float celsius, const Config& c)
{
    if (c.unitSystem == 1) return celsius * 1.8f + 32.0f; // °F
    return celsius;
}

const char* temperatureUnitLabel(const Config& c)
{
    return (c.unitSystem == 1) ? "F" : "C";
}

float rainForOutput(float mm, const Config& c)
{
    if (c.unitSystem == 1) return mm / 25.4f; // in
    return mm;
}

const char* rainUnitLabel(const Config& c)
{
    return (c.unitSystem == 1) ? "in" : "mm";
}

float rainRateForOutput(float mmPerHour, const Config& c)
{
    if (c.unitSystem == 1) return mmPerHour / 25.4f; // in/h
    return mmPerHour;
}

const char* rainRateUnitLabel(const Config& c)
{
    return (c.unitSystem == 1) ? "in/h" : "mm/h";
}

float pressureForOutput(float hPa, const Config& c)
{
    if (c.unitSystem == 1) return hPa / 33.8639f; // inHg
    return hPa;
}

const char* pressureUnitLabel(const Config& c)
{
    return (c.unitSystem == 1) ? "inHg" : "hPa";
}
