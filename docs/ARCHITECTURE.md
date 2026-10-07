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
| Distance sensor | HC-SR04 ultrasonic, VCC wired to **BAT+** (not 5V/VUSB — unpowered on battery) |
| Power | Single-cell Li-ion/LiPo, ~3.0–4.2V |
| Antenna | External u.FL antenna via onboard RF switch (GPIO3 enable, GPIO14 select) |
| LED | Onboard, active-low, GPIO15 — on while awake, off just before sleep |

### Pinout (`include/config.h`)

| Signal | GPIO | Board label | Notes |
|---|---|---|---|
| HC-SR04 TRIG | GPIO16 | D6 | output |
| HC-SR04 ECHO | GPIO17 | D7 | input, through a 1kΩ/3kΩ divider (ECHO's HIGH tracks BAT+, not a fixed 5V) |
| Battery sense | GPIO0 | D0 / ADC1_CH0 | through a ~1MΩ/1MΩ divider tapped off BAT+; ratio calibrated empirically (`BATTERY_DIVIDER_RATIO = 2.051`), not just nominal 1:1 math |
| WiFi antenna enable | GPIO3 | — | drive low to activate RF switch control |
| WiFi antenna select | GPIO14 | — | drive high to route to external antenna (low = onboard ceramic) |
| Onboard LED | GPIO15 | — | active-low |
| *(reserved, V2)* | GPIO21 | D3 | MOSFET-switched HC-SR04 VCC — wired but unused in V1, so the sensor draws current between wakes; see To-dos |

### ECHO level-shifting divider

ECHO's HIGH level follows the HC-SR04's VCC, which is BAT+ (up to 4.2V on a
full cell). The ESP32-C6 GPIOs run at VDD = 3.3V and are rated for at most
VDD + 0.3V ≈ 3.6V on an input. A direct connection would push ~0.6V past that
limit on every echo pulse, forward-biasing the pin's internal clamp diode into
the 3.3V rail. That's out of spec and causes damage over time, so the input
needs a divider.

The divider is 1kΩ on top (from ECHO) and 3kΩ to GND, ratio 0.75. VIH min on
the ESP32-C6 is 0.75 × VDD = 2.475V:

| Battery | ECHO HIGH at GPIO17 | vs. limits |
|---|---|---|
| 4.2V (full) | 3.15V | under the 3.6V max, clean HIGH |
| 3.7V (nominal) | 2.78V | valid HIGH |
| 3.3V | 2.48V | right at VIH min |
| 3.0V (empty) | 2.25V | below the datasheet VIH min |

The ratio protects the pin well at full charge but leaves no margin at the
low end. Below ~3.3V battery, the datasheet no longer guarantees the divided
HIGH. It still works in practice because the real input threshold is
typically around 1.5–1.8V, but that's not a spec'd margin. The classic
5V-rated HC-SR04 also gets unreliable well before the cell reaches 3.0V, so
the sensor usually fails before the logic level does.

Options if this ever needs tightening:
- **Higher ratio**, e.g. 1kΩ/3.9kΩ (0.80): 3.34V at 4.2V, 2.63V at 3.3V.
- **3.3V-rated sensor variant** (HC-SR04P / RCWL-9610, rated 3–5.5V) powered
  from the 3.3V rail. ECHO then never exceeds 3.3V, so it needs no divider,
  and the sensor supply stays constant as the battery drains.

## Firmware modules (`src/`, `include/`)

| Module | Responsibility |
|---|---|
| `main.c` | Orchestrates one wake cycle: measure → connect → POST (+ flush backlog) → sleep. `app_main()` runs once per wake and never returns — deep sleep is effectively a full reset. |
| `sensor_hcsr04.c` | Triggers the HC-SR04, times the echo pulse, converts to cm. `hcsr04_measure_filtered_cm()` takes up to `avg_sample_count` samples (100ms spacing), rejects outliers >3σ from the median, and returns the mean plus RMS deviation of the survivors. |
| `battery.c` | Reads `BATTERY_ADC_CHANNEL` via ESP-IDF's calibrated ADC oneshot driver, falls back to an uncalibrated linear approximation if calibration is unavailable, scales by `BATTERY_DIVIDER_RATIO`. |
| `chip_temp.c` | Reads the ESP32C6's internal die/package temperature sensor. Diagnostic only — self-heating from WiFi TX/CPU load reads several °C above real ambient. |
| `wifi_comm.c` | Selects the external antenna, connects to WiFi (10s timeout), builds the POST JSON body by hand (fixed schema, no JSON library needed), sends it with the mbedTLS default CA bundle, and does a minimal substring-scan parse of the response for `wakeup_period_min`/`avg_sample_count` config updates. |
| `storage.c` | NVS-backed: persists `tank_config_t` (rarely written), a flash-write counter, and the pending-measurement ring buffer used to survive WiFi outages. |

## One wake cycle

1. LED on. Load persisted config (`wakeup_period_min`, `avg_sample_count`) from NVS.
2. Read battery voltage and chip temperature.
3. Take a filtered HC-SR04 reading. On timeout (no echo) or an implausible
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
- **V1 leaves the HC-SR04 powered continuously** (VCC tied to BAT+). The V2
  MOSFET gate on GPIO21/D3 is wired but not yet used in firmware — cutting
  sensor power between wakes is the next battery-life lever.
- **ECHO logic HIGH is out of datasheet spec below ~3.3V battery.** The
  1k/3k divider keeps it under VIH min (2.475V) on a low cell. It still works
  in practice, but there's no guaranteed margin. See "ECHO level-shifting
  divider" above.

## Related documents

- `server-handoff/device.md` — the full device↔server API contract (request/
  response schemas, validation ranges, retry semantics). Self-contained;
  treat it as authoritative for wire format.
- `server-handoff/CLAUDE.md` — server-side project brief and suggested stack.
- `docs/CHANGELOG.md` — dated log of what changed and why, going forward.
