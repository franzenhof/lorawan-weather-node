# LoRaWAN Weather Node

Firmware for a custom-designed PCB based on a **Heltec WiFi LoRa 32 V3** (ESP32-S3).  
The node measures wind speed, wind direction, rainfall, temperature, humidity, and barometric pressure and transmits the data via **LoRaWAN (EU868, OTAA)** to a private ChirpStack server.

---

## 📄 License & Open Source

This project is fully open-source. To support both software developers and hardware makers, it is dual-licensed:

* **Software / Firmware:** Licensed under the **[MIT License](LICENSE-SOFTWARE)**. You can freely use, modify, and distribute the code.
* **Hardware (Schematics, PCB & 3D Files):** Licensed under the **[CERN-OHL-P v2](LICENSE-HARDWARE)** (CERN Open Hardware Licence – Permissive). You are free to manufacture, modify, and distribute the hardware designs.

*Developed with 🚜 and 💻 by Gerhard Massenbichler at Franzenhof.*


## Use Case

```
[USB-C power supply] ──► [Heltec WiFi LoRa V3]
                              │        │
                   [Davis 6410]    [BME280 via RJ45]
                   Wind sensor           │
                   │                [DS18B20 × 3]
                   [Davis AeroCone]  [LiPo optional]
                   Rain sensor
                              │
                           [LoRaWAN EU868]
                              │
                       [ChirpStack server]
```

The node runs continuously (no deep sleep), measures without interruption, and transmits a LoRaWAN uplink every configurable number of seconds. Typical send interval: 3–60 seconds.

---

## Hardware

| Component | Function |
|-----------|----------|
| Heltec WiFi LoRa 32 V3 | Microcontroller (ESP32-S3), LoRa SX1262, OLED display 128×64 |
| Davis 6410 Anemometer | Wind speed (reed switch) + wind direction (potentiometer 20 kΩ) |
| Davis AeroCone / Standard | Rainfall (reed switch, 0.2 mm/pulse) |
| BME280 | Temperature / humidity / barometric pressure (I²C 0x76, up to 2 m RJ45 cable) |
| DS18B20 | Up to 3 temperature sensors (1-Wire, external) |
| LiPo battery | Optional; voltage measurement via voltage divider + ADC |
| Custom PCB Rev 1.2 / 1.3 | ESD protection, pull-ups, RC filter, polyfuse |

### Custom PCB

In addition to mechanically connecting the connectors, the PCB provides the following protection functions for all external interfaces:

- **TVS diodes** (SMLVT3V3) directly at each external connector as primary ESD coarse protection  
- **BAT43 Schottky clamp diodes** as secondary protection clamps before the GPIO pins  
- **Series resistors** (330 Ω / 1 kΩ) for current limiting  
- **RC low-pass filter** (4.7 kΩ + 100 nF, τ ≈ 0.47 ms) for hardware debouncing of the reed switches  
- **Polyfuse F1** (250 mA) for short-circuit protection of the 3.3 V sensor supply  
- **Additional I²C pull-ups** (4.7 kΩ, jumper J1/J2) for longer BME280 cable runs up to 2 m

---

## GPIO Pinout

### Fixed Pins (revision-independent)

| GPIO | Function | Direction | Note |
|------|----------|-----------|------|
| **0** | User button | IN (pull-up) | Short press: display 20 s; long press (≥ 3 s): Wi-Fi portal |
| **1** | LiPo ADC | IN (ADC1_CH0) | Voltage divider R13/R14 (390 Ω / 100 Ω) |
| **37** | BAT_CTRL | OUT | HIGH enables ADC measurement path (Heltec V3 internal from board rev. 3.2) |
| **41** | I²C SDA (BME280) | IN/OUT | Wire2, 50 kHz, address 0x76 |
| **42** | I²C SCL (BME280) | OUT | Wire2 |

### Revision-Dependent Pins

