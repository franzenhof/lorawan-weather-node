#pragma once
static const char DECODER_JS[] PROGMEM = R"DECODERJS(
/**
 * ChirpStack Payload Decoder – LoRaWAN Weather Node
 *
 * Compatible with ChirpStack v3 (Decode function) and v4 (decodeUplink function).
 *
 * Two uplink payload formats, selected by FPort (firmware config `payloadFormat`):
 *   FPort 1 → CayenneLPP (custom channel mapping, see below) — default
 *   FPort 3 → compact custom binary (~50% smaller), see "Custom Binary Decoder" below
 * Both produce the same named JSON fields; downstream consumers don't need
 * to know which format was used on the wire.
 *
 * LPP channel mapping:
 *   Ch 1  Direction        → wind direction (°)
 *   Ch 1  Analog Input     → avg wind speed (m/s or km/h, resolution 0.01)
 *   Ch 2  Analog Input     → wind gust max (m/s or km/h, resolution 0.01)
 *   Ch 3  Analog Input     → rain rate (mm/h, resolution 0.01)
 *   Ch 203 Analog Input    → free heap (KiB, debug mode)
 *   Ch 204 Analog Input    → uptime (h, debug mode)
 *   Ch 1  Distance         → rain current cycle (mm)
 *   Ch 2  Distance         → rain since start (mm)
 *   Ch 2  Digital Input    → status byte (bitmask, see below)
 *   Ch 200 Digital Input   → cycle counter (0–255, debug mode)
 *   Ch 202 Digital Input   → send fail counter (0–255, debug mode)
 *   Ch 1  Voltage          → LiPo voltage (V, optional)
 *   Ch 1  Rel. Humidity    → bme280 humidity (%, BME280)
 *   Ch 1  Baro. Pressure   → bme280 barometric pressure QNH (hPa, BME280)
 *   Ch 2  Baro. Pressure   → bme280 barometric pressure absolute (hPa, BME280)
 *   Ch 1  Temperature      → bme280 temperature (°C, BME280)
 *   Ch 2–4 Temperature     → DS18B20 sensor 0–2 (°C)
 *   Ch 201 Temperature     → CPU temperature (°C, debug mode)
 *
 * Status byte (Ch 2 Digital Input):
 *   Bit 0 : BME280 present (1=yes)
 *   Bit 1 : 1-Wire has sensors (1=yes)
 *   Bit 2–5: send fail counter (0–15, clipped)
 *   Bit 6 : Imperial units in use (°F, in, in/h, inHg, mph) for ALL of
 *           temperature/rain/pressure/wind in this uplink — values are
 *           already converted, this bit only tells you which unit they're in
 *   Bit 7 : wind speed unit if Metric (0=km/h, 1=m/s; ignored if Bit 6 set)
 *
 * Rain accumulator note:
 *   "rain_since_start_mm" is a continuous counter since the last power-on.
 *   Lost packets do NOT affect accuracy: the delta between two received packets
 *   gives the actual rainfall in that period, even if packets were missed in between.
 *   Example: packet 10 → 4.0 mm, packet 12 → 4.8 mm → Δ = 0.8 mm in two cycles.
 *   A reset via downlink 0x01 clears the counter to 0.
 */

// ── CayenneLPP Type Constants ──────────────────────────────────────────────
var LPP_DIGITAL_INPUT    = 0;   // 1 Byte unsigned
var LPP_ANALOG_INPUT     = 2;   // 2 Byte signed, ÷100
var LPP_TEMPERATURE      = 103; // 2 Byte signed, ÷10
var LPP_HUMIDITY         = 104; // 1 Byte unsigned, ÷2
var LPP_BAROMETRIC       = 115; // 2 Byte unsigned, ÷10
var LPP_VOLTAGE          = 116; // 2 Byte unsigned, ÷100
var LPP_DIRECTION        = 132; // 2 Byte unsigned (Grad)
var LPP_DISTANCE         = 130; // 4 Byte unsigned, ÷1000 (firmware maps result to mm)

// ── Helper Functions ────────────────────────────────────────────────────────

function uint16BE(bytes, i) {
    return (bytes[i] << 8) | bytes[i + 1];
}

