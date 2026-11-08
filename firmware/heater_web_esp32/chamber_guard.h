// ============================================================================
//  CHAMBER GUARD - keeps the burner hot enough not to coke up. Runs in every
//  mode (Manual, Thermostat, Frost guard). Pure logic, no Arduino calls, so it
//  can be simulated on a PC.
//
//  While the flame is lit, if the chamber sits under the floor for 3 min it adds
//  one level on top of whatever level you / the thermostat picked ("bump"). Up to
//  +2 levels, never past level 6. Never starts or stops the heater.
//
//  Waits before judging: 10 min after light-up, 5 min after anyone changes the
//  level, 8 min after its own bump (the chamber needs time to answer).
//  Lets go of one notch after it has held 20 min AND the chamber has been at
//  least `relC` over the floor for 15 min. If it has to bump again in the same
//  run, it keeps that bump for the rest of the run (no hunting up and down).
//  Maxed out (+2 or level 6) and still under the floor for 10 min -> warning.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>

struct GuardIn {
    uint32_t now = 0;        // ms
    bool     on = true;      // toggle
    float    floorC = 120;   // chamber floor, C
    bool     lit = false;    // running with the flame established
    float    chamberC = NAN; // NAN = no reading
    uint8_t  base = 0;       // level wanted without the guard (1-6)
};

struct GuardState {
    uint8_t  bump = 0;            // notches requested on top of base
    bool     latched = false;     // re-bumped after a release this run: hold it
    bool     released = false;    // let go of a notch earlier this run
    uint32_t litMs = 0, belowMs = 0, warmMs = 0, lastBumpMs = 0, lastExtMs = 0, maxColdMs = 0;
    uint8_t  alert = 0;           // 1 = maxed out and still cold
    char     msg[96] = "";
    char     log[80] = "";
};

static inline void guardExternal(GuardState &g, uint32_t now) {   // someone else changed the level
    g.bump = 0; g.latched = false; g.released = false;
    g.belowMs = 0; g.warmMs = 0; g.maxColdMs = 0; g.lastExtMs = now ? now : 1;
}

// Returns the level that should actually be on the heater (base + usable bump).
static uint8_t guardStep(const GuardIn &in, GuardState &g, bool unitF) {
    g.log[0] = 0; g.alert = 0;
    const uint32_t now = in.now ? in.now : 1;
    uint8_t base = in.base < 1 ? 1 : (in.base > 6 ? 6 : in.base);
    auto deg = [&](char *b, size_t n, float c) {
        if (unitF) snprintf(b, n, "%.0f\xC2\xB0" "F", c * 9.f / 5.f + 32.f); else snprintf(b, n, "%.0f\xC2\xB0" "C", c);
    };
    char f[16], c[16];
    deg(f, sizeof f, in.floorC);

    if (!in.on || !in.lit) {              // off, or not burning: forget this run
        bool had = g.bump > 0;
        g.bump = 0; g.latched = false; g.released = false;
        g.litMs = 0; g.belowMs = 0; g.warmMs = 0; g.maxColdMs = 0;
        if (had && in.on) snprintf(g.log, sizeof g.log, "GUARD reset (run ended)");
        snprintf(g.msg, sizeof g.msg, in.on ? "Chamber guard armed (floor %s)" : "", f);
        return base;
    }
    if (!g.litMs) g.litMs = now;

    uint8_t room = 6 - base; if (room > 2) room = 2;          // how many notches can actually be added
    if (g.bump > room) g.bump = room;
    uint8_t lvl = base + g.bump;

    if (isnan(in.chamberC)) {
        g.belowMs = 0; g.warmMs = 0;
        snprintf(g.msg, sizeof g.msg, "Chamber guard: no chamber reading (turn chamber poll on)");
        return lvl;
    }
    deg(c, sizeof c, in.chamberC);
    bool settled = now - g.litMs >= 600000UL && (!g.lastExtMs || now - g.lastExtMs >= 300000UL) &&
                   (!g.lastBumpMs || now - g.lastBumpMs >= 480000UL);

    if (in.chamberC < in.floorC) {
        g.warmMs = 0;
        if (!g.belowMs) g.belowMs = now;
        if (!settled) {
            snprintf(g.msg, sizeof g.msg, "Chamber %s under %s - settling, watching", c, f);
        } else if (g.bump < room) {
            if (now - g.belowMs >= 180000UL) {
                g.bump++; g.lastBumpMs = now; g.belowMs = 0;
                if (g.released) g.latched = true;
                lvl = base + g.bump;
                snprintf(g.log, sizeof g.log, "GUARD chamber %s < %s: L%u -> L%u", c, f, (unsigned)(lvl - 1), (unsigned)lvl);
                snprintf(g.msg, sizeof g.msg, "Chamber %s under %s - raised to L%u", c, f, (unsigned)lvl);
            } else snprintf(g.msg, sizeof g.msg, "Chamber %s under %s - raising soon", c, f);
        } else {
            if (!g.maxColdMs) g.maxColdMs = now;
            if (now - g.maxColdMs >= 600000UL) g.alert = 1;
            snprintf(g.msg, sizeof g.msg, "Chamber %s under %s even at L%u", c, f, (unsigned)lvl);
        }
        return lvl;
    }

    g.belowMs = 0; g.maxColdMs = 0;
    if (g.bump && !g.latched) {
        if (in.chamberC >= in.floorC + 25.f) { if (!g.warmMs) g.warmMs = now; } else g.warmMs = 0;
        if (g.warmMs && now - g.warmMs >= 900000UL && now - g.lastBumpMs >= 1200000UL) {
            g.bump--; g.released = true; g.warmMs = 0; g.lastBumpMs = now;
            uint8_t nl = base + g.bump;
            snprintf(g.log, sizeof g.log, "GUARD chamber %s, easing L%u -> L%u", c, (unsigned)lvl, (unsigned)nl);
            lvl = nl;
        }
    }
    if (g.bump) snprintf(g.msg, sizeof g.msg, "Chamber %s - guard holding +%u (L%u)%s", c, (unsigned)g.bump, (unsigned)lvl, g.latched ? " for this run" : "");
    else        snprintf(g.msg, sizeof g.msg, "Chamber %s - above %s, OK", c, f);
    return lvl;
}