| GPIO Rev 1.2 | GPIO Rev 1.3 | Function |
|:------------:|:------------:|----------|
| **2** | **4** | Wind direction ADC (ADC1_CH1) – Davis potentiometer 0–3.3 V → 0–359°, C6 = 10 µF low-pass |
| **3** | **2** | Wind speed (PCNT Unit 0, reed switch) |
| **4** | **5** | Rain (PCNT Unit 1, reed switch) |
| **5** | **6** | 1-Wire bus (DS18B20, up to 3 sensors) |
| **6** | **3** | unused / free |
| – | **47** | 1-Wire high-side switch (OUT, HIGH = bus powered) |
| – | **7** | completely free (no firmware use, no PCB connection) |

> The active PCB revision is configured in the Wi-Fi portal (default: **12** for Rev 1.2).

### Wind Sensor (Davis 6410) – RJ11 6-pin

| Pin | Color | Function |
|-----|-------|----------|
| 1 | – | n/c |
| 2 | Yellow | VCC 3.3 V (potentiometer supply) |
| 3 | Green | Wind direction (potentiometer wiper → ADC) |
| 4 | Red | GND |
| 5 | Black | Wind speed (reed switch → PCNT) |
| 6 | – | n/c |

### Rain Sensor – RJ11 6-pin

| Pin | Color | Function |
|-----|-------|----------|
| 3 | Green/Yellow | Reed switch → PCNT |
| 4 | Red | GND |
| others | – | n/c |

> Wire colors 3 and 4 may be swapped depending on the cable batch – both variants work since the reed switch is polarity-independent.

### BME280 – RJ45 (solder module-side onto breakout board)

| Pins | Function |
|------|----------|
| 1, 2, 4, 6 | GND |
| 7, 8 | VCC 3.3 V |
| 3 | SCL |
| 5 | SDA |

> Jumpers J1 and J2 on the PCB activate the additional 4.7 kΩ I²C pull-ups for cable lengths over 0.5 m.

---

## Measurement Principles

### Wind Speed
The Davis 6410 anemometer contains a **reed switch** that closes once per revolution.  
The ESP32 hardware pulse counter (**PCNT**) counts edges without CPU load.  
Conversion per Davis specification:

```
1 revolution/s = 2.25 mph = 3.62 km/h
Wind speed [km/h] = pulses/s × 2.25 × 1.60934
```

The **maximum wind gust** (maximum per send interval) is also captured and transmitted separately.

### Wind Direction
The Davis 6410 contains a **potentiometer (20 kΩ)** powered directly from VCC3.3.  
The wiper delivers a linear voltage from 0–3.3 V proportional to 0–359°.  
A configurable **north offset** (–180° to +180°) corrects the mounting orientation.

```
Raw value (0–4095, 12-bit ADC) → map(0, 4095, 0, 359) → + offset → mod 360
```

The low-pass filter C6 (10 µF + potentiometer source impedance ~10 kΩ) gives τ ≈ 100 ms (settling time ~500 ms), which is appropriate for typical vane movement speeds.

### Rainfall
The reed switch in the tipping bucket rain gauge fires one pulse per **0.2 mm of rainfall**.  
PCNT counts pulses in hardware. Rain rate is calculated as mm/h from the interval:

```
Rain [mm] = pulses × 0.2
Rain rate [mm/h] = rain [mm] / (interval [ms] / 3,600,000)
```

The since-start accumulator (`rainMMSinceStart`) survives normal operation in **RTC_DATA_ATTR** (lost on power interruption, resettable via downlink `0x01`).

### Temperatures (DS18B20)
Up to **3 sensors per bus** are detected automatically (`getDeviceCount()`).  
The index corresponds to the order of ROM addresses on the bus.

### Barometric Pressure (BME280)
The measured absolute pressure is converted to **sea level (QNH)** when a station altitude is configured:

```
QNH = absolute pressure × (1 - 0.0000226 × altitude[m])^(-5.255)
```

