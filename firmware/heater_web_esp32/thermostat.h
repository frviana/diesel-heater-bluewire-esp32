// ============================================================================
//  AUTO MODES - thermostat and frost guard. Pure logic, no Arduino calls, so it
//  can be simulated on a PC against a model of the shed.
//
//  MANUAL ...... you drive it (page buttons or the OEM display). Nothing automatic.
//  THERMOSTAT .. full power (level 6) while the room is more than `ease` below
//                the setpoint. Starts when the room falls `onBand` below.
//                Optional: shut off if still `offBand` too warm at idle.
//                SMOOTH HOLD (thSmooth, v1.9.5, on by default): eases down on the
//                way up so it arrives near the level that holds the setpoint,
//                then trims one level at a time to hold it - see thermoSmooth().
//                Works out the level that holds it ("needs ~4.3") and starts
//                from that next time. Off = the old way: on reaching the
//                setpoint it drops to idle (or the learned level - 1), then
//                steps up one level at a time (every 7 min at most, 2 if it is
//                dropping fast) while the room is below and still falling, and
//                back down while it's 1 F+ above.
//  FROST GUARD . stays off until the room drops to `frostOn`, then runs at
//                `frostLvl` until it reaches `frostOff`, then shuts off. If the
//                room keeps dropping below `frostOn` while it runs, it boosts
//                to level 5 until it climbs back. COLD-NIGHT IDLE: if it has to
//                restart within `coldRestMin` of its last stop, it's a cold
//                night - from then on it idles at level 1 at `frostOff` instead
//                of shutting off, and only stops if it gets 4 F past `frostOff`
//                at idle (sun's up).
//
//  Every behaviour has an on/off toggle in AutoCfg (thStart, thFull, shutoff,
//  frBoost, frIdle, dispManual).
//
//  Guard rails (diesel heaters hate short cycles):
//   - never stops a run younger than minRunMin, never restarts sooner than
//     minOffMin after it went off
//   - max 3 starts an hour, then auto pauses with an alert
//   - a FAULT drops back to MANUAL (no endless restarts on an empty tank)
//   - no room reading: no starts; a running heater drops to level 2 and is
//     stopped after 2 h without a reading
//   - levels step at most every 7 min near the setpoint (2 min if well off it);
//     smooth hold: every 5 min (3 min if 1.5 F+ off and getting worse)
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>

enum AutoMode : uint8_t { AM_MANUAL = 0, AM_THERMO = 1, AM_FROST = 2 };

struct AutoCfg {
    uint8_t  mode = AM_MANUAL;
    float    sp = 15.5f;          // thermostat setpoint, C (60 F)
    float    ease = 2.8f;         // full power until this far below setpoint, C (5 F)
    float    onBand = 1.1f;       // thermostat (re)starts this far below setpoint, C (2 F)
    float    offBand = 1.7f;      // optional shutoff this far above setpoint, C (3 F)
    bool     shutoff = false;     // thermostat may shut the heater off when warm
    float    frostOn = 1.7f;      // frost guard starts at or below, C (35 F)
    float    frostOff = 10.0f;    // frost guard stops at or above, C (50 F)
    uint8_t  frostLvl = 2;        // low and long beats short hot bursts for a diesel
    uint16_t minRunMin = 30;
    uint16_t minOffMin = 15;
    // toggles
    bool     thStart = true;      // thermostat starts the heater by itself when it's cold
    bool     thFull = true;       // thermostat: full power until close, idle on arrival (off = gentle steps only)
    bool     thSmooth = true;     // thermostat: smooth hold - ease in, then small trims (off = the v1.9.4 way)
    bool     frBoost = true;      // frost guard boosts to level 5 if it keeps losing ground
    bool     frIdle = true;       // frost guard idles instead of shutting off on cold nights
    uint16_t coldRestMin = 90;    // "cold night" = had to restart within this long after a stop
    bool     dispManual = true;   // a stop at the heater's own display switches to Manual (used by the caller)
    // chamber guard (runs in every mode - see chamber_guard.h)
    bool     chGuard = true;      // bump the level if the chamber runs too cold
    float    chFloor = 120.0f;    // chamber floor, C (248 F)
};

