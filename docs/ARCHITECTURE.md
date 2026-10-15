# Architecture — Water Tank Level Monitor

Firmware for a battery-powered ultrasonic water tank sensor. It wakes on a
timer, takes a filtered distance reading plus battery/chip-temperature
telemetry, POSTs it to a server over WiFi/HTTPS, and goes back to deep sleep.
No RTC, no persistent WiFi connection, no on-device calibration — the device
is deliberately dumb; the server owns tank calibration and visualization.

For the graphical wiring diagram and system data-flow diagram, see the
published artifact (link kept in the project's working notes / most recent
conversation — regenerate via Claude Code if the link is lost, source data
below is enough to rebuild it). For the full device↔server API contract, see
`server-handoff/device.md` — that file is the authoritative schema and is
kept self-contained on purpose (server implementers shouldn't need this repo).

## Hardware

| Component | Detail |
|---|---|
| MCU | Seeed XIAO ESP32C6, ESP-IDF framework, 4MB flash (single-app 1.5MB partition — WiFi/TLS via mbedTLS no longer fits the default 1MB app partition) |
| Distance sensor | A02YYUW waterproof ultrasonic (DFRobot SEN0311), UART output, VCC wired to **3V3**. Replaced the original HC-SR04 in 2026-10 |
| Power | Single-cell Li-ion/LiPo, ~3.0–4.2V |
| Antenna | External u.FL antenna via onboard RF switch (GPIO3 enable, GPIO14 select) |
| LED | Onboard, active-low, GPIO15 — on while awake, off just before sleep |

### Pinout (`include/config.h`)

| Signal | GPIO | Board label | Notes |
|---|---|---|---|
| A02YYUW RX (mode select, yellow) | GPIO16 | D6 | output: low = real-time frames, high = sensor-processed frames |
| A02YYUW TX (white) | GPIO17 | D7 | UART1 RX, 9600 8N1, through a 1kΩ series resistor |
| Battery sense | GPIO0 | D0 / ADC1_CH0 | through a ~1MΩ/1MΩ divider tapped off BAT+; ratio calibrated empirically (`BATTERY_DIVIDER_RATIO = 2.051`), not just nominal 1:1 math |
| WiFi antenna enable | GPIO3 | — | drive low to activate RF switch control |
| WiFi antenna select | GPIO14 | — | drive high to route to external antenna (low = onboard ceramic) |
| Onboard LED | GPIO15 | — | active-low |
| *(reserved, V2)* | GPIO21 | D3 | MOSFET-switched sensor VCC — wired but unused in V1, so the sensor draws current between wakes; see To-dos |

### Sensor wiring notes

- **Power from 3V3, not BAT+.** The A02YYUW is rated for 3.3–5V; BAT+ falls
  to ~3.0V on a low cell. The 3V3 rail stays constant for the whole battery
  range. The XIAO's 5V pin is USB-only and dead on battery.
- **No level shifting needed.** Powered from 3V3, the sensor's TX can't
  exceed 3.3V, so it connects straight to the ESP32-C6. The 1kΩ series
  resistor is the top half of the old HC-SR04 ECHO divider (the 3kΩ to GND
  was removed). With no load on the node it drops nothing, and the RC delay
  is negligible at 9600 baud. It limits current if GPIO17 is ever
  misconfigured as an output.
- **GPIO16/17 are UART0's default console pins** on the ESP32-C6. The
  firmware resets GPIO16 to a plain output (detaching console TX from it)
  and reads the sensor on UART1. The ROM bootloader still prints on GPIO16
  at boot, which briefly toggles the sensor's mode pin; frames from before
  the firmware sets the mode are flushed.

The HC-SR04 needed a 1kΩ/3kΩ divider on ECHO because its HIGH level tracked
BAT+ (up to 4.2V), above the ESP32-C6's VDD + 0.3V ≈ 3.6V input limit. That
divider is no longer needed with the A02YYUW.

## Firmware modules (`src/`, `include/`)

| Module | Responsibility |
|---|---|
| `main.c` | Orchestrates one wake cycle: measure → connect → POST (+ flush backlog) → sleep. `app_main()` runs once per wake and never returns — deep sleep is effectively a full reset. |
| `sensor_a02yyuw.c` | Reads the A02YYUW's 4-byte UART frames (`0xFF`, distance mm high, low, checksum), resyncing on the header and checking the checksum. `a02_measure_filtered_cm()` reads `avg_sample_count` consecutive frames (~100ms each in real-time mode), rejects outliers >3σ (MAD-derived) from the median, and returns the mean plus RMS deviation of the survivors. If the first frame never arrives, the sensor is treated as absent and the wake doesn't wait out every remaining timeout. |
| `battery.c` | Reads `BATTERY_ADC_CHANNEL` via ESP-IDF's calibrated ADC oneshot driver, falls back to an uncalibrated linear approximation if calibration is unavailable, scales by `BATTERY_DIVIDER_RATIO`. |
| `chip_temp.c` | Reads the ESP32C6's internal die/package temperature sensor. Diagnostic only — self-heating from WiFi TX/CPU load reads several °C above real ambient. |
| `wifi_comm.c` | Selects the external antenna, connects to WiFi (10s timeout), builds the POST JSON body by hand (fixed schema, no JSON library needed), sends it with the mbedTLS default CA bundle, and does a minimal substring-scan parse of the response for `wakeup_period_min`/`avg_sample_count` config updates. |
| `storage.c` | NVS-backed: persists `tank_config_t` (rarely written), a flash-write counter, and the pending-measurement ring buffer used to survive WiFi outages. |

## One wake cycle

1. LED on. Load persisted config (`wakeup_period_min`, `avg_sample_count`) from NVS.
2. Read battery voltage and chip temperature.
3. Take a filtered sensor reading. On no valid reading or an implausible
   result (outside 0–500cm), both `distance_cm` and `distance_std_cm` are
   swapped for the shared sentinel `-1.00` rather than sent or queued as-is —
   otherwise a single implausible reading would sit at the head of the
   pending buffer forever, since the server rejects a batch atomically and an
   unacknowledged batch never clears.
4. Select external antenna, connect to WiFi.
   - **On success:** pop the entire NVS backlog, POST it plus the current
     reading (oldest first, current last) plus WiFi RSSI plus the device's
     current config, as one JSON body. On a 2xx response, clear the backlog;
     apply any valid config updates the server sent back and persist them.
   - **On failure** (WiFi connect failure, network error, or non-2xx): push
     the current reading onto the NVS ring buffer (`PENDING_BUFFER_SIZE = 192`
     entries, overwrite-oldest) for retry on a future successful connection.
5. LED off. Deep-sleep for `wakeup_period_min` minutes (timer wakeup).

## Persisted state (NVS)

- `tank_config_t` — `wakeup_period_min`, `avg_sample_count`. Rarely written;
  only changes when the server pushes an update in its POST response.
- Pending-measurement ring buffer — up to 192 queued readings, survives power
  loss/reset (not just deep sleep).
- Flash-write counter — piggybacked onto whatever NVS commit is already
  happening, for visibility into flash wear over the device's lifetime.

## Known limitations / gaps

- **No device identifier or auth on the request.** Fine for a single-device
  setup; adding multi-device support or auth is a firmware change, not just
  a server one.
- **No on-device tank calibration.** `distance_cm` is raw sensor-to-surface
  distance; converting to level%/liters is entirely the server's job.
- **No RTC/NTP.** The server reconstructs each reading's approximate
  timestamp by counting backward from request-receipt time in
  `wakeup_period_min` steps — drifts if the interval changed mid-outage.
- **The sensor is powered continuously** (VCC tied to 3V3), and the A02YYUW
  streams frames non-stop, drawing several mA even while the ESP32 deep
  sleeps. That dominates the sleep current. The V2 MOSFET gate on GPIO21/D3
  is wired but not yet used in firmware — cutting sensor power between
  wakes is the next battery-life lever.

## Related documents

- `server-handoff/device.md` — the full device↔server API contract (request/
  response schemas, validation ranges, retry semantics). Self-contained;
  treat it as authoritative for wire format.
- `server-handoff/CLAUDE.md` — server-side project brief and suggested stack.
- `docs/CHANGELOG.md` — dated log of what changed and why, going forward.
