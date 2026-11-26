#pragma once

#include <Arduino.h>

namespace BlueWire {
constexpr int DATA_PIN = 27;

bool begin(int pollSeconds);
bool command(const String &text);
bool readStatus(char (&line)[120]);
bool readEvent(char (&line)[120]);
uint32_t badFrames();
}
