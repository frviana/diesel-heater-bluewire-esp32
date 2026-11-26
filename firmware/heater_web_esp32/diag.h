// ============================================================================
//  DIAGNOSTICS ("black box") - v1.9.1
//  Catches the controller dropping off the network and records why, in flash,
//  so it can be read later at http://<ip>/diag without a USB cable.
//
//  On every start it writes one line: why it restarted (power-on, crash,
//  watchdog, brown-out, our own restart...), how long it had been up, how low
//  free memory got, the WiFi signal, when the web page last got a request,
//  and WHICH PART OF THE PROGRAM it was in when it went down. After a crash it
//  also writes the crash address list (decodable against the build's .elf).
//
//  While running it watches itself:
//   - LOOP WATCHDOG: the main loop stuck for 20 s -> restart (and say where).
//   - WEB SELF-CHECK: every 3 min it asks its own web server for /api/ping (via 127.0.0.1).
//     Three misses in a row -> logged and restarted. Only arms after the
//     first check passes, so it can never restart-loop on a network that
//     doesn't allow it.
//   - WIFI: dropped -> reconnects every 30 s, restarts after 10 min. On the
//     HeaterCtl hotspot it retries your WiFi every 5 min and rejoins.
//   - Memory / signal / slow-loop warnings, and one summary line a day.
//
//  Included from the sketch after the web server, log and clock helpers.
// ============================================================================
#pragma once
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <vector>

enum DiagStage : uint8_t { DS_NONE, DS_SETUP, DS_WEB, DS_OTA, DS_BUS, DS_ROOM, DS_LED, DS_STATS, DS_SAFETY,
                           DS_HIST, DS_ALERTS, DS_AUTO, DS_GRAPH, DS_DIAG, DS_IDLE };
static const char *DS_NAME[] = { "?", "start-up", "web page", "OTA update", "blue-wire bus", "room sensor", "LED",
                                 "stats", "safety", "history", "alerts", "auto modes", "graph", "self-check", "idle" };
enum DiagWhy : uint8_t { DW_NONE, DW_WEB, DW_WIFI, DW_OTA, DW_HOTSPOT };
static const char *DW_NAME[] = { "", "web self-check failed 3x", "WiFi gone 10 min", "OTA update", "rejoined home WiFi" };

// Survives crashes, watchdog and software restarts (not a power cut).
struct RtcDiag {
    uint32_t magic, upS, heapMin, lastWebS, blockMin, loopMaxMs;
    int8_t   rssi; uint8_t stage, why, pad;
    uint16_t boots, wifiDrops;
};
RTC_NOINIT_ATTR RtcDiag rtcD;
#define RTCD_MAGIC 0x44494147UL

const char *DIAG_LOG = "/diag.log";
#define DIAG_LOG_MAX 16384           // trim to the newest half past this

struct {
    uint32_t loopStart = 0, loopMax = 0, slowLogMs = 0;
    uint8_t  slowStage = 0;
    // web
    uint32_t webHits = 0, lastWebMs = 0, busBlips = 0;
    // self-check
    WiFiClient cli; uint8_t scState = 0; uint32_t scAt = 0, scNext = 60000; uint8_t scFails = 0; bool scArmed = false; uint32_t scOk = 0;
    // wifi
    uint32_t wifiLostMs = 0, wifiRetryMs = 0, apTryMs = 0; uint16_t drops = 0; int8_t rssiMin = 0;
    // memory warnings
    bool lowHeapLogged = false, lowBlockLogged = false;
    uint32_t dayStartS = 0; int lastDay = -1;
    String bootLine;
} DG;

static inline void diagStage(uint8_t s) { rtcD.stage = s; }