---

## CayenneLPP Payload Channels

| Channel | Type | Content | Unit | Active when |
|---------|------|---------|------|-------------|
| Ch 1 | Direction | Wind direction | ° | `windEn = 1` |
| Ch 1 | Analog Input | Avg wind speed | km/h | `windEn = 1` |
| Ch 2 | Analog Input | Wind gust (maximum) | km/h | `windEn = 1` |
| Ch 3 | Analog Input | Rain rate | mm/h | `rainEn = 1` |
| Ch 1 | Distance | Rain current cycle | mm | `rainEn = 1` |
| Ch 2 | Distance | Rain since start | mm | `rainEn = 1` |
| Ch 1 | Temperature | CPU temperature (internal) | °C | always |
| Ch 1 | Digital Input | Cycle counter | 0–255 | always |
| Ch 1 | Voltage | LiPo voltage | V | `lipoEn = 1` |
| Ch 1 | Rel. Humidity | Humidity | % | BME280 detected |
| Ch 1 | Baro. Pressure | Barometric pressure QNH | hPa | BME280 detected |
| Ch 2 | Baro. Pressure | Barometric pressure absolute | hPa | BME280 detected |
| Ch 2 | Temperature | BME280 temperature | °C | BME280 detected |
| Ch 3–5 | Temperature | DS18B20 sensor 0–2 | °C | sensor detected |

> **Payload size:**  
> Minimal config (DS18B20 only): approx. 15–30 bytes – fits in DR0 (SF12, 51 bytes).  
> Full config (wind + rain + BME280 + 3× DS18B20 + LiPo): approx. 70–75 bytes – requires **DR2 (SF10)** or higher.  
> With a private ChirpStack server and normal gateway distance this is not an issue.

---

## Initial Configuration (Wi-Fi Portal)

### First boot
On first power-up (no DevEUI stored in NVS) the Wi-Fi portal opens **automatically**:

1. The OLED display shows **"Config Portal"**, the AP SSID, the URL and brief instructions
2. Connect your phone or laptop to the Wi-Fi network **"WeatherNode"** (no password)
3. Open **`http://192.168.4.1`** in a browser (most devices redirect automatically)
4. Enter all parameters and click *Save* → the device restarts automatically

### Re-opening the portal later
The portal can be opened at any time in two ways:

| Method | How |
|--------|-----|
| **Button long press** | Hold the user button (GPIO 0) for **≥ 3 seconds** during normal operation |
| **LoRaWAN downlink** | Send byte `0x03` on FPort 1 (takes effect after the current send cycle) |

> **Note:** GPIO 0 must **not** be pressed during power-on or reset – this would activate the ESP32 bootloader instead of the firmware. The portal is therefore triggered by a long press *during operation* only.

| Parameter | Description | Default |
|-----------|-------------|---------|
| DevEUI | 16 hex chars, from ChirpStack | – |
| JoinEUI | 16 hex chars, from ChirpStack | – |
| AppKey | 32 hex chars, from ChirpStack | – |
| PCB revision | 12 = Rev 1.2, 13 = Rev 1.3 | 12 |
| Altitude above sea level (m) | Station altitude for QNH calculation | 560 |
| Send interval (s) | Target interval per measurement cycle; EU868 duty cycle: min. 5 s at SF7 | 3 |
| Sample interval (s) | Sub-sample interval within a cycle; ≤ send interval | 3 |
| Wind direction offset (°) | North correction during installation (−180 to +180) | 0 |
| Wind sensor (1=Yes) | Davis wind sensor enabled | 1 |
| Rain sensor (1=Yes) | Davis rain sensor enabled | 1 |
| LiPo measurement (1=Yes) | Voltage measurement via voltage divider enabled | 1 |
| CPU frequency (80/240 MHz) | 80 MHz saves power; below 80 MHz not allowed (PCNT-APB dependency) | 80 |

