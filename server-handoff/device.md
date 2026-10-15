# Device API Contract — Water Tank Level Monitor

Firmware: XIAO ESP32C6 + A02YYUW waterproof ultrasonic sensor (UART output;
replaced the original HC-SR04 in 2026-10 — wire format unchanged), battery powered, deep-sleep
between wakes. This document is self-contained — it describes device behavior,
not firmware source (the firmware lives in a separate repo).

## Transport

- HTTPS POST, one request per wake cycle.
- `Content-Type: application/json`.
- Device validates the server's TLS certificate against the standard CA
  bundle (mbedTLS default bundle) — the server needs a properly chain-signed
  cert (e.g. Let's Encrypt). Self-signed certs will fail the handshake.
- Any HTTP status outside 200-299 is treated as failure by the device: the
  measurement is queued locally and retried on a future wake (see Retry
  semantics below). The response body is only parsed on 2xx.
- Request timeout on the device side is 8s.

## Known gaps (not yet implemented on the device)

- **No device identifier or auth.** The request carries no device ID, API
  key, or token. Fine for a single-device v1; if the server will ever need
  to support multiple devices or basic auth, that's a firmware change, not
  just a server one — flag it early if it matters.
- **No tank calibration on-device.** `distance_cm` is raw sensor-to-surface
  distance, not a fill level or volume. Converting to level%/liters (sensor
  mounting height, empty/full distances, tank geometry) is entirely a server
  concern.

## Request body (device → server)

```json
{
  "config": {
    "wakeup_period_min": 15,
    "avg_sample_count": 30
  },
  "rssi": -66,
  "readings": [
    {"distance_cm": 50.50, "distance_std_cm": 0.31, "battery_mv": 4200, "chip_temp_c": 34.12},
    {"distance_cm": 50.60, "distance_std_cm": 0.28, "battery_mv": 4195, "chip_temp_c": 34.05},
    {"distance_cm": 50.47, "distance_std_cm": 0.35, "battery_mv": 4223, "chip_temp_c": 33.98}
  ]
}
```

**New fields as of this update: `readings[].chip_temp_c` and
`readings[].distance_std_cm`.** If the server's request schema rejects unknown
fields, these need to be accepted (or at least tolerated) *before* the
corresponding firmware ships, or every POST will 422 and — per the Retry
semantics below — permanently wedge the device's backlog, since a rejected
batch is never acknowledged/cleared.

**TEMPORARY diagnostic field, not yet flashed to the real device:
`readings[].samples_cm`.** Gated behind `DEBUG_SEND_RAW_SAMPLES` in the
firmware's `config.h`. When enabled, the *last* entry in `readings` (the
current wake, never a backlog entry) carries an extra array of the fully
raw, pre-outlier-rejection sensor samples for that wake — e.g.
`"samples_cm": [50.5, 50.4, -1.00, 50.5, ...]`, always exactly
`avg_sample_count` values long: one entry per attempted ping, in order,
nothing dropped. A ping that failed (no valid frame from the sensor, or out of its range) shows up as
`-1.00` (the same sentinel `distance_cm` uses) rather than being omitted —
so a burst of failed pings is visible in the data, not silently invisible.
This exists to let raw sample distributions be studied offline to design a
better outlier filter — it's meant to be flipped off again afterward, so the
server should tolerate (accept and, ideally, just log/store raw rather than
fully model) an unrecognized `samples_cm` key the same way it needs to
tolerate any other unknown field, per the note above.

