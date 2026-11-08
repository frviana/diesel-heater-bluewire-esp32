// ============================================================================
//  SAFETY - master shut-off and the Manual keep-running timer.
//  Pure logic (no Arduino calls) so it can be tested on a PC.
//
//  MASTER SHUT-OFF: room at or over 75 F for a minute -> heater OFF, whatever
//  mode it's in. Software starts (page, thermostat, frost guard)
//  stay blocked until the room is back under 70 F. If it gets started at the
//  heater's own display while the room is still over 75 F, it gets stopped
//  again. There is no switch to turn this off.
//
//  KEEP-RUNNING TIMER (Manual only): after mtMin minutes of running it asks
//  "keep it running?" on the page. No answer in 15 min -> heater OFF.
//  Thermostat and frost guard are meant to run long, so they don't get asked.
// ============================================================================
#pragma once
#include <math.h>
#include <stdint.h>

#define HI_LIMIT_C   23.9f     // 75 F  - heater off in every mode
#define HI_CLEAR_C   21.1f     // 70 F  - starts allowed again below this
#define HI_HOLD_MS   60000UL   // over the limit this long before it acts (one bad read won't trip it)
#define HI_RETRY_MS  30000UL   // still running after our OFF? send it again
#define SP_MAX_C     21.12f    // 70 F  - highest setpoint / stop temperature accepted
#define MT_GRACE_MS  900000UL  // 15 min to answer "keep it running?"

struct SafeCfg {
    bool     mtOn  = true;      // keep-running timer on
    uint16_t mtMin = 180;       // ask after this many minutes of a Manual run
};

struct SafeIn {
    uint32_t now = 0;           // ms
    char     st = 'U';          // 'O' off, 'R' run (incl. start-up), 'C' cooling, 'F' fault, 'U' unknown
    float    roomC = NAN;       // NAN = no reading
    bool     manual = true;     // mode is Manual
};

enum SafeEvt : uint8_t { SE_NONE, SE_HI_TRIP, SE_HI_AGAIN, SE_HI_CLEAR, SE_MT_ASK, SE_MT_STOP };

struct SafeOut {
    bool    sendOff = false;
    uint8_t evt = SE_NONE;
};

struct SafeState {
    bool     hiLatched = false;  // over the limit (or not yet back under 70 F): no software starts
    uint32_t hiOverMs = 0;       // first reading at/over the limit in this stretch (0 = none)
    uint32_t hiOffMs = 0;        // last OFF we sent for the limit (0 = none)
    bool     hiStoppedRun = false; // we've already stopped the current run
    uint8_t  hiTrips = 0;        // runs stopped since it went over (first = "tripped", later = "started again")
    bool     mtArmed = false;    // a Manual run is being timed
    uint32_t mtFromMs = 0;       // timing from here (run start in Manual, or the last "keep")
    bool     mtAsking = false;
    uint32_t mtAskMs = 0;
};

static inline bool sAfter(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }

static void safeStep(const SafeCfg &cfg, const SafeIn &in, SafeState &s, SafeOut &out) {
    out = SafeOut();
    const uint32_t now = in.now ? in.now : 1;
    const float T = in.roomC;
    const bool running = in.st == 'R';

    // ---- master shut-off ----
    if (!isnan(T) && T >= HI_LIMIT_C) {
        if (!s.hiOverMs) s.hiOverMs = now;
        if (sAfter(now, s.hiOverMs, HI_HOLD_MS)) {
            s.hiLatched = true;
            if (running && (!s.hiStoppedRun || sAfter(now, s.hiOffMs, HI_RETRY_MS))) {
                out.sendOff = true;
                if (!s.hiStoppedRun) { out.evt = s.hiTrips ? SE_HI_AGAIN : SE_HI_TRIP; s.hiTrips++; }   // retries for the same run stay quiet
                s.hiOffMs = now; s.hiStoppedRun = true;
            }
        }
    } else if (!isnan(T)) {
        s.hiOverMs = 0;
        if (s.hiLatched && T < HI_CLEAR_C) {
            s.hiLatched = false; s.hiOffMs = 0; s.hiTrips = 0;
            if (out.evt == SE_NONE) out.evt = SE_HI_CLEAR;
        }
    }
    if (in.st == 'U') return;                                 // lost touch with the heater: hold everything as it is
    if (!running) s.hiStoppedRun = false;                     // next run that needs stopping gets its own message

    // ---- keep-running timer (Manual) ----
    const bool timed = cfg.mtOn && in.manual && running;
    if (!timed) { s.mtArmed = false; s.mtAsking = false; }
    else {
        if (!s.mtArmed) { s.mtArmed = true; s.mtFromMs = now; s.mtAsking = false; }
        if (!s.mtAsking && sAfter(now, s.mtFromMs, (uint32_t)cfg.mtMin * 60000UL)) {
            s.mtAsking = true; s.mtAskMs = now;
            if (out.evt == SE_NONE) out.evt = SE_MT_ASK;
        } else if (s.mtAsking && sAfter(now, s.mtAskMs, MT_GRACE_MS)) {
            s.mtAsking = false; s.mtArmed = false;
            out.sendOff = true;
            if (out.evt == SE_NONE) out.evt = SE_MT_STOP;
        }
    }
}

// "Keep it running" from the page. False if there's no timed run to extend.
static inline bool safeKeep(SafeState &s, uint32_t now) {
    if (!s.mtArmed) return false;
    s.mtFromMs = now ? now : 1; s.mtAsking = false;
    return true;
}

// Seconds until it asks (or, while asking, until it stops). -1 = not timing.
static inline long safeLeftS(const SafeCfg &cfg, const SafeState &s, uint32_t now) {
    if (!s.mtArmed) return -1;
    if (s.mtAsking) { uint32_t d = now - s.mtAskMs; return d >= MT_GRACE_MS ? 0 : (long)((MT_GRACE_MS - d) / 1000); }
    uint32_t lim = (uint32_t)cfg.mtMin * 60000UL, d = now - s.mtFromMs;
    return d >= lim ? 0 : (long)((lim - d) / 1000);
}