> All settings are stored persistently in the **NVS (Non-Volatile Storage)** of the ESP32-S3 and survive restarts and firmware updates.

---

## User Button (GPIO 0)

| Press duration | When | Action |
|---------------|------|--------|
| — | First boot (no DevEUI stored) | Wi-Fi portal opens automatically |
| Short press (< 3 s, on release) | Normal operation | Turn on OLED display for 20 s |
| Long press (≥ 3 s) | Normal operation or setup loops (join retries etc.) | Open Wi-Fi config portal → save → restart |

Button presses are detected via a **hardware interrupt** (GPIO CHANGE), so a short press is never missed – even while the LoRa send/receive window is blocking the main task for several seconds.

The display is **off** during normal operation to protect the SSD1306 driver and save power. It turns off automatically after 20 seconds. During the startup sequence (radio init, LoRaWAN join retries) the display stays on permanently so that status and error messages remain visible.

---

## ChirpStack Payload Decoder

The file `chirpstack/decoder.js` contains the JavaScript decoder for ChirpStack v3 and v4.

**Adding the decoder to ChirpStack:**
1. In the ChirpStack web interface: *Device Profiles → Codec → JavaScript codec functions*
2. Paste the contents of `chirpstack/decoder.js`
3. Save – all values will appear as named JSON fields in the uplink events

**Example output (JSON):**
```json
{
  "wind_direction_deg": 247,
  "wind_speed_avg_kmh": 12.30,
  "wind_gust_kmh": 18.50,
  "rain_rate_mmh": 0.00,
  "rain_cycle_mm": 0.0,
  "rain_since_start_mm": 14.2,
  "cpu_temperature_c": 43.5,
  "cycle_counter": 42,
  "status": { "bme280_present": true, "bus1_has_sensors": true, "send_fail_count": 0 },
  "battery_v": 3.98,
  "humidity_pct": 67.5,
  "pressure_qnh_hpa": 1013.2,
  "pressure_abs_hpa": 952.1,
  "bme280_temperature_c": 21.3,
  "ds18b20_0_c": 18.6,
  "ds18b20_1_c": 19.1
}
```

**Rain accumulator evaluation:**  
`rain_since_start_mm` is a cumulative counter. The delta between two received packets gives the actual rainfall – even if packets were lost in between.

---



## LoRaWAN Downlink Commands

Downlinks are sent on **FPort 1** with a single byte:

| Byte | Function |
|------|----------|
| `0x01` | Reset rain accumulator (rain since start) to 0 |
| `0x02` | Restart device (triggers a new OTAA join) |
| `0x03` | Open Wi-Fi config portal (takes effect after the current send cycle completes) |

---

## Error Handling / Robustness

### Watchdog Timer
A **hardware watchdog** (120 s timeout) protects against hanging code.  
It is activated only **after a successful LoRaWAN join** – allowing slow joins and Wi-Fi portal operation to run without a WDT reset.

### Automatic Restart on Send Failures
After **10 consecutive send failures** an automatic restart is triggered to renew the LoRaWAN join (e.g. after gateway outage or session timeout).

### ADC Averaging
Wind ADC (direction) and battery ADC are averaged over **8 samples** to reduce ESP32-S3 ADC noise.

---

## Send Interval and Timing

The configured send interval is a **target value**: at the end of each cycle the code only waits the remaining time until the target interval (`millis()` delta). This prevents the measurement time (1-Wire conversion ~750 ms, LoRa airtime ~500 ms) from adding up twice.

```
Cycle start → measure → send → sleep(max(0, target − elapsed))
```

**Minimum practical interval** in EU868 at DR5 (SF7): ~5 s (duty cycle restriction).  
**Default value of 3 s** is sufficient for gateways with a private ChirpStack server without FUP restrictions.

---

## PCB Revisions

