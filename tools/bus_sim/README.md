# Bus simulator

Runs the ESP32 pulse decoder and bus scheduler or the original Nano sketch on a PC against a simulated OEM display and heater. The ESP32 simulation models the GPIO edges and transmitted RMT waveform.

```sh
g++ -std=c++17 -O1 -DESP32_BRIDGE -I. -o bus_sim bus_sim.cpp
for scenario in A B C D E F G H I J K; do
    ./bus_sim "$scenario" || exit 1
done
```

Omit `-DESP32_BRIDGE` to run the original Nano sketch with scenarios A–H.

| Scenario | What it does |
|----------|--------------|
| A | OEM display on, ON / LEVEL 5 / LEVEL 2 / UP / DOWN / OFF / ON |
| B | display sitting on its chamber page |
| C | no display (standalone) |
| D | chamber poll off |
| E | display unplugged mid-run |
| F | start, run out of fuel (E-08 fault), clear with ON |
| G | heater stops answering, queued command expires |
| H | full start and stop, every status line printed (phases, volts) |
| I | no display, chamber poll off |
| J | display starts the heater after the ESP32 queues START |
| K | standalone mode loses heater replies before a START request |

The heater model is simplified (phase timing is shortened) - it checks the bus timing and the bridge's decoding, not the heater.
