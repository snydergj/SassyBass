// =====================================================================
//  SassyBass  -  hands-free talking Billy Bass
//  https://github.com/snydergj/SassyBass
//  Waveshare ESP32-S3-AUDIO-Board + DRV8833 + Google Gemini Live API
//
//  Say "Hey Billy" -> chat back and forth -> say "goodbye".
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
#include <ESP_SR.h>
#include <freertos/stream_buffer.h>
#include "es8311.h"
#include "config.h"
#include "portal.h"
#include "ws_client.h"
#include "ota.h"

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
volatile bool wakeHeard = false;        // set by the recognizer callback
volatile bool srTimedOut = false;
bool sessionReady = false;
bool turnDone = false;
bool endRequested = false;
bool srOk = false;
uint32_t endRequestedAt = 0, lastHeard = 0, lastBillyAudio = 0;
String heardThisTurn;

static const sr_cmd_t wakeCommands[] = WAKE_PHRASES;
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

void es8311Init() {
  es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false; clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = I2S_RATE * 256;
  clk.sample_frequency = I2S_RATE;
  es8311_init(es8311, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  es8311_microphone_config(es8311, false);
  es8311_voice_volume_set(es8311, volume, NULL);
}

void setVolume(int v) {
  volume = constrain(v, 0, 100);
  es8311_voice_volume_set(es8311, volume, NULL);
  prefs.begin("billy", false);
  prefs.putInt("vol", volume);
  prefs.end();
  Serial.printf("[vol] %d\n", volume);
}

// ---------------------------------------------------------------- motors
void motorsInit() {
  ledcAttach(MOUTH_IN1, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(MOUTH_IN2, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(BODY_IN1, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
  ledcAttach(BODY_IN2, MOTOR_PWM_HZ, MOTOR_PWM_BITS);
}
void drive(int in1, int in2, int speed) {      // speed -255..255, 0 = coast
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
    } else if (srOk) {
      xStreamBufferSend(srBuf, frame, got, 0);
    }
  }
}

// Feeds the wake recognizer from srBuf (called by ESP-SR's own task).
esp_err_t srFill(void *, void *out, size_t len, size_t *bytesRead, uint32_t timeoutMs) {
  size_t got = 0;
  while (got < len) {
    got += xStreamBufferReceive(srBuf, (uint8_t *)out + got, len - got, pdMS_TO_TICKS(1000));
  }
  *bytesRead = got;
  return ESP_OK;
}

void srEvent(void *, sr_event_t event, int commandId, int phraseId) {
  if (event == SR_EVENT_COMMAND) wakeHeard = true;
  else if (event == SR_EVENT_TIMEOUT) srTimedOut = true;
}

// Speaker task: always writes 20 ms blocks (silence when idle) so I2S never
// replays stale data. Resamples 24 kHz -> 16 kHz and drives the mouth.
void audioTask(void *) {
  static int16_t in[480 + 3];                  // 20 ms @ 24 kHz (+ carry)
  static int16_t out[320 * 2];                 // 20 ms @ 16 kHz stereo
  int carry = 0;
  int envHist[MOUTH_LAG_BLOCKS + 1] = {0};
  int envIdx = 0;
  bool mouthOpen = false;
  uint32_t openedAt = 0, closedAt = 0, silentBlocks = 0;

  for (;;) {
    size_t got = xStreamBufferReceive(playBuf, (uint8_t *)(in + carry), (480 - carry) * 2, 0) / 2;
    int total = carry + got, groups = total / 3, outN = 0;
    long envSum = 0;
    for (int g = 0; g < groups; g++) {         // 3 in -> 2 out (linear interpolation)
      int16_t a = in[g * 3], b = in[g * 3 + 1], c = in[g * 3 + 2];
      int16_t o0 = a, o1 = (int16_t)(((int)b + c) / 2);
      out[outN * 2] = out[outN * 2 + 1] = o0; outN++;
      out[outN * 2] = out[outN * 2 + 1] = o1; outN++;
      envSum += abs(o0) + abs(o1);
    }
    carry = total - groups * 3;
    for (int i = 0; i < carry; i++) in[i] = in[groups * 3 + i];
    for (int i = outN; i < 320; i++) out[i * 2] = out[i * 2 + 1] = 0;   // pad with silence

    i2s.write((uint8_t *)out, sizeof(out));    // blocks ~20 ms: paces this task

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


void flushPlayback() { xStreamBufferReset(playBuf); }

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
  {"name":"end_conversation",
   "description":"End the chat. Call this when the user says goodbye, bye, see you later, or asks Billy to stop listening or go to sleep."},
  {"name":"move_fish",
   "description":"Physically move Billy's body. Use when the user asks Billy to dance, wiggle, flap his tail, or turn his head.",
   "parameters":{"type":"OBJECT","properties":{
     "action":{"type":"STRING","enum":["flap_tail","turn_head","dance"]}},
     "required":["action"]}},
  {"name":"set_volume",
   "description":"Change Billy's speaker volume. Use when the user says louder, quieter, turn it up/down, or names a volume level. The result reports the new volume.",
   "parameters":{"type":"OBJECT","properties":{
     "change":{"type":"STRING","enum":["louder","quieter","set"]},
     "level":{"type":"INTEGER","description":"0-100, only used when change is set"}},
     "required":["change"]}},
  {"name":"set_light",
   "description":"Set the color of Billy's status light.",
   "parameters":{"type":"OBJECT","properties":{
     "color":{"type":"STRING","enum":["red","green","blue","purple","white","off"]}},
     "required":["color"]}}
]}]
)JSON";

