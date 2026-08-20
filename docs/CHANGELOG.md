# Changelog

All notable changes to the water tank level firmware, in reverse-chronological
order. This is the durable, git-tracked record of *what changed and why* —
add an entry here whenever a change is worth explaining to someone reading
the project cold. Group entries by date; use `Added` / `Changed` / `Fixed`
sub-headings within a date when it helps.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## Unreleased

### Added
- `readings[].samples_cm` (temporary, diagnostic-only): when
  `DEBUG_SEND_RAW_SAMPLES` (`config.h`, currently `1`) is enabled, the
  current wake's raw pre-outlier-rejection HC-SR04 samples are attached to
  the POST, to study real sample distributions offline and design a better
  outlier filter (the existing median+3σ rejection isn't catching some false
  points in the field). Only ever on the current reading, never on backlog
  entries (raw samples aren't persisted to NVS). `hcsr04_measure_filtered_cm()`
  gained optional `out_samples`/`out_sample_count` params to expose them;
  `wifi_post_measurement()` gained `raw_samples`/`raw_sample_count` params to
  attach them. Meant to be flipped back to `0` once the filter redesign is
  done — see `server-handoff/device.md` for the field's schema.

## 2026-08-02

### Fixed
- `sdkconfig`: flash size was set to 2MB (`CONFIG_ESPTOOLPY_FLASHSIZE_2MB`)
  despite the XIAO ESP32C6's real 4MB embedded flash (confirmed via
  `esptool flash_id`). Harmless while the build fit under 2MB, but wrong —
  fixed before it silently caps future firmware growth or OTA.
  `MMU_PAGE_SIZE` 32KB→64KB followed automatically from the flash-size fix.
- `MAX_AVG_SAMPLE_COUNT` lowered from 1000 to 100 to match
  `HCSR04_MAX_SAMPLES`, the firmware's actual hard sampling cap. Before this,
  the server-facing config validation range could imply a sample count the
  device would never actually run.

### Changed
- `DEFAULT_WAKEUP_PERIOD_MIN` lowered from 15 to 1 minute, for faster
  iteration while testing/flashing. `PENDING_BUFFER_SIZE`'s outage-coverage
  comment updated to match (3.2h instead of 48h of backlog headroom at the
  new default cadence).

## 2026-07-24

### Added
- `readings[].chip_temp_c`: ESP32C6 internal die/package temperature per
  reading (sentinel `-999.00` on sensor read failure). Diagnostic/device-health
  signal, not ambient temperature — see `server-handoff/device.md`.
- `readings[].distance_std_cm`: RMS deviation from the mean across the
  `avg_sample_count` HC-SR04 samples taken that wake, computed after the same
  median+3σ outlier rejection used for `distance_cm`. Shares `distance_cm`'s
  `-1.00` sentinel.

### Changed
- `secrets.h`'s `POST_URL` switched from the `httpbin.org` test echo to the
  real ingest endpoint (`https://sensors.la-7.eu/watertank/api/readings/...`).
  Both changes above were held back until server-side tolerance for the two
  new fields was confirmed, then flashed to the real device together — an
  unrecognized field would 422 every POST and permanently wedge the on-device
  backlog (a rejected batch is never acknowledged/cleared).

## 2026-07-23

### Added
- Initial firmware: XIAO ESP32C6 + HC-SR04, wakes on a timer, measures
  distance/battery/chip temperature, POSTs to the ingest server as JSON, and
  queues locally (NVS ring buffer) on send failure.
- Fixed an NVS key-length bug found during bring-up: config keys were
  silently failing to persist, which pinned `wakeup_period_min` at its
  1-minute default regardless of server-pushed config updates.
