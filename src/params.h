#pragma once

#include <stdint.h>

// Applies a single (paramId, value) pair to the live Config + hardware
// state. Shared between the LoRaWAN downlink handler (FPort 1, cmd 0x10)
// and the MQTT command topic. Returns false if paramId/value is invalid;
// the caller decides whether to persist via saveConfig().
bool applyDownlinkParam(uint8_t paramId, int16_t value);

// Named, transport-agnostic command actions. Both the LoRaWAN downlink
// byte commands and the MQTT JSON commands dispatch into these.
void cmdResetRainAccumulator();
void cmdRestart();
void cmdSetDebugMode(bool enabled);
void cmdRequestConfigPortal();

// Wipes the entire "config" NVS namespace (cfgdata partition) and restarts
// into the first-boot config portal. Does not return.
void cmdFactoryReset();