String diagStamp() {
    if (clockOk()) { time_t n = time(nullptr); struct tm t; localtime_r(&n, &t); char b[20]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", &t); return String(b); }
    return "boot+" + String(millis() / 1000) + "s";
}
String diagDur(uint32_t s) {
    if (s < 120) return String(s) + " s";
    if (s < 7200) return String(s / 60) + " min";
    if (s < 172800) return String(s / 3600) + "h " + String((s % 3600) / 60) + "m";
    return String(s / 86400) + "d " + String((s % 86400) / 3600) + "h";
}

void diagWrite(const String &line) {
    Serial.println("[diag] " + line);
    if (!histFs) return;
    File f = LittleFS.open(DIAG_LOG, "a");
    if (!f) return;
    f.println(diagStamp() + "  " + line);
    size_t sz = f.size(); f.close();
    if (sz > DIAG_LOG_MAX) {                              // keep the newest half
        File r = LittleFS.open(DIAG_LOG, "r"); if (!r) return;
        r.seek(sz - DIAG_LOG_MAX / 2); r.readStringUntil('\n');
        String keep = r.readString(); r.close();
        File w = LittleFS.open(DIAG_LOG, "w"); if (!w) return;
        w.print("... older lines trimmed ...\n"); w.print(keep); w.close();
    }
}

const char *resetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power on";
        case ESP_RST_SW:       return "software restart";
        case ESP_RST_PANIC:    return "CRASH (panic)";
        case ESP_RST_INT_WDT:  return "CRASH (interrupt watchdog)";
        case ESP_RST_TASK_WDT: return "HANG (task watchdog)";
        case ESP_RST_WDT:      return "HANG (watchdog)";
        case ESP_RST_BROWNOUT: return "BROWN-OUT (supply voltage dipped)";
        case ESP_RST_DEEPSLEEP:return "deep sleep";
        case ESP_RST_EXT:      return "reset pin / EN button";
        default:               return "unknown";
    }
}

// Call once, early in setup() (after histBegin() mounted the flash).
void diagBegin() {
    esp_reset_reason_t rr = esp_reset_reason();
    bool prev = rtcD.magic == RTCD_MAGIC && rr != ESP_RST_POWERON;   // a brown-out usually keeps it too
    String l = "BOOT - " + String(resetName(rr));
    if (rr == ESP_RST_SW && prev && rtcD.why) l += String(": ") + DW_NAME[rtcD.why < 5 ? rtcD.why : 0];
    if (prev) {
        l += " | before: up " + diagDur(rtcD.upS) + ", was in: " + DS_NAME[rtcD.stage < 15 ? rtcD.stage : 0];
        l += ", free mem low " + String(rtcD.heapMin / 1024) + " KB, biggest block low " + String(rtcD.blockMin / 1024) + " KB";
        l += ", WiFi " + String(rtcD.rssi) + " dBm, drops " + String(rtcD.wifiDrops);
        l += ", last web request " + (rtcD.lastWebS ? diagDur(rtcD.upS > rtcD.lastWebS ? rtcD.upS - rtcD.lastWebS : 0) + " before" : String("none"));
        l += ", slowest loop " + String(rtcD.loopMaxMs) + " ms";
    } else if (rtcD.magic == RTCD_MAGIC) l += " (power was cut - no earlier details)";
    // crash record left by the ESP32 itself
    if (esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
        if (s && esp_core_dump_get_summary(s) == ESP_OK) {
            char b[200]; int n = snprintf(b, sizeof b, " | crash in task '%s' at 0x%08lx, backtrace", s->exc_task, (unsigned long)s->exc_pc);
            for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 8 && n < (int)sizeof b - 12; i++) n += snprintf(b + n, sizeof b - n, " 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
            l += b;
        }
        free(s);
        esp_core_dump_image_erase();
    }
    DG.bootLine = l;
    diagWrite(l);
    bool bad = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT || (rr == ESP_RST_SW && prev && (rtcD.why == DW_WEB || rtcD.why == DW_WIFI));
    addLog(bad ? "BOOT after " + String(resetName(rr)) + " - details at /diag" : "BOOT (" + String(resetName(rr)) + ")");
    uint16_t boots = rtcD.magic == RTCD_MAGIC ? rtcD.boots + 1 : 1;
    memset(&rtcD, 0, sizeof rtcD);
    rtcD.magic = RTCD_MAGIC; rtcD.boots = boots; rtcD.heapMin = ESP.getFreeHeap(); rtcD.blockMin = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    rtcD.stage = DS_SETUP;
}

// End of setup(): main-loop watchdog. Arduino feeds it after every loop() pass.
void diagWatchdogOn() {
    esp_task_wdt_config_t c = { .timeout_ms = 20000, .idle_core_mask = 0, .trigger_panic = true };   // only our loop is watched
    esp_task_wdt_reconfigure(&c);
    enableLoopWDT();
}
void diagOtaStart() { diagStage(DS_OTA); esp_task_wdt_reset(); diagWrite("OTA update starting"); rtcD.why = DW_OTA; }
void diagOtaTick()  { esp_task_wdt_reset(); }

