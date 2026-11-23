// The desktop platform's HardwareSerial prints to stderr; the Blackbox has no serial port, so Serial is a sink with the same interface.
#pragma once

#include "Stream.h"
#include <stdint.h>

class HardwareSerial : public Stream {
public:
    void begin(unsigned long /*baud*/) {}
    void end() {}

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t *, size_t n) override { return n; }
    operator bool() const { return true; }
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;
