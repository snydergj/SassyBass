// =====================================================================
//  Settings storage + Wi-Fi setup page (captive portal)
//  The Gemini key lives in the board's settings storage (NVS), never in the
//  public firmware. Updates (OTA or web flasher without erase) keep it.
// =====================================================================
#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"

String wifiSsid, wifiPass, geminiKey;
Preferences prefs;
WebServer web(80);
DNSServer dns;
bool portalMode = false;
String scannedOptions;

void testMotor(const String &which);   // in the main sketch

void loadSettings() {
  prefs.begin("billy", true);
  wifiSsid  = prefs.getString("ssid", "");
  wifiPass  = prefs.getString("pass", "");
  geminiKey = prefs.getString("gkey", "");
  prefs.end();
#if __has_include("secrets.h")              // optional: your own local builds can seed the key
#include "secrets.h"
  if (geminiKey.isEmpty() && strlen(GEMINI_API_KEY) > 20) {
    geminiKey = GEMINI_API_KEY;
    prefs.begin("billy", false); prefs.putString("gkey", geminiKey); prefs.end();
  }
#endif
}

void saveSettings() {
  prefs.begin("billy", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putString("gkey", geminiKey);
  prefs.end();
}

static String htmlEscape(const String &s) {
  String o; o.reserve(s.length() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;  case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;   case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

static const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SassyBass Setup</title>
<style>
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:480px;margin:auto;padding:18px;background:#0f2a1d;color:#eef}
h1{margin:4px 0 2px;font-size:1.7em}.sub{opacity:.75;margin-bottom:16px}
label{display:block;margin:14px 0 5px}
input{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;border:1px solid #577;background:#163;color:#fff;font-size:1.05em}
button{width:100%;padding:13px;border:0;border-radius:9px;background:#3c6;color:#032;font-weight:700;font-size:1.05em;margin-top:18px}
.owner{margin-top:18px;padding:10px 12px;border:1px dashed #6a8;border-radius:9px}
.hint{font-size:.8em;opacity:.7;margin-top:4px}
.tests{margin-top:28px;opacity:.85;font-size:.9em}.tests button{width:auto;padding:8px 12px;background:#355;color:#dfe;margin:6px 4px 0 0;font-size:.9em}
.ver{margin-top:22px;font-size:.75em;opacity:.5}
</style></head><body><h1>&#x1F41F; Hi, I'm Billy!</h1>
<div class="sub">Pick your Wi-Fi so I can start talking.</div>
)HTML";

void handleRoot() {
  bool showKey = geminiKey.isEmpty() || web.hasArg("owner");
  String p = FPSTR(PAGE_HEAD);
  p += "<form method='POST' action='/save'>"
       "<label>Wi-Fi network</label>"
       "<input name='ssid' list='nets' autocomplete='off' value='" + htmlEscape(wifiSsid) + "' required>"
       "<datalist id='nets'>" + scannedOptions + "</datalist>"
       "<label>Wi-Fi password</label><input name='pass' type='password'>";
  if (showKey) {
    p += "<div class='owner'><label>Gemini API key (owner setup)</label>"
         "<input name='gkey' type='password' autocomplete='off' placeholder='" +
         String(geminiKey.length() ? "(saved - leave blank to keep)" : "AIza...") + "'>"
         "<div class='hint'>Stored only on this fish. Firmware updates keep it.</div></div>";
  }
  p += "<button type='submit'>Connect</button></form>"
       "<div class='tests'>Motor check: "
       "<button onclick=\"fetch('/test?m=mouth')\">Mouth</button>"
       "<button onclick=\"fetch('/test?m=head')\">Head</button>"
       "<button onclick=\"fetch('/test?m=tail')\">Tail</button></div>"
       "<div class='ver'>SassyBass firmware " FW_VERSION "</div>"
       "</body></html>";
  web.send(200, "text/html", p);
}

void handleSave() {
  wifiSsid = web.arg("ssid"); wifiSsid.trim();
  wifiPass = web.arg("pass");
  String k = web.arg("gkey"); k.trim();
  if (k.length()) geminiKey = k;
  saveSettings();
  web.send(200, "text/html", String(FPSTR(PAGE_HEAD)) +
           "<p>Thanks! I'm joining <b>" + htmlEscape(wifiSsid) + "</b> now.</p>"
           "<p>Say <b>\"Hi E-S-P\"</b> and start chatting. If my light turns purple again, "
           "the password didn't work, so join <b>" SETUP_AP_SSID "</b> and try again.</p></body></html>");
  delay(1500);
  ESP.restart();
}

void startPortal() {
  portalMode = true;
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) scannedOptions += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>";
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/test", HTTP_GET, [] { testMotor(web.arg("m")); web.send(200, "text/plain", "ok"); });
  web.onNotFound([] {               // send every phone "connectivity check" to the setup page
    web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
  Serial.printf("Setup portal: join '%s' (password '%s'), then open http://%s\n",
                SETUP_AP_SSID, SETUP_AP_PASS, WiFi.softAPIP().toString().c_str());
}

void portalLoop() {
  if (!portalMode) return;
  dns.processNextRequest();
  web.handleClient();
}
