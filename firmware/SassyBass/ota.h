// =====================================================================
//  Over-the-air updates from GitHub Pages
//  version.json:  {"version":"1.2.0","bin":"https://.../SassyBass.ino.bin"}
//  The new image is written to the spare app slot and verified before the
//  fish switches to it, so a failed or partial download leaves the old
//  firmware running.
// =====================================================================
#pragma once
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include "config.h"

void setLed(uint8_t r, uint8_t g, uint8_t b);   // in the main sketch

// "1.10.2" -> comparable number; anything non-numeric (e.g. "dev") -> 0
static uint32_t versionNumber(const String &v) {
  int a = 0, b = 0, c = 0;
  if (sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c) < 1) return 0;
  return (uint32_t)a * 1000000UL + (uint32_t)b * 1000UL + (uint32_t)c;
}

static String jsonValue(const String &json, const char *key) {
  int k = json.indexOf(String("\"") + key + "\"");
  if (k < 0) return "";
  int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  return (q1 < 0 || q2 < 0) ? "" : json.substring(q1 + 1, q2);
}

static void secureClient(NetworkClientSecure &c) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12)
  c.useBuiltinCACertBundle();
#else
  c.setInsecure();
#endif
}

// Returns only if no update was installed (an update reboots the board).
void checkForUpdate() {
  if (versionNumber(FW_VERSION) == 0) {
    Serial.println("[ota] dev build - auto-update off");
    return;
  }
  NetworkClientSecure client; secureClient(client);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, UPDATE_URL)) return;
  int code = http.GET();
  if (code != 200) { Serial.printf("[ota] check failed (%d)\n", code); http.end(); return; }
  String body = http.getString();
  http.end();

  String latest = jsonValue(body, "version");
  String bin = jsonValue(body, "bin");
  Serial.printf("[ota] running %s, latest %s\n", FW_VERSION, latest.c_str());
  if (versionNumber(latest) <= versionNumber(FW_VERSION) || bin.isEmpty()) return;

  Serial.println("[ota] updating from " + bin);
  setLed(50, 50, 50);
  NetworkClientSecure dl; secureClient(dl);
  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    static int lastPct = -1;
    int pct = total ? cur * 100 / total : 0;
    if (pct / 10 != lastPct / 10) { Serial.printf("[ota] %d%%\n", pct); lastPct = pct; }
    setLed((pct / 10) % 2 ? 50 : 10, (pct / 10) % 2 ? 50 : 10, (pct / 10) % 2 ? 50 : 10);
  });
  t_httpUpdate_return r = httpUpdate.update(dl, bin);
  if (r == HTTP_UPDATE_FAILED)
    Serial.printf("[ota] failed: %s\n", httpUpdate.getLastErrorString().c_str());
  setLed(0, 0, 0);
}
