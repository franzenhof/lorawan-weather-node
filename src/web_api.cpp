#include "web_api.h"
#include "globals.h"
#include "params.h"
#include "json_payload.h"
#include "wifi_mqtt.h"
#include "readme_md.h"
#include "help_page.h"
#include "generated_version.h"
#include "serial_log.h"

#include <WebServer.h>
#include <ArduinoJson.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace {

WebServer server(80);
bool      serverStarted = false;

void handleApiData()
{
    JsonDocument doc;
    buildJsonPayload(doc);
    char buf[1024];
    serializeJson(doc, buf, sizeof(buf));
    server.send(200, "application/json", buf);
}

// Rendered README help page, works fully offline (no CDN dependency) — see
// include/help_page.h. Same route pair as the WiFiManager portal's /help.
void handleHelp()
{
    server.send_P(200, "text/html; charset=utf-8", HELP_PAGE_HTML);
}

void handleHelpMd()
{
    server.send_P(200, "text/plain; charset=utf-8", README_MD);
}

// Status landing page for "/" — the last measured values (same data as
// /api/data, rendered as HTML) plus device/connectivity info, so hitting the
// device's bare IP in a browser shows something useful instead of a 404.
void addRow(String& html, const String& label, const String& value)
{
    html += "<tr><td>" + label + "</td><td>" + value + "</td></tr>";
}