| Revision | Status | GPIO Wind Speed | GPIO Wind Dir | GPIO Rain | GPIO 1-Wire |
|----------|--------|:---------------:|:-------------:|:---------:|:-----------:|
| **Rev 1.2** | In use | GPIO 3 | GPIO 2 | GPIO 4 | GPIO 5 |
| **Rev 1.3** | In preparation | GPIO 2 | GPIO 4 | GPIO 5 | GPIO 6 |

> Rev 1.3: The 5V input on the PCB (overvoltage protected) connects directly to the **5V pin of the Heltec WiFi LoRa V3** (Pin 2 on the J2 header of the controller board). GPIO 7 is completely free and unused.

> **Note on current firmware:** Both revisions now support only **one** 1-Wire bus (Rev 1.2: GPIO 5, Rev 1.3: GPIO 6). The former second bus is no longer addressed by the software.

**Changes in Rev 1.3 vs. 1.2:**
- TVS diode (SMLVT3V3 / U4) added at rain sensor connector
- GPIO numbers shifted by one (GPIO 3 was close to JTAG)
- 2nd 1-Wire bus removed; GPIO 6 freed → slot used for the 5V input

---

## Project Structure

```
├── src/
│   └── main.cpp             # Complete firmware
├── chirpstack/
│   └── decoder.js           # ChirpStack v3/v4 JavaScript payload decoder
├── pcblayout/
│   ├── pcb-layout with esd protection.fzz        # Fritzing Rev 1.3
│   ├── pcb-layout with esd protection_Schaltplan.png
│   ├── pcb-layout with esd protection_Leiterplatte.png
│   ├── pcb-layout with esd protection_bom.html
│   ├── pcb-layout with esd protection rev12.zip  # Gerber files Rev 1.2
│   ├── pcb-layout with esd protection rev13.zip  # Gerber files Rev 1.3
│   └── design_readme.txt    # Connector pin assignments
├── platformio.ini
└── README.md
```

---

## Libraries

| Library | Version | Purpose |
|---------|---------|---------|
| RadioLib | ^7.6.0 | LoRaWAN stack (OTAA, EU868) |
| Heltec_ESP32_LoRa_v3 | ^0.9.2 | Board support (display, heltec_temperature) |
| LoRaWAN_ESP32 | ^1.2.0 | LoRaWAN session management (persistence) |
| CayenneLPP | ^1.6.1 | Payload encoding |
| WiFiManager | ^2.0.16-rc.2 | Wi-Fi configuration portal |
| Adafruit BME280 | ^2.2.4 | BME280 driver |
| Adafruit Unified Sensor | ^1.1.15 | Sensor abstraction |
| OneWire | ^2.3.8 | 1-Wire protocol |
| DallasTemperature | ^4.0.6 | DS18B20 driver |
| esp_task_wdt | (IDF built-in) | Hardware watchdog |

---

## Technical Notes

### PCNT and CPU Frequency
The hardware pulse counter (PCNT) uses the **APB clock** for its debounce filter.  
At CPU frequencies ≥ 80 MHz (PLL mode) the APB clock stays constant at 80 MHz.  
Below 80 MHz (XTAL mode) the APB clock drops proportionally → debounce filter timing becomes invalid.  
**Minimum allowed CPU frequency: 80 MHz.**

### 1-Wire with Runtime-Configured Pins
Because the GPIO numbers for 1-Wire are determined at runtime from the PCB revision, the `OneWire` and `DallasTemperature` objects are allocated on the heap with `new` (no static constructor with hard-coded pins possible).

### LoRaWAN Session Persistence
The `LoRaWAN_ESP32` library stores the LoRaWAN session (frame counters, DevAddr, session keys) in NVS. A restart does not necessarily trigger a new OTAA join.

### ADC Linearity
The ESP32-S3 ADC is non-linear (especially below 150 mV and above 3.1 V). For the wind direction potentiometer this is uncritical since values are mapped to 0–359° and the absolute measurement error remains < 5° in practice. For the battery voltage measurement a calibration factor is included in the code.
