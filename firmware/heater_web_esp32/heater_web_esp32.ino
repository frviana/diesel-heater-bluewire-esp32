/*
 * ============================================================================
 *  HEATER WEB CONTROLLER  -  ESP32 WROOM  -  v2.0.0
 * ============================================================================
 *  Reads the heater blue-wire bus through a BSS138 and serves a phone page.
 *  Protocol timing runs in a separate task with GPIO edge capture and RMT TX.
 *
 *  v1.9.5
 *   - SMOOTH HOLD for the thermostat (thermostat.h, thermoSmooth()). Before, it
 *     went full power to the setpoint, dropped straight to idle, slid ~2 F,
 *     then climbed back toward full (Nov 5: 65 -> idle -> 63 -> level 5; the day
 *     before spent 2.7 h at idle and 2.3 h at full). Now it eases down on the
 *     way up so it arrives near the level that holds the room, then trims one
 *     level at a time (5 min apart, 3 if it's 1.5 F+ off and getting worse),
 *     working out the level that holds it as it goes ("needs ~4.5" on the page)
 *     and starting from that next run. Tuned on a model of the shed fitted to the
 *     Nov 5 warm-up (the heater's output takes 5-15 min to reach the room).
 *     Switch "Smooth hold" under More settings > Thermostat; off = v1.9.4's way.
 *   - /api/status auto: thSmooth, need (level that holds it, null until known).
 *     /api/auto: thSmooth=1|0. Saved as "aThM".
 *
 *  v1.9.4
 *   - HOME-SCREEN ICON: a flame icon + web app manifest (/manifest.json,
 *     /icon-192.png, /icon-512.png, /favicon.ico) so "Add to home screen"
 *     gets a proper icon and the name "Shed Heater" instead of a blank letter.
 *     Icon drawn in web_src/icons/icon.svg, built into web_icons.h.
 *
 *  v1.9.3
 *   - WiFi power-saving OFF. With the ESP32's default modem sleep it can miss a
 *     phone's "who has 192.168.1.229?" (ARP) - devices that talked to it
 *     recently keep working, a phone that has to ask fresh can't connect
 *     (Nov 4: PC fine, Android phone couldn't reach it at all). ~0.5 W more.
 *   - Page says "can't reach the controller" after a few failed updates
 *     instead of sitting on "connecting..." forever.
 *   - Web self-check talks to 127.0.0.1 with a short timeout (asking its own
 *     WiFi address never answered and stalled the loop 1.5 s every 3 min).
 *
 *  v1.9.2
 *   - FIX: false "not warming - may not have lit" alert after the ESP32 restarts
 *     (or an update) while the heater is already burning. The check now only
 *     runs on a run it watched start from OFF; a run it joined part-way is
 *     already lit, its chamber is already hot, so "hasn't warmed 20 C" meant
 *     nothing.
 *
 *  v1.9.1  BLACK BOX (diag.h) - after the Nov 4 drop-off (web page unreachable,
 *          controller still running)
 *   - Every start logs WHY it restarted (power, crash, hang, brown-out, its own
 *     restart), how long it had been up, lowest free memory, WiFi signal, last
 *     web request, and which part of the program it was in. Crash addresses
 *     too. Read it at http://<ip>/diag (linked at the bottom of History).
 *   - Watches itself: main loop stuck 20 s -> restart; web page stops
 *     answering its own self-check 3x -> restart; WiFi lost -> reconnect,
 *     restart after 10 min; on the HeaterCtl hotspot it keeps trying your
 *     WiFi and rejoins. A restart of the ESP32 never touches the heater.
 *   - Low-memory / slow-loop warnings and a daily summary line in the log.
 *   - Settings can't be saved as NaN (would break the page).
 *   - FIX: a single garbled bus reply (state UNK for a moment) no longer counts
 *     as the heater stopping and restarting - that reset "This run" and gave a
 *     false "not warming" alert (Nov 4 log: RUN->UNK word D884 -> RUN).
 *
 *  v1.9
 *   - HISTORY PAGE (/history, link under the stats on every page style):
 *     24 h / 7 days / 30 days / Year. Run time (coloured by level), starts,
 *     light-up time, room low/high, time at each level, lowest idle battery
 *     volts, master shut-offs / keep-running stops / faults / flame-outs, and
 *     a day-by-day list. Hourly detail for 7 days, daily totals for a year.
 *     Saved to the ESP32's flash (LittleFS) every 10 min and on the hour, so
 *     it survives power cuts (a cut can lose up to the last 10 min).
 *     Recording starts once the clock is set from the internet.
 *
 *  v1.8  = v1.7 (chamber guard) + master shut-off + keep-running check
 *   - MASTER SHUT-OFF (safety.h): room at 75 F for a minute -> heater off, in
 *     every mode, Manual included. No starts from the page or auto until it's
 *     back under 70 F; started at the heater's display while it's still over
 *     75 F -> stopped again. Always on. Setpoints top out at 70 F.
 *   - KEEP-RUNNING CHECK: a Manual run gets asked "keep it running?" on the
 *     page after 3 h (adjustable, can be switched off). No answer in 15 min ->
 *     heater off.
 *   - Pages stored gzipped (more flash room), core.js?v=18 so phones pick up
 *     the new page script right after the update.
 *
 *  v1.7
 *   - CHAMBER GUARD (chamber_guard.h): in every mode, if the chamber sits under
 *     the floor (default 120 C / 248 F) for 3 min while lit, it adds a level
 *     (max +2, never past 6). Eases back off once it's well over the floor.
 *     Toggle + floor in More settings. Needs chamber poll on.
 *
 *  v1.6
 *   - ROOM SENSOR: DHT22 on GPIO32 (+ to 3.3V, - to GND). Room temperature and
 *     humidity on every page, plus a room-temperature graph for the last hour.
 *     Needs Adafruit's "DHT sensor library" (Library Manager - also install
 *     "Adafruit Unified Sensor" when it asks).
 *   - AUTO MODES (thermostat.h tab), picked on the page:
 *       Manual ..... like before.
 *       Thermostat . full power until it's within a few degrees of the setpoint,
 *                    then eases off and steps a level at a time to hold it.
 *                    Starts by itself when the room drops below the setpoint;
 *                    optional shut-off when it stays warm at idle.
 *       Frost guard  starts when the room drops to the "start below"
 *                    temperature, runs at a low level until "stop at", then
 *                    shuts off. Boosts if it keeps losing ground.
 *     Guard rails: 30 min minimum run, 15 min minimum rest, max 3 starts an
 *     hour, a fault or a stop at the heater's own display switches back to
 *     Manual, no starts without a room reading. Settings survive power cuts.
 *     STOP or a level change from the page switches back to Manual; START
 *     leaves the thermostat in charge.
 *     COLD-NIGHT IDLE (frost guard): if it has to restart soon after a stop,
 *     it idles at level 1 at the stop temperature instead of cycling.
 *     Every one of these behaviours has an on/off toggle on the page.
 *   - LED: the idle heartbeat turns ice-blue while an auto mode is armed.
 *
 *  v1.5
 *   - WIRING CHANGE: the Nano link now comes in on GPIO4 (was GPIO16). This
 *     board's RGB LED sits on GPIO16, so the Nano's data was lighting it up at
 *     random. Move the yellow divider wire from 16 to 4.
 *   - LIGHT SHOW on the RGB LED (led_show.h tab): rainbow at power-up, blue
 *     radar ping while looking for WiFi, dim green heartbeat when idle (purple
 *     on the hotspot), red double blink if the Nano goes quiet, amber pulse if
 *     the heater does, then the heater itself - fan spin-up, glow plug throb,
 *     ignition sparks, a flame that grows with the level, embers, cooling
 *     orange-to-blue, red fault flashes, alert flashes, white blip on commands.
 *     LED_MAX below sets the brightness (0 = LED off).
 *
 *  v1.4.2
 *   - FIX: page stuck on "connecting..." after a power-up. When the Nano and
 *     ESP32 start together the Nano's first few bytes can be noise; those raw
 *     bytes went into the event log and made the status data unreadable for
 *     the phone. Now only clean, known Nano lines are logged (junk is counted
 *     and shown on the Serial Monitor as "[nano junk]"), status lines with a
 *     bad state are ignored, and the log text is scrubbed before it's sent.
 *     /api/status has a "junk" count - it should stay at 0-3 (boot noise only).
 *
 *  v1.4.1
 *   - FIX: false "flame-out?" alert about 50 s after lighting on the 2nd+ run of
 *     the day. The 2-minute chamber buffer was emptied at each start but its
 *     write position wasn't, so the check compared against the LAST run's hot
 *     readings (first shed test Oct 24: "dropped 117 C" right after light-up).
 *     Pages, Nano and everything else unchanged.
 *
 *  v1.4
 *   - THREE PAGE STYLES, picked from the dropdown at the top of any page:
 *       Burner (the v1.3 cutaway), LCD (OEM-style 7-segment panel with round
 *       buttons), Dash (gauge with needle, warning lamps, LED level bar).
 *     The choice is saved on the ESP32. Direct links: /burner /lcd /dash.
 *     All three share one script (/core.js) so they behave the same.
 *   - PHASES from bridge v1.2: START, FAN UP, PREHEAT, IGNITION, RUN,
 *     SHUTDOWN, CLEARING, PURGE, COOLING - shown on every style.
 *   - IDLE VOLTS (the 12 V the OEM display shows) with low / high warning.
 *   - FAULT CODE (tentative E-number) and START clears a fault (bridge v1.2).
 *   - STATS: this run, last light-up time, today, total hours, number of starts.
 *     Total / starts / today are saved, so they survive power cuts. "Today"
 *     uses internet time (Pacific); on the hotspot it shows "since boot".
 *   - GRAPH: chamber temperature for the last hour (every 15 s), with a
 *     running / cooling / fault strip under it. Touch it to read a point.
 *   - ALERTS (banner at the top): fault, slow ignition, flame-out, low battery.
 *   - Log lines carry the clock time once internet time is in.
 *   - OTA upload still on (Tools > Port > "heater").
 *
 *  v1.3 burner page, v1.2 C/F + poll button, v1.1 chamber table - see the
 *  READMEs in the project folder.
 *
 *  WIRING
 *    GPIO27 <-> BSS138 LV1/HV1 <-> heater blue wire, common ground
 *    GPIO16 = the board's own RGB LED (leave nothing plugged into 16)
 *    GPIO32 <- DHT22 data   3.3V -> DHT22 +   GND -> DHT22 -
 *
 *  SETUP
 *    1. Put your WiFi password in WIFI_PASS below - same one as before.
 *    2. Upload by USB, or over WiFi: Tools > Port > network port "heater".
 *    3. Phone: the IP the Serial Monitor prints (115200), or http://heater.local
 *       If WiFi fails it makes hotspot "HeaterCtl" / heater123 / http://192.168.4.1
 *
 *  The web pages live in the web_*.h tabs next to this file (gzipped - edit
 *  page_sources and run build_pages.py to change them).
 *  Libraries (Library Manager): Adafruit "DHT sensor library" + "Adafruit
 *  Unified Sensor".
 *  Everything else is built into the ESP32 core.
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <LittleFS.h>
#include "web_core.h"
#include "web_burner.h"
#include "web_lcd.h"
#include "web_dash.h"
#include "web_hist.h"
#include "web_manifest.h"
#include "web_icons.h"
#include "led_show.h"
#include "dht22.h"
#include "thermostat.h"
#include "chamber_guard.h"
#include "safety.h"
#include "histlog.h"
#include "bluewire.h"

// ---------------- CONFIG ----------------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
constexpr char WIFI_PASS[] = "CHANGE_ME";   // <- your WiFi password (the sketch won't compile until it's in)
const char* HOSTNAME  = "heater";
const char* AP_SSID   = "HeaterCtl";
const char* AP_PASS   = "heater123";
const char* TZ_INFO   = "PST8PDT,M3.2.0,M11.1.0";   // Pacific, for "today" and log times
#define RGB_PIN     16       // onboard RGB LED
#define LED_MAX     160      // LED brightness cap 0-255 (0 = LED off)
#define DHT_PIN     32       // DHT22 room sensor data
#define FW_VER      "2.0.0"
// Stops an OTA upload that would leave the ESP32 unable to get back on WiFi.
constexpr bool notSet(const char *s) { return !s[0] || (s[0]=='C' && s[1]=='H' && s[2]=='A' && s[3]=='N' && s[4]=='G' && s[5]=='E' && s[6]=='_' && s[7]=='M' && s[8]=='E'); }
static_assert(!notSet(WIFI_PASS), "Put your WiFi password in WIFI_PASS near the top of the sketch");
// ----------------------------------------

// ---------------- ALERT TUNING ----------------
#define IGN_PHASE_MAX_S    180   // still in IGNITION (glow + fuel) this long -> warn
#define IGN_PUMP_S         240   // fuel pump on this long...
#define IGN_RISE_C          20   // ...and chamber hasn't risen this much -> warn
#define FLAMEOUT_DROP_C     40   // chamber falls this much inside 2 min while running -> alert
#define LEVEL_DOWN_QUIET_S 180   // ignore drops this long after turning the level down
#define VOLTS_WARN          11
#define VOLTS_BAD           10
#define VOLTS_HIGH          15
// ----------------------------------------------

// ---------------- CHAMBER CALIBRATION ----------------
// raw count from the heater (falls as it heats) -> degrees C, as the OEM display shows it.
// Keep the list ordered from cold (high raw) to hot (low raw).
struct CalPt { int raw; float c; };
const CalPt CAL[] = {
    {3859,  34}, {3722,  50}, {3254, 114}, {3144, 160},
    {3118, 180}, {3110, 190}, {3100, 200},
};
const int NCAL = sizeof(CAL) / sizeof(CAL[0]);

float chamberC(int raw, bool &approx) {
    approx = (raw > CAL[0].raw) || (raw < CAL[NCAL - 1].raw);
    int i = 0;
    if (raw <= CAL[NCAL - 1].raw) {
        i = NCAL - 2;
    } else {
        while (i < NCAL - 2 && raw < CAL[i + 1].raw) i++;
    }
    float r0 = CAL[i].raw, r1 = CAL[i + 1].raw;
    float c = CAL[i].c + (raw - r0) * (CAL[i + 1].c - CAL[i].c) / (r1 - r0);
    if (c < -40) c = -40;
    if (c > 350) c = 350;
    return c;
}
// ------------------------------------------------------

WebServer server(80);
Preferences prefs;
bool unitF = false;               // page shows F instead of C (saved)
int  pollPref = -1;               // chamber poll seconds the user picked (-1 = bus default)
String theme = "burner";          // saved page style

// ---- last status line from the bridge ----
struct {
    String state = "UNK", word = "0000", mode = "--", phase = "UNK";
    int level = 0, run = 0, pump = 0, glow = 0, chamber = -1, age = 999, poll = -1;
    int volts = 0, vage = 999, ecode = -1, bridge = 0;   // bridge 11 or 12
    unsigned long lastLineMs = 0;
} S;


bool linkStale()    { return S.lastLineMs == 0 || millis() - S.lastLineMs > 5000; }
bool heaterLive()   { return !linkStale() && S.age <= 5; }
bool chamberFresh() { return heaterLive() && S.chamber >= 0; }
float chamberNow()  { bool ap; return chamberFresh() ? chamberC(S.chamber, ap) : NAN; }

// ---- clock ----
bool clockOk() { return time(nullptr) > 1700000000; }
String stamp() {
    if (!clockOk()) return String(millis() / 1000) + "s";
    time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
    char b[12]; strftime(b, sizeof b, "%H:%M:%S", &t); return String(b);
}
int dayKey() {
    if (!clockOk()) return -1;
    time_t n = time(nullptr); struct tm t; localtime_r(&n, &t);
    return (t.tm_year + 1900) * 1000 + t.tm_yday;
}

#define LOG_N 20
String eventLog[LOG_N]; uint8_t logHead = 0;
void addLog(const String &s) {
    eventLog[logHead] = stamp() + " " + s;
    logHead = (logHead + 1) % LOG_N;
}

// ---- stats (saved in NVS) ----
uint32_t totRunS = 0, starts = 0, todayRunS = 0;
int todayKey = -1, lastLightS = -1;
bool statsDirty = false; unsigned long lastSaveMs = 0;
unsigned long runStartMs = 0;     // 0 = not running
bool lightPending = false;
bool runFromOff = false;          // we saw this run start from OFF/FAULT (not joined part-way after a restart)

void saveStats() {
    prefs.putUInt("totRun", totRunS);
    prefs.putUInt("starts", starts);
    prefs.putUInt("todRun", todayRunS);
    prefs.putInt("todKey", todayKey);
    prefs.putInt("light", lastLightS);
    statsDirty = false; lastSaveMs = millis();
}

// ---- room sensor + auto modes ----
uint32_t ledBootMs = 0, ledFlashMs = 0, lastLedMs = 0;   // LED timing (used by the auto code too)
RoomSensor room;
AutoCfg acfg;
AutoState ast;
AutoOut aout;
unsigned long offSinceMs = 0;      // when the heater last went OFF (0 = not seen)
unsigned long lastOffCmdMs = 0;    // last time WE sent OFF (page or auto) - tells our stops from the display's
unsigned long autoEchoUntil = 0;   // hide bus ACK LEVEL lines for auto level changes
unsigned long lastAutoMs = 0;
GuardState gst;                    // chamber guard
uint8_t gApplied = 0;              // levels the guard has added on top of the wanted level
uint8_t gLastLevel = 0;            // last level seen (spots hand changes)
unsigned long gOurUntil = 0;       // level changes before this are ours (auto / guard)
unsigned long gNextSendMs = 0;

float roomNow() { return room.fresh(120000) ? room.tC : NAN; }

// ---- history (histlog.h), kept in flash ----
HistLog hlog;
bool histFs = false;
const char *HIST_H = "/hist_h.bin", *HIST_D = "/hist_d.bin";
struct HistHdr { uint32_t magic; uint16_t recSize, count; };
// Load one ring from flash, or start it fresh if it's missing / from a different layout.
bool histLoad(const char *path, uint32_t magic, void *buf, uint16_t recSize, uint16_t count) {
    HistHdr want = { magic, recSize, count };
    File f = LittleFS.open(path, "r");
    if (f && f.size() == sizeof want + (size_t)recSize * count) {
        HistHdr h; f.read((uint8_t *)&h, sizeof h);
        if (!memcmp(&h, &want, sizeof h) && f.read((uint8_t *)buf, (size_t)recSize * count) == (size_t)recSize * count) { f.close(); return true; }
    }
    if (f) f.close();
    memset(buf, 0, (size_t)recSize * count);
    f = LittleFS.open(path, "w");
    if (!f) return false;
    f.write((const uint8_t *)&want, sizeof want);
    f.write((const uint8_t *)buf, (size_t)recSize * count);
    f.close();
    return false;
}
void histSave(bool isDay, uint16_t slot, const void *rec, uint16_t n) {
    if (!histFs) return;
    File f = LittleFS.open(isDay ? HIST_D : HIST_H, "r+");
    if (!f) return;
    f.seek(sizeof(HistHdr) + (size_t)slot * n);
    f.write((const uint8_t *)rec, n);
    f.close();
}
void histBegin() {
    hlog.clear();
    hlog.save = histSave;
    histFs = LittleFS.begin(true);                 // formats the data partition the first time
    if (!histFs) { addLog("HIST no flash storage - history won't survive a restart"); return; }
    bool h = histLoad(HIST_H, 0x31484C48UL, hlog.hours, sizeof(HourRec), HL_HOURS);   // "HLH1"
    bool d = histLoad(HIST_D, 0x31444C48UL, hlog.days, sizeof(DayRec), HL_DAYS);      // "HLD1"
    int n = 0; for (auto &x : hlog.days) if (x.day) n++;
    addLog(h && d ? "HIST loaded " + String(n) + " days" : String("HIST started fresh"));
}
// once a second
void tickHist() {
    HistIn in;
    if (clockOk()) {
        time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
        in.utc = (uint32_t)t;
        in.day = hlDay(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
    }
    bool live = heaterLive();
    in.running = live && S.state == "RUN";
    in.lit = in.running && S.phase == "RUN";
    in.level = S.level;
    in.roomC = roomNow();
    in.idleVolts = (live && S.state == "OFF" && S.volts > 0 && S.vage <= 60) ? S.volts : 0;
    hlog.tick(in, millis());
}

#include "diag.h"

// ---- master shut-off + keep-running check ----
SafeCfg scfg;
SafeState sst;
void saveSafe() { prefs.putBool("sMtOn", scfg.mtOn); prefs.putUShort("sMtMin", scfg.mtMin); }
void loadSafe() {
    SafeCfg d;
    scfg.mtOn = prefs.getBool("sMtOn", d.mtOn); scfg.mtMin = prefs.getUShort("sMtMin", d.mtMin);
    if (scfg.mtMin < 60 || scfg.mtMin > 720) scfg.mtMin = d.mtMin;
}
String tF(float c) {                // a temperature in the page's unit, for messages
    if (isnan(c)) return "--";
    return unitF ? String((int)lroundf(c * 9 / 5 + 32)) + "\xC2\xB0" "F" : String(c, 1) + "\xC2\xB0" "C";
}
String fmtDur(long s) {
    if (s < 60) return "under a minute";
    if (s < 3600) return String(s / 60) + " min";
    long m = (s % 3600) / 60;
    return String(s / 3600) + "h " + (m < 10 ? "0" : "") + String(m) + "m";
}

const char *modeName(uint8_t m) { return m == AM_THERMO ? "thermo" : (m == AM_FROST ? "frost" : "manual"); }

void saveAuto() {
    prefs.putUChar("aMode", acfg.mode);
    prefs.putFloat("aSp", acfg.sp);       prefs.putFloat("aEase", acfg.ease);
    prefs.putFloat("aOnB", acfg.onBand);  prefs.putFloat("aOffB", acfg.offBand);
    prefs.putBool("aShut", acfg.shutoff);
    prefs.putFloat("aFOn", acfg.frostOn); prefs.putFloat("aFOff", acfg.frostOff);
    prefs.putUChar("aFLvl", acfg.frostLvl);
    prefs.putUShort("aMinR", acfg.minRunMin); prefs.putUShort("aMinO", acfg.minOffMin);
    prefs.putBool("aThS", acfg.thStart);  prefs.putBool("aThF", acfg.thFull);  prefs.putBool("aThM", acfg.thSmooth);
    prefs.putBool("aFrB", acfg.frBoost);  prefs.putBool("aFrI", acfg.frIdle);
    prefs.putUShort("aCold", acfg.coldRestMin); prefs.putBool("aDisp", acfg.dispManual);
    prefs.putBool("aCG", acfg.chGuard); prefs.putFloat("aCF", acfg.chFloor);
}
void loadAuto() {
    AutoCfg d;
    acfg.mode = prefs.getUChar("aMode", d.mode);
    acfg.sp = prefs.getFloat("aSp", d.sp);           acfg.ease = prefs.getFloat("aEase", d.ease);
    acfg.onBand = prefs.getFloat("aOnB", d.onBand);  acfg.offBand = prefs.getFloat("aOffB", d.offBand);
    acfg.shutoff = prefs.getBool("aShut", d.shutoff);
    acfg.frostOn = prefs.getFloat("aFOn", d.frostOn); acfg.frostOff = prefs.getFloat("aFOff", d.frostOff);
    acfg.frostLvl = prefs.getUChar("aFLvl", d.frostLvl);
    acfg.minRunMin = prefs.getUShort("aMinR", d.minRunMin); acfg.minOffMin = prefs.getUShort("aMinO", d.minOffMin);
    acfg.thStart = prefs.getBool("aThS", d.thStart);  acfg.thFull = prefs.getBool("aThF", d.thFull);
    acfg.thSmooth = prefs.getBool("aThM", d.thSmooth);
    acfg.frBoost = prefs.getBool("aFrB", d.frBoost);  acfg.frIdle = prefs.getBool("aFrI", d.frIdle);
    acfg.coldRestMin = prefs.getUShort("aCold", d.coldRestMin); acfg.dispManual = prefs.getBool("aDisp", d.dispManual);
    acfg.chGuard = prefs.getBool("aCG", d.chGuard); acfg.chFloor = prefs.getFloat("aCF", d.chFloor);
    if (acfg.mode > AM_FROST) acfg.mode = AM_MANUAL;
    {   // a NaN saved by an older version would break the page's status data - put defaults back
        float *fs[] = { &acfg.sp, &acfg.ease, &acfg.onBand, &acfg.offBand, &acfg.frostOn, &acfg.frostOff, &acfg.chFloor };
        float  df[] = { d.sp, d.ease, d.onBand, d.offBand, d.frostOn, d.frostOff, d.chFloor };
        for (int i = 0; i < 7; i++) if (isnan(*fs[i]) || isinf(*fs[i])) *fs[i] = df[i];
    }
    if (acfg.sp > SP_MAX_C) acfg.sp = SP_MAX_C;                 // v1.8: nothing above 70 F
    if (acfg.frostOff > SP_MAX_C) acfg.frostOff = SP_MAX_C;
    if (acfg.frostOn > acfg.frostOff - 1.0f) acfg.frostOn = acfg.frostOff - 1.0f;
}
void setMode(uint8_t m, const String &why) {
    if (m == acfg.mode) return;
    acfg.mode = m; autoReset(ast); saveAuto();
    addLog(String("AUTO ") + modeName(m) + (why.length() ? " - " + why : ""));
}

// ---- alert watchers ----
unsigned long pumpSinceMs = 0, ignSinceMs = 0, lastLevelDownMs = 0, flameMs = 0;
float pumpStartC = NAN;
String flameTxt;
String alertTxt, alertLvl;
#define FO_N 12                    // 12 x 10 s = 2 min of chamber samples while burning
float foBuf[FO_N]; uint8_t foCount = 0, foHead = 0; unsigned long foLastMs = 0;

// ---- chamber history: 240 x 15 s = 1 hour ----
#define HIST_N 240
#define HIST_MS 15000UL
int16_t histC[HIST_N]; int16_t histR[HIST_N]; char histS[HIST_N]; uint16_t histHead = 0; unsigned long lastHistMs = 0;

// Phase from the flags, for a v1.1 bridge that doesn't send one (same rules as bridge v1.2).
String guessPhase() {
    bool f = S.run, p = S.pump, g = S.glow;
    if (S.state == "OFF")   return "IDLE";
    if (S.state == "FAULT") return "FAULT";
    if (S.state == "RUN") {
        if (!f && !p && !g) return "START";
        if ( f && !p && !g) return "FAN";
        if ( f && !p &&  g) return "PREHEAT";
        if ( p &&  g)       return "IGNITION";
        return "RUN";
    }
    if (S.state == "COOL") {
        if (p) return "SHUTDOWN";
        if (g) return "CLEARING";
        if (f) return "PURGE";
        return "COOL";
    }
    return "UNK";
}

void raiseFlame(const String &t) {
    if (flameMs) return;           // once per run
    flameMs = millis(); if (!flameMs) flameMs = 1;
    flameTxt = t;
    addLog("ALERT flame-out?");
    hlog.addFlame();
}

// Called after every status line: starts, light-up time, watchers.
void onUpdate(const String &prev, const String &prevPhase, int prevLevel) {
    unsigned long now = millis();
    if (S.state != prev) {
        if (S.state == "RUN") {
            runStartMs = now ? now : 1;
            if (prev == "OFF" || prev == "FAULT") {
                starts++; lightPending = true; saveStats(); hlog.addStart(); runFromOff = true;
            } else { lightPending = false; runFromOff = false; }   // ESP32 joined mid-run: start time unknown, already lit
            flameMs = 0; foCount = 0; foHead = 0; pumpSinceMs = 0; ignSinceMs = 0; pumpStartC = NAN;
        }
        if (prev == "RUN") { runStartMs = 0; lightPending = false; saveStats(); }
        if (S.state == "FAULT" && prev != "UNK") hlog.addFault();
        if (S.state == "OFF" && (prev == "COOL" || prev == "RUN" || prev == "FAULT")) offSinceMs = now ? now : 1;
        if (prev == "RUN" && S.state == "COOL" && acfg.mode != AM_MANUAL && acfg.dispManual && now - lastOffCmdMs > 15000UL)
            setMode(AM_MANUAL, "stopped at the heater");          // someone hit STOP on the OEM display
    }
    if (S.state != "RUN") { pumpSinceMs = 0; ignSinceMs = 0; foCount = 0; foHead = 0; return; }

    if (lightPending && S.phase == "RUN") {
        lastLightS = (now - runStartMs) / 1000; lightPending = false;
        addLog("EVT lit after " + String(lastLightS) + "s");
        hlog.addLight(lastLightS);
        saveStats();
    }
    if (prevPhase == "RUN" && (S.phase == "PREHEAT" || S.phase == "IGNITION"))
        raiseFlame("The glow plug came back on while it was running - the heater is re-lighting. The flame may have gone out (fuel?).");
    if (prevLevel > 0 && S.level < prevLevel) lastLevelDownMs = now;

    if (S.phase == "IGNITION") { if (!ignSinceMs) ignSinceMs = now ? now : 1; } else ignSinceMs = 0;
    if (S.pump) {
        if (!pumpSinceMs) { pumpSinceMs = now ? now : 1; pumpStartC = chamberNow(); }
        else if (isnan(pumpStartC)) pumpStartC = chamberNow();
    } else { pumpSinceMs = 0; pumpStartC = NAN; }
}

// S,<state>,<level>,<run>,<pump>,<glow>,<word>,<mode>,<chamber_raw>,<age_s>[,<poll_s>[,<volts>,<vage>,<phase>,<ecode>]]
void parseStatus(const String &l) {
    String f[16]; int n = 0, from = 0;
    while (n < 16) {
        int c = l.indexOf(',', from);
        if (c < 0) { f[n++] = l.substring(from); break; }
        f[n++] = l.substring(from, c);
        from = c + 1;
    }
    if (n < 10) return;
    String prev = S.state, prevPhase = S.phase; int prevLevel = S.level;
    // v1.9.1: one garbled reply on the bus shows up as UNK for a second or two (seen Nov 4:
    // RUN->UNK->RUN). Ride through it instead of treating it as a stop and a new start - that
    // reset the run timer and set off a false "not warming" alert. UNK for 20 s+ is real.
    static unsigned long unkSinceMs = 0;
    if (f[1] == "UNK" && S.state != "UNK" && S.lastLineMs) {
        if (!unkSinceMs) { unkSinceMs = millis() ? millis() : 1; DG.busBlips++; }
        if (millis() - unkSinceMs < 20000UL) { S.lastLineMs = millis(); return; }
    } else if (f[1] != "UNK") unkSinceMs = 0;
    S.state   = f[1];
    S.level   = f[2].toInt();
    S.run     = f[3].toInt();
    S.pump    = f[4].toInt();
    S.glow    = f[5].toInt();
    S.word    = f[6];
    S.mode    = f[7];
    S.chamber = f[8].toInt();
    S.age     = f[9].toInt();
    S.poll    = (n >= 11) ? f[10].toInt() : -1;     // -1 = v1.0 bridge, no poll field
    if (n >= 15) {
        S.volts = f[11].toInt(); S.vage = f[12].toInt();
        S.phase = f[13]; S.ecode = f[14].toInt(); S.bridge = 12;
    } else {
        S.volts = 0; S.vage = 999; S.phase = guessPhase(); S.ecode = -1; S.bridge = 11;
    }
    S.lastLineMs = millis();
    onUpdate(prev, prevPhase, prevLevel);
}

void pollBlueWire() {
    char line[120];
    if (BlueWire::readStatus(line)) parseStatus(line);
    while (BlueWire::readEvent(line)) {
        Serial.print("[blue wire] ");
        Serial.println(line);
        if (strncmp(line, "R,", 2) == 0) continue;
        const bool autoEcho = strncmp(line, "ACK LEVEL", 9) == 0 && millis() < autoEchoUntil;
        if (!autoEcho) addLog(line);
    }
}

// ---------------- once a second: stats, alerts, history ----------------
void tickStats() {
    static unsigned long lastMs = 0, acc = 0;
    unsigned long now = millis();
    if (!lastMs) lastMs = now;
    if (S.state == "RUN" && heaterLive()) acc += now - lastMs;
    lastMs = now;
    while (acc >= 1000) { acc -= 1000; totRunS++; todayRunS++; statsDirty = true; }
    int k = dayKey();
    if (k >= 0 && k != todayKey) {
        if (todayKey != -1) todayRunS = 0;     // new day (or first clock since a different day)
        todayKey = k; saveStats();
    }
    if (statsDirty && now - lastSaveMs > 300000UL) saveStats();   // every 5 min while running
}

String two(int n) { return n < 10 ? "0" + String(n) : String(n); }

String faultHint(int e) {
    if (e == 8) return " E-08 is the code seen when a heater ran out of fuel / couldn't light.";
    return " Check the heater manual for this code.";
}

void tickAlerts() {
    unsigned long now = millis();
    float c = chamberNow();

    // flame-out by temperature: sample every 10 s while burning
    if (S.state == "RUN" && S.phase == "RUN" && !isnan(c)) {
        if (now - foLastMs >= 10000) {
            foLastMs = now;
            foBuf[foHead] = c; foHead = (foHead + 1) % FO_N; if (foCount < FO_N) foCount++;
            if (foCount >= 6) {
                float mx = -1000; for (uint8_t i = 0; i < foCount; i++) mx = max(mx, foBuf[i]);
                bool quiet = lastLevelDownMs && now - lastLevelDownMs < LEVEL_DOWN_QUIET_S * 1000UL;
                if (mx - c >= FLAMEOUT_DROP_C && !quiet)
                    raiseFlame("Chamber dropped " + String((int)(mx - c)) + "\xC2\xB0" "C in under 2 min while running - the flame may have gone out.");
            }
        }
    } else if (S.state != "RUN") { foCount = 0; foHead = 0; }

    String txt, lvl, tag;
    bool live = heaterLive();
    if (sst.hiLatched && sst.hiTrips && !(live && S.state == "FAULT")) {
        lvl = "bad"; tag = "master shut-off";
        txt = "Master shut-off: the room hit " + tF(HI_LIMIT_C) + " so the heater was turned off. No starts until it's under " + tF(HI_CLEAR_C) + " (room " + tF(roomNow()) + " now).";
    } else if (sst.mtAsking && S.state == "RUN") {
        lvl = "warn"; tag = "keep running?";
        long left = safeLeftS(scfg, sst, millis());
        txt = "Running " + fmtDur(runStartMs ? (millis() - runStartMs) / 1000 : 0) + " in Manual. Still need it? It shuts off in " + String(left / 60 + 1) + " min unless you tap Keep running.";
    } else if (live && S.state == "FAULT") {
        lvl = "bad"; tag = "fault";
        txt = "Heater stopped on a fault";
        if (S.ecode >= 0) { txt += " E-" + two(S.ecode) + " (tentative)."; txt += faultHint(S.ecode); tag += " E-" + two(S.ecode); }
        else txt += ".";
        txt += S.bridge >= 12 ? " Tap START to clear it and restart." : " Clear it on the heater's own display.";
    } else if (flameMs && S.state == "RUN" && now - flameMs < 600000UL) {
        lvl = "bad"; tag = "flame-out?"; txt = flameTxt;
    } else if (live && S.state == "RUN" && ignSinceMs && now - ignSinceMs > IGN_PHASE_MAX_S * 1000UL) {
        lvl = "warn"; tag = "slow ignition";
        txt = "Still trying to light after " + String((now - ignSinceMs) / 60000) + " min (glow plug + fuel on). Check the fuel supply.";
    } else if (live && S.state == "RUN" && runFromOff && pumpSinceMs && now - pumpSinceMs > IGN_PUMP_S * 1000UL &&
               !isnan(c) && !isnan(pumpStartC) && c < pumpStartC + IGN_RISE_C) {
        lvl = "warn"; tag = "not warming";
        txt = "Fuel pump on " + String((now - pumpSinceMs) / 60000) + " min but the chamber has only warmed " +
              String((int)(c - pumpStartC)) + "\xC2\xB0" "C. It may not have lit.";
    } else if (live && S.state == "RUN" && acfg.chGuard && gst.alert) {
        lvl = "warn"; tag = "cold chamber";
        txt = String(gst.msg) + " for 10 min. Check fuel, air intake and exhaust - or lower the chamber floor.";
    } else if (acfg.mode != AM_MANUAL && aout.alert) {
        lvl = aout.alert >= 2 ? "bad" : "warn"; tag = "auto"; txt = String(aout.msg);
        if (String(aout.msg).startsWith("No room")) txt = "No reading from the room sensor - auto mode is paused (no starts). Check the DHT22 wiring.";
    } else if (acfg.mode == AM_MANUAL && S.state == "OFF" && !isnan(roomNow()) && roomNow() <= 1.0f) {
        lvl = "warn"; tag = "near freezing";
        float r = roomNow();
        txt = "The room is at " + String(unitF ? r * 9 / 5 + 32 : r, 0) + (unitF ? "\xC2\xB0" "F" : "\xC2\xB0" "C") + " and frost guard is off.";
    } else if (live && S.state == "OFF" && S.volts > 0 && S.volts <= VOLTS_WARN) {
        lvl = S.volts <= VOLTS_BAD ? "bad" : "warn"; tag = "low battery";
        txt = "Supply is " + String(S.volts) + " V at idle - the battery is getting low.";
    } else if (live && S.state == "OFF" && S.volts >= VOLTS_HIGH) {
        lvl = "warn"; tag = "high volts";
        txt = "Supply is " + String(S.volts) + " V at idle - check the charger.";
    }
    static String lastTag;
    if (tag != lastTag) {                  // log each new alert once, not every time its minutes tick
        if (tag.length() && tag != "flame-out?") addLog("ALERT " + tag);   // flame-out logs when raised
        lastTag = tag;
    }
    alertTxt = txt; alertLvl = lvl;
}

void tickHistory() {
    if (millis() - lastHistMs < HIST_MS) return;
    lastHistMs = millis();
    float c = chamberNow();
    histC[histHead] = isnan(c) ? INT16_MIN : (int16_t)lroundf(c);
    float r = roomNow();
    histR[histHead] = isnan(r) ? INT16_MIN : (int16_t)lroundf(r * 10);
    char s = '-';
    if (heaterLive()) s = S.state == "RUN" ? 'R' : S.state == "COOL" ? 'C' : S.state == "FAULT" ? 'F' : S.state == "OFF" ? 'O' : '-';
    histS[histHead] = s;
    histHead = (histHead + 1) % HIST_N;
}

// ---------------- auto modes, every 5 s ----------------
void tickAuto() {
    if (millis() - lastAutoMs < 5000) return;
    lastAutoMs = millis();
    unsigned long now = millis();
    bool lit = heaterLive() && S.state == "RUN" && S.phase == "RUN";

    // level changed by hand (page button or the heater's display)? that becomes the new base
    if (S.level != gLastLevel) {
        if (gLastLevel && now > gOurUntil) {
            if (gst.bump) addLog("GUARD reset - level changed by hand");
            guardExternal(gst, now); gApplied = 0;
        }
        gLastLevel = S.level;
    }
    int logical = S.level - gApplied;            // the level without the guard's extra notches
    if (logical < 1) logical = S.level;

    AutoIn in;
    in.now = now ? now : 1;
    in.heaterLive = heaterLive();
    in.st = S.state == "OFF" ? 'O' : S.state == "RUN" ? 'R' : S.state == "COOL" ? 'C' : S.state == "FAULT" ? 'F' : 'U';
    in.lit = (S.phase == "RUN");
    in.level = logical;
    in.roomC = roomNow();
    in.runStartMs = runStartMs;
    in.offSinceMs = offSinceMs;
    in.noStart = sst.hiLatched;
    autoStep(acfg, in, ast, aout, unitF);
    if (aout.log[0]) addLog(aout.log);
    switch (aout.cmd) {
        case AC_ON:
            if (BlueWire::command("ON")) ledFlashMs = now ? now : 1;
            else addLog("ERR ON: blue-wire command queue unavailable");
            break;
        case AC_OFF:
            if (BlueWire::command("OFF")) {
                lastOffCmdMs = now;
                ledFlashMs = now ? now : 1;
            } else addLog("ERR OFF: blue-wire command queue unavailable");
            break;
        default: break;
    }

    // chamber guard - every mode (it also sends the thermostat's level changes)
    uint8_t base = (aout.cmd == AC_LEVEL) ? aout.level : (uint8_t)logical;
    GuardIn gi;
    gi.now = in.now; gi.on = acfg.chGuard; gi.floorC = acfg.chFloor;
    gi.lit = lit; gi.chamberC = chamberNow(); gi.base = base;
    uint8_t want = guardStep(gi, gst, unitF);
    if (gst.log[0]) addLog(gst.log);
    if (!lit) gApplied = 0;
    else if (S.level >= 1) {
        if (want != S.level && now >= gNextSendMs && BlueWire::command("LEVEL " + String(want))) {
            autoEchoUntil = now + 30000UL; gOurUntil = now + 30000UL; gNextSendMs = now + 30000UL;
        }
        gApplied = want > base ? want - base : 0;
    }
    if (aout.toManual) setMode(AM_MANUAL, "");
}

// ---------------- master shut-off + keep-running check, every second ----------------
void tickSafety() {
    SafeIn in;
    in.now = millis();
    in.st = !heaterLive() ? 'U' : S.state == "OFF" ? 'O' : S.state == "RUN" ? 'R' : S.state == "COOL" ? 'C' : S.state == "FAULT" ? 'F' : 'U';
    in.roomC = roomNow();
    in.manual = acfg.mode == AM_MANUAL;
    SafeOut o;
    static bool hadTrip = false;
    safeStep(scfg, in, sst, o);
    if (o.sendOff) {
        if (BlueWire::command("OFF")) {
            lastOffCmdMs = millis();
            ledFlashMs = millis(); if (!ledFlashMs) ledFlashMs = 1;
            addLog(o.evt == SE_MT_STOP ? "SAFE > OFF (no answer to keep-running)" : "SAFE > OFF (room over " + tF(HI_LIMIT_C) + ")");
        } else addLog("ERR safety OFF: blue-wire command queue unavailable");
    }
    switch (o.evt) {
        case SE_HI_TRIP:  hadTrip = true; hlog.addHiTrip(); addLog("SAFE master shut-off: room " + tF(in.roomC)); break;
        case SE_HI_AGAIN: hlog.addHiTrip(); addLog("SAFE stopped a restart at the display: room " + tF(in.roomC)); break;
        case SE_HI_CLEAR: if (hadTrip) { addLog("SAFE room under " + tF(HI_CLEAR_C) + " - starts allowed"); hadTrip = false; } break;
        case SE_MT_ASK:   addLog("SAFE keep-running? asked"); break;
        case SE_MT_STOP:  hlog.addMtStop(); break;
        default: break;
    }
}

// ---------------- RGB LED ----------------
uint8_t ledR = 1, ledG = 1, ledB = 1;

void ledTick(bool force = false) {
    uint32_t now = millis();
    if (!force && now - lastLedMs < 20) return;          // 50 frames a second
    lastLedMs = now;
    uint8_t r = 0, g = 0, b = 0;
    if (LED_MAX > 0) {
        LedIn in;
        in.wifi   = (WiFi.getMode() == WIFI_AP) ? 2 : (WiFi.status() == WL_CONNECTED ? 1 : 0);
        in.link   = !linkStale();
        in.heater = heaterLive();
        in.st     = S.state == "OFF" ? 'O' : S.state == "RUN" ? 'R' : S.state == "COOL" ? 'C' : S.state == "FAULT" ? 'F' : 'U';
        in.phase  = ledPhaseFromName(S.phase.c_str());
        in.level  = S.level;
        in.chamberC = chamberNow();
        in.alert  = alertLvl == "bad" ? 2 : (alertLvl == "warn" ? 1 : 0);
        in.flashMs = ledFlashMs;
        in.bootMs  = ledBootMs;
        in.armed   = acfg.mode != AM_MANUAL;
        ledToBytes(ledCompute(in, now), LED_MAX, r, g, b);
    }
    if (r != ledR || g != ledG || b != ledB) { rgbLedWrite(RGB_PIN, r, g, b); ledR = r; ledG = g; ledB = b; }
}

// ---------------- WEB ----------------
void sendGz(const char *type, const uint8_t *data, size_t len) {     // pages are stored gzipped (build_pages.py)
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, type, (PGM_P)data, len);
}
void sendPage(const String &t) {
    if      (t == "lcd")  sendGz("text/html", PAGE_LCD, PAGE_LCD_LEN);
    else if (t == "dash") sendGz("text/html", PAGE_DASH, PAGE_DASH_LEN);
    else                  sendGz("text/html", PAGE_BURNER, PAGE_BURNER_LEN);
}
void handleRoot() { diagWebHit(); sendPage(theme); }
void sendIcon(const uint8_t *data, size_t len) {                     // v1.9.4: home-screen icon
    server.sendHeader("Cache-Control", "max-age=604800");
    server.send_P(200, "image/png", (PGM_P)data, len);
}
void handleManifest() {
    server.sendHeader("Cache-Control", "max-age=86400");
    sendGz("application/manifest+json", MANIFEST_JSON, MANIFEST_JSON_LEN);
}
void handleCore() {
    server.sendHeader("Cache-Control", "max-age=86400");
    sendGz("application/javascript", CORE_JS, CORE_JS_LEN);
}

String jsonEsc(const String &in) {        // keep JSON valid no matter what ends up in a string
    String s; s.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '\\') s += '/';
        else if (c == '"') s += '\'';
        else if ((uint8_t)c < 0x20) s += ' ';
        else s += c;                          // bytes 0x80+ are UTF-8 (the degree sign) - keep
    }
    return s;
}

void handleStatus() {
    diagWebHit();
    bool stale = linkStale();
    String lg;
    for (uint8_t i = 0; i < LOG_N; i++) {
        const String &e = eventLog[(logHead + LOG_N - 1 - i) % LOG_N];
        if (e.length()) { lg += jsonEsc(e); lg += "\\n"; }
    }
    String j; j.reserve(4000);
    j += "{\"fw\":195,\"state\":\""; j += jsonEsc(S.state); j += "\"";
    j += ",\"phase\":\"";  j += jsonEsc(S.phase); j += "\"";
    j += ",\"level\":";    j += S.level;
    j += ",\"run\":";      j += S.run;
    j += ",\"pump\":";     j += S.pump;
    j += ",\"glow\":";     j += S.glow;
    j += ",\"chamber\":";  j += S.chamber;
    j += ",\"chamberC\":";
    if (S.chamber >= 0) {
        bool ap; float c = chamberC(S.chamber, ap);
        j += String(c, 1);
        j += ",\"approx\":"; j += ap ? "true" : "false";
    } else {
        j += "null,\"approx\":false";
    }
    j += ",\"volts\":";    j += S.volts;
    j += ",\"vage\":";     j += S.vage;
    j += ",\"ecode\":";    j += S.ecode;
    j += ",\"bridge\":";   j += S.bridge;
    j += ",\"junk\":";     j += BlueWire::badFrames();
    j += ",\"unit\":\"";   j += unitF ? "F" : "C"; j += "\"";
    j += ",\"theme\":\"";  j += theme; j += "\"";
    j += ",\"poll\":";     j += S.poll;
    j += ",\"age\":";      j += S.age;
    j += ",\"stale\":";    j += stale ? "true" : "false";
    j += ",\"word\":\"";   j += jsonEsc(S.word); j += "\"";
    j += ",\"mode\":\"";   j += jsonEsc(S.mode); j += "\"";
    j += ",\"alert\":\"";  j += jsonEsc(alertTxt); j += "\"";
    j += ",\"alertLvl\":\""; j += alertLvl; j += "\"";
    j += ",\"stats\":{\"today\":"; j += todayRunS;
    j += ",\"total\":";    j += totRunS;
    j += ",\"starts\":";   j += starts;
    j += ",\"light\":";    j += lastLightS;
    j += ",\"runFor\":";   j += (runStartMs && S.state == "RUN") ? (long)((millis() - runStartMs) / 1000) : -1L;
    j += ",\"clock\":";    j += clockOk() ? "true" : "false";
    j += "}";
    j += ",\"room\":{\"t\":";
    float rt = roomNow();
    if (isnan(rt)) j += "null,\"rh\":null"; else { j += String(rt, 1); j += ",\"rh\":"; j += String(room.rh, 0); }
    j += ",\"age\":";   j += room.okMs ? (long)((millis() - room.okMs) / 1000) : -1L;
    j += ",\"reads\":"; j += room.reads;
    j += ",\"fails\":"; j += room.fails;
    j += "}";
    j += ",\"auto\":{\"mode\":\""; j += modeName(acfg.mode); j += "\"";
    j += ",\"sp\":";       j += String(acfg.sp, 2);
    j += ",\"ease\":";     j += String(acfg.ease, 2);
    j += ",\"onBand\":";   j += String(acfg.onBand, 2);
    j += ",\"offBand\":";  j += String(acfg.offBand, 2);
    j += ",\"shutoff\":";  j += acfg.shutoff ? "true" : "false";
    j += ",\"frostOn\":";  j += String(acfg.frostOn, 2);
    j += ",\"frostOff\":"; j += String(acfg.frostOff, 2);
    j += ",\"frostLvl\":"; j += acfg.frostLvl;
    j += ",\"minRun\":";   j += acfg.minRunMin;
    j += ",\"minOff\":";   j += acfg.minOffMin;
    j += ",\"thStart\":";  j += acfg.thStart ? "true" : "false";
    j += ",\"thFull\":";   j += acfg.thFull ? "true" : "false";
    j += ",\"thSmooth\":"; j += acfg.thSmooth ? "true" : "false";
    j += ",\"frBoost\":";  j += acfg.frBoost ? "true" : "false";
    j += ",\"frIdle\":";   j += acfg.frIdle ? "true" : "false";
    j += ",\"coldRest\":"; j += acfg.coldRestMin;
    j += ",\"dispManual\":"; j += acfg.dispManual ? "true" : "false";
    j += ",\"chGuard\":"; j += acfg.chGuard ? "true" : "false";
    j += ",\"chFloor\":"; j += String(acfg.chFloor, 1);
    j += ",\"gBump\":";   j += gApplied;
    j += ",\"gMsg\":\""; j += jsonEsc(acfg.chGuard ? String(gst.msg) : String("")); j += "\"";
    j += ",\"coldNight\":"; j += ast.coldNight ? "true" : "false";
    j += ",\"target\":";   j += aout.target;
    j += ",\"need\":";     if (acfg.mode == AM_THERMO && acfg.thSmooth && !isnan(ast.need)) j += String(ast.need, 1); else j += "null";
    j += ",\"msg\":\"";   j += jsonEsc(acfg.mode == AM_MANUAL ? String("") : String(aout.msg)); j += "\"}";
    j += ",\"safe\":{\"hi\":";  j += sst.hiLatched ? "true" : "false";
    j += ",\"trips\":";     j += sst.hiTrips;
    j += ",\"limit\":";     j += String(HI_LIMIT_C, 1);
    j += ",\"clear\":";     j += String(HI_CLEAR_C, 1);
    j += ",\"spMax\":";     j += String(SP_MAX_C, 2);
    j += ",\"mtOn\":";      j += scfg.mtOn ? "true" : "false";
    j += ",\"mtMin\":";     j += scfg.mtMin;
    j += ",\"left\":";      j += safeLeftS(scfg, sst, millis());
    j += ",\"ask\":";       j += sst.mtAsking ? "true" : "false";
    j += "}";
    j += ",\"ip\":\"";     j += (WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString() : WiFi.localIP().toString()); j += "\"";
    j += ",\"log\":\"";    j += lg; j += "\"}";
    server.send(200, "application/json", j);
}

void handleHistory() {
    String j; j.reserve(HIST_N * 11 + 320);
    j += "{\"step\":15,\"c\":[";
    for (uint16_t i = 0; i < HIST_N; i++) {
        int16_t v = histC[(histHead + i) % HIST_N];
        if (i) j += ',';
        if (v == INT16_MIN) j += "null"; else j += v;
    }
    j += "],\"r\":[";
    for (uint16_t i = 0; i < HIST_N; i++) {
        int16_t v = histR[(histHead + i) % HIST_N];
        if (i) j += ',';
        if (v == INT16_MIN) j += "null"; else j += String(v / 10.0f, 1);
    }
    j += "],\"s\":\"";
    for (uint16_t i = 0; i < HIST_N; i++) j += histS[(histHead + i) % HIST_N];
    j += "\"}";
    server.send(200, "application/json", j);
}

// /api/hist - every hour of the last 7 days and every day of the last year that has a record,
// oldest first. hours: [hr, runS, litS, avgLevel x10, starts, room avg tenths C | null]
// days: [day, runS, starts, avg light-up s | null, room low, room high, room avg (tenths C | null),
//        lowest idle volts | null, [s at L1..L6], master shut-offs, keep-running stops, faults, flame-outs]
// Streamed in pieces - the whole year is ~30 KB.
void handleHist() {
    diagWebHit();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    uint32_t hn = hlog.hourNow(), dn = hlog.dayNow();
    String b; b.reserve(1800);
    b = "{\"v\":1,\"clock\":"; b += clockOk() ? "true" : "false";
    b += ",\"hrNow\":"; b += hn; b += ",\"dayNow\":"; b += dn; b += ",\"hours\":[";
    bool first = true;
    auto t10 = [](int32_t sum, uint32_t n) -> String { return n ? String((int)lroundf((float)sum / n)) : String("null"); };
    auto flush = [&]() { if (b.length() > 1400) { server.sendContent(b); b = ""; esp_task_wdt_reset(); } };   // a slow phone can't trip the watchdog
    if (hn) for (uint32_t k = hn - (HL_HOURS - 1); k <= hn; k++) {
        const HourRec &h = hlog.hours[k % HL_HOURS];
        if (h.hr != k) continue;
        if (!first) b += ','; first = false;
        b += '['; b += h.hr; b += ','; b += h.runS; b += ','; b += h.litS; b += ',';
        b += h.litS ? (int)lroundf(h.lvlSum * 10.0f / h.litS) : 0; b += ','; b += h.starts; b += ','; b += t10(h.rSum, h.rN); b += ']';
        flush();
    }
    b += "],\"days\":["; first = true;
    if (dn) for (uint32_t k = dn - (HL_DAYS - 1); k <= dn; k++) {
        const DayRec &d = hlog.days[k % HL_DAYS];
        if (d.day != k) continue;
        if (!first) b += ','; first = false;
        b += '['; b += d.day; b += ','; b += d.runS; b += ','; b += d.starts; b += ',';
        b += d.lightN ? String(d.lightSum / d.lightN) : String("null"); b += ',';
        b += d.rMin == HL_NONE ? String("null") : String(d.rMin); b += ',';
        b += d.rMax == HL_NONE ? String("null") : String(d.rMax); b += ',';
        b += t10(d.rSum, d.rN); b += ',';
        b += d.vMin ? String(d.vMin) : String("null"); b += ",[";
        for (int i = 0; i < 6; i++) { if (i) b += ','; b += d.lvlS[i]; }
        b += "],"; b += d.hiTrips; b += ','; b += d.mtStops; b += ','; b += d.faults; b += ','; b += d.flames; b += ']';
        flush();
    }
    b += "]}";
    server.sendContent(b);
    server.sendContent("");                         // end of the chunked reply
}

void handleCmd() {
    diagWebHit();
    String c = server.arg("c"); c.toLowerCase();
    String out;
    if      (c == "on")    out = "ON";
    else if (c == "off")   out = "OFF";
    else if (c == "up")    out = "UP";
    else if (c == "down")  out = "DOWN";
    else if (c == "level") {
        int n = server.arg("n").toInt();
        if (n < 1 || n > 6) { server.send(400, "text/plain", "level 1-6"); return; }
        out = "LEVEL " + String(n);
    }
    else if (c == "chamber") {
        int n = server.arg("n").toInt();
        if (n < 0 || n > 60) { server.send(400, "text/plain", "chamber 0-60"); return; }
        out = "CHAMBER " + String(n);
    }
    else { server.send(400, "text/plain", "unknown"); return; }
    if (c == "on" && sst.hiLatched && S.state != "RUN") {
        server.send(409, "text/plain", "Master shut-off: room over " + tF(HI_LIMIT_C) + " - no starts until it's under " + tF(HI_CLEAR_C));
        return;
    }
    if (!BlueWire::command(out)) {
        server.send(503, "text/plain", "blue-wire command queue unavailable");
        return;
    }
    if (c == "chamber") {
        pollPref = server.arg("n").toInt();
        prefs.putInt("poll", pollPref);
    }
    if (c == "off") lastOffCmdMs = millis();
    if (c == "level" || c == "up" || c == "down") {
        if (gst.bump) addLog("GUARD reset - level changed by hand");
        guardExternal(gst, millis()); gApplied = 0; gOurUntil = 0;
    }
    // STOP or a level change from the page takes over; START leaves the thermostat / frost guard in charge
    if (c != "chamber" && c != "on" && acfg.mode != AM_MANUAL) setMode(AM_MANUAL, "manual control from the page");
    addLog("> " + out);
    ledFlashMs = millis(); if (!ledFlashMs) ledFlashMs = 1;
    server.send(200, "text/plain", "sent " + out);
}

// /api/auto?mode=manual|thermo|frost  and/or any of: sp ease onBand offBand frostOn frostOff frostLvl minRun minOff coldRest mtMin
//   chFloor (C)   toggles (1/0): shutoff thStart thFull thSmooth frBoost frIdle dispManual chGuard mtOn
// temperatures in C, bands in C degrees
void handleAuto() {
    String err;
    auto num = [&](const char *k, float lo, float hi, float &dst) {
        if (!server.hasArg(k)) return;
        float v = server.arg(k).toFloat();
        if (isnan(v) || v < lo || v > hi) { err += String(k) + " out of range; "; return; }
        dst = v;
    };
    AutoCfg c = acfg;
    num("sp", 0, SP_MAX_C, c.sp);  num("ease", 0.5f, 10, c.ease);
    num("onBand", 0.3f, 5, c.onBand);  num("offBand", 0.3f, 6, c.offBand);
    num("frostOn", -20, 15, c.frostOn); num("frostOff", -15, SP_MAX_C, c.frostOff);
    float f;
    f = c.frostLvl;  num("frostLvl", 1, 6, f);  c.frostLvl = (uint8_t)lroundf(f);
    f = c.minRunMin; num("minRun", 10, 180, f); c.minRunMin = (uint16_t)lroundf(f);
    f = c.minOffMin; num("minOff", 5, 120, f);  c.minOffMin = (uint16_t)lroundf(f);
    f = c.coldRestMin; num("coldRest", 30, 360, f); c.coldRestMin = (uint16_t)lroundf(f);
    num("chFloor", 60, 200, c.chFloor);
    auto flag = [&](const char *k, bool &dst) { if (server.hasArg(k)) dst = server.arg(k) == "1" || server.arg(k) == "true"; };
    flag("shutoff", c.shutoff); flag("thStart", c.thStart); flag("thFull", c.thFull); flag("thSmooth", c.thSmooth);
    flag("frBoost", c.frBoost); flag("frIdle", c.frIdle); flag("dispManual", c.dispManual);
    flag("chGuard", c.chGuard);
    if (c.frostOff < c.frostOn + 1.0f) err += "stop temperature must be at least 1 C above start; ";
    SafeCfg sc = scfg;
    f = sc.mtMin; num("mtMin", 60, 720, f); sc.mtMin = (uint16_t)lroundf(f);
    flag("mtOn", sc.mtOn);
    if (err.length()) { server.send(400, "text/plain", err); return; }
    if (sc.mtOn != scfg.mtOn || sc.mtMin != scfg.mtMin) { scfg = sc; saveSafe(); }
    uint8_t m = acfg.mode;
    if (server.hasArg("mode")) {
        String s = server.arg("mode");
        if (s == "manual") m = AM_MANUAL; else if (s == "thermo") m = AM_THERMO; else if (s == "frost") m = AM_FROST;
        else { server.send(400, "text/plain", "mode=manual|thermo|frost"); return; }
    }
    c.mode = acfg.mode;
    acfg = c;
    saveAuto();
    setMode(m, "from the page");
    lastAutoMs = 0;                          // act on it right away
    server.send(200, "text/plain", modeName(acfg.mode));
}

void handleKeep() {
    if (!safeKeep(sst, millis())) { server.send(409, "text/plain", "nothing to keep - no Manual run being timed"); return; }
    addLog("SAFE keep running (page)");
    server.send(200, "text/plain", "kept");
}

void handleUnit() {
    String u = server.arg("u"); u.toUpperCase();
    if (u != "C" && u != "F") { server.send(400, "text/plain", "u=C or u=F"); return; }
    unitF = (u == "F");
    prefs.putBool("unitF", unitF);
    server.send(200, "text/plain", unitF ? "F" : "C");
}

void handleTheme() {
    String t = server.arg("t"); t.toLowerCase();
    if (t != "burner" && t != "lcd" && t != "dash") { server.send(400, "text/plain", "t=burner|lcd|dash"); return; }
    theme = t;
    prefs.putString("theme", theme);
    server.send(200, "text/plain", theme);
}

// ---------------- SETUP ----------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nHEATER WEB v" FW_VER);
    ledBootMs = millis();

    for (uint16_t i = 0; i < HIST_N; i++) { histC[i] = INT16_MIN; histR[i] = INT16_MIN; histS[i] = '-'; }

    prefs.begin("heater", false);
    unitF      = prefs.getBool("unitF", false);
    pollPref   = prefs.getInt("poll", -1);
    if (!BlueWire::begin(pollPref)) {
        Serial.println("ERR could not initialize blue-wire bus on GPIO27");
        addLog("ERR blue-wire bus initialization failed");
    }
    ledTick(true);
    theme      = prefs.getString("theme", "burner");
    totRunS    = prefs.getUInt("totRun", 0);
    starts     = prefs.getUInt("starts", 0);
    todayRunS  = prefs.getUInt("todRun", 0);
    todayKey   = prefs.getInt("todKey", -1);
    lastLightS = prefs.getInt("light", -1);
    loadAuto();
    loadSafe();
    histBegin();
    diagBegin();
    room.begin(DHT_PIN);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                          // v1.9.3: no modem sleep - phones couldn't find it (missed ARP)
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Joining "); Serial.print(WIFI_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        for (int i = 0; i < 25; i++) { delay(20); ledTick(); }       // keep the LED animating
        Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(); Serial.print("IP: "); Serial.println(WiFi.localIP());
        configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
        addLog("WiFi " + WiFi.localIP().toString());
    } else {
        Serial.println("\nWiFi failed - starting hotspot");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
        addLog("AP mode " + WiFi.softAPIP().toString());
    }
    if (MDNS.begin(HOSTNAME)) Serial.println("mDNS: http://heater.local");

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.onStart(diagOtaStart);
    ArduinoOTA.onProgress([](unsigned int, unsigned int) { diagOtaTick(); });
    ArduinoOTA.begin();

    server.on("/", handleRoot);
    server.on("/burner", []() { sendPage("burner"); });
    server.on("/lcd",    []() { sendPage("lcd"); });
    server.on("/dash",   []() { sendPage("dash"); });
    server.on("/history", []() { sendGz("text/html", PAGE_HIST, PAGE_HIST_LEN); });
    server.on("/api/hist", handleHist);
    server.on("/api/ping", handlePing);
    server.on("/diag", handleDiag);
    server.on("/core.js", handleCore);
    server.on("/manifest.json", handleManifest);
    server.on("/icon-192.png", []() { sendIcon(ICON_192, ICON_192_LEN); });
    server.on("/icon-512.png", []() { sendIcon(ICON_512, ICON_512_LEN); });
    server.on("/apple-touch-icon.png", []() { sendIcon(ICON_192, ICON_192_LEN); });
    server.on("/favicon.ico", []() { sendIcon(ICON_48, ICON_48_LEN); });
    server.on("/api/status", handleStatus);
    server.on("/api/history", handleHistory);
    server.on("/api/cmd", handleCmd);
    server.on("/api/unit", handleUnit);
    server.on("/api/theme", handleTheme);
    server.on("/api/auto", handleAuto);
    server.on("/api/keep", handleKeep);
    server.begin();

    diagWatchdogOn();                              // last: from here a stuck loop restarts itself
}

void loop() {
    diagLoopStart();
    diagStage(DS_WEB);  server.handleClient();
    diagStage(DS_OTA);  ArduinoOTA.handle();
    diagStage(DS_BUS);  pollBlueWire();
    diagStage(DS_ROOM); room.tick();
    diagStage(DS_LED);  ledTick();
    static unsigned long lastTick = 0;
    if (millis() - lastTick >= 1000) {
        lastTick = millis();
        diagStage(DS_STATS);  tickStats();
        diagStage(DS_SAFETY); tickSafety();
        diagStage(DS_HIST);   tickHist();
        diagStage(DS_ALERTS); tickAlerts();
        diagStage(DS_AUTO);   tickAuto();
        diagStage(DS_DIAG);   diagTick();
    }
    diagStage(DS_GRAPH); tickHistory();
    diagLoopEnd();
}
