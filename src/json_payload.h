#pragma once

#include <ArduinoJson.h>

// Builds the shared telemetry snapshot into `doc`, mirroring the same
// values/conditions as the CayenneLPP uplink (buildLppPacket() in main.cpp).
// Consumed by both the MQTT publish and the REST API (/api/data).
void buildJsonPayload(JsonDocument& doc);