function int16BE(bytes, i) {
    var v = uint16BE(bytes, i);
    return v >= 0x8000 ? v - 0x10000 : v;
}

function uint32BE(bytes, i) {
    return ((bytes[i] << 24) | (bytes[i+1] << 16) | (bytes[i+2] << 8) | bytes[i+3]) >>> 0;
}

// Bit 6 selects Imperial units (°F, in, inHg, mph) for ALL temperature/
// rain/pressure/wind fields in this uplink — a single node-wide setting, not
// per-field. Values in the payload are ALREADY in the indicated unit (the
// firmware converts before encoding); this function only labels them.
function decodeStatus(byte) {
    var imperial = (byte & 0x40) !== 0;
    return {
        bme280_present:   (byte & 0x01) !== 0,
        bus1_has_sensors: (byte & 0x02) !== 0,
        send_fail_count:  (byte >> 2) & 0x0F,
        wind_speed_unit:  imperial ? "mph" : ((byte & 0x80) !== 0 ? "m/s" : "km/h"),
        temperature_unit: imperial ? "F" : "C",
        rain_unit:        imperial ? "in" : "mm",
        rain_rate_unit:   imperial ? "in/h" : "mm/h",
        pressure_unit:    imperial ? "inHg" : "hPa"
    };
}

// ── Main Decoder ──────────────────────────────────────────────────────────

function parseLPP(bytes) {
    var result = {};
    var i = 0;

    // counters for sensor indices per type+channel
    var analogCount  = {};
    var tempCount    = {};
    var baroCount    = {};
    var distCount    = {};
    var digiCount    = {};

    while (i < bytes.length) {
        var channel = bytes[i++];
        var type    = bytes[i++];

        switch (type) {

            case LPP_DIRECTION:  // wind direction
                if (channel === 1) result.wind_direction_deg = uint16BE(bytes, i);
                i += 2;
                break;

            case LPP_ANALOG_INPUT:
                var aval = int16BE(bytes, i) / 100.0;
                i += 2;
                if (channel === 1) result.wind_speed_avg = aval;
                else if (channel === 2) result.wind_gust  = aval;
                else if (channel === 3) result.rain_rate_mmh  = aval;
                else if (channel === 203) result.debug_free_heap_kib = aval;
                else if (channel === 204) result.debug_uptime_h = aval;
                break;

            case LPP_DISTANCE:
                // Firmware writes rain values directly in mm via addDistance().
                // LPP distance raw ÷1000 reconstructs the original mm value.
                var dval = uint32BE(bytes, i) / 1000.0;
                i += 4;
                if (channel === 1) result.rain_cycle_mm       = parseFloat(dval.toFixed(1));
                else if (channel === 2) result.rain_since_start_mm = parseFloat(dval.toFixed(1));
                break;

            case LPP_TEMPERATURE:
                var tval = int16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.bme280_temperature_c = tval;
                else if (channel >= 2 && channel <= 4)
                    result["ds18b20_" + (channel - 2) + "_c"] = tval;
                else if (channel === 201) result.debug_cpu_temperature_c = tval;
                break;

            case LPP_DIGITAL_INPUT:
                var dival = bytes[i++];
                if (channel === 1) result.cycle_counter = dival; // legacy payloads
                else if (channel === 2) result.status = decodeStatus(dival);
                else if (channel === 200) result.debug_cycle_counter = dival;
                else if (channel === 202) result.debug_send_fail_count = dival;
                break;

            case LPP_VOLTAGE:
                result.battery_v = uint16BE(bytes, i) / 100.0;
                i += 2;
                break;

            case LPP_HUMIDITY:
                result.bme280_humidity_pct = bytes[i++] / 2.0;
                break;

            case LPP_BAROMETRIC:
                var pval = uint16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.bme280_pressure_qnh_hpa = pval;
                else if (channel === 2) result.bme280_pressure_abs_hpa = pval;
                break;

            default:
                // Unknown type – stop parsing to avoid incorrect offsets
                console.error("LPP: unknown type 0x" + type.toString(16) + " at offset " + (i-1));
                i = bytes.length;
                break;
        }
    }
    return result;
}