struct AutoIn {
    uint32_t now = 0;             // ms
    bool     heaterLive = false;  // bus link up and heater answering
    char     st = 'U';            // 'O' off, 'R' run, 'C' cool, 'F' fault, 'U' unknown
    bool     lit = false;         // phase RUN (flame established)
    uint8_t  level = 0;
    float    roomC = NAN;         // NAN = no reading
    uint32_t runStartMs = 0;      // when this run started (0 = unknown)
    uint32_t offSinceMs = 0;      // when it last went OFF (0 = long ago / unknown)
    bool     noStart = false;     // master shut-off is holding: don't start it
};

enum AutoCmd : uint8_t { AC_NONE, AC_ON, AC_OFF, AC_LEVEL };

struct AutoOut {
    AutoCmd cmd = AC_NONE;
    uint8_t level = 0;            // for AC_LEVEL
    uint8_t alert = 0;            // 0 none, 1 warn, 2 bad
    bool    toManual = false;     // auto gave up, switch to MANUAL
    char    msg[96] = "";         // status line for the page
    char    log[80] = "";         // one-off event for the log
    int8_t  target = -1;          // level the thermostat wants (-1 = n/a)
};

struct AutoState {
    uint32_t lastLevelMs = 0, startCmdMs = 0, warmSinceMs = 0, sensorLostMs = 0, windowMs = 0;
    uint8_t  starts = 0;          // auto starts in the current hour window
    bool     startPending = false;
    bool     windowSet = false;
    float    trend[6] = {0, 0, 0, 0, 0, 0};   // room temp once a minute, for the 5-min trend
    uint8_t  trendIdx = 0, trendN = 0;
    uint32_t trendMs = 0;
    uint8_t  holdLevel = 0;       // level that last held the setpoint steady (0 = not learned yet)
    bool     boost = false;       // frost guard boosting on a hard night
    bool     fromFull = false;    // thermostat is on its full-power warm-up (ease off once it gets there)
    uint32_t autoStopMs = 0;      // when frost guard last shut the heater off
    bool     coldNight = false;   // frost guard: restarted soon after a stop -> idle instead of stopping
    bool     idling = false;      // frost guard cold-night idle in progress
    uint32_t idleWarmMs = 0;
    // smooth hold
    float    tf = NAN;            // room, smoothed (C)
    uint32_t tfMs = 0;
    float    rbuf[10] = {0};      // smoothed room every 30 s, for the rate
    uint8_t  rIdx = 0, rN = 0;
    uint32_t rMs = 0;
    float    rate = 0;            // C per minute, over the last ~5 min
    float    need = NAN;          // the level that holds it, worked out as it goes (NAN = not yet this run)
    float    learned = NAN;       // ...remembered from the last time it held steady (kept between runs)
    uint32_t steadyMs = 0;        // when it started holding steady
    uint32_t smMs = 0;            // last time the smooth hold ran
    bool     easing = false;      // coming in off full power
};

static inline void autoReset(AutoState &s) { s = AutoState(); }

static inline float aC2F(float c) { return c * 9.f / 5.f + 32.f; }

// Format a temperature for status text in the page's unit.
static inline void aFmt(char *b, size_t n, float c, bool f) {
    if (f) snprintf(b, n, "%.0f\xC2\xB0" "F", aC2F(c));
    else   snprintf(b, n, "%.1f\xC2\xB0" "C", roundf(c * 2.f) / 2.f);     // half degrees, like the page
}

// ---------------------------------------------------------------------------
// SMOOTH HOLD. Works in "level units": `need` is the level that would hold the
// room right where it is, worked out as it goes (an integral). The level it
// wants is need + a push from how far off it is - looking ~6 min ahead, because
// the room only feels a level change several minutes later (fit to JR's shed,
// Nov 5 2026: the heater's output lags ~5-15 min behind a level change).
//   - far below (`ease` or more) with thFull: level 6, like before
//   - coming in off full power: steps down early, so it lands near `need`
//     instead of on idle
//   - near the setpoint: one level at a time, at most every 5 min (3 min when
//     it's 1.5 F+ off and getting worse)
//   - `need` is remembered once it has held steady 20 min, and the next run
//     starts from it
// Smoothed room: 1-min average of the DHT22 (0.1 C steps, read every 3 s).
// ---------------------------------------------------------------------------
#ifndef SM_LOOK_MIN
#define SM_LOOK_MIN 6.0f          // look-ahead, minutes
#endif
#ifndef SM_EXIT_LOOK
#define SM_EXIT_LOOK 3.0f         // lets go of full power when it'll be within `ease` this many minutes from now
#endif
#ifndef SM_KP
#define SM_KP 2.0f                // levels per C of (predicted) error
#endif
#ifndef SM_KI
#define SM_KI 0.10f               // levels per C per minute (1 C off for 10 min = 1 level)
#endif
#ifndef SM_STEP_MIN
#define SM_STEP_MIN 5.0f          // minutes between steps near the setpoint
#endif
#ifndef SM_FAST_MIN
#define SM_FAST_MIN 3.0f          // ...when it's well off and getting worse
#endif
#ifndef SM_HYST
#define SM_HYST 0.65f             // how far past the next level `want` has to be before it steps
#endif
#ifndef SM_EASE_I
#define SM_EASE_I 0.6f            // coming in off full power: start working out the holding level this close (C)
#endif
#ifndef SM_QUIET
#define SM_QUIET 0.12f            // within this of the setpoint (C, ~0.2 F) and staying there: no change
#endif
#ifndef SM_GUESS
#define SM_GUESS 4.0f             // starting guess for the holding level, before it has learned one
#endif

