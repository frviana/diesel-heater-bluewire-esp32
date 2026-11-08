// ============================================================================
//  HISTORY LOG - hour-by-hour for the last 7 days and day-by-day for a year.
//  Pure logic (no Arduino calls) so it's tested on a PC. The sketch feeds it
//  once a second and saves records to flash through the save callback.
//
//  Per hour:  seconds running, seconds lit, average level, starts, room average.
//  Per day:   seconds running, seconds at each level 1-6, starts, light-up
//             times, room low / high / average, lowest idle battery volts,
//             master shut-offs, keep-running stops, faults, flame-outs.
//  Hours count in UTC (unix hour); days are local calendar days (the page
//  turns both into your local time). Nothing is recorded until the clock is
//  set from the internet - without a date there's nowhere to file it.
// ============================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#define HL_HOURS 168               // 7 days
#define HL_DAYS  366               // a year
#define HL_NONE  INT16_MIN
#define HL_SAVE_MS 600000UL        // checkpoint the current hour + day every 10 min

struct HourRec {
    uint32_t hr;                   // unix time / 3600 (0 = empty slot)
    uint32_t lvlSum;               // level x seconds, while lit
    int32_t  rSum;                 // room, tenths C x readings
    uint16_t runS, litS;           // seconds in RUN (lighting up included), seconds lit
    uint16_t rN;                   // room readings
    uint8_t  starts, pad;
};

struct DayRec {
    uint32_t day;                  // days since 1970-01-01 in local time (0 = empty slot)
    uint32_t runS;                 // seconds in RUN
    uint32_t lvlS[6];              // seconds lit at level 1..6
    int32_t  rSum;                 // room, tenths C x readings
    uint32_t rN;
    uint16_t starts, lightSum;     // lightSum = total seconds from START to lit, over lightN starts
    int16_t  rMin, rMax;           // room low / high, tenths C (HL_NONE = no reading)
    uint8_t  lightN, vMin;         // vMin = lowest idle volts seen (0 = none)
    uint8_t  hiTrips, mtStops, faults, flames;
};

struct HistIn {
    uint32_t utc = 0;              // UTC seconds (0 = clock not set)
    uint32_t day = 0;              // local day number (hlDay)
    bool     running = false;      // heater state RUN
    bool     lit = false;          // phase RUN (flame on)
    uint8_t  level = 0;
    float    roomC = NAN;
    int      idleVolts = 0;        // only when the heater is OFF and reporting (0 = none)
};

// days since 1970-01-01 for a calendar date (Howard Hinnant's days_from_civil)
static inline uint32_t hlDay(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468);
}

class HistLog {
public:
    HourRec hours[HL_HOURS];
    DayRec  days[HL_DAYS];
    // save(isDay, slot, record, size) - the sketch writes it to flash
    void (*save)(bool, uint16_t, const void *, uint16_t) = nullptr;

    void clear() { memset(hours, 0, sizeof hours); memset(days, 0, sizeof days); _h = _d = -1; }

    // once a second
    void tick(const HistIn &in, uint32_t ms) {
        if (in.utc < 1700000000UL || !in.day) return;               // no clock yet
        roll(in.utc / 3600, in.day);
        HourRec &h = hours[_h]; DayRec &d = days[_d];
        if (in.running) { if (h.runS < 3600) h.runS++; d.runS++; }
        if (in.running && in.lit && in.level >= 1 && in.level <= 6) {
            if (h.litS < 3600) { h.litS++; h.lvlSum += in.level; }
            d.lvlS[in.level - 1]++;
        }
        if (!isnan(in.roomC) && in.roomC > -60 && in.roomC < 90) {
            int16_t t = (int16_t)lroundf(in.roomC * 10);
            if (h.rN < 65000) { h.rSum += t; h.rN++; }
            d.rSum += t; d.rN++;
            if (d.rMin == HL_NONE || t < d.rMin) d.rMin = t;
            if (d.rMax == HL_NONE || t > d.rMax) d.rMax = t;
        }
        if (in.idleVolts > 0 && in.idleVolts < 40 && (!d.vMin || in.idleVolts < d.vMin)) d.vMin = (uint8_t)in.idleVolts;
        if (!_lastSave) _lastSave = ms ? ms : 1;
        if (ms - _lastSave >= HL_SAVE_MS) { checkpoint(); _lastSave = ms ? ms : 1; }
    }

    // events (ignored until the clock is set and the first tick has run)
    void addStart()          { if (ok()) { if (hours[_h].starts < 255) hours[_h].starts++; days[_d].starts++; } }
    void addLight(uint16_t s){ if (ok() && s < 3600) { days[_d].lightSum += s; if (days[_d].lightN < 255) days[_d].lightN++; } }
    void addHiTrip()         { if (ok() && days[_d].hiTrips < 255) days[_d].hiTrips++; }
    void addMtStop()         { if (ok() && days[_d].mtStops < 255) days[_d].mtStops++; }
    void addFault()          { if (ok() && days[_d].faults < 255) days[_d].faults++; }
    void addFlame()          { if (ok() && days[_d].flames < 255) days[_d].flames++; }

    void checkpoint() {
        if (!ok() || !save) return;
        save(false, (uint16_t)_h, &hours[_h], sizeof(HourRec));
        save(true, (uint16_t)_d, &days[_d], sizeof(DayRec));
    }
    bool ok() const { return _h >= 0 && _d >= 0; }
    uint32_t hourNow() const { return _h >= 0 ? hours[_h].hr : 0; }
    uint32_t dayNow() const { return _d >= 0 ? days[_d].day : 0; }

private:
    int _h = -1, _d = -1;
    uint32_t _lastSave = 0;

    void roll(uint32_t hr, uint32_t day) {
        int hs = (int)(hr % HL_HOURS), ds = (int)(day % HL_DAYS);
        if (_h != hs || hours[hs].hr != hr) {
            if (_h >= 0 && save) save(false, (uint16_t)_h, &hours[_h], sizeof(HourRec));   // finish the old hour
            if (hours[hs].hr != hr) { memset(&hours[hs], 0, sizeof(HourRec)); hours[hs].hr = hr; }   // (same hour after a reboot: keep adding)
            _h = hs;
        }
        if (_d != ds || days[ds].day != day) {
            if (_d >= 0 && save) save(true, (uint16_t)_d, &days[_d], sizeof(DayRec));
            if (days[ds].day != day) {
                memset(&days[ds], 0, sizeof(DayRec)); days[ds].day = day;
                days[ds].rMin = days[ds].rMax = HL_NONE;
            }
            _d = ds;
        }
    }
};