| Field | Type | Notes |
|---|---|---|
| `config.wakeup_period_min` | uint, minutes | Device's *current* wake interval (echoes back what it's running, doesn't necessarily match what you last told it — see Response below). |
| `config.avg_sample_count` | uint | Device's current sensor sample count (median+MAD-filtered mean per wake — see below). |
| `rssi` | int, dBm | WiFi signal strength of *this* connection only. Not present per-reading — there's no historical RSSI for backlog entries (no WiFi = no RSSI at the time they were queued). |
| `readings` | array | Oldest first, **current reading is always last**. Array has 1 entry in the normal case; more than 1 when flushing a backlog after a WiFi outage (queue holds up to 192 entries / 48h at 15-min sampling, ring-buffer overwrite-oldest if exceeded). |
| `readings[].distance_cm` | float, 2 decimals | Raw sensor distance. **Sentinel value `-1.00` means the sensor gave no valid reading that wake** — filter/flag these, don't treat as a literal reading. |
| `readings[].distance_std_cm` | float, 2 decimals | RMS deviation from the mean across the `avg_sample_count` sensor readings taken that wake, computed after the same median+MAD outlier rejection used for `distance_cm` (so it reflects the spread of the samples actually averaged in, not the raw noise floor) — see below. The sensor reports whole millimetres (so values are multiples of 0.1cm) with a datasheet accuracy of about ±1cm, so values well below that reflect measurement consistency, not calibrated precision. **Shares `distance_cm`'s sentinel: `-1.00` whenever `distance_cm` is `-1.00`** (no echo / implausible range that wake) — treat both fields as sentinel together. |
| `readings[].battery_mv` | int | Battery voltage in mV (~3000-4200 for the single-cell LiPo in use). |
| `readings[].chip_temp_c` | float, 2 decimals | ESP32-C6's internal **die/package temperature, not ambient** — self-heating from WiFi TX and CPU load typically reads several °C above actual ambient air/water temperature. Diagnostic/device-health signal only; not a substitute for a real ambient sensor. **Sentinel value `-999.00` means the on-chip sensor read failed** that wake (out of its configured -10..80°C range, or a driver error) — filter/flag these like the distance sentinel. |
| `readings[].samples_cm` | array of float, 2 decimals | **TEMPORARY, diagnostic-only** (see note above) — only present on the current-wake reading, never on backlog entries, and only when the firmware's `DEBUG_SEND_RAW_SAMPLES` is enabled. Fully raw per-ping distances, before outlier rejection/averaging, one entry per attempted ping (`avg_sample_count` long) with no drops — failed pings (no valid reading / out of range) appear as `-1.00`, the same sentinel `distance_cm` uses. |

### On-device outlier filter (median + MAD)

This is the filter that turns `samples_cm` (raw, one entry per attempted
ping) into `distance_cm`/`distance_std_cm` (one filtered value per wake). It
runs on-device, once per wake, over that wake's `avg_sample_count` pings —
`config.avg_sample_count`/`n` below is that same value. Exact algorithm
(`a02_filter_samples()` in the firmware; the low-end floor in step 0 is
`a02_measure_filtered_cm()`, one level up):

