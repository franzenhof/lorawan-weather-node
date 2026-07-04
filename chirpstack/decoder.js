/**
 * ChirpStack Payload Decoder – LoRaWAN Weather Node
 *
 * Compatible with ChirpStack v3 (Decode function) and v4 (decodeUplink function).
 * Payload format: CayenneLPP (custom channel mapping, see below).
 *
 * LPP channel mapping:
 *   Ch 1  Direction        → wind direction (°)
 *   Ch 1  Analog Input     → avg wind speed (km/h, resolution 0.01)
 *   Ch 2  Analog Input     → wind gust max (km/h, resolution 0.01)
 *   Ch 3  Analog Input     → rain rate (mm/h, resolution 0.01)
 *   Ch 1  Distance         → rain current cycle (m, ×1000 → mm)
 *   Ch 2  Distance         → rain since start (m, ×1000 → mm)
 *   Ch 1  Temperature      → CPU temperature (°C)
 *   Ch 1  Digital Input    → cycle counter (0–255)
 *   Ch 2  Digital Input    → status byte (bitmask, see below)
 *   Ch 1  Voltage          → LiPo voltage (V, optional)
 *   Ch 1  Rel. Humidity    → humidity (%, BME280)
 *   Ch 1  Baro. Pressure   → barometric pressure QNH (hPa, BME280)
 *   Ch 2  Baro. Pressure   → barometric pressure absolute (hPa, BME280)
 *   Ch 2  Temperature      → temperature (°C, BME280)
 *   Ch 3–5 Temperature     → DS18B20 sensor 0–2 (°C)
 *
 * Status byte (Ch 2 Digital Input):
 *   Bit 0 : BME280 present (1=yes)
 *   Bit 1 : 1-Wire has sensors (1=yes)
 *   Bit 2–5: send fail counter (0–15, clipped)
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
var LPP_DISTANCE         = 130; // 4 Byte unsigned, ÷1000 (Meter)

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

function decodeStatus(byte) {
    return {
        bme280_present:   (byte & 0x01) !== 0,
        bus1_has_sensors: (byte & 0x02) !== 0,
        send_fail_count:  (byte >> 2) & 0x0F
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
                if (channel === 1) result.wind_speed_avg_kmh = aval;
                else if (channel === 2) result.wind_gust_kmh  = aval;
                else if (channel === 3) result.rain_rate_mmh  = aval;
                break;

            case LPP_DISTANCE:
                // Unit in payload: meters (÷1000). Firmware sends mm/1000 → value in meters.
                // ×1000 gives mm.
                var dval = uint32BE(bytes, i) / 1000.0 * 1000.0;  // → mm
                i += 4;
                if (channel === 1) result.rain_cycle_mm       = parseFloat(dval.toFixed(1));
                else if (channel === 2) result.rain_since_start_mm = parseFloat(dval.toFixed(1));
                break;

            case LPP_TEMPERATURE:
                var tval = int16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.cpu_temperature_c   = tval;
                else if (channel === 2) result.bme280_temperature_c = tval;
                else if (channel >= 3 && channel <= 5)
                    result["ds18b20_" + (channel - 3) + "_c"] = tval;
                break;

            case LPP_DIGITAL_INPUT:
                var dival = bytes[i++];
                if (channel === 1) result.cycle_counter = dival;
                else if (channel === 2) result.status = decodeStatus(dival);
                break;

            case LPP_VOLTAGE:
                result.battery_v = uint16BE(bytes, i) / 100.0;
                i += 2;
                break;

            case LPP_HUMIDITY:
                result.humidity_pct = bytes[i++] / 2.0;
                break;

            case LPP_BAROMETRIC:
                var pval = uint16BE(bytes, i) / 10.0;
                i += 2;
                if (channel === 1) result.pressure_qnh_hpa = pval;
                else if (channel === 2) result.pressure_abs_hpa = pval;
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

// ── ChirpStack v4 Entry Point ──────────────────────────────────────────────

function decodeUplink(input) {
    try {
        var data = parseLPP(input.bytes);
        return { data: data, errors: [] };
    } catch (e) {
        return { data: {}, errors: [e.toString()] };
    }
}

// ── ChirpStack v3 Entry Point ──────────────────────────────────────────────

function Decode(fPort, bytes) {
    try {
        return parseLPP(bytes);
    } catch (e) {
        return { error: e.toString() };
    }
}
