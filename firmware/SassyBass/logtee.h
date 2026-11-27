// =====================================================================
//  Log: prints to Serial and keeps the last few KB in RAM so the web page
//  can show it (Logs tab). Use Log.print/printf/println like Serial.
// =====================================================================
#pragma once
#include <Arduino.h>

class LogTee : public Print {
public:
  static const size_t CAP = 8192;
  size_t write(uint8_t c) override {
    Serial.write(c);
    _buf[_head] = (char)c;
    _head = (_head + 1) % CAP;
    if (_len < CAP) _len++;
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    Serial.write(b, n);
    for (size_t i = 0; i < n; i++) {
      _buf[_head] = (char)b[i];
      _head = (_head + 1) % CAP;
      if (_len < CAP) _len++;
    }
    return n;
  }
  String text() const {
    String s; s.reserve(_len + 1);
    size_t start = (_head + CAP - _len) % CAP;
    for (size_t i = 0; i < _len; i++) s += _buf[(start + i) % CAP];
    return s;
  }
private:
  char _buf[CAP];
  size_t _head = 0, _len = 0;
};

LogTee Log;