void diagRestart(uint8_t why, const String &msg) {
    diagWrite("RESTART - " + msg);
    rtcD.why = why;
    delay(200);
    ESP.restart();
}

void diagWebHit() { DG.webHits++; DG.lastWebMs = millis(); rtcD.lastWebS = millis() / 1000; }

// Around each pass of loop(): measures the slowest pass and where it was slow.
void diagLoopStart() { DG.loopStart = millis(); }
void diagLoopEnd() {
    uint32_t d = millis() - DG.loopStart;
    if (d > DG.loopMax) { DG.loopMax = d; rtcD.loopMaxMs = d; }
    if (d > 3000 && millis() - DG.slowLogMs > 600000UL) {
        DG.slowLogMs = millis();
        diagWrite("slow loop " + String(d) + " ms (last part: " + DS_NAME[rtcD.stage < 15 ? rtcD.stage : 0] + ")");
    }
    rtcD.stage = DS_IDLE;
}

// Self-check: ask our own web server for /api/ping without blocking the loop.
void diagSelfCheck() {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) { if (DG.scState) { DG.cli.stop(); DG.scState = 0; } return; }
    if (DG.scState == 0) {
        if ((int32_t)(now - DG.scNext) < 0) return;
        diagStage(DS_DIAG);
        DG.scNext = now + 180000UL;
        if (DG.cli.connect(IPAddress(127, 0, 0, 1), 80, 300)) {
            DG.cli.print("GET /api/ping HTTP/1.0\r\nHost: heater\r\n\r\n");
            DG.scState = 1; DG.scAt = now;
        } else { DG.scState = 2; }                         // couldn't even connect: count as a miss
    }
    if (DG.scState == 1) {
        if (DG.cli.available()) {
            String first = DG.cli.readStringUntil('\n');
            DG.cli.stop(); DG.scState = 0;
            if (first.indexOf(" 200") > 0) {
                if (!DG.scArmed) { DG.scArmed = true; diagWrite("web self-check working - armed"); }
                if (DG.scFails) diagWrite("web self-check OK again after " + String(DG.scFails) + " miss(es)");
                DG.scFails = 0; DG.scOk++;
                return;
            }
            DG.scState = 2;
        } else if (now - DG.scAt > 10000) { DG.cli.stop(); DG.scState = 2; }
        else return;
    }
    if (DG.scState == 2) {                                 // a miss
        DG.scState = 0;
        if (!DG.scArmed) return;                           // never seen it work: don't act on it
        DG.scFails++;
        diagWrite("web self-check MISSED (" + String(DG.scFails) + " of 3) - free mem " + String(ESP.getFreeHeap() / 1024) + " KB, WiFi " + String(WiFi.RSSI()) + " dBm");
        DG.scNext = now + 60000UL;                         // check again sooner
        if (DG.scFails >= 3) diagRestart(DW_WEB, "web page stopped answering (3 self-checks missed)");
    }
}

