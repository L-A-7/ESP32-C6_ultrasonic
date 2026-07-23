# Water Tank Level Monitor — Server

Backend for a battery-powered ESP32C6 + HC-SR04 firmware that wakes
periodically (default 15 min, adjustable), measures water tank level via
ultrasonic distance, and POSTs the reading over WiFi/HTTPS.

**Read `device.md` in this directory first** — it's the full, self-contained
API contract (exact request/response JSON schemas, validation ranges,
retry/backlog semantics, known gaps). The firmware itself lives in a
separate repo and isn't available here; treat `device.md` as authoritative
for what the device sends and expects back.

## Goal

- Accept the device's POST, store readings in a time-series-friendly way.
- Serve/expose the data for visualization (Grafana was the plan — pointed
  directly at the DB, no need for Influx/Prometheus unless there's a reason).
- Optionally: respond with config updates (`wakeup_period_min`, `avg_sample_count`)
  to let the device be retuned without reflashing. Bonus, not required for v1.

## Suggested stack (not mandatory, just where prior discussion landed)

FastAPI + Postgres (SQLite is fine to start if that's simpler to stand up).
No strong opinion beyond: keep it simple, avoid infra you don't need yet
(no message queues, no microservices, no auth system until the device
actually needs one — see `device.md`'s "Known gaps" section, the device
currently sends no ID/auth token at all).

## Working style (carried over from the firmware side of this project)

- Keep it functional and minimal — no speculative abstractions, no building
  for hypothetical future requirements. Three similar lines beats a
  premature abstraction.
- Confirm non-obvious design decisions before implementing them, especially
  anything affecting the API contract (changes there need to stay in sync
  with the firmware, which won't be trivial to re-flash/re-test repeatedly).
- Test incrementally — e.g. stand up the POST endpoint and verify it with a
  hand-built curl/httpie request matching `device.md`'s schema *before*
  wiring up the real device, same way the firmware side was built and
  tested one module at a time.
- The device is currently pointed at `https://httpbin.org/post` for testing
  (a public echo service). Once this server has a real deployed URL, that
  needs to be updated in the firmware's `include/secrets.h` (`POST_URL`) —
  that's a firmware-repo change, flag it as a to-do rather than assuming
  it's handled here.

## First steps (suggested, not prescriptive)

1. Design the DB schema for readings (device sends raw `distance_cm` +
   `battery_mv` per reading, plus `rssi` and current `config` once per
   request — see `device.md` for exact shapes).
2. Build the POST endpoint, validate against the documented request schema.
3. Decide the tank calibration approach (empty/full distance → level% or
   liters) — this lives entirely server-side per `device.md`.
4. Only then: Grafana dashboard, and (optional/bonus) config-sync responses.
