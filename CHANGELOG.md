# Changelog

Firmware versions are the ESP32 sketch's. The Nano bridge is v1.2 throughout.

## v1.9.5 - 2026-11-05
- Thermostat **smooth hold**: eases down on the way up to the setpoint, then trims one level at a time and learns the level that holds the room. Tuned on a two-node shed model fitted to a real warm-up; switch under More settings > Thermostat.
- Protocol doc: cross-reference to sashko2000's independent decode, closed-form chamber thermistor curve, unknown display byte `0x54`.

## v1.9.4 - 2026-11-04
- Home-screen icon + web manifest.

## v1.9.3 - 2026-11-04
- WiFi power saving off (fixes phones that couldn't reach the controller after being idle). Page reports "can't reach the controller" instead of spinning.

## v1.9.2
- Fix: false "not warming" alert when the ESP32 restarted mid-run.

## v1.9.1 - black box
- `/diag`: restart reason, uptime, lowest free memory, WiFi, last request. Loop and web-server watchdogs with self-restart. Garbled single frames no longer count as a stop/start.

## v1.9 - history
- `/history`: 24 h / 7 d / 30 d / year on LittleFS, saved every 10 min.

## v1.8
- Master shut-off at 75 °F in every mode; "keep it running?" prompt on long Manual runs. Pages gzipped.

## v1.7
- Chamber guard: adds a level when the chamber sits under the floor while lit.

## v1.6
- DHT22 room sensor; Thermostat and Frost guard modes with guard rails.

## v1.5 - 2026-10-25
- First public release: Nano bridge v1.2, three page styles, stats, chamber graph, alerts, LED show, OTA.