static int thermoSmooth(const AutoCfg &cfg, const AutoIn &in, AutoState &s, AutoOut &out, float T, int cur,
                        uint32_t runFor, bool unitF) {
    const uint32_t now = in.now;
    char a[16], b[16];
    aFmt(a, sizeof a, cfg.sp, unitF);

    // smoothed room and its rate (C/min) over the last ~5 min
    float dtMin = s.tfMs ? (now - s.tfMs) / 60000.f : 0.f;
    if (isnan(s.tf) || dtMin > 5.f) { s.tf = T; s.rN = 0; s.rMs = 0; }
    else s.tf += (T - s.tf) * (1.f - expf(-dtMin / 1.0f));
    s.tfMs = now ? now : 1;
    if (!s.rMs || now - s.rMs >= 30000UL) {
        s.rbuf[s.rIdx] = s.tf; s.rIdx = (s.rIdx + 1) % 10; if (s.rN < 10) s.rN++;
        s.rMs = now ? now : 1;
        if (s.rN >= 4) {                                     // least-squares slope over what we have (up to 4.5 min)
            float sx = 0, sy = 0, sxx = 0, sxy = 0; int n = s.rN;
            for (int i = 0; i < n; i++) {
                float x = i * 0.5f, y = s.rbuf[(s.rIdx + 10 - n + i) % 10];
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            float d = n * sxx - sx * sx;
            s.rate = d > 0 ? (n * sxy - sx * sy) / d : 0.f;
        } else s.rate = 0.f;
    }
    const float e = cfg.sp - s.tf;                           // + = too cold
    const float ePred = cfg.sp - (s.tf + s.rate * SM_LOOK_MIN);
    float stepDt = s.smMs ? (now - s.smMs) / 60000.f : 0.f;
    if (stepDt > 1.f) stepDt = 1.f;
    s.smMs = now ? now : 1;
    uint32_t since = s.lastLevelMs ? now - s.lastLevelMs : 0xFFFFFFFFUL;

    // far below: full power, like before - but let go a little early if it's coming up fast
    const float Tp = s.tf + (s.rate > 0.f ? s.rate : 0.f) * SM_EXIT_LOOK;
    if (cfg.thFull && T < cfg.sp - cfg.ease && Tp < cfg.sp - cfg.ease) {
        s.easing = true; s.need = NAN;
        snprintf(out.msg, sizeof out.msg, "Heating to %s - full power", a);
        return 6;
    }
    if (isnan(s.need))                                       // start from what held it last time; switched on mid-run: from here
        s.need = !isnan(s.learned) ? s.learned : (s.easing || runFor < 600000UL ? SM_GUESS : (float)cur);

    // work out the holding level: lean it toward whichever way the room is drifting
    float want = s.need + SM_KP * ePred;
    bool satHi = want >= 6.f && ePred > 0, satLo = want <= 1.f && ePred < 0;
    if (!satHi && !satLo && fabsf(e) < 2.0f && (!s.easing || e < SM_EASE_I)) {   // not while it's still coming in
        s.need += SM_KI * e * stepDt;
        if (s.need < 1.f) s.need = 1.f;
        if (s.need > 6.f) s.need = 6.f;
    }
    want = s.need + SM_KP * ePred;
    if (want < 1.f) want = 1.f;
    if (want > 6.f) want = 6.f;

    // remember it once it's held steady for 20 min
    if (fabsf(e) <= 0.3f && fabsf(s.rate) <= 0.02f) {
        if (!s.steadyMs) s.steadyMs = now ? now : 1;
        if (now - s.steadyMs >= 1200000UL) s.learned = s.need;
    } else s.steadyMs = 0;

    // pick the level: one step at a time, not too often
    int target = cur;
    bool farOff = fabsf(e) >= 0.8f && ((e > 0 && s.rate <= 0.f) || (e < 0 && s.rate >= 0.f));   // 1.5 F+ off and not coming back
    uint32_t gap = (uint32_t)((farOff ? SM_FAST_MIN : SM_STEP_MIN) * 60000.f);
    if (s.easing) gap = 120000UL;                            // coming in off full power: let it ease down at its own pace
    bool settled = fabsf(e) < SM_QUIET && fabsf(ePred) < SM_QUIET * 1.5f;      // right on it: leave the level alone
    if ((since >= gap || since == 0xFFFFFFFFUL) && !settled) {
        if (want >= cur + SM_HYST && cur < 6) target = cur + 1;
        else if (want <= cur - SM_HYST && cur > 1) target = cur - 1;
        if (farOff && fabsf(want - cur) >= 2.5f) target = (int)lroundf(want);   // door left open, that kind of thing
        else if (s.easing && want <= cur - 1.5f) { target = (int)lroundf(want); if (target < cur - 2) target = cur - 2; }
    }
    if (s.easing && (e < 0.3f || s.rate <= 0.005f)) s.easing = false;   // it's there, or it's stopped climbing

    // optional shut-off when it's still too warm at idle (same as before)
    if (cfg.shutoff && cur <= 1 && T >= cfg.sp + cfg.offBand) {
        if (!s.warmSinceMs) s.warmSinceMs = now ? now : 1;
        if (runFor >= (uint32_t)cfg.minRunMin * 60000UL && now - s.warmSinceMs >= 300000UL) {
            out.cmd = AC_OFF;
            aFmt(b, sizeof b, T, unitF);
            snprintf(out.msg, sizeof out.msg, "Warm enough (%s) - shutting off", b);
            snprintf(out.log, sizeof out.log, "AUTO off: room %s, set %s", b, a);
            return cur;
        }
        snprintf(out.msg, sizeof out.msg, "Above %s at idle - shuts off if it stays warm", a);
        return target;
    }
    s.warmSinceMs = 0;
    int nd = (int)lroundf(s.need * 10);
    if (s.easing && e > 0.3f) snprintf(out.msg, sizeof out.msg, "Easing in to %s - level %d", a, target);
    else snprintf(out.msg, sizeof out.msg, "Holding %s - level %d (needs ~%d.%d)", a, target, nd / 10, nd % 10);
    return target;
}

// Called every few seconds. Fills `out`; the caller sends the command.
static void autoStep(const AutoCfg &cfg, const AutoIn &in, AutoState &s, AutoOut &out, bool unitF) {
    out = AutoOut();
    if (cfg.mode == AM_MANUAL) { s.startPending = false; return; }
    const uint32_t now = in.now;
    const uint32_t minRun = (uint32_t)cfg.minRunMin * 60000UL, minOff = (uint32_t)cfg.minOffMin * 60000UL;
    char a[16], b[16];

    if (!s.windowSet || now - s.windowMs > 3600000UL) { s.windowSet = true; s.windowMs = now; s.starts = 0; }

    if (!in.heaterLive) { snprintf(out.msg, sizeof out.msg, "Waiting for the heater"); return; }

    if (in.st == 'F') {
        out.toManual = true; out.alert = 2;
        snprintf(out.msg, sizeof out.msg, "Heater faulted - auto switched off");
        snprintf(out.log, sizeof out.log, "AUTO off: heater fault");
        return;
    }

    // a start we sent: did it take?
    if (s.startPending) {
        if (in.st == 'R' || in.st == 'C') s.startPending = false;
        else if (now - s.startCmdMs > 30000UL) s.startPending = false;   // lost; counted in s.starts already
    }

    uint32_t runFor = (in.st == 'R' && in.runStartMs) ? now - in.runStartMs : 0;

    if (isnan(in.roomC)) {
        if (!s.sensorLostMs) s.sensorLostMs = now ? now : 1;
        out.alert = 1;
        s.trendN = 0;
        snprintf(out.msg, sizeof out.msg, "No room temperature - auto paused");
        if (in.st == 'R') {
            if (runFor >= minRun && now - s.sensorLostMs > 7200000UL) {
                out.cmd = AC_OFF;
                snprintf(out.log, sizeof out.log, "AUTO stop: no room reading for 2 h");
            } else if (in.lit && in.level != 2 && now - s.lastLevelMs >= 90000UL) {
                out.cmd = AC_LEVEL; out.level = 2; s.lastLevelMs = now;      // keep a low flame going blind
            }
            snprintf(out.msg, sizeof out.msg, "No room temperature - holding level 2");
        }
        return;
    }
    s.sensorLostMs = 0;
    const float T = in.roomC;
    if (!s.trendMs || now - s.trendMs >= 60000UL) {             // one sample a minute
        s.trend[s.trendIdx] = T; s.trendIdx = (s.trendIdx + 1) % 6; if (s.trendN < 6) s.trendN++;
        s.trendMs = now ? now : 1;
    }
    const float slope5 = (s.trendN >= 6) ? T - s.trend[s.trendIdx] : 0.f;   // change over ~5 min
    const bool thermo = (cfg.mode == AM_THERMO);

    if (in.st == 'O') {
        uint32_t offFor = in.offSinceMs ? now - in.offSinceMs : 0xFFFFFFFFUL;
        float startAt = thermo ? cfg.sp - cfg.onBand : cfg.frostOn;
        bool want = T <= startAt;
        aFmt(a, sizeof a, startAt, unitF);
        if (!thermo && s.autoStopMs && now - s.autoStopMs > (uint32_t)cfg.coldRestMin * 60000UL) s.coldNight = false;   // long rest: not that cold any more
        if (thermo && !cfg.thStart) {
            snprintf(out.msg, sizeof out.msg, want ? "Cold - start it and the thermostat takes over" : "Off - start it and the thermostat takes over");
        } else if (!want) {
            snprintf(out.msg, sizeof out.msg, thermo ? "Off - starts below %s" : "Armed - starts below %s", a);
        } else if (s.startPending) {
            snprintf(out.msg, sizeof out.msg, "Starting...");
        } else if (in.noStart) {
            snprintf(out.msg, sizeof out.msg, "Master shut-off - no starts until the room cools");
        } else if (offFor < minOff) {
            unsigned left = (unsigned)((minOff - offFor) / 60000UL) + 1;
            snprintf(out.msg, sizeof out.msg, "Cold - restarts in %u min (rest time)", left);
        } else if (s.starts >= 3) {
            out.alert = 1;
            snprintf(out.msg, sizeof out.msg, "3 starts this hour - auto paused");
        } else {
            out.cmd = AC_ON; s.startPending = true; s.startCmdMs = now; s.starts++;
            if (!thermo && cfg.frIdle && s.autoStopMs && now - s.autoStopMs < (uint32_t)cfg.coldRestMin * 60000UL) s.coldNight = true;
            s.idling = false; s.idleWarmMs = 0;
            aFmt(b, sizeof b, T, unitF);
            snprintf(out.msg, sizeof out.msg, "Starting - room %s", b);
            snprintf(out.log, sizeof out.log, "AUTO start: room %s", b);
        }
        return;
    }

    if (in.st != 'R') { s.boost = false; s.fromFull = false; s.need = NAN; s.easing = false; s.steadyMs = 0; }
    if (in.st == 'C') { snprintf(out.msg, sizeof out.msg, "Cooling down"); return; }
    if (in.st != 'R') { snprintf(out.msg, sizeof out.msg, "Waiting"); return; }

    // ---- running ----
    int target;
    if (!thermo) {
        target = cfg.frostLvl < 1 ? 1 : (cfg.frostLvl > 6 ? 6 : cfg.frostLvl);
        if (cfg.frBoost) {
            if (T < cfg.frostOn - 1.0f) s.boost = true;           // losing ground on a hard night...
            else if (T >= cfg.frostOn + 1.0f) s.boost = false;    // ...until it's clearly back up
        } else s.boost = false;
        bool boost = s.boost && target < 5;
        if (boost) target = 5;
        aFmt(a, sizeof a, cfg.frostOff, unitF);
        bool coldIdle = cfg.frIdle && s.coldNight;
        if (coldIdle && s.idling && T <= cfg.frostOn + 1.0f) s.idling = false;   // idle isn't holding it: back to work
        if (T >= cfg.frostOff && coldIdle) s.idling = true;
        if (coldIdle && s.idling && !boost) {
            target = 1;
            if (T >= cfg.frostOff + 2.2f) {                       // 4 F past the stop temp even at idle: it's warming up out there
                if (!s.idleWarmMs) s.idleWarmMs = now ? now : 1;
                if (runFor >= minRun && now - s.idleWarmMs >= 600000UL) {
                    out.cmd = AC_OFF; s.autoStopMs = now ? now : 1; s.coldNight = false; s.idling = false;
                    snprintf(out.msg, sizeof out.msg, "Warm even at idle - shutting off");
                    snprintf(out.log, sizeof out.log, "AUTO stop: warm at idle");
                    return;
                }
            } else s.idleWarmMs = 0;
            snprintf(out.msg, sizeof out.msg, "Cold night - idling at %s instead of shutting off", a);
        } else if (T >= cfg.frostOff) {
            if (runFor >= minRun) {
                out.cmd = AC_OFF; s.autoStopMs = now ? now : 1;
                snprintf(out.msg, sizeof out.msg, "Reached %s - shutting off", a);
                snprintf(out.log, sizeof out.log, "AUTO stop: room reached %s", a);
                return;
            }
            snprintf(out.msg, sizeof out.msg, "Reached %s - %u min left of min run", a, (unsigned)((minRun - runFor) / 60000UL) + 1);
        } else {
            snprintf(out.msg, sizeof out.msg, boost ? "Frost guard: still cold, boosting - running to %s" : "Frost guard: running to %s", a);
        }
    } else if (cfg.thSmooth) {
        int cur = (in.level >= 1 && in.level <= 6) ? in.level : 3;
        target = thermoSmooth(cfg, in, s, out, T, cur, runFor, unitF);
        if (out.cmd == AC_OFF) return;
    } else {
        int cur = (in.level >= 1 && in.level <= 6) ? in.level : 3;
        target = cur;
        const float db = 0.6f;                                    // deadband around the setpoint, C (~1 F)
        uint32_t since = s.lastLevelMs ? now - s.lastLevelMs : 0xFFFFFFFFUL;
        aFmt(a, sizeof a, cfg.sp, unitF);
        // learn the level that holds it: near the setpoint, steady, and left alone for 10 min
        if (fabsf(T - cfg.sp) <= 0.4f && fabsf(slope5) <= 0.2f && s.trendN >= 6 && since >= 600000UL && since != 0xFFFFFFFFUL)
            s.holdLevel = (uint8_t)cur;
        if (cfg.thFull && T < cfg.sp - cfg.ease) {
            target = 6; s.fromFull = true;                        // far below: full power
        } else if (T >= cfg.sp) {
            if (s.fromFull && cur >= 5 && slope5 > 0.f)           // got there on full power: ease right off, once
                target = s.holdLevel ? (s.holdLevel > 1 ? s.holdLevel - 1 : 1) : 1;
            else if (T >= cfg.sp + db && cur > 1 && slope5 > -0.15f && since >= (T >= cfg.sp + 1.f ? 120000UL : 420000UL))
                target = cur - 1;                                 // above and not already falling: one step down
        } else if (T <= cfg.sp - db && cur < 6 && slope5 < 0.15f && since >= (slope5 < -0.4f ? 120000UL : 420000UL)) {
            target = cur + 1;                                     // below and not recovering: one step up (faster if dropping fast)
        }
        if (T >= cfg.sp && target < 5) s.fromFull = false;
        if (cfg.shutoff && cur <= 1 && T >= cfg.sp + cfg.offBand) {
            if (!s.warmSinceMs) s.warmSinceMs = now ? now : 1;
            if (runFor >= minRun && now - s.warmSinceMs >= 300000UL) {
                out.cmd = AC_OFF;
                aFmt(b, sizeof b, T, unitF);
                snprintf(out.msg, sizeof out.msg, "Warm enough (%s) - shutting off", b);
                snprintf(out.log, sizeof out.log, "AUTO off: room %s, set %s", b, a);
                return;
            }
            snprintf(out.msg, sizeof out.msg, "Above %s at idle - shuts off if it stays warm", a);
        } else {
            s.warmSinceMs = 0;
            if (target >= 6 && T < cfg.sp - cfg.ease && cfg.thFull) snprintf(out.msg, sizeof out.msg, "Heating to %s - full power", a);
            else if (target <= 1)          snprintf(out.msg, sizeof out.msg, "Holding %s - idling", a);
            else                           snprintf(out.msg, sizeof out.msg, "Holding %s - level %d", a, target);
        }
    }
    out.target = (int8_t)target;
    if (in.lit && target != in.level && (!s.lastLevelMs || now - s.lastLevelMs >= 90000UL)) {
        out.cmd = AC_LEVEL; out.level = (uint8_t)target; s.lastLevelMs = now;
    }
}