void requestEnd() {
  if (!endRequested) {
    endRequested = true; endRequestedAt = millis(); turnDone = false;
    Serial.println("[chat] goodbye heard");
  }
}

String runTool(const String &name, const char *callJson) {
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

// toolCall: {"toolCall":{"functionCalls":[{"id":"..","name":"..","args":{..}}, ...]}}
void handleToolCall(char *msg) {
  String responses;
  const char *p = msg;
  size_t l;
  while (true) {
    char *idp = findStr(p, "id", l);
    if (!idp) break;
    String id(idp, l);
    char *np = findStr(idp, "name", l);
    if (!np) break;
    String name(np, l);
    const char *next = strstr(np, "\"id\"");
    String one = next ? String(np).substring(0, next - np) : String(np);
    String result = runTool(name, one.c_str());
    Serial.printf("[tool] %s -> %s\n", name.c_str(), result.c_str());
    if (responses.length()) responses += ",";
    responses += "{\"id\":\"" + id + "\",\"name\":\"" + name +
                 "\",\"response\":{\"result\":\"" + jsonEscape(result.c_str()) + "\"}}";
    if (!next) break;
    p = next;
  }
  ws.sendText("{\"toolResponse\":{\"functionResponses\":[" + responses + "]}}");
}

// ---------------------------------------------------------------- Gemini Live
// Automatic voice-activity detection is ON: Gemini decides when you've
// finished talking, so no button is needed.
void sendSetup() {
  String j = "{\"setup\":{"
             "\"model\":\"models/" GEMINI_MODEL "\","
             "\"generationConfig\":{"
               "\"responseModalities\":[\"AUDIO\"],"
               "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"" GEMINI_VOICE "\"}}}"
             "},"
             "\"systemInstruction\":{\"parts\":[{\"text\":\"" + jsonEscape(BILLY_PERSONA) + "\"}]},"
             "\"inputAudioTranscription\":{},"
             "\"outputAudioTranscription\":{},"
             + String(FPSTR(TOOL_DECLS)) +
             "}}";
  ws.sendText(j);
}

bool saidGoodbye(String t) {
  t.toLowerCase();
  return t.indexOf("goodbye") >= 0 || t.indexOf("good bye") >= 0 || t.indexOf("bye billy") >= 0 ||
         t.startsWith("bye") || t.indexOf(" bye") >= 0;
}

void onWsMessage(char *msg, size_t len) {
  if (findKey(msg, "setupComplete")) { sessionReady = true; Serial.println("[ai] session ready"); return; }
  if (findKey(msg, "toolCall")) { handleToolCall(msg); return; }
  if (findKey(msg, "goAway")) { Serial.println("[ai] session time limit - wrapping up"); requestEnd(); return; }
  if (!findKey(msg, "serverContent")) return;

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
      Serial.print("[you] "); Serial.println(piece);
      if (saidGoodbye(heardThisTurn)) requestEnd();      // backup in case the tool isn't called
    }
  }
  const char *ot = findKey(msg, "outputTranscription");
  if (ot) { size_t tl; char *t = findStr(ot, "text", tl); if (t) { Serial.print("[billy] "); Serial.write(t, tl); Serial.println(); } }
  if (interrupted) flushPlayback();

  if (!interrupted) {
    const char *p = msg;
    size_t dl;
    while (char *d = findStr(p, "data", dl)) {
      size_t olen = 0;
      if (mbedtls_base64_decode((unsigned char *)d, dl, &olen, (const unsigned char *)d, dl) == 0 && olen) {
        micMuted = true;                                // Billy is about to talk
        lastBillyAudio = millis();
        size_t sent = 0;
        while (sent < olen)
          sent += xStreamBufferSend(playBuf, (const uint8_t *)d + sent, olen - sent, pdMS_TO_TICKS(50));
      }
      p = d + dl + 1;
    }
  }
  if (complete) { turnDone = true; heardThisTurn = ""; lastHeard = millis(); }
}

