// =====================================================================
//  SassyBass  -  hands-free talking Billy Bass
//  https://github.com/snydergj/SassyBass
//  Waveshare ESP32-S3-AUDIO-Board + DRV8833 + Google Gemini Live API
//
//  Say "Hi E-S-P" (Espressif's built-in wake word) -> chat -> say "goodbye".
//  The red button (or BOOT / K2) also starts or ends a chat.
//
//  Arduino IDE settings:
//    Board: ESP32S3 Dev Module   (esp32 core 3.3.12 or newer recommended)
//    Flash Size: 16MB
//    Partition Scheme: ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)   <-- needed for the wake phrase
//    PSRAM: OPI PSRAM   USB CDC On Boot: Enabled
//  No extra libraries needed (the es8311 codec driver is included in this folder).
//  The Gemini key is entered per fish on the setup page or over serial (key=...).
//
//  Wi-Fi setup: first boot, failed Wi-Fi, or hold the red button / K1 at power-up
//               -> join "BillyBass-Setup" (password billybass).
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>          // pulls in the speech models (srmodels.bin) and esp-sr libraries
extern "C" {
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_wn_iface.h"
#include "model_path.h"
}
#include <freertos/stream_buffer.h>
#include "es8311.h"
#include "config.h"
#include <ESPmDNS.h>
#include "logtee.h"
#include "portal.h"
#include "ws_client.h"
#include "ota.h"
#include "persona.h"
#include "webui.h"

// ---------------------------------------------------------------- globals
I2SClass i2s;
WsClient ws;
es8311_handle_t es8311 = nullptr;

StreamBufferHandle_t micBuf;     // 16 kHz mono int16 for Gemini
StreamBufferHandle_t srBuf;      // 16 kHz stereo int16 for the wake-phrase recognizer
StreamBufferHandle_t playBuf;    // 24 kHz mono int16 from Gemini

enum Mode { WAKE_LISTEN, CHATTING };
volatile Mode mode = WAKE_LISTEN;
volatile bool micToGemini = false;      // micTask routes audio to Gemini (else to the recognizer)
volatile bool micMuted = false;         // true while Billy is talking (stops him hearing himself)
volatile bool speechPlaying = false;    // set by the audio task while sound is coming out
volatile bool replyComplete = true;     // Gemini finished sending the current reply's audio
volatile int digitalGainPct = 100;     // serial "gain=NN": scale samples before the codec (headroom test)
volatile bool waitWholeReply = false;  // serial "buffer=full": play only after the whole reply arrived (Wi-Fi quiet)
volatile uint32_t audioGaps = 0;        // playback ran dry mid-reply (diagnostics)
// Per-reply audio diagnostics, printed when Billy finishes talking
volatile uint32_t stChunks = 0, stBytes = 0, stBadChunks = 0, stOddChunks = 0;
volatile uint32_t stLateWrites = 0, stMaxWriteGapMs = 0, stMinBufMs = 99999;
uint8_t oddByte = 0; bool haveOddByte = false;
// Diagnostic capture of the last reply: what Gemini sent (24 kHz) and what we
// sent to the speaker (16 kHz). Download at http://<fish-ip>/  (in.wav / out.wav)
const size_t CAP_IN_MAX = API_OUT_RATE * 20, CAP_OUT_MAX = I2S_RATE * 20;   // 20 s each
int16_t *capIn = nullptr, *capOut = nullptr;
volatile size_t capInLen = 0, capOutLen = 0;
void startStationWeb();
void drainStream(StreamBufferHandle_t sb);
volatile bool flushRequested = false;   // set by flushPlayback(); serviced by the speaker task
volatile bool wakeHeard = false;        // set by the recognizer callback
bool sessionReady = false;
bool turnDone = false;
bool endRequested = false;
bool srOk = false;                      // wake-word models loaded and engine works
srmodel_list_t *srModels = nullptr;
volatile bool wakeRunning = false;      // wake engine running (only while waiting for "Hi ESP")
volatile int wakeTasksAlive = 0;
uint32_t endRequestedAt = 0, lastHeard = 0, lastBillyAudio = 0;
String heardThisTurn;

const esp_afe_sr_iface_t *afe = nullptr;   // wake-word front end (WakeNet only, no MultiNet)
esp_afe_sr_data_t *afeData = nullptr;
int volume = SPEAKER_VOLUME;            // 0-100, saved across power cycles

// ---------------------------------------------------------------- helpers
void setLed(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(STATUS_LED_PIN, r, g, b); }

// ---- TCA9555 IO expander (direct register access)
void tcaWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9555_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
uint8_t tcaRead(uint8_t reg) {
  Wire.beginTransmission(TCA9555_ADDR); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TCA9555_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}
void tcaInit() {
  tcaWrite(0x03, 0x01);   // port1 output: EXIO8 (speaker amp enable) high
  tcaWrite(0x07, 0xFE);   // port1 config: EXIO8 output, EXIO9-15 inputs
}
bool exioKeyDown(uint8_t exio) { return !(tcaRead(0x01) & (1 << (exio - 8))); }