void handleRoot()
{
    JsonDocument doc;
    buildJsonPayload(doc);

    const char* name = gCfg.deviceName[0] ? gCfg.deviceName : "WeatherNode";

    String html;
    html.reserve(2560);
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>"; html += name; html += " status</title></head><body>";
    html += "<h3>"; html += name; html += " status</h3>";
    html += "<p>Firmware: " FIRMWARE_VERSION "</p>";

    html += "<h4>Measurements</h4><table>";
    if (gCfg.windEn) {
        addRow(html, "Wind direction",     String(doc["windDirDeg"].as<int>()) + "&deg;");
        addRow(html, "Wind speed",         String(doc["windSpeed"].as<float>(), 1) + " " + doc["windUnit"].as<const char*>());
        addRow(html, "Wind gust",          String(doc["windGust"].as<float>(),  1) + " " + doc["windUnit"].as<const char*>());
    }
    if (gCfg.rainEn) {
        addRow(html, "Rain rate",          String(doc["rainRate"].as<float>(), 2)       + " " + doc["rainRateUnit"].as<const char*>());
        addRow(html, "Rain (this cycle)",  String(doc["rain"].as<float>(), 2)           + " " + doc["rainUnit"].as<const char*>());
        addRow(html, "Rain since start",   String(doc["rainSinceStart"].as<float>(), 2) + " " + doc["rainUnit"].as<const char*>());
    }
    if (bme280Present) {
        addRow(html, "Temperature",        String(doc["temp"].as<float>(), 1) + "&deg;" + doc["tempUnit"].as<const char*>());
        addRow(html, "Humidity",           String(doc["humidityPct"].as<float>(), 1) + " %");
        addRow(html, "Pressure (absolute)",String(doc["pressureAbs"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
        addRow(html, "Pressure (QNH)",     String(doc["pressureQnh"].as<float>(), 1) + " " + doc["pressureUnit"].as<const char*>());
    }
    if (bus1Count > 0) {
        JsonArray ds18b20 = doc["ds18b20"].as<JsonArray>();
        int i = 0;
        for (JsonVariant v : ds18b20)
            addRow(html, "DS18B20 #" + String(i++), String(v.as<float>(), 1) + "&deg;" + doc["ds18b20Unit"].as<const char*>());
    }
    if (gCfg.lipoEn) addRow(html, "Battery", String(doc["batteryVolt"].as<float>(), 3) + " V");
    html += "</table>";

    html += "<h4>Status</h4><table>";
    if (gCfg.debugMode) addRow(html, "Measurement cycle", String(doc["cycle"].as<int>()));
    addRow(html, "Next measurement in", String(doc["nextCycleInSec"].as<uint32_t>()) + " s");
    addRow(html, "BME280 sensor",      bme280Present ? "present" : "not detected");
    addRow(html, "DS18B20 sensors",    String(bus1Count));
    addRow(html, "Uplink send failures", String(sendFailCount));
    addRow(html, "Uptime",             String((float)millis() / 3600000.0f, 2) + " h");
    addRow(html, "Free heap",          String((float)ESP.getFreeHeap() / 1024.0f, 1) + " KiB");
    addRow(html, "LoRaWAN",            gCfg.loraEn ? "enabled" : "disabled");
    addRow(html, "WiFi",               wifiIsConnected() ? ("connected (" + WiFi.localIP().toString() + ")") : "disconnected");
    addRow(html, "MQTT",               !gCfg.mqttEn ? "disabled" :
                                        (mqttIsConnected() ? ("connected (" + String(gCfg.mqttHost) + ":" + String(gCfg.mqttPort) + ")") : "disconnected"));
    html += "</table>";

    html += "<p><a href='/api/settings'>Settings</a> &middot; <a href='/api/log'>Log</a> &middot; "
            "<a href='/help'>Help / Documentation</a> &middot; <a href='/api/data'>Raw JSON</a></p>";
    html += "</body></html>";

    server.send(200, "text/html; charset=utf-8", html);
}

// Gates both viewing and saving the settings page behind HTTP Basic Auth
// (browser's built-in login prompt) using the admin password configured via
// the WiFi portal. No password set at all -> access is refused outright
// rather than prompting for one that can never match.
bool requireAuth()
{
    if (gCfg.webAdminPass[0] == '\0') {
        server.send(403, "text/plain", "Forbidden: no admin password configured (set one via the WiFi portal's \"WiFi + MQTT Parameter\" section).");
        return false;
    }
    if (!server.authenticate("admin", gCfg.webAdminPass)) {
        server.requestAuthentication(BASIC_AUTH, "WeatherNode settings");
        return false;
    }
    return true;
}

// Rolling view of dbgSerial's ring buffer (this device's own log output —
// see serial_log.h for exactly what is/isn't captured: our own code, not
// library-internal prints, and never anything before setup() runs). The
// WebServer library has no server-push mechanism, so the page just polls
// /api/log/raw on an interval instead.
const char LOG_PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WeatherNode log</title>
<style>
  body{font-family:sans-serif;max-width:900px;margin:1rem auto;padding:0 1rem;}
  nav a{margin-right:0.75rem;}
  #log{background:#111;color:#ddd;font-family:ui-monospace,Consolas,monospace;font-size:0.85rem;
       padding:0.75rem;border-radius:6px;height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-word;}
  button{margin:0.5rem 0.5rem 0.5rem 0;}
</style>
</head>
<body>
<nav><a href="/">Status</a><a href="/api/settings">Settings</a><a href="/help">Help / Documentation</a></nav>
<h3>Device log</h3>
<p>Rolling view of this device's own serial output (library-internal messages, e.g. from WiFiManager/RadioLib, aren't captured). Auto-refreshes every second.</p>
<button id="toggle">Pause</button>
<button id="clear">Clear</button>
<pre id="log">Loading...</pre>
<script>
var paused = false, atBottom = true;
var logEl = document.getElementById('log');
logEl.addEventListener('scroll', function () {
  atBottom = (logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight) < 20;
});
document.getElementById('toggle').addEventListener('click', function (e) {
  paused = !paused;
  e.target.textContent = paused ? 'Resume' : 'Pause';
});
document.getElementById('clear').addEventListener('click', function () {
  fetch('/api/log/clear', { method: 'POST' }).then(refresh);
});
function refresh() {
  if (paused) return;
  fetch('/api/log/raw').then(function (r) { return r.text(); }).then(function (t) {
    logEl.textContent = t;
    if (atBottom) logEl.scrollTop = logEl.scrollHeight;
  }).catch(function (e) { logEl.textContent = 'Failed to load log: ' + e; });
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)HTML";

void handleLogPage()
{
    if (!requireAuth()) return;
    server.send_P(200, "text/html; charset=utf-8", LOG_PAGE_HTML);
}

void handleLogRaw()
{
    if (!requireAuth()) return;
    static char buf[8192];
    dbgSerial.snapshot(buf, sizeof(buf));
    server.send(200, "text/plain; charset=utf-8", buf);
}

void handleLogClear()
{
    if (!requireAuth()) return;
    dbgSerial.clear();
    server.send(200, "text/plain", "cleared");
}

// Deliberately NOT a route into the full WiFiManager AP portal (which holds
// WiFi credentials, LoRaWAN AppKey, MQTT credentials/CA cert) — this exposes
// only the same operational parameters as applyDownlinkParam() (interval,
// sensor enables, debug mode). Full portal stays reachable only via
// button-press / first-boot / LoRaWAN downlink 0x03.
void handleSettingsGet()
{
    if (!requireAuth()) return;

    String html;
    html.reserve(900);
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>WeatherNode settings</title></head><body>";
    html += "<h3>WeatherNode operational settings</h3>";
    html += "<p><a href='/'>Status</a> &middot; <a href='/api/log'>Log</a> &middot; <a href='/help'>Help / Documentation</a></p>";
    html += "<p><em>Only the operational settings below can be changed here. Everything "
            "else — WiFi credentials, static IP, LoRaWAN keys/region, MQTT broker/"
            "credentials, sensor calibration, etc. — requires the WiFi config portal "
            "(hold the button for 3&nbsp;s, first boot, or LoRaWAN downlink "
            "<code>0x03</code>).</em></p>";
    html += "<form method='POST' action='/api/settings'>";
    html += "Uplink interval (s): <input name='uplinksec' value='" + String(gCfg.uplinkIntervalSec) + "'><br>";
    html += "Sample interval (s): <input name='samplesec' value='" + String(gCfg.sampleSec) + "'><br>";
    html += "Wind sensor enabled: <input type='checkbox' name='winden'" + String(gCfg.windEn ? " checked" : "") + "><br>";
    html += "Rain sensor enabled: <input type='checkbox' name='rainen'" + String(gCfg.rainEn ? " checked" : "") + "><br>";
    html += "Debug mode: <input type='checkbox' name='debugmode'" + String(gCfg.debugMode ? " checked" : "") + "><br>";
    html += "Imperial units (°F, in, inHg, mph): <input type='checkbox' name='imperial'" + String(gCfg.unitSystem ? " checked" : "") + "><br><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleSettingsPost()
{
    if (!requireAuth()) return;

    if (server.hasArg("uplinksec")) applyDownlinkParam(1, (int16_t)server.arg("uplinksec").toInt());
    if (server.hasArg("samplesec")) applyDownlinkParam(2, (int16_t)server.arg("samplesec").toInt());
    applyDownlinkParam(3, server.hasArg("winden") ? 1 : 0);
    applyDownlinkParam(4, server.hasArg("rainen") ? 1 : 0);
    applyDownlinkParam(11, server.hasArg("imperial") ? 1 : 0);
    applyDownlinkParam(6, server.hasArg("debugmode") ? 1 : 0);
    saveConfig(gCfg);

    server.sendHeader("Location", "/api/settings");
    server.send(303);
}

} // namespace

void webApiSetup()
{
    server.on("/",             HTTP_GET,  handleRoot);
    server.on("/api/data",     HTTP_GET,  handleApiData);
    server.on("/api/settings", HTTP_GET,  handleSettingsGet);
    server.on("/api/settings", HTTP_POST, handleSettingsPost);
    server.on("/api/log",       HTTP_GET,  handleLogPage);
    server.on("/api/log/raw",   HTTP_GET,  handleLogRaw);
    server.on("/api/log/clear", HTTP_POST, handleLogClear);
    server.on("/help",         HTTP_GET,  handleHelp);
    server.on("/help.md",      HTTP_GET,  handleHelpMd);
}

void webApiPoll()
{
    if (!gCfg.wifiEn || !gCfg.webApiEn) {
        if (serverStarted) {
            server.stop();
            serverStarted = false;
        }
        return;
    }

    if (!serverStarted && wifiIsConnected()) {
        server.begin();
        serverStarted = true;
        dbgSerial.println("Web API started on port 80 (/api/data, /api/settings, /help).");
    }

    if (serverStarted) server.handleClient();
}

void webApiStop()
{
    if (serverStarted) {
        server.stop();
        serverStarted = false;
        dbgSerial.println("Web API stopped (port 80 freed for the config portal).");
    }
}