0. Before this filter even runs: any raw ping below `DISTANCE_MIN_PLAUSIBLE_CM`
   (currently 10cm — this tank's minimum physically-possible sensor-to-water
   distance at max fill) is dropped outright, alongside the pre-existing
   no-reading/out-of-range exclusion. Both are cheaper and more certain
   than relying on statistics to catch a physically-impossible reading; see
   the `samples_cm` sentinel note above for how excluded pings show up in
   the raw array (unaffected by this floor — it's fully raw).
1. `median` = the middle element of the sorted remaining samples
   (`sorted[n/2]`; for even `n` this is the upper of the two middle values,
   not their average).
2. `mad` (median absolute deviation) = `median(|x_i - median|)` over the same
   samples — a robust measure of spread, unlike a mean-derived sigma (see
   *Why MAD, not RMS-from-mean* below).
3. `threshold = max(3 * 1.4826 * mad, 1.0cm)`. The `1.4826` constant scales
   MAD to be comparable to a normal distribution's sigma; the `1.0cm` floor
   exists because the sensor's real per-ping noise is around its 1mm resolution, which can
   otherwise collapse `mad` to ~0 and make the threshold reject everything
   but an exact median match.
4. Keep every sample within `threshold` of the `median`; reject the rest.
5. `distance_cm` = mean of the kept samples (falls back to `median` if
   somehow none survive).
6. `distance_std_cm` = RMS deviation of the kept samples from `distance_cm`
   (0 if fewer than 2 survive).

**Why MAD, not RMS-from-mean.** An earlier version of this filter rejected
samples more than `3 * sigma` from the median, where `sigma` was the RMS
deviation from the *mean of all samples — including the outliers being
screened for*. That's not a robust statistic: a cluster of far-off echoes
(e.g. a multipath/second reflection off the tank wall, foam, or a ripple)
drags the mean toward them and inflates `sigma` right along with it, widening
the very acceptance window meant to exclude them. Working the algebra out,
that filter had a sharp breakdown point at ~12.7% contamination — once that
fraction of a wake's samples shared a common deviation, *all of them*
survived rejection regardless of magnitude, dragging `distance_cm` toward
the wrong cluster (observed in the field as `distance_std_cm` spiking to
10-30cm on bad wakes, vs. ~0.03cm on clean ones — both figures otherwise
consistent with what the new filter still expects). MAD doesn't have this
self-defeating property: a minority cluster of bad samples barely moves it,
since it only shifts where those samples land in sort order, not by how far
off they are. Its breakdown point is 50% contamination instead of ~12.7%.
Also note the sensor's stated physical accuracy is ~0.3cm (see
`distance_std_cm` above): a low `distance_std_cm` on a given wake means the
surviving samples agree with each other, not that they agree with reality —
it's not proof an outlier didn't survive rejection.

### Timestamp reconstruction

The device does not send timestamps (no on-device RTC/NTP by design choice —
see rationale below). Readings are spaced exactly `config.wakeup_period_min` apart,
since the device pushes to the queue once per wake regardless of success.
Reconstruct each reading's approximate time by counting backward from
request-receipt time:

```
reading[i].time ≈ receipt_time - (len(readings) - 1 - i) * wakeup_period_min minutes
```

Caveat: this drifts if `wakeup_period_min` changed partway through an outage (rare,
acceptable approximation — not worth on-device NTP complexity for this).

## Response body (server → device) — config sync

Optional. If you want to push new settings to the device, respond with:

```json
{"wakeup_period_min": 20, "avg_sample_count": 25}
```

**Critical formatting constraint:** the device's response parser is
intentionally minimal — a plain substring search for `"wakeup_period_min"` and
`"avg_sample_count"` anywhere in the response text, not a full JSON parser.
**Do not echo the request body, wrap the values in a nested object, or
include these key names anywhere else in the response** (logging fields,
debug echoes, etc.) — any occurrence will be picked up and misapplied. Keep
the response body to just these two flat keys (both optional; include only
the ones you want to change). Anything else in the response is ignored.

Validation (enforced device-side, silently — the server gets no rejection
feedback, only a device-side log line):

| Field | Range | Out-of-range behavior |
|---|---|---|
| `wakeup_period_min` | 1 – 1440 (24h) | Ignored, device keeps its current value |
| `avg_sample_count` | 1 – 100 | Ignored, device keeps its current value (matches the device's actual hard sampling cap, for battery/timing reasons) |

Each field is validated and applied independently — a valid `wakeup_period_min`
with an invalid `avg_sample_count` still applies the `wakeup_period_min` change.

## Retry / failure semantics

- Failed POST (network error, non-2xx, or WiFi connect failure) → current
  reading is appended to an on-device ring buffer (NVS-backed, survives
  power loss/reset, not just deep sleep) and retried on the next successful
  connection, batched with whatever else has accumulated.
- Buffer capacity: 192 entries (~48h at 15-min sampling). Beyond that,
  oldest entries are silently overwritten — there is no way to know
  server-side that entries were dropped except a gap larger than expected
  between consecutive reading timestamps.
