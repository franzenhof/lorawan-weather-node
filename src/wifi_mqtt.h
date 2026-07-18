#pragma once

// Call once from setup(), regardless of wifiEn (checks the flag itself).
void wifiMqttSetup();

// Call on every loop() iteration. Non-blocking except for the (rate-limited,
// bounded-timeout) MQTT connect attempt itself; never blocks when WiFi/MQTT
// are disabled or unreachable — a cycle with no connection just does nothing.
void wifiMqttPoll();

bool wifiIsConnected();
bool mqttIsConnected();

// Publishes the current JSON telemetry snapshot to the data topic. No-op if
// wifiEn/mqttEn are disabled or MQTT isn't currently connected — the caller
// (main loop, once per uplink cycle) does not need to check state itself.
void mqttPublishTelemetry();