bool connectGemini() {
  sessionReady = false;
  String err;
  String path = String(GEMINI_PATH) + "?key=" + geminiKey;
  Serial.println("[ai] connecting...");
  if (!ws.connect(GEMINI_HOST, path, "", err)) {
    Serial.println("[ai] connect failed: " + err);
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
  xStreamBufferReset(srBuf);
  if (srOk) sr_set_mode(SR_MODE_COMMAND);
  bodyRelax();
  setLed(0, 0, 0);
  Serial.println("[wake] listening for \"Hey Billy\"");
}

void startChat() {
  Serial.println("[chat] start");
  mode = CHATTING;
  if (srOk) sr_set_mode(SR_MODE_OFF);
  xStreamBufferReset(micBuf);
  flushPlayback();
  micMuted = false;
  micToGemini = true;                 // mic audio buffers while we connect
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
  Serial.println("[chat] end");
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
  Serial.printf("[mem] %s: internal free %u (largest %u), PSRAM free %u\n", tag,
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
    Serial.println("!! Wake phrase crashed on this firmware before - disabled. Button still works.");
    return;
  }

  printHeap("before wake recognizer");
  const char *fmt = MIC_CHANNEL == 0 ? "MN" : "NM";
  srOk = sr_start(srFill, NULL, SR_CHANNELS_STEREO, SR_MODE_COMMAND, fmt,
                  wakeCommands, sizeof(wakeCommands) / sizeof(wakeCommands[0]), srEvent, NULL) == ESP_OK;
  printHeap("after wake recognizer");
  if (!srOk) Serial.println("!! Wake phrase unavailable - check Partition Scheme = ESP SR 16M. Button still works.");

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
  if (!psramFound()) Serial.println("!! PSRAM not enabled - set Tools > PSRAM > OPI PSRAM");

  loadSettings();
  Serial.printf("\nSassyBass firmware %s\n", FW_VERSION);
  prefs.begin("billy", true);
  volume = prefs.getInt("vol", SPEAKER_VOLUME);
  prefs.end();
  motorsInit();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  tcaInit();

  i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCLK_PIN);
  if (!i2s.begin(I2S_MODE_STD, I2S_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO))
    Serial.println("!! I2S begin failed");
  es8311Init();
  es7210Init();

  static StaticStreamBuffer_t micSB, srSB, playSB;
  size_t micSize = I2S_RATE * 2 * 20;            // 20 s
  size_t srSize = I2S_RATE * 4 * 2;              // 2 s stereo
  size_t playSize = API_OUT_RATE * 2 * 60;       // 60 s
  micBuf = xStreamBufferCreateStatic(micSize, 1, (uint8_t *)ps_malloc(micSize + 1), &micSB);
  srBuf = xStreamBufferCreateStatic(srSize, 1, (uint8_t *)ps_malloc(srSize + 1), &srSB);
  playBuf = xStreamBufferCreateStatic(playSize, 1, (uint8_t *)ps_malloc(playSize + 1), &playSB);

  xTaskCreatePinnedToCore(audioTask, "audio", 6144, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 6, NULL, 1);
  ws.onMessage(onWsMessage);

  bool forceSetup = digitalRead(TALK_BTN_PIN) == LOW || exioKeyDown(EXIO_KEY1);
  if (wifiSsid.isEmpty() || geminiKey.isEmpty() || forceSetup) {
    if (geminiKey.isEmpty()) Serial.println("No Gemini key saved - enter it on the setup page or send key=AIza... over serial");
    setLed(30, 0, 40); startPortal(); return;
  }

  startWakeRecognizer();   // before Wi-Fi/TLS, while internal RAM is still plentiful

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  Serial.print("Wi-Fi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); Serial.print('.'); }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi failed - starting setup portal");
    setLed(30, 0, 40); startPortal(); return;
  }
  Serial.println("Wi-Fi OK: " + WiFi.localIP().toString());
  checkForUpdate();                                    // reboots if a newer release was installed

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
    Serial.println("Gemini key saved. Type restart to apply.");
  } else if (line.startsWith("wifi=")) {
    int comma = line.indexOf(',');
    wifiSsid = line.substring(5, comma < 0 ? line.length() : comma);
    wifiPass = comma < 0 ? "" : line.substring(comma + 1);
    saveSettings();
    Serial.println("Wi-Fi saved. Type restart to apply.");
  } else if (line == "wake=reset") {
    prefs.begin("billy", false); prefs.remove("srbad"); prefs.end();
    Serial.println("Wake phrase re-enabled. Type restart to apply.");
  } else if (line == "restart") {
    ESP.restart();
  } else if (line.length()) {
    Serial.println("Commands: key=..., wifi=SSID,password, wake=reset, restart");
  }
}