// ── Custom Binary Decoder (FPort 3) ─────────────────────────────────────────
//
// Layout: [version:1][presence bitmask:4, little-endian][field data...]
// Only fields whose bit is set in the mask are present, in ascending
// bit-index order. Field values use the same scaling as CayenneLPP above,
// but big-endian and fixed-width (no per-field channel/type byte).
// Bit order is authoritative in firmware: enum BinField in main.cpp.

var BIN_FIELD = {
    WIND_DIR: 0, WIND_SPEED: 1, WIND_GUST: 2,
    RAIN_RATE: 3, RAIN_MM: 4, RAIN_SINCE: 5,
    STATUS: 6, BATTERY: 7,
    BME_HUMIDITY: 8, BME_PRESS_QNH: 9, BME_PRESS_ABS: 10, BME_TEMP: 11,
    DS18B20_0: 12, DS18B20_1: 13, DS18B20_2: 14,
    DBG_CYCLE: 15, DBG_CPU_TEMP: 16, DBG_FAIL_COUNT: 17, DBG_FREE_HEAP: 18, DBG_UPTIME: 19
};

function uint32LE(bytes, i) {
    return ((bytes[i + 3] << 24) | (bytes[i + 2] << 16) | (bytes[i + 1] << 8) | bytes[i]) >>> 0;
}

function parseBin(bytes) {
    var result = {};
    // bytes[0] = protocol version, currently unused for decoding (reserved
    // for future format changes).
    var mask = uint32LE(bytes, 1);
    var i    = 5;

    function has(bit) { return (mask & (1 << bit)) !== 0; }

    if (has(BIN_FIELD.WIND_DIR))       { result.wind_direction_deg = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.WIND_SPEED))     { result.wind_speed_avg     = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.WIND_GUST))      { result.wind_gust          = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.RAIN_RATE))      { result.rain_rate_mmh      = int16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.RAIN_MM))        { result.rain_cycle_mm       = parseFloat((uint16BE(bytes, i) / 10.0).toFixed(1)); i += 2; }
    if (has(BIN_FIELD.RAIN_SINCE))     { result.rain_since_start_mm = parseFloat((uint16BE(bytes, i) / 10.0).toFixed(1)); i += 2; }
    if (has(BIN_FIELD.STATUS))         { result.status = decodeStatus(bytes[i]); i += 1; }
    if (has(BIN_FIELD.BATTERY))        { result.battery_v = uint16BE(bytes, i) / 100.0; i += 2; }
    if (has(BIN_FIELD.BME_HUMIDITY))   { result.bme280_humidity_pct     = bytes[i] / 2.0; i += 1; }
    if (has(BIN_FIELD.BME_PRESS_QNH))  { result.bme280_pressure_qnh_hpa = uint16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.BME_PRESS_ABS))  { result.bme280_pressure_abs_hpa = uint16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.BME_TEMP))       { result.bme280_temperature_c    = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_0))      { result.ds18b20_0_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_1))      { result.ds18b20_1_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DS18B20_2))      { result.ds18b20_2_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DBG_CYCLE))      { result.debug_cycle_counter     = bytes[i]; i += 1; }
    if (has(BIN_FIELD.DBG_CPU_TEMP))   { result.debug_cpu_temperature_c = int16BE(bytes, i) / 10.0; i += 2; }
    if (has(BIN_FIELD.DBG_FAIL_COUNT)) { result.debug_send_fail_count   = bytes[i]; i += 1; }
    if (has(BIN_FIELD.DBG_FREE_HEAP))  { result.debug_free_heap_kib     = uint16BE(bytes, i); i += 2; }
    if (has(BIN_FIELD.DBG_UPTIME))     { result.debug_uptime_h          = uint16BE(bytes, i) / 10.0; i += 2; }

    return result;
}

// ── ChirpStack v4 Entry Point ──────────────────────────────────────────────

function decodeUplink(input) {
    try {
        var data = (input.fPort === 3) ? parseBin(input.bytes) : parseLPP(input.bytes);
        return { data: data, errors: [] };
    } catch (e) {
        return { data: {}, errors: [e.toString()] };
    }
}

// ── ChirpStack v3 Entry Point ──────────────────────────────────────────────

function Decode(fPort, bytes) {
    try {
        return (fPort === 3) ? parseBin(bytes) : parseLPP(bytes);
    } catch (e) {
        return { error: e.toString() };
    }
}

)DECODERJS";