// ---- ES7210 microphone ADC (register sequence from Espressif's es7210 driver,
//      16 kHz, MCLK = 256 x fs = 4.096 MHz, 16-bit standard I2S, no TDM)
void es7210Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
void es7210Init() {
  es7210Write(0x00, 0xFF); delay(10);
  es7210Write(0x00, 0x32);
  es7210Write(0x09, 0x30); es7210Write(0x0A, 0x30);            // power-up timing
  es7210Write(0x23, 0x2A); es7210Write(0x22, 0x0A);            // high-pass filters
  es7210Write(0x21, 0x2A); es7210Write(0x20, 0x0A);
  es7210Write(0x11, 0x60);                                     // 16-bit, I2S
  es7210Write(0x12, 0x00);                                     // TDM off
  es7210Write(0x40, 0xC3);                                     // analog power / VMID
  es7210Write(0x41, 0x70); es7210Write(0x42, 0x70);            // mic bias 2.87 V
  for (uint8_t r = 0x43; r <= 0x46; r++) es7210Write(r, 0x0A | 0x10);   // 30 dB gain
  for (uint8_t r = 0x47; r <= 0x4A; r++) es7210Write(r, 0x08);          // mic power
  es7210Write(0x07, 0x20);                                     // OSR
  es7210Write(0x02, 0x01 | (1 << 6) | (1 << 7));               // adc_div 1, doubler, DLL
  es7210Write(0x04, 0x01); es7210Write(0x05, 0x00);            // LRCK divider 256
  es7210Write(0x06, 0x04);                                     // power down DLL
  es7210Write(0x4B, 0x0F); es7210Write(0x4C, 0x0F);            // bias/ADC/PGA on
  es7210Write(0x00, 0x71); es7210Write(0x00, 0x41);            // enable
}

// The codec's 0-100 scale is linear in dB from -95 dB to +32 dB, so "40" was
// nearly silent and "80" added gain that can clip. Map Billy's 0-100 onto the
// useful range (about -32 dB .. 0 dB).
int codecVolume(int v) { return v <= 0 ? 0 : 50 + v / 4; }

