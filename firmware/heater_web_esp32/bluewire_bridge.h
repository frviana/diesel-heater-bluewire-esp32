// Bus state machine ported from JR's heater_bridge_nano. Included by the
// platform implementation and by the host bus simulator.
#pragma once

#include <Arduino.h>
#include "bluewire_rx.h"

namespace BlueWire {
namespace {
enum HState : uint8_t { ST_UNK, ST_OFF, ST_RUN, ST_COOL, ST_FAULT };

constexpr uint8_t B_HEARTBEAT = 0x59;
constexpr uint8_t B_CHAMBER = 0x50;
constexpr uint8_t M_POWER = 0x04;
constexpr uint8_t M_DOWN = 0x02;
constexpr uint8_t M_UP = 0x05;
constexpr uint32_t BOOT_LISTEN_MS = 2500;
constexpr uint32_t LCD_TIMEOUT_MS = 3000;
constexpr uint32_t OWN_REPLY_MS = 600;
constexpr uint32_t CHAMBER_STALE_MS = 15000;
constexpr uint32_t STATUS_MIN_MS = 900;
constexpr uint32_t STATUS_MAX_MS = 1500;
constexpr uint32_t CMD_EXPIRE_MS = 8000;
constexpr uint32_t POWER_STATE_MAX_MS = 5000;
constexpr uint8_t MAX_LEVEL_STEPS = 8;

struct {
    HState   state = ST_UNK;
    HState   observedState = ST_UNK;
    uint32_t observedMs = 0;
    uint8_t  level = 0;
    bool     run = false, pump = false, glow = false;
    uint16_t word = 0;
    int      chamberRaw = -1;
    unsigned long chamberMs = 0;
    unsigned long lastReplyMs = 0;
    uint8_t  volts = 0;            // supply volts, only sent while idle
    unsigned long voltsMs = 0;
    int8_t   ecode = -1;           // tentative E-number while faulted
} H;

bool     piggyback = false;
bool     ownReplyPending = false;
uint32_t lastTxMs = 0;
uint32_t startedMs = 0;
unsigned long lastLcdMs = 0;
bool     rawEcho = false;
uint8_t  cmdMask = 0;              // pending command, XOR'd onto 0x59
unsigned long cmdQueuedMs = 0;
HState powerFrom = ST_UNK;
int8_t   targetLevel = -1;
uint8_t  levelSteps = 0;
uint8_t  chamberPollS = 5;
unsigned long lastChamberReqMs = 0;
unsigned long lastHbMs = 0;
unsigned long lastStatusMs = 0;
uint8_t  seenReq[32];              // one bit per display byte seen since boot
int16_t  reqToReport = -1;

const char* stateName(HState s) {
    switch (s) {
        case ST_OFF:   return "OFF";
        case ST_RUN:   return "RUN";
        case ST_COOL:  return "COOL";
        case ST_FAULT: return "FAULT";
        default:       return "UNK";
    }
}

void printHex2(uint8_t b) { if (b < 0x10) output.print('0'); output.print(b, HEX); }

void queueCmd(uint8_t m) { cmdMask = m; cmdQueuedMs = millis(); }

// What the heater is doing inside the state, from the fan/pump/glow flags.
const __FlashStringHelper* phaseName() {
    bool f = H.run, p = H.pump, g = H.glow;
    switch (H.state) {
        case ST_OFF:   return F("IDLE");
        case ST_FAULT: return F("FAULT");
        case ST_RUN:
            if (!f && !p && !g) return F("START");
            if ( f && !p && !g) return F("FAN");
            if ( f && !p &&  g) return F("PREHEAT");
            if ( p &&  g)       return F("IGNITION");
            return F("RUN");
        case ST_COOL:
            if (p) return F("SHUTDOWN");
            if (g) return F("CLEARING");
            if (f) return F("PURGE");
            return F("COOL");
        default:       return F("UNK");
    }
}

// FAULT / UNK need two words in a row so one bad frame can't flash an alarm.
bool setState(HState ns, uint16_t w) {
    static HState oddState = ST_OFF;   // ST_OFF = none pending
    static uint8_t oddCount = 0;
    if (ns == ST_FAULT || ns == ST_UNK) {
        if (oddState != ns) { oddState = ns; oddCount = 0; }
        if (oddCount < 2) ++oddCount;
        if (oddCount < 2) return false;
    } else { oddState = ST_OFF; oddCount = 0; }
    if (ns != H.state) {
        output.print(F("EVT ")); output.print(stateName(H.state));
        output.print(F("->"));   output.print(stateName(ns));
        if (ns == ST_FAULT || ns == ST_UNK) { output.print(F(" word ")); output.print(w, HEX); }
        output.println();
        H.state = ns;
    }
    return true;
}

void handleHeaterWord(uint16_t w) {
    H.lastReplyMs = millis();
    uint8_t top = w >> 12;
    if (top == 0xB) { H.chamberRaw = w & 0x0FFF; H.chamberMs = millis(); return; }

    uint8_t flags = (w >> 8) & 0x0F;
    uint8_t lo    =  w       & 0xFF;
    HState ns = ST_UNK;
    if (top == 0x9) {
        if      (lo >= 0xE0)               ns = ST_OFF;
        else if (lo >= 0xBA && lo <= 0xBF) ns = ST_RUN;
        else if ((lo & 0xF0) == 0x30)      ns = ST_COOL;
    } else if (top == 0x7) ns = ST_FAULT;

    H.observedState = ns;
    H.observedMs = millis();
    if (!setState(ns, w)) return;
    H.word = w;
    H.run   = !(flags & 0x08);
    H.pump  = !(flags & 0x04);
    H.glow  = !(flags & 0x01);
    H.level = (ns == ST_RUN) ? (0xC0 - lo) : 0;
    if (ns == ST_OFF) { H.volts = 0xFF - lo; H.voltsMs = millis(); }
    if (ns == ST_FAULT) H.ecode = (int8_t)(((uint8_t)~lo) & 0x3F) - 1;
    else                H.ecode = -1;
    if (ns == ST_UNK) H.run = H.pump = H.glow = false;
}

void printStatus() {
    int cr = (H.chamberMs && millis() - H.chamberMs < CHAMBER_STALE_MS) ? H.chamberRaw : -1;
    output.print(F("S,")); output.print(stateName(H.state));
    output.print(','); output.print(H.level);
    output.print(','); output.print(H.run  ? 1 : 0);
    output.print(','); output.print(H.pump ? 1 : 0);
    output.print(','); output.print(H.glow ? 1 : 0);
    output.print(','); output.print(H.word, HEX);
    output.print(','); output.print(piggyback ? F("PB") : F("SA"));
    output.print(','); output.print(cr);
    output.print(','); output.print(H.lastReplyMs ? (millis() - H.lastReplyMs) / 1000 : 999);
    output.print(','); output.print(chamberPollS);
    output.print(','); output.print(H.volts);
    output.print(','); output.print(H.voltsMs ? (millis() - H.voltsMs) / 1000 : 999);
    output.print(','); output.print(phaseName());
    output.print(','); output.println(H.ecode);
    lastStatusMs = millis();
}

// ============================================================================
// Scheduling
// ============================================================================
bool chamberDue() {
    if (chamberPollS == 0) return false;
    if (H.state != ST_RUN && H.state != ST_COOL) return false;
    unsigned long iv = (unsigned long)chamberPollS * 1000UL;
    if (H.chamberMs && millis() - H.chamberMs < iv) return false;          // fresh (display may be on chamber page)
    if (lastChamberReqMs && millis() - lastChamberReqMs < iv) return false;
    return true;
}

// A power command is a toggle on the wire. Recheck the latest state at
// transmission time in case the OEM display acted after the command queued.
void checkPendingPower() {
    if (cmdMask != M_POWER) return;
    const bool fresh = millis() - H.observedMs <= POWER_STATE_MAX_MS;
    const bool start = powerFrom == ST_OFF || powerFrom == ST_FAULT;
    const bool matches = start ? H.observedState == ST_OFF || H.observedState == ST_FAULT
                               : H.observedState == ST_RUN;
    if (!fresh || !matches) {
        cmdMask = 0;
        output.println(F("ERR power command cancelled: heater state changed or stale"));
    }
}

bool transmit(uint8_t byte) {
    if (!sendFrame(byte)) return false;
    lastTxMs = millis();
    ownReplyPending = true;
    return true;
}

// Called only in the quiet gap following a heater reply to the OEM display.
void piggybackSlot() {
    checkPendingPower();
    if (cmdMask) {
        if (transmit(B_HEARTBEAT ^ cmdMask)) cmdMask = 0;
    } else if (chamberDue() && transmit(B_CHAMBER)) {
        lastChamberReqMs = millis();
    }
}

void standaloneTick() {
    const uint32_t now = millis();
    if (now - startedMs < BOOT_LISTEN_MS || ownReplyPending || now - lastHbMs < 1000) return;
    checkPendingPower();
    const bool poll = !cmdMask && chamberDue();
    const uint8_t byte = cmdMask ? B_HEARTBEAT ^ cmdMask : poll ? B_CHAMBER : B_HEARTBEAT;
    if (!transmit(byte)) return;
    cmdMask = 0;
    lastHbMs = now;
    if (poll) lastChamberReqMs = now;
}

// LEVEL n: one step per display cycle, and only after the reply to the last
// step has shown the new level.
void stepTowardTarget() {
    if (targetLevel < 0 || cmdMask || ownReplyPending) return;
    if (H.state != ST_RUN) { targetLevel = -1; output.println(F("ERR LEVEL stopped, not running")); return; }
    if (H.level == targetLevel) {
        targetLevel = -1; output.print(F("ACK LEVEL done ")); output.println(H.level); return;
    }
    if (++levelSteps > MAX_LEVEL_STEPS) { targetLevel = -1; output.println(F("ERR LEVEL gave up")); return; }
    queueCmd((H.level < targetLevel) ? M_UP : M_DOWN);
}

void onLcdFrame(uint8_t b) {
    lastLcdMs = millis();
    if (rawEcho) { output.print(F("R,LCD,")); printHex2(b); output.println(); }
    if (!(seenReq[b >> 3] & (1 << (b & 7)))) {
        seenReq[b >> 3] |= (1 << (b & 7));
        reqToReport = b;                      // reported with the heater's answer
    }
}

void onHeaterFrame(uint16_t w, uint32_t endUs) {
    bool ours = ownReplyPending;
    ownReplyPending = false;
    handleHeaterWord(w);
    if (rawEcho) { output.print(ours ? F("R,HTR*,") : F("R,HTR,")); output.println(w, HEX); }
    if (reqToReport >= 0 && !ours) {
        output.print(F("REQ ")); printHex2((uint8_t)reqToReport);
        output.print(F(" -> ")); output.println(w, HEX);
        reqToReport = -1;
    }
    if (millis() - lastStatusMs >= STATUS_MIN_MS) printStatus();   // bus is quiet now
    // Budget for a whole exchange before the next nominal display poll.
    if (piggyback && !ours && micros() - endUs < 100000) piggybackSlot();
}

// ============================================================================
// Commands from the web controller
// ============================================================================
void handleCommand(char *c) {
    for (char *p = c; *p; p++) *p = toupper(static_cast<unsigned char>(*p));

    if (!strcmp(c, "PING"))         { output.println(F("PONG")); }
    else if (!strcmp(c, "STATUS"))  { printStatus(); }
    else if (!strcmp(c, "RAW ON"))  { rawEcho = true;  output.println(F("ACK RAW ON")); }
    else if (!strcmp(c, "RAW OFF")) { rawEcho = false; output.println(F("ACK RAW OFF")); }
    else if (!strcmp(c, "ON")) {
        if (H.state != ST_OFF && H.state != ST_FAULT) { output.print(F("ERR ON refused, heater is ")); output.println(stateName(H.state)); }
        else { targetLevel = -1; powerFrom = H.state; queueCmd(M_POWER); output.println(H.state == ST_FAULT ? F("ACK ON (clear fault)") : F("ACK ON")); }
    }
    else if (!strcmp(c, "OFF")) {
        if (H.state != ST_RUN) { output.print(F("ERR OFF refused, heater is ")); output.println(stateName(H.state)); }
        else { targetLevel = -1; powerFrom = H.state; queueCmd(M_POWER); output.println(F("ACK OFF")); }   // OFF beats any pending step
    }
    else if (!strcmp(c, "UP") || !strcmp(c, "DOWN")) {
        bool up = (c[0] == 'U');
        if (H.state != ST_RUN)              output.println(F("ERR not running"));
        else if (cmdMask == M_POWER)        output.println(F("ERR busy"));
        else if (up && H.level >= 6)        output.println(F("ERR already level 6"));
        else if (!up && H.level <= 1)       output.println(F("ERR already level 1"));
        else { targetLevel = -1; queueCmd(up ? M_UP : M_DOWN); output.println(up ? F("ACK UP") : F("ACK DOWN")); }
    }
    else if (!strncmp(c, "LEVEL ", 6)) {
        int n = atoi(c + 6);
        if (n < 1 || n > 6)                 output.println(F("ERR level 1-6"));
        else if (H.state != ST_RUN)         output.println(F("ERR not running"));
        else if (cmdMask == M_POWER)        output.println(F("ERR busy"));
        else { targetLevel = n; levelSteps = 0; output.print(F("ACK LEVEL ")); output.println(n); }
    }
    else if (!strncmp(c, "CHAMBER ", 8)) {
        int n = atoi(c + 8);
        if (n < 0 || n > 60) output.println(F("ERR chamber 0-60"));
        else { chamberPollS = n; output.print(F("ACK CHAMBER ")); output.println(n); }
    }
    else output.println(F("ERR unknown"));
}

void startBridge(int pollSeconds) {
    if (pollSeconds >= 0 && pollSeconds <= 60) chamberPollS = pollSeconds;
    startedMs = millis();
    seenReq[B_HEARTBEAT >> 3] |= 1 << (B_HEARTBEAT & 7);
    seenReq[B_CHAMBER >> 3] |= 1 << (B_CHAMBER & 7);
    output.println(F("HEATER BRIDGE ESP32 ready"));
}

void tickBridge() {
    const uint32_t now = millis();
    if (cmdMask && now - cmdQueuedMs > CMD_EXPIRE_MS) {
        cmdMask = 0;
        targetLevel = -1;
        output.println(F("ERR command expired"));
    }

    const Frame frame = takeFrame();
    if (frame.bits == 8) {
        onLcdFrame(static_cast<uint8_t>(frame.word));
        if (!piggyback) {
            piggyback = true;
            output.println(F("MODE PB"));
        }
    } else if (frame.bits == 16) {
        onHeaterFrame(frame.word, frame.endUs);
    }

    if (ownReplyPending && millis() - lastTxMs > OWN_REPLY_MS) ownReplyPending = false;
    if (piggyback && millis() - lastLcdMs >= LCD_TIMEOUT_MS) {
        piggyback = false;
        output.println(F("MODE SA"));
    }

    char cmd[24];
    if (takeCommand(cmd) && cmd[0]) handleCommand(cmd);
    if (!piggyback) standaloneTick();
    stepTowardTarget();
    if (millis() - lastStatusMs >= STATUS_MAX_MS) printStatus();
}
}  // namespace
}  // namespace BlueWire
