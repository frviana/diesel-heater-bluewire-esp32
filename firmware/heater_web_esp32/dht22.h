// ============================================================================
//  ROOM SENSOR - DHT22 / AM2302 via Adafruit's "DHT sensor library"
//  (Library Manager: "DHT sensor library" by Adafruit - say yes to
//   "Adafruit Unified Sensor" when it asks).
//  Wiring: + to 3.3V (not 5V - the data line would then sit at 5V), - to GND,
//  data/out to DHT_PIN. 3-pin modules have their pull-up built in; a bare
//  4-pin sensor needs 10k from data to 3.3V. Keep the cable under ~2 m at 3.3V.
//  Reads every 3 s, median of the last 3 good reads, junk readings dropped.
// ============================================================================
#pragma once
#include <DHT.h>

class RoomSensor {
public:
    float tC = NAN, rh = NAN;          // filtered values (NAN until the first good read)
    uint32_t okMs = 0;                 // millis() of the last good read
    uint32_t reads = 0, fails = 0;

    void begin(uint8_t pin) { _dht = new DHT(pin, DHT22); _dht->begin(); _nextMs = millis() + 2500; }

    void tick() {
        if (!_dht) return;
        uint32_t now = millis();
        if ((int32_t)(now - _nextMs) < 0) return;
        _nextMs = now + 3000;                                   // the sensor needs 2 s between reads
        float t = _dht->readTemperature(false, true);           // force a fresh read
        float h = _dht->readHumidity();                         // same read, cached
        if (isnan(t) || isnan(h) || t < -40 || t > 80 || h < 0 || h > 100) { fails++; return; }
        if (_n && !isnan(tC) && fabsf(t - tC) > 8) _n = 0;       // big jump: start the median over
        reads++; okMs = now;
        _t[_n % 3] = t; _h[_n % 3] = h; _n++;
        if (_n >= 3) { tC = med3(_t[0], _t[1], _t[2]); rh = med3(_h[0], _h[1], _h[2]); }
        else { tC = t; rh = h; }
    }
    bool fresh(uint32_t maxAgeMs = 60000) const { return okMs && millis() - okMs < maxAgeMs; }

private:
    DHT *_dht = nullptr;
    uint32_t _nextMs = 0, _n = 0;
    float _t[3], _h[3];
    static float med3(float a, float b, float c) { return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c)); }
};