void es8311Init() {
  es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false; clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = I2S_RATE * 256;
  clk.sample_frequency = I2S_RATE;
  es8311_init(es8311, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  es8311_microphone_config(es8311, false);
  es8311_voice_volume_set(es8311, codecVolume(volume), NULL);
}

void setVolume(int v) {
  volume = constrain(v, 0, 100);
  es8311_voice_volume_set(es8311, codecVolume(volume), NULL);
  prefs.begin("billy", false);
  prefs.putInt("vol", volume);
  prefs.end();
  Log.printf("[vol] %d\n", volume);
}

// ---------------------------------------------------------------- motors
void motorsInit() {
  ledcAttach(MOUTH_IN1, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(MOUTH_IN2, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(BODY_IN1, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(BODY_IN2, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
}
bool motorsOn = true;                          // serial "motors=off" to test audio without motor noise
void drive(int in1, int in2, int speed) {      // speed -255..255, 0 = coast
  if (!motorsOn) speed = 0;
  if (speed >= 0) { ledcWrite(in1, speed); ledcWrite(in2, 0); }
  else            { ledcWrite(in1, 0);     ledcWrite(in2, -speed); }
}
void mouth(bool open) { drive(MOUTH_IN1, MOUTH_IN2, open ? MOUTH_DUTY : 0); }
void headOut()   { drive(BODY_IN1, BODY_IN2, BODY_DUTY); }
void tailOut()   { drive(BODY_IN1, BODY_IN2, -BODY_DUTY); }
void bodyRelax() { drive(BODY_IN1, BODY_IN2, 0); }

void testMotor(const String &which) {
  if (which == "mouth") { for (int i = 0; i < 3; i++) { mouth(true); delay(200); mouth(false); delay(200); } }
  else if (which == "head") { headOut(); delay(1200); bodyRelax(); }
  else if (which == "tail") { for (int i = 0; i < 3; i++) { tailOut(); delay(250); bodyRelax(); delay(250); } }
}


// ---------------------------------------------------------------- audio tasks
// Mic task: the only reader of the I2S mic. Routes audio either to the wake
// recognizer (stereo) or to Gemini (mono), so they never fight over the bus.
void micTask(void *) {
  static int16_t frame[320 * 2];               // 20 ms stereo
  static int16_t mono[320];
  for (;;) {
    size_t got = i2s.readBytes((char *)frame, sizeof(frame));
    if (micToGemini) {
      if (micMuted) continue;
      size_t n = got / 4;
      for (size_t i = 0; i < n; i++) mono[i] = frame[i * 2 + MIC_CHANNEL];
      xStreamBufferSend(micBuf, mono, n * 2, 0);
    } else if (wakeRunning) {
      xStreamBufferSend(srBuf, frame, got, 0);
    }
  }
}

// Wake-word engine (Espressif AFE + WakeNet). It is started only while we
// wait for "Hi ESP" and completely torn down during a chat, so nothing of it
// runs (or holds RAM) while Billy is talking.

void wakeFeedTask(void *) {
  int chunk = afe->get_feed_chunksize(afeData);
  int ch = afe->get_feed_channel_num(afeData);
  size_t bytes = chunk * ch * sizeof(int16_t);
  int16_t *buf = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t got = 0;
  while (wakeRunning) {
    got += xStreamBufferReceive(srBuf, (uint8_t *)buf + got, bytes - got, pdMS_TO_TICKS(100));
    if (got == bytes) { afe->feed(afeData, buf); got = 0; }
  }
  memset(buf, 0, bytes);                         // a few silent chunks let fetch() return
  for (int i = 0; i < 4; i++) afe->feed(afeData, buf);
  free(buf);
  wakeTasksAlive--;
  vTaskDelete(NULL);
}

void wakeDetectTask(void *) {
  while (wakeRunning) {
    afe_fetch_result_t *res = afe->fetch(afeData);
    if (!res || res->ret_value != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (res->wakeup_state == WAKENET_DETECTED && wakeRunning) wakeHeard = true;
  }
  wakeTasksAlive--;
  vTaskDelete(NULL);
}

bool wakeStart() {
  if (!srModels || wakeRunning) return wakeRunning;
  afe_config_t *cfg = afe_config_init(MIC_CHANNEL == 0 ? "MN" : "NM", srModels, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!cfg) return false;
  afe = esp_afe_handle_from_config(cfg);
  afeData = afe ? afe->create_from_config(cfg) : nullptr;
  afe_config_free(cfg);
  if (!afeData) return false;
  drainStream(srBuf);
  wakeRunning = true;
  wakeTasksAlive = 2;
  xTaskCreatePinnedToCore(wakeFeedTask, "wakeFeed", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(wakeDetectTask, "wakeDetect", 6144, NULL, 2, NULL, 0);
  return true;
}

void wakeStop() {
  if (!wakeRunning) return;
  wakeRunning = false;
  uint32_t t0 = millis();
  while (wakeTasksAlive > 0 && millis() - t0 < 2000) delay(10);
  if (wakeTasksAlive > 0) { Log.println("[wake] tasks didn't stop - leaving engine loaded"); return; }
  afe->destroy(afeData);
  afeData = nullptr;
}

// Speaker task: always writes 20 ms blocks (silence when idle) so I2S never
// replays stale data. Resamples 24 kHz -> 16 kHz and drives the mouth.
void audioTask(void *) {
  // 24 kHz -> 16 kHz with a proper anti-alias filter (polyphase FIR:
  // upsample x2, low-pass at 7.2 kHz, keep every 3rd sample). The old
  // "average two samples" shortcut folded everything above 8 kHz back into
  // the audible band, which made S/F sounds crackle ("six", "seven"...).
  const int TAPS = 64, HIST = TAPS / 2;
  static float h[TAPS];
  static float xin[HIST + 480];                // HIST samples of history + one block
  static int16_t raw[480];
  static int16_t out[320 * 2];                 // 20 ms @ 16 kHz stereo
  {
    const float fc = 7200.0f / 48000.0f;
    float sum = 0;
    for (int k = 0; k < TAPS; k++) {
      float m = k - (TAPS - 1) / 2.0f;
      float sinc = m == 0 ? 2 * fc : sinf(2 * PI * fc * m) / (PI * m);
      float w = 0.42f - 0.5f * cosf(2 * PI * k / (TAPS - 1)) + 0.08f * cosf(4 * PI * k / (TAPS - 1));  // Blackman
      h[k] = sinc * w; sum += h[k];
    }
    for (int k = 0; k < TAPS; k++) h[k] *= 2.0f / sum;   // x2 gain makes up for zero-stuffing
    memset(xin, 0, sizeof(xin));
  }
  int envHist[MOUTH_LAG_BLOCKS + 1] = {0};
  int envIdx = 0;
  bool mouthOpen = false;
  uint32_t openedAt = 0, closedAt = 0, silentBlocks = 0;

  for (;;) {
    // Jitter buffer: network audio arrives in bursts. Start (or resume after a
    // gap) only once PREBUFFER_MS is queued, or the reply is complete.
    static bool flowing = false;
    if (flushRequested) { drainStream(playBuf); flowing = false; flushRequested = false; }
    size_t avail = xStreamBufferBytesAvailable(playBuf);
    bool enough = waitWholeReply ? replyComplete : avail >= (size_t)API_OUT_RATE * 2 * PREBUFFER_MS / 1000;
    if (!flowing && avail > 0 && (enough || replyComplete))
      flowing = true;
    const size_t need = sizeof(raw);
    if (flowing && avail < need && !replyComplete) { flowing = false; audioGaps++; }   // re-buffer, never pad mid-reply
    if (flowing && avail == 0) flowing = false;
    size_t got = flowing ? xStreamBufferReceive(playBuf, (uint8_t *)raw, need, 0) / 2 : 0;
    for (size_t i = got; i < 480; i++) raw[i] = 0;          // end of a reply: finish with silence

    for (int i = 0; i < 480; i++) xin[HIST + i] = raw[i];
    long envSum = 0;
    for (int m = 0; m < 320; m++) {
      int p = 3 * m + 2 * HIST;                // position in the 48 kHz (zero-stuffed) timeline
      float acc = 0;
      for (int k = (p & 1); k < TAPS; k += 2)  // only even positions hold real samples
        acc += h[k] * xin[(p - k) >> 1];
      int v = (int)(acc * digitalGainPct / 100);
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      out[m * 2] = out[m * 2 + 1] = (int16_t)v;
      envSum += abs(v);
    }
    memmove(xin, xin + 480, HIST * sizeof(float));
    int outN = got ? 320 : 0;
    if (got && capOut) {
      size_t n = min((size_t)320, CAP_OUT_MAX - capOutLen);
      for (size_t i = 0; i < n; i++) capOut[capOutLen + i] = out[i * 2];
      capOutLen += n;
    }

    static uint32_t lastWrite = 0;
    uint32_t tw = millis();
    if (flowing && lastWrite) {
      uint32_t gapMs = tw - lastWrite;
      if (gapMs > stMaxWriteGapMs) stMaxWriteGapMs = gapMs;
      if (gapMs > 40) stLateWrites++;
      uint32_t bufMs = avail * 1000 / (API_OUT_RATE * 2);
      if (bufMs < stMinBufMs) stMinBufMs = bufMs;
    }
    i2s.write((uint8_t *)out, sizeof(out));    // blocks ~20 ms: paces this task
    lastWrite = millis();

    // ----- lip sync (delayed to match DMA/speaker latency)
    envHist[envIdx] = outN ? envSum / outN : 0;
    envIdx = (envIdx + 1) % (MOUTH_LAG_BLOCKS + 1);
    int lagged = envHist[envIdx];
    uint32_t now = millis();
    bool wantOpen = mouthOpen ? (lagged > MOUTH_THRESH * 6 / 10) : (lagged > MOUTH_THRESH);
    if (mouthOpen && now - openedAt > MOUTH_MAX_OPEN_MS) wantOpen = false;      // force a flap
    if (!mouthOpen && wantOpen && now - closedAt < 60) wantOpen = false;        // let it close first
    if (wantOpen != mouthOpen) {
      mouthOpen = wantOpen;
      mouth(mouthOpen);
      if (mouthOpen) openedAt = now; else closedAt = now;
    }

    if (outN > 0) { speechPlaying = true; silentBlocks = 0; }
    else if (++silentBlocks > MOUTH_LAG_BLOCKS + 5) speechPlaying = false;
  }
}


// Empty a stream buffer by reading it out. Do NOT use xStreamBufferReset():
// in ESP-IDF it re-fills the whole buffer storage (hundreds of KB of PSRAM)
// inside a critical section with interrupts off. Called every loop while
// Billy talked, that starved the speaker's DMA and caused the stutter/echo.
void drainStream(StreamBufferHandle_t sb) {
  static uint8_t scratch[1024];
  while (xStreamBufferReceive(sb, scratch, sizeof(scratch), 0) > 0) {}
}

// The speaker task is playBuf's only reader, so it does the flush itself.
void flushPlayback() { flushRequested = true; }

// ---------------------------------------------------------------- JSON helpers
String jsonEscape(const char *s) {
  String o;
  for (; *s; s++) {
    char c = *s;
    switch (c) {
      case '"': o += "\\\""; break;  case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;  case '\r': break;
      default: if ((uint8_t)c >= 0x20) o += c;
    }
  }
  return o;
}

// Finds "key" and returns a pointer just past the ':' and any spaces.
const char *findKey(const char *from, const char *key) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(from, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

// Finds "key": "value" -> pointer to value (inside the quotes) and its length.
char *findStr(const char *from, const char *key, size_t &len) {
  const char *p = findKey(from, key);
  if (!p || *p != '"') return nullptr;
  p++;
  const char *e = p;
  while (*e && !(*e == '"' && e[-1] != '\\')) e++;
  len = e - p;
  return (char *)p;
}


// ---------------------------------------------------------------- voice commands (function calling)
// Gemini decides when to call these, based on the name + description.
// To control something else (like a Roomba), add a declaration here and a
// matching branch in runTool().
static const char TOOL_DECLS[] PROGMEM = R"JSON(
"tools":[{"functionDeclarations":[
  {"name":"end_conversation","behavior":"NON_BLOCKING",
   "description":"End the chat. Call this when the user says goodbye, bye, see you later, or asks Billy to stop listening or go to sleep."},
  {"name":"move_fish","behavior":"NON_BLOCKING",
   "description":"Physically move Billy's body. Use when the user asks Billy to dance, wiggle, flap his tail, or turn his head.",
   "parameters":{"type":"OBJECT","properties":{
     "action":{"type":"STRING","enum":["flap_tail","turn_head","dance"]}},
     "required":["action"]}},
  {"name":"set_volume","behavior":"NON_BLOCKING",
   "description":"Change Billy's speaker volume. Use when the user says louder, quieter, turn it up/down, or names a volume level. The result reports the new volume.",
   "parameters":{"type":"OBJECT","properties":{
     "change":{"type":"STRING","enum":["louder","quieter","set"]},
     "level":{"type":"INTEGER","description":"0-100, only used when change is set"}},
     "required":["change"]}},
  {"name":"set_light","behavior":"NON_BLOCKING",
   "description":"Set the color of Billy's status light.",
   "parameters":{"type":"OBJECT","properties":{
     "color":{"type":"STRING","enum":["red","green","blue","purple","white","off"]}},
     "required":["color"]}}
]}]
)JSON";

void requestEnd() {
  if (!endRequested) {
    endRequested = true; endRequestedAt = millis(); turnDone = false;
    Log.println("[chat] goodbye heard");
  }
}

String runTool(const String &name, const char *callJson) {
  String pr = runPersonaTool(name, callJson, findStr, findKey);
  if (pr.length()) return pr;
  size_t l;
  if (name == "end_conversation") { requestEnd(); return "ok, ending after your goodbye"; }
  if (name == "move_fish") {
    char *a = findStr(callJson, "action", l);
    String action = a ? String(a, l) : "";
    if (action == "flap_tail") testMotor("tail");
    else if (action == "turn_head") testMotor("head");
    else if (action == "dance") { for (int i = 0; i < 3; i++) { headOut(); delay(300); tailOut(); delay(300); } bodyRelax(); }
    else return "unknown action";
    return "done";
  }
  if (name == "set_volume") {
    char *c = findStr(callJson, "change", l);
    String change = c ? String(c, l) : "set";
    const char *lv = findKey(callJson, "level");
    if (change == "louder") setVolume(volume + 15);
    else if (change == "quieter") setVolume(volume - 15);
    else if (lv) setVolume((int)atof(lv));
    return "volume is now " + String(volume) + " out of 100";
  }
  if (name == "set_light") {
    char *c = findStr(callJson, "color", l);
    String color = c ? String(c, l) : "";
    if (color == "red") setLed(60, 0, 0);
    else if (color == "green") setLed(0, 60, 0);
    else if (color == "blue") setLed(0, 0, 60);
    else if (color == "purple") setLed(40, 0, 60);
    else if (color == "white") setLed(50, 50, 50);
    else setLed(0, 0, 0);
    return "light is now " + color;
  }
  return "unknown function";
}

// Returns the end of the JSON object starting at p ('{'), skipping strings.
static const char *objectEnd(const char *p) {
  int depth = 0; bool inStr = false;
  for (; *p; p++) {
    if (inStr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') inStr = false; continue; }
    if (*p == '"') inStr = true;
    else if (*p == '{') depth++;
    else if (*p == '}' && --depth == 0) return p;
  }
  return nullptr;
}

// toolCall: {"toolCall":{"functionCalls":[{"name":"..","args":{..},"id":".."}, ...]}}
// Key order varies, so each call object is cut out and searched on its own.
void handleToolCall(char *msg) {
  String responses;
  const char *p = findKey(msg, "functionCalls");
  if (p) p = strchr(p, '[');
  while (p && (p = strchr(p, '{'))) {
    const char *e = objectEnd(p);
    if (!e) break;
    String one(p, e - p + 1);
    p = e + 1;
    size_t l;
    char *np = findStr(one.c_str(), "name", l);
    if (!np) continue;
    String name(np, l);
    char *idp = findStr(one.c_str(), "id", l);
    String id = idp ? String(idp, l) : "";
    String result = runTool(name, one.c_str());
    Log.printf("[tool] %s -> %s\n", name.c_str(), result.c_str());
    if (name == "switch_persona") turnDone = false;   // wait for the farewell to finish
    if (responses.length()) responses += ",";
    responses += "{";
    if (id.length()) responses += "\"id\":\"" + id + "\",";
    responses += "\"name\":\"" + name + "\",\"response\":{\"result\":\"" + jsonEscape(result.c_str()) + "\",\"scheduling\":\"SILENT\"}}";
    const char *close = strchr(p, ']');                  // stop at the end of the functionCalls array
    const char *nextObj = strchr(p, '{');
    if (!nextObj || (close && close < nextObj)) break;
  }
  if (responses.isEmpty()) { Log.println("[tool] couldn't parse call"); return; }
  ws.sendText("{\"toolResponse\":{\"functionResponses\":[" + responses + "]}}");
}

// ---------------------------------------------------------------- Gemini Live
// Automatic voice-activity detection is ON: Gemini decides when you've
// finished talking, so no button is needed.
void sendSetup() {
  String tools = FPSTR(TOOL_DECLS);
  tools.replace("\"functionDeclarations\":[", String("\"functionDeclarations\":[") + FPSTR(PERSONA_TOOL_DECLS));
  String j = "{\"setup\":{"
             "\"model\":\"models/" GEMINI_MODEL "\","
             "\"generationConfig\":{"
               "\"responseModalities\":[\"AUDIO\"],"
               "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"" + jsonEscape(activeVoice().c_str()) + "\"}}}"
             "},"
             "\"systemInstruction\":{\"parts\":[{\"text\":\"" + jsonEscape(buildInstructions().c_str()) + "\"}]},"
             "\"inputAudioTranscription\":{},"
             "\"outputAudioTranscription\":{},"
             + tools +
             "}}";
  Log.printf("[ai] persona: %s (voice %s)\n", activeName().c_str(), activeVoice().c_str());
  ws.sendText(j);
}

bool saidGoodbye(String t) {
  t.toLowerCase();
  return t.indexOf("goodbye") >= 0 || t.indexOf("good bye") >= 0 || t.indexOf("bye billy") >= 0 ||
         t.startsWith("bye") || t.indexOf(" bye") >= 0;
}

void onWsMessage(char *msg, size_t len) {
  if (findKey(msg, "setupComplete")) {
    sessionReady = true;
    Log.println("[ai] session ready");
    // Billy speaks first so you know he's awake (no need to say "Hey Billy").
    ws.sendText("{\"realtimeInput\":{\"text\":\"(Someone just woke you up. Greet them in one short, sassy sentence.)\"}}");
    return;
  }
  if (findKey(msg, "toolCall")) { handleToolCall(msg); return; }
  if (findKey(msg, "goAway")) { Log.println("[ai] session time limit - wrapping up"); requestEnd(); return; }
  if (!findKey(msg, "serverContent")) {
    if (len > 8 && !findKey(msg, "usageMetadata") && !findKey(msg, "voiceActivity") &&
        !findKey(msg, "sessionResumptionUpdate"))
      Log.printf("[ai] other: %.200s\n", msg);    // unexpected - log it
    return;
  }

  // Read the text fields first: decoding audio in place scrambles the text after it.
  bool interrupted = findKey(msg, "interrupted");
  bool complete = findKey(msg, "turnComplete");
  const char *it = findKey(msg, "inputTranscription");
  if (it) {
    size_t tl; char *t = findStr(it, "text", tl);
    if (t) {
      String piece(t, tl);
      heardThisTurn += piece;
      lastHeard = millis();
      Log.print("[you] "); Log.println(piece);
      if (saidGoodbye(heardThisTurn)) requestEnd();      // backup in case the tool isn't called
    }
  }
  const char *ot = findKey(msg, "outputTranscription");
  if (ot) {
    size_t tl; char *t = findStr(ot, "text", tl);
    bool placeholder = t && (strncmp(t, "\\u003c!--", 10) == 0 || strncmp(t, "<!--", 4) == 0);   // Gemini's "empty response" marker
    if (t && !placeholder) { Log.print("[billy] "); Log.write((const uint8_t *)t, tl); Log.println(); }
  }
  if (interrupted) flushPlayback();

  if (!interrupted) {
    const char *p = msg;
    size_t dl;
    while (char *d = findStr(p, "data", dl)) {
      size_t olen = 0;
      int rc = mbedtls_base64_decode((unsigned char *)d, dl, &olen, (const unsigned char *)d, dl);
      if (rc != 0) {
        stBadChunks++;
        Log.printf("[audio] base64 decode failed (%d), %u chars, starts: %.24s\n", rc, (unsigned)dl, d);
      } else if (olen) {
        micMuted = true;                                // Billy is about to talk
        lastBillyAudio = millis();
        if (replyComplete) { capInLen = 0; capOutLen = 0; }   // first audio of a new reply
        replyComplete = false;
        stChunks++; stBytes += olen;
        if (capIn) {
          size_t n = min((size_t)(olen / 2), CAP_IN_MAX - capInLen);
          memcpy(capIn + capInLen, d, n * 2);           // note: d still at chunk start here
          capInLen += n;
        }
        uint8_t *bytes = (uint8_t *)d;
        if (haveOddByte) {                              // finish a sample split across chunks
          uint8_t pair[2] = {oddByte, bytes[0]};
          while (xStreamBufferSend(playBuf, pair, 2, pdMS_TO_TICKS(50)) != 2) {}
          bytes++; olen--; haveOddByte = false;
        }
        if (olen & 1) { stOddChunks++; oddByte = bytes[olen - 1]; haveOddByte = true; olen--; }
        size_t sent = 0;
        while (sent < olen)
          sent += xStreamBufferSend(playBuf, bytes + sent, olen - sent, pdMS_TO_TICKS(50));
      }
      p = d + dl + 1;
    }
  }
  if (interrupted) replyComplete = true;
  if (complete) { turnDone = true; replyComplete = true; heardThisTurn = ""; lastHeard = millis(); }
}

bool connectGemini() {
  sessionReady = false;
  String err;
  String path = String(GEMINI_PATH) + "?key=" + geminiKey;
  Log.println("[ai] connecting...");
  if (!ws.connect(GEMINI_HOST, path, "", err)) {
    Log.println("[ai] connect failed: " + err);
    return false;
  }
  sendSetup();
  return true;
}

// Streams mic audio to Gemini in 100 ms chunks.
void pumpMicAudio() {
  static int16_t chunk[1600];
  static char b64[4400];
  static String j;
  while (xStreamBufferBytesAvailable(micBuf) >= sizeof(chunk)) {
    size_t n = xStreamBufferReceive(micBuf, chunk, sizeof(chunk), 0);
    size_t bl = 0;
    mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &bl, (const unsigned char *)chunk, n);
    j = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
    j.concat(b64, bl);
    j += "\"}}}";
    ws.sendText(j);
  }
}

// ---------------------------------------------------------------- chat start / stop
void listenForWake() {
  mode = WAKE_LISTEN;
  micToGemini = false;
  micMuted = false;
  drainStream(srBuf);
  bodyRelax();
  setLed(0, 0, 0);
  bool on = srOk && wakeStart();
  Log.println(on ? "[wake] listening for \"Hi E-S-P\"" : "[wake] wake word off - use the button");
}

void startChat() {
  Log.println("[chat] start");
  mode = CHATTING;
  drainStream(micBuf);
  flushPlayback();
  micMuted = false;
  micToGemini = true;                 // mic audio buffers while we connect
  wakeStop();                         // free the CPU and RAM the wake engine uses
  endRequested = false;
  turnDone = false;
  heardThisTurn = "";
  lastHeard = millis();
  setLed(0, 0, 60);
  headOut(); delay(400); bodyRelax();  // "I'm listening"
  if (!connectGemini()) {
    setLed(60, 0, 0);
    tailOut(); delay(300); bodyRelax();
    delay(1500);
    listenForWake();
  }
}

void endChat() {
  Log.println("[chat] end");
  micToGemini = false;
  ws.close();
  sessionReady = false;
  flushPlayback();
  tailOut(); delay(200); bodyRelax(); delay(150); tailOut(); delay(200); bodyRelax();
  listenForWake();
}

// ---------------------------------------------------------------- buttons
bool talkPressed() {
  return digitalRead(TALK_BTN_PIN) == LOW || digitalRead(BOOT_BTN_PIN) == LOW || exioKeyDown(EXIO_KEY2);
}

// ---------------------------------------------------------------- wake recognizer
static void printHeap(const char *tag) {
  Log.printf("[mem] %s: internal free %u (largest %u), PSRAM free %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Starts ESP-SR. If it ever crashes the board, it stays off for this firmware
// version (button-only mode) so a gift fish can never get stuck in a reboot
// loop. A new firmware version tries again.
void startWakeRecognizer() {
  prefs.begin("billy", false);
  if (prefs.getBool("srtry", false) && esp_reset_reason() == ESP_RST_PANIC)
    prefs.putString("srbad", FW_VERSION);              // crashed during the last attempt
  prefs.putBool("srtry", false);
  bool disabled = prefs.getString("srbad", "") == FW_VERSION;
  if (!disabled) prefs.putBool("srtry", true);
  prefs.end();
  if (disabled) {
    Log.println("!! Wake phrase crashed on this firmware before - disabled. Button still works.");
    return;
  }

  printHeap("before wake recognizer");
  srModels = esp_srmodel_init("model");
  if (!srModels || srModels->num == 0) {
    Log.println("!! No speech models found - check Partition Scheme = ESP SR 16M. Button still works.");
  } else {
    srOk = wakeStart();               // first start inside the crash guard
    if (!srOk) Log.println("!! Wake word failed to start. Button still works.");
  }
  printHeap("after wake recognizer");

  prefs.begin("billy", false);
  prefs.putBool("srtry", false);
  prefs.end();
}

// ---------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(TALK_BTN_PIN, INPUT_PULLUP);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  setLed(20, 20, 20);
  if (!psramFound()) Log.println("!! PSRAM not enabled - set Tools > PSRAM > OPI PSRAM");

  loadSettings();
  storageBegin();
  Log.printf("\nSassyBass firmware %s\n", FW_VERSION);
  prefs.begin("billy", true);
  volume = prefs.getInt("vol", SPEAKER_VOLUME);
  prefs.end();
  motorsInit();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  tcaInit();

  i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCLK_PIN);
  if (!i2s.begin(I2S_MODE_STD, I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Log.println("!! I2S begin failed");
  es8311Init();
  es7210Init();

  static StaticStreamBuffer_t micSB, srSB, playSB;
  size_t micSize = I2S_RATE * 2 * 20;            // 20 s
  size_t srSize = I2S_RATE * 4 * 2;              // 2 s stereo
  size_t playSize = API_OUT_RATE * 2 * 60;       // 60 s
  micBuf = xStreamBufferCreateStatic(micSize, 1, (uint8_t *)ps_malloc(micSize + 1), &micSB);
  srBuf = xStreamBufferCreateStatic(srSize, 1, (uint8_t *)ps_malloc(srSize + 1), &srSB);
  playBuf = xStreamBufferCreateStatic(playSize, 1, (uint8_t *)ps_malloc(playSize + 1), &playSB);

  xTaskCreatePinnedToCore(audioTask, "audio", 6144, NULL, 10, NULL, 1);   // speaker: highest
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 9, NULL, 1);
  ws.onMessage(onWsMessage);

  bool forceSetup = digitalRead(TALK_BTN_PIN) == LOW || exioKeyDown(EXIO_KEY1);
  if (wifiSsid.isEmpty() || geminiKey.isEmpty() || forceSetup) {
    if (geminiKey.isEmpty()) Log.println("No Gemini key saved - enter it on the setup page or send key=AIza... over serial");
    setLed(30, 0, 40); startPortal(); return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Log.print("Wi-Fi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); Log.print('.'); }
  Log.println();
  if (WiFi.status() != WL_CONNECTED) {
    Log.println("Wi-Fi failed - starting setup portal");
    setLed(30, 0, 40); startPortal(); return;
  }
  Log.println("Wi-Fi OK: " + WiFi.localIP().toString());
  startStationWeb();
  checkForUpdate();                                    // reboots if a newer release was installed
  startWakeRecognizer();                               // after the update check so it has RAM to itself

  headOut(); delay(500); bodyRelax(); tailOut(); delay(250); bodyRelax();   // hello wiggle
  listenForWake();
}

// ---------------------------------------------------------------- loop
// USB serial commands:  key=AIza...   (save Gemini key)
//                       wifi=SSID,password
//                       wake=reset  (re-enable a wake phrase that was disabled after a crash)
//                       restart
void serialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("key=")) {
    geminiKey = line.substring(4); geminiKey.trim();
    saveSettings();
    Log.println("Gemini key saved. Type restart to apply.");
  } else if (line.startsWith("wifi=")) {
    int comma = line.indexOf(',');
    wifiSsid = line.substring(5, comma < 0 ? line.length() : comma);
    wifiPass = comma < 0 ? "" : line.substring(comma + 1);
    saveSettings();
    Log.println("Wi-Fi saved. Type restart to apply.");
  } else if (line == "motors=off" || line == "motors=on") {
    motorsOn = line.endsWith("on");
    drive(MOUTH_IN1, MOUTH_IN2, 0); drive(BODY_IN1, BODY_IN2, 0);
    Log.printf("Motors %s (until restart)\n", motorsOn ? "ON" : "OFF");
  } else if (line.startsWith("gain=")) {
    digitalGainPct = constrain(line.substring(5).toInt(), 5, 200);
    Log.printf("Digital gain %d%% (until restart)\n", digitalGainPct);
  } else if (line == "buffer=full" || line == "buffer=stream") {
    waitWholeReply = line.endsWith("full");
    Log.printf("Playback: %s (until restart)\n", waitWholeReply ? "wait for the whole reply, then play" : "stream as it arrives");
  } else if (line == "replay") {
    // Play the last captured Gemini reply again through the speaker
    if (!capIn || !capInLen) { Log.println("Nothing captured yet."); return; }
    Log.printf("Replaying last reply (%u ms)...\n", (unsigned)(capInLen * 1000 / API_OUT_RATE));
    size_t n = capInLen, sent = 0;
    while (sent < n * 2) sent += xStreamBufferSend(playBuf, (uint8_t *)capIn + sent, n * 2 - sent, portMAX_DELAY);
  } else if (line == "tone loud") {
    // Like "tone" but near full scale, as loud as speech peaks
    Log.println("Playing LOUD test tone...");
    static int16_t blk2[480];
    float ph = 0;
    for (int n = 0; n < API_OUT_RATE * 4; n += 480) {
      for (int i = 0; i < 480; i++) {
        float f = (n + i) < API_OUT_RATE * 2 ? 440.0f : 200.0f + 1800.0f * (n + i - API_OUT_RATE * 2) / (API_OUT_RATE * 2);
        ph += 2 * PI * f / API_OUT_RATE; if (ph > 2 * PI) ph -= 2 * PI;
        blk2[i] = (int16_t)(28000 * sinf(ph));
      }
      xStreamBufferSend(playBuf, blk2, sizeof(blk2), portMAX_DELAY);
    }
  } else if (line == "tone") {
    // 2 s of 440 Hz then 2 s of a slow sweep, through the same path as Billy's voice
    Log.println("Playing test tone...");
    static int16_t blk[480];
    float ph = 0;
    for (int n = 0; n < API_OUT_RATE * 4; n += 480) {
      for (int i = 0; i < 480; i++) {
        float f = (n + i) < API_OUT_RATE * 2 ? 440.0f : 200.0f + 1800.0f * (n + i - API_OUT_RATE * 2) / (API_OUT_RATE * 2);
        ph += 2 * PI * f / API_OUT_RATE; if (ph > 2 * PI) ph -= 2 * PI;
        blk[i] = (int16_t)(8000 * sinf(ph));
      }
      xStreamBufferSend(playBuf, blk, sizeof(blk), portMAX_DELAY);
    }
  } else if (line == "wake=reset") {
    prefs.begin("billy", false); prefs.remove("srbad"); prefs.end();
    Log.println("Wake phrase re-enabled. Type restart to apply.");
  } else if (line == "restart") {
    ESP.restart();
  } else if (line.length()) {
    Log.println("Commands: key=..., wifi=SSID,password, motors=off|on, tone, tone loud, replay, gain=NN, buffer=full|stream, wake=reset, restart");
  }
}

const char *billyStateText() {
  if (mode != CHATTING) return "waiting for \"Hi ESP\"";
  if (speechPlaying) return "talking";
  return "listening";
}

// ---- tiny diagnostics web page on the home network
void sendWav(const int16_t *data, size_t samples, uint32_t rate) {
  uint8_t hdr[44];
  uint32_t dataBytes = samples * 2, riff = 36 + dataBytes, byteRate = rate * 2;
  memcpy(hdr, "RIFF", 4); memcpy(hdr + 4, &riff, 4); memcpy(hdr + 8, "WAVEfmt ", 8);
  uint32_t fmtLen = 16; uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  memcpy(hdr + 16, &fmtLen, 4); memcpy(hdr + 20, &pcm, 2); memcpy(hdr + 22, &ch, 2);
  memcpy(hdr + 24, &rate, 4); memcpy(hdr + 28, &byteRate, 4); memcpy(hdr + 32, &align, 2);
  memcpy(hdr + 34, &bits, 2); memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &dataBytes, 4);
  web.setContentLength(44 + dataBytes);
  web.send(200, "audio/wav", "");
  WiFiClient c = web.client();
  c.write(hdr, 44);
  const uint8_t *p = (const uint8_t *)data;
  for (size_t off = 0; off < dataBytes; off += 4096) c.write(p + off, min((size_t)4096, (size_t)(dataBytes - off)));
}

void startStationWeb() {
  capIn = (int16_t *)ps_malloc(CAP_IN_MAX * 2);
  capOut = (int16_t *)ps_malloc(CAP_OUT_MAX * 2);
  web.on("/diag", HTTP_GET, [] {
    web.send(200, "text/html",
      "<html><body style='font-family:sans-serif'><h2>SassyBass last reply</h2>"
      "<p>From Gemini (24 kHz):<br><audio controls src='/in.wav'></audio> <a href='/in.wav'>download</a></p>"
      "<p>Sent to speaker (16 kHz):<br><audio controls src='/out.wav'></audio> <a href='/out.wav'>download</a></p>"
      "</body></html>");
  });
  web.on("/in.wav", HTTP_GET, [] { sendWav(capIn, capInLen, API_OUT_RATE); });
  web.on("/out.wav", HTTP_GET, [] { sendWav(capOut, capOutLen, I2S_RATE); });
  registerWebApi();
  web.begin();
  if (MDNS.begin("billybass")) MDNS.addService("http", "tcp", 80);
  Log.println("[web] settings at http://billybass.local  (or http://" + WiFi.localIP().toString() + "/)");
}

void loop() {
  serialCommands();
  if (portalMode) { portalLoop(); return; }
  web.handleClient();

  uint32_t now = millis();

  // Button: start or end a chat (edge-triggered)
  static bool lastPressed = false;
  static uint32_t lastPressAt = 0;
  bool pressed = talkPressed();
  bool click = pressed && !lastPressed && now - lastPressAt > 400;
  lastPressed = pressed;
  if (click) lastPressAt = now;

  // K1 = volume down, K3 = volume up
  static bool lastDown = false, lastUp = false;
  bool down = exioKeyDown(EXIO_KEY1), up = exioKeyDown(EXIO_KEY3);
  if (down && !lastDown) setVolume(volume - 10);
  if (up && !lastUp) setVolume(volume + 10);
  lastDown = down; lastUp = up;

  if (mode == WAKE_LISTEN) {
    personaSwitchPending = false;       // applies automatically at the next chat
    static uint32_t lastUpdateCheck = 0;
    if (lastUpdateCheck == 0) lastUpdateCheck = now;   // boot already checked
    if (now - lastUpdateCheck > UPDATE_EVERY_MS) {
      lastUpdateCheck = now;
      wakeStop();                         // free RAM for the TLS download
      checkForUpdate();
      wakeStart();
    }
    if (wakeHeard || click) { wakeHeard = false; startChat(); }
    delay(5);
    return;
  }

  // ---- CHATTING
  ws.poll();
  now = millis();     // poll() can stamp newer times (lastHeard etc.); keep now >= them

  // New persona/voice requested (by voice or the web page): reconnect once Billy is quiet
  if (personaSwitchPending && !endRequested && turnDone && replyComplete && xStreamBufferIsEmpty(playBuf) && !speechPlaying) {
    personaSwitchPending = false;
    Log.println("[chat] switching persona - reconnecting");
    ws.close();
    sessionReady = false;
    turnDone = false;
    lastHeard = millis();
    if (!connectGemini()) { endChat(); return; }
  }
  if (!ws.connected()) { Log.println("[ai] connection closed"); endChat(); return; }

  // Billy's turn lasts from his first audio until Gemini says the reply is
  // complete AND the speaker has drained. The mic stays muted the whole time,
  // even through network gaps, so he can't hear himself and interrupt
  // (that caused the stutter-then-repeat behaviour).
  bool billyTalking = speechPlaying || !xStreamBufferIsEmpty(playBuf) || !replyComplete;
  static bool wasTalking = false;
  if (billyTalking) {
    lastBillyAudio = now;
    if (!wasTalking) { headOut(); setLed(0, 40, 0); }  // head out while he talks
  } else if (now - lastBillyAudio > MIC_HOLDOFF_MS) {
    if (micMuted) { micMuted = false; drainStream(micBuf); bodyRelax(); setLed(0, 0, 60); }
  }
  if (wasTalking && !billyTalking) {
    Log.printf("[audio] reply: %u chunks, %u ms audio, gaps %u, bad %u, odd %u, late writes %u (max %u ms), min buffer %u ms\n",
                  (unsigned)stChunks, (unsigned)(stBytes * 1000 / (API_OUT_RATE * 2)), (unsigned)audioGaps,
                  (unsigned)stBadChunks, (unsigned)stOddChunks, (unsigned)stLateWrites, (unsigned)stMaxWriteGapMs,
                  (unsigned)(stMinBufMs == 99999 ? 0 : stMinBufMs));
    audioGaps = stChunks = stBytes = stBadChunks = stOddChunks = stLateWrites = stMaxWriteGapMs = 0;
    stMinBufMs = 99999;
  }
  wasTalking = billyTalking;

  if (sessionReady && !micMuted) pumpMicAudio();


  // Ending: wait for Billy's goodbye to finish playing
  if (endRequested) {
    bool farewellDone = turnDone && !billyTalking;
    if (farewellDone || now - endRequestedAt > END_GRACE_MS) { endChat(); return; }
  }
  if (click) { requestEnd(); endRequestedAt = now - END_GRACE_MS; }   // button = end right away
  if (!billyTalking && now - lastHeard > CONVO_IDLE_MS && now - lastBillyAudio > CONVO_IDLE_MS) {
    Log.println("[chat] nobody's talking - going back to sleep");
    endChat();
    return;
  }
  delay(2);
}
