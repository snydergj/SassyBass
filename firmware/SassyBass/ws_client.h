// =====================================================================
//  Minimal secure WebSocket client (RFC 6455) for the Gemini Live API.
//  Written in-house instead of a library so large audio frames (tens of KB)
//  land in PSRAM without size limits.
// =====================================================================
#pragma once
#include <NetworkClientSecure.h>
#include "mbedtls/base64.h"
#include "esp_random.h"
#include "logtee.h"

class WsClient {
public:
  typedef void (*MessageHandler)(char *msg, size_t len);

  void onMessage(MessageHandler h) { _handler = h; }
  bool connected() { return _open && _tls.connected(); }

  // Returns true when the 101 Switching Protocols handshake succeeds.
  bool connect(const char *host, const String &path, const String &extraHeaders, String &errOut) {
    close();
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12)
    _tls.useBuiltinCACertBundle();      // verifies the server's certificate
#else
#warning "Arduino-ESP32 core < 3.3.12: TLS certificate NOT verified. Update the core."
    _tls.setInsecure();
#endif
    _tls.setTimeout(10);
    if (!_tls.connect(host, 443)) { errOut = "TLS connect failed"; return false; }

    uint8_t key[16]; char keyB64[32]; size_t kl = 0;
    esp_fill_random(key, sizeof(key));
    mbedtls_base64_encode((unsigned char *)keyB64, sizeof(keyB64), &kl, key, sizeof(key));
    keyB64[kl] = 0;

    String req = "GET " + path + " HTTP/1.1\r\n"
                 "Host: " + String(host) + "\r\n"
                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "Sec-WebSocket-Key: " + String(keyB64) + "\r\n" +
                 extraHeaders + "\r\n";
    _tls.print(req);

    String status = readLine(8000);
    if (!status.startsWith("HTTP/1.1 101")) {
      // Grab whatever the server said (e.g. 401 invalid API key) for the log.
      String body; uint32_t t0 = millis();
      while (millis() - t0 < 1500 && body.length() < 600) {
        while (_tls.available() && body.length() < 600) body += (char)_tls.read();
        delay(5);
      }
      errOut = status + " " + body;
      _tls.stop();
      return false;
    }
    while (true) {                       // skip remaining headers
      String line = readLine(5000);
      if (line.length() == 0) break;
    }
    _open = true;
    return true;
  }

  void close() {
    if (_open && _tls.connected()) {
      uint8_t frame[6] = {0x88, 0x80, 0, 0, 0, 0};   // masked close, empty payload
      _tls.write(frame, sizeof(frame));
    }
    _open = false;
    _tls.stop();
    _msgLen = 0;
  }

  bool sendText(const char *data, size_t len) { return sendFrame(0x1, (const uint8_t *)data, len); }
  bool sendText(const String &s) { return sendText(s.c_str(), s.length()); }

  // Call often. Reads complete frames if any data is waiting.
  void poll() {
    if (!_open) return;
    if (!_tls.connected()) { _open = false; return; }
    while (_open && _tls.available() >= 2) {
      if (!readFrame()) { close(); return; }
    }
  }

private:
  NetworkClientSecure _tls;
  bool _open = false;
  MessageHandler _handler = nullptr;
  char *_msg = nullptr;       // reassembly buffer (PSRAM)
  size_t _msgCap = 0, _msgLen = 0;
  uint8_t _maskBuf[1024];

  String readLine(uint32_t timeoutMs) {
    String s; uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
      if (_tls.available()) {
        char c = _tls.read();
        if (c == '\n') { s.trim(); return s; }
        s += c;
      } else delay(1);
    }
    s.trim();
    return s;
  }

  bool readExact(uint8_t *dst, size_t n, uint32_t timeoutMs = 8000) {
    size_t got = 0; uint32_t t0 = millis();
    while (got < n) {
      int a = _tls.available();
      if (a > 0) {
        int r = _tls.read(dst + got, min((size_t)a, n - got));
        if (r > 0) { got += r; t0 = millis(); }
      } else {
        if (!_tls.connected() || millis() - t0 > timeoutMs) return false;
        delay(1);
      }
    }
    return true;
  }

  bool ensureCap(size_t need) {
    if (need + 1 <= _msgCap) return true;
    size_t cap = max(need + 1, _msgCap * 2 + 4096);
    char *p = (char *)ps_realloc(_msg, cap);
    if (!p) return false;
    _msg = p; _msgCap = cap;
    return true;
  }

  bool readFrame() {
    uint8_t h[2];
    if (!readExact(h, 2)) return false;
    bool fin = h[0] & 0x80;
    uint8_t op = h[0] & 0x0F;
    bool masked = h[1] & 0x80;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) { uint8_t e[2]; if (!readExact(e, 2)) return false; len = (e[0] << 8) | e[1]; }
    else if (len == 127) {
      uint8_t e[8]; if (!readExact(e, 8)) return false;
      len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    uint8_t mask[4] = {0};
    if (masked && !readExact(mask, 4)) return false;
    if (len > 4 * 1024 * 1024) return false;   // sanity limit

    if (op == 0x8) {                            // server closed - log the reason
      uint8_t p[126] = {0};
      if (len && len <= 125 && readExact(p, len)) {
        int code = len >= 2 ? (p[0] << 8) | p[1] : 0;
        Log.printf("[ws] server closed (%d): %.*s\n", code, (int)(len > 2 ? len - 2 : 0), (char *)p + 2);
      }
      return false;
    }
    if (op == 0x9 || op == 0xA) {               // ping / pong (control frames <= 125 bytes)
      uint8_t p[125];
      if (len && !readExact(p, len)) return false;
      if (op == 0x9) sendFrame(0xA, p, len);
      return true;
    }
    if (op == 0x1 || op == 0x2) _msgLen = 0;    // new message
    if (!ensureCap(_msgLen + len)) return false;
    if (len && !readExact((uint8_t *)_msg + _msgLen, len)) return false;
    if (masked) for (size_t i = 0; i < len; i++) _msg[_msgLen + i] ^= mask[i & 3];
    _msgLen += len;
    if (fin) {
      _msg[_msgLen] = 0;
      if (_handler) _handler(_msg, _msgLen);
      _msgLen = 0;
    }
    return true;
  }

  bool sendFrame(uint8_t op, const uint8_t *data, size_t len) {
    if (!_open) return false;
    uint8_t hdr[14]; size_t hl = 0;
    hdr[hl++] = 0x80 | op;
    if (len < 126) hdr[hl++] = 0x80 | len;
    else if (len < 65536) { hdr[hl++] = 0x80 | 126; hdr[hl++] = len >> 8; hdr[hl++] = len & 0xFF; }
    else { hdr[hl++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[hl++] = (uint64_t)len >> (8 * i); }
    uint8_t mask[4]; esp_fill_random(mask, 4);
    memcpy(hdr + hl, mask, 4); hl += 4;
    if (_tls.write(hdr, hl) != hl) return false;
    size_t off = 0;
    while (off < len) {                          // client frames must be masked
      size_t n = min(len - off, sizeof(_maskBuf));
      for (size_t i = 0; i < n; i++) _maskBuf[i] = data[off + i] ^ mask[(off + i) & 3];
      if (_tls.write(_maskBuf, n) != n) return false;
      off += n;
    }
    return true;
  }
};
