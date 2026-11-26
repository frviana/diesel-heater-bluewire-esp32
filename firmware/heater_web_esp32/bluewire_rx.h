#pragma once

#include <stdint.h>

namespace BlueWire {
constexpr uint32_t FRAME_GAP_US = 12000;
constexpr uint16_t START_LOW_US = 30000;
constexpr uint16_t BIT_PERIOD_US = 12050;
constexpr uint16_t SHORT_LOW_US = 4000;
constexpr uint16_t LONG_LOW_US = 8000;

struct Frame {
    uint32_t endUs;
    uint16_t word;
    uint8_t bits;
};

// Called under the same lock by the GPIO ISR and bus task. All timestamps
// are unsigned microseconds so subtraction also works across micros() wrap.
struct PulseCapture {
    uint32_t lastEdge = 0;
    uint32_t highGap = 0;
    uint32_t endUs = 0;
    uint32_t rejected = 0;
    uint16_t word = 0;
    uint8_t bits = 0;
    bool active = false;
    bool high = true;
    Frame pending{};

    inline __attribute__((always_inline)) void finish() {
        if (!active) return;
        if (bits == 8 || bits == 16) {
            if (pending.bits) ++rejected;
            pending = {endUs, word, bits};
        } else {
            ++rejected;
        }
        active = false;
    }

    inline __attribute__((always_inline)) void edge(uint32_t now, bool level) {
        if (level == high) return;
        const uint32_t width = now - lastEdge;
        lastEdge = now;
        high = level;

        if (!high) {
            highGap = width;
            if (width >= FRAME_GAP_US) finish();
            return;
        }
        if (width >= 24000 && width <= 36000) {
            // The heater can start its reply before the request's idle gap
            // expires. Its start pulse also closes the preceding frame.
            finish();
            word = 0;
            bits = 0;
            active = true;
            return;
        }
        if (!active) return;

        const bool zero = width >= 2500 && width <= 5500;
        const bool one = width >= 6500 && width <= 9500;
        const uint32_t slot = highGap + width;
        if (bits == 16 || (!zero && !one) || slot < 10000 || slot > 14000) {
            ++rejected;
            active = false;
            return;
        }
        word = (word << 1) | one;
        ++bits;
        endUs = now;
    }
};
}