void loop() {
  serialCommands();
  if (portalMode) { portalLoop(); return; }

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
    static uint32_t lastUpdateCheck = millis();
    if (now - lastUpdateCheck > UPDATE_EVERY_MS) { lastUpdateCheck = now; checkForUpdate(); }
    if (srTimedOut) { srTimedOut = false; if (srOk) sr_set_mode(SR_MODE_COMMAND); }   // keep listening
    if (wakeHeard || click) { wakeHeard = false; startChat(); }
    delay(5);
    return;
  }

  // ---- CHATTING
  ws.poll();
  if (!ws.connected()) { Serial.println("[ai] connection closed"); endChat(); return; }

  bool billyTalking = speechPlaying || !xStreamBufferIsEmpty(playBuf);
  if (billyTalking) {
    lastBillyAudio = now;
    headOut();                                         // head out while he talks
    setLed(0, 40, 0);
  } else if (now - lastBillyAudio > MIC_HOLDOFF_MS) {
    if (micMuted) { micMuted = false; xStreamBufferReset(micBuf); bodyRelax(); setLed(0, 0, 60); }
  }

  if (sessionReady && !micMuted) pumpMicAudio();
  else if (micMuted) xStreamBufferReset(micBuf);

  // Ending: wait for Billy's goodbye to finish playing
  if (endRequested) {
    bool farewellDone = turnDone && !billyTalking;
    if (farewellDone || now - endRequestedAt > END_GRACE_MS) { endChat(); return; }
  }
  if (click) { requestEnd(); endRequestedAt = now - END_GRACE_MS; }   // button = end right away
  if (!billyTalking && now - lastHeard > CONVO_IDLE_MS && now - lastBillyAudio > CONVO_IDLE_MS) {
    Serial.println("[chat] nobody's talking - going back to sleep");
    endChat();
    return;
  }
  delay(2);
}
