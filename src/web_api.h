#pragma once

// Registers routes once; call from setup() regardless of webApiEn.
void webApiSetup();

// Call on every loop() iteration. Starts/stops the listening socket based on
// wifiEn/webApiEn and current WiFi state; no-op (cheap) when disabled.
void webApiPoll();

// Stops the listening socket immediately, freeing port 80. Call before
// entering the WiFiManager config portal — its own captive-portal web
// server also binds port 80, and having both listening at once is at best
// undefined (whichever gets the incoming connection) and at worst leaves
// the portal's server unable to bind at all, since our own socket is
// already sitting on that port. webApiPoll() will happily re-open it after
// a restart if wifiEn/webApiEn are still on; no need to call this outside
// of that one case.
void webApiStop();
