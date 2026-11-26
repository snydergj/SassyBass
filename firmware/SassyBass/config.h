// =====================================================================
//  SassyBass - hardware + personality settings
//  Board: Waveshare ESP32-S3-AUDIO-Board (ES8311 speaker codec,
//         ES7210 mic ADC, TCA9555 IO expander) + DRV8833 motor driver
//  AI:    Google Gemini Live API (free tier)
// =====================================================================
#pragma once

// ---------- Firmware version + updates ----------
// fw_version.h is overwritten by the GitHub Actions build with the release tag.
// Local builds report "dev" and never auto-update (so your test builds stick).
#include "fw_version.h"
#define UPDATE_URL      "https://snydergj.github.io/SassyBass/version.json"
#define UPDATE_EVERY_MS (24UL * 60 * 60 * 1000)   // also re-check once a day while idle

// ---------- Gemini ----------
// The API key is NOT in the firmware. Enter it once per fish on the setup page
// (it appears automatically when no key is saved, or at http://192.168.4.1/?owner=1),
// or over USB serial:  key=AIza...
// Live models change often: check https://ai.google.dev/gemini-api/docs/live-api/capabilities
// and confirm the model has free-tier quota at https://aistudio.google.com/rate-limit
#define GEMINI_MODEL   "gemini-3.8-live"
#define GEMINI_HOST    "generativelanguage.googleapis.com"
#define GEMINI_PATH    "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"

// Voice: listen to them in AI Studio (Stream / Live page).
// A few to try:  Algenib (gravelly), Charon (informative), Puck (upbeat),
//                Fenrir (excitable), Orus (firm), Kore (firm), Zephyr (bright), Aoede (breezy)
#define GEMINI_VOICE   "Algenib"

// Personality AND accent live in the same instructions text.
#define BILLY_PERSONA \
  "You are Billy, a singing largemouth bass mounted on a wooden plaque. " \
  "You are a wisecracking, good-natured old fish who loves fishing stories, " \
  "bad puns, and gently roasting whoever is talking to you. " \
  "Keep every answer to one to three short sentences, because your mouth motor gets tired. " \
  "Never say you are an AI; you are a fish. Always speak English. " \
  "When the user says goodbye or wants to stop chatting, say a quick goodbye and call end_conversation. " \
  "Voice and delivery: speak with a thick, folksy Texas drawl, warm and a little gravelly, " \
  "like an old fishing guide, at a lively pace."

// ---------- Audio (fixed by the board) ----------
#define I2S_MCLK_PIN   12
#define I2S_BCLK_PIN   13
#define I2S_WS_PIN     14
#define I2S_DIN_PIN    15   // ES7210 ASDOUT -> ESP (mic)
#define I2S_DOUT_PIN   16   // ESP -> ES8311 DSDIN (speaker)
#define I2C_SDA_PIN    11
#define I2C_SCL_PIN    10

#define ES7210_ADDR    0x40
#define TCA9555_ADDR   0x20
#define EXIO_KEY1      9     // user keys K1..K3 (active low); EXIO8 = amp enable
#define EXIO_KEY2      10    // K2 = start/end chat
#define EXIO_KEY3      11    // K1 = volume down, K3 = volume up (K1 at power-up = Wi-Fi setup)

#define STATUS_LED_PIN 38    // WS2812 ring (we only drive the first LED)
#define BOOT_BTN_PIN   0

// Gemini takes 16 kHz mic audio (same as our I2S rate) and returns 24 kHz,
// which we resample to 16 kHz for the speaker.
#define I2S_RATE       16000
#define API_OUT_RATE   24000

// Which ES7210 channel carries the microphone in a stereo I2S frame.
// 0 = left (MIC1), 1 = right (MIC2). Flip if Billy can't hear you.
#define MIC_CHANNEL    0

// ---------- Motors / button (LCD header pins, free when no LCD) ----------
// Check the silkscreen on your board's LCD header before wiring.
#define MOUTH_IN1      4    // DRV8833 AIN1
#define MOUTH_IN2      5    // DRV8833 AIN2
#define BODY_IN1       7    // DRV8833 BIN1  (forward = head turns out)
#define BODY_IN2       8    // DRV8833 BIN2  (reverse = tail flaps)
#define TALK_BTN_PIN   9    // Billy's red button, other side to GND

#define MOTOR_PWM_HZ   20000
#define MOTOR_PWM_BITS 8
#define MOUTH_DUTY     230   // 0-255
#define BODY_DUTY      200   // 0-255

// ---------- Wake word ----------
// Uses Espressif's built-in always-on WakeNet model "Hi ESP" (say "Hi E-S-P").
// Needs Tools > Partition Scheme > "ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)".
// A custom "Hey Billy" needs a custom-trained WakeNet model.

// ---------- Behaviour ----------
#define SPEAKER_VOLUME        80     // 0-100 starting volume (voice/button changes are remembered)
#define MOUTH_THRESH         900     // average |sample| that opens the mouth (lower = more flapping)
#define MOUTH_LAG_BLOCKS        3    // delay mouth vs audio (20 ms blocks) to match speaker latency
#define MOUTH_MAX_OPEN_MS     350    // force a "flap" if held open longer
#define MIC_HOLDOFF_MS        400    // keep the mic muted this long after Billy stops talking (no self-echo)
#define CONVO_IDLE_MS       45000    // end the chat if nobody says anything for this long
#define END_GRACE_MS         8000    // after "goodbye", wait at most this long for Billy's farewell
#define SETUP_AP_SSID   "BillyBass-Setup"
#define SETUP_AP_PASS   "billybass"  // 8+ chars required by WPA2