// Once a second.
void diagTick() {
    uint32_t now = millis();
    rtcD.upS = now / 1000;
    uint32_t heap = ESP.getFreeHeap(), block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (heap < rtcD.heapMin) rtcD.heapMin = heap;
    if (block < rtcD.blockMin) rtcD.blockMin = block;
    if (heap < 40000 && !DG.lowHeapLogged) { DG.lowHeapLogged = true; diagWrite("LOW MEMORY: " + String(heap / 1024) + " KB free"); }
    if (block < 12000 && !DG.lowBlockLogged) { DG.lowBlockLogged = true; diagWrite("MEMORY BROKEN UP: biggest free block " + String(block / 1024) + " KB"); }

    // WiFi
    if (WiFi.getMode() == WIFI_STA || WiFi.getMode() == WIFI_AP_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            int8_t r = WiFi.RSSI(); rtcD.rssi = r; if (r < DG.rssiMin || !DG.rssiMin) DG.rssiMin = r;
            if (DG.wifiLostMs) {
                diagWrite("WiFi back after " + diagDur((now - DG.wifiLostMs) / 1000) + ", " + WiFi.localIP().toString() + ", " + String(r) + " dBm");
                addLog("WiFi back " + WiFi.localIP().toString());
                DG.wifiLostMs = 0;
            }
        } else if (WiFi.getMode() == WIFI_STA) {
            if (!DG.wifiLostMs) {
                DG.wifiLostMs = now ? now : 1; DG.wifiRetryMs = now; DG.drops++; rtcD.wifiDrops = DG.drops;
                diagWrite("WiFi LOST (signal was " + String(rtcD.rssi) + " dBm)");
            } else if (now - DG.wifiRetryMs > 30000) { DG.wifiRetryMs = now; WiFi.reconnect(); }
            if (now - DG.wifiLostMs > 600000UL) diagRestart(DW_WIFI, "WiFi gone for 10 min");
        }
    }
    // on the hotspot: keep trying the home WiFi
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        if (WiFi.getMode() == WIFI_AP_STA && WiFi.status() == WL_CONNECTED) diagRestart(DW_HOTSPOT, "home WiFi is back - leaving the hotspot");
        if (now - DG.apTryMs > 300000UL) { DG.apTryMs = now; WiFi.mode(WIFI_AP_STA); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    }
    diagSelfCheck();

    // one summary line a day (local midnight, or every 24 h without a clock)
    int day = -1;
    if (clockOk()) { time_t n = time(nullptr); struct tm t; localtime_r(&n, &t); day = t.tm_yday; }
    bool roll = (day >= 0 && DG.lastDay >= 0 && day != DG.lastDay) || (day < 0 && rtcD.upS - DG.dayStartS >= 86400);
    if (day >= 0) DG.lastDay = day;
    if (roll) {
        DG.dayStartS = rtcD.upS;
        diagWrite("DAY - up " + diagDur(rtcD.upS) + ", free mem " + String(heap / 1024) + " KB (low " + String(rtcD.heapMin / 1024) + "), biggest block " + String(block / 1024) +
                  " KB (low " + String(rtcD.blockMin / 1024) + "), WiFi " + String(rtcD.rssi) + " dBm (worst " + String(DG.rssiMin) + "), drops " + String(DG.drops) +
                  ", self-checks " + String(DG.scOk) + " ok, slowest loop " + String(DG.loopMax) + " ms, web requests " + String(DG.webHits) +
                  ", bus glitches " + String(DG.busBlips));
        DG.rssiMin = 0; DG.loopMax = 0;
    }
}

// /api/ping - the self-check target
void handlePing() { server.send(200, "text/plain", "ok"); }

// /diag - live numbers + the black-box log, newest first
void handleDiag() {
    diagWebHit();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain; charset=utf-8", "");
    String h = "SHED HEATER v" FW_VER " - diagnostics\n";
    h += "up " + diagDur(millis() / 1000) + "   starts since power-on " + String(rtcD.boots) + "\n";
    h += "free memory " + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(rtcD.heapMin / 1024) + "), biggest block " +
         String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024) + " KB (lowest " + String(rtcD.blockMin / 1024) + ")\n";
    h += "WiFi " + String(WiFi.status() == WL_CONNECTED ? "connected" : "NOT connected") + ", " + String(WiFi.RSSI()) + " dBm, drops " + String(DG.drops) + "\n";
    h += "web self-check " + String(DG.scArmed ? "armed" : "not armed yet") + ", " + String(DG.scOk) + " ok, misses in a row " + String(DG.scFails) + "\n";
    h += "web requests " + String(DG.webHits) + ", slowest loop " + String(DG.loopMax) + " ms, bus glitches ridden through " + String(DG.busBlips) + "\n";
    h += "this start: " + DG.bootLine + "\n\n--- log, newest first ---\n";
    server.sendContent(h);
    if (histFs) {
        File f = LittleFS.open(DIAG_LOG, "r");
        if (f) {
            std::vector<String> lines;                         // newest first
            while (f.available()) { String ln = f.readStringUntil('\n'); if (ln.length()) lines.push_back(ln); }
            f.close();
            String out;
            for (int i = (int)lines.size() - 1; i >= 0; i--) { out += lines[i]; out += '\n'; if (out.length() > 1200) { server.sendContent(out); out = ""; esp_task_wdt_reset(); } }
            server.sendContent(out);
        } else server.sendContent("(no log yet)\n");
    } else server.sendContent("(no flash storage)\n");
    server.sendContent("");
}
