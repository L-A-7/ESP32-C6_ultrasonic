#pragma once

// --- Onboard LED (Seeed XIAO ESP32C6), active-low ---
#define LED_GPIO        GPIO_NUM_15
#define LED_ACTIVE_LOW  1

// --- WiFi antenna RF switch (Seeed XIAO ESP32C6) ---
// External u.FL antenna is installed; select it over the onboard ceramic
// antenna. GPIO3 must be driven low to activate RF switch control, then
// GPIO14 high routes to the external antenna (low = onboard ceramic).
#define WIFI_ANT_ENABLE_GPIO  GPIO_NUM_3
#define WIFI_ANT_CONFIG_GPIO  GPIO_NUM_14

// --- A02YYUW ultrasonic sensor (DFRobot SEN0311, UART output) ---
// VCC wired to the 3V3 pin (sensor rated 3.3-5V; BAT+ sags below that on a
// low cell). Sensor TX -> D7 through a 1k series resistor (leftover from the
// HC-SR04's level-shifting divider; harmless at 9600 baud). Sensor RX is a
// mode select, not a UART input: low = real-time output (~100ms/frame),
// high/floating = the sensor's own processed output (~100-300ms/frame).
#define A02_UART_NUM       UART_NUM_1   // UART0 is the console
#define A02_TX_GPIO        GPIO_NUM_17  // D7, sensor TX -> ESP32 RX
#define A02_MODE_GPIO      GPIO_NUM_16  // D6, drives the sensor's RX pin
// Real-time mode: independent raw readings, filtered on our side by
// a02_filter_samples() (and visible as-is in DEBUG_SEND_RAW_SAMPLES).
#define A02_REALTIME_MODE  1
// V2: MOSFET-switched VCC gated by GPIO21 (D3), reserved/unused in V1.

// --- Battery voltage divider (~1MOhm / 1MOhm), tapped from BAT+ ---
// Ratio is calibrated empirically against a multimeter (folds in both
// resistor tolerance and ADC error), not just the nominal 1:1 divider math.
#define BATTERY_ADC_CHANNEL     0        // D0 / GPIO0 / ADC1_CH0
#define BATTERY_DIVIDER_RATIO   2.051f

// --- Timing ---
#define DEFAULT_WAKEUP_PERIOD_MIN      1
#define MIN_WAKEUP_PERIOD_MIN           1
#define MAX_WAKEUP_PERIOD_MIN           1440  // 24h

// --- Measurement ---
#define DEFAULT_AVG_SAMPLE_COUNT 30  // median+3sigma-filtered mean of N sensor readings per wake
#define MIN_AVG_SAMPLE_COUNT     1
#define MAX_AVG_SAMPLE_COUNT     100  // config-field validation range; see A02_MAX_SAMPLES
                                        // in sensor_a02yyuw.c for the firmware's tighter sampling cap

// Diagnostic capture: when enabled, the current wake's raw (pre-outlier-
// -rejection) sensor samples are attached to the POST as
// "readings[current].samples_cm" so they can be studied offline to design a
// better outlier filter. Backlog entries never carry raw samples (only the
// aggregate is persisted to NVS). TEMPORARY -- flip back to 0 once the
// filter redesign is done, this isn't meant to ship permanently.
#define DEBUG_SEND_RAW_SAMPLES   1

// Mirrors the server's plausibility check (server-handoff/device.md): a
// reading outside this range can never be a real tank distance, so it's
// swapped for the sentinel below instead of being sent/queued as-is. Without
// this, a single implausible reading gets stuck at the head of the pending
// buffer forever -- the server rejects the whole batch atomically, so it's
// never acknowledged/cleared, and every later reading piles up behind it.
#define DISTANCE_MAX_PLAUSIBLE_CM 500.0f

// Minimum physically-possible sensor-to-water-surface distance for this
// tank's mounting (sensor height above max fill level). Unlike
// DISTANCE_MAX_PLAUSIBLE_CM (applied once, to the final averaged distance),
// this is applied per-raw-sample inside a02_measure_filtered_cm(),
// before outlier rejection even runs -- a reading below this is physically
// impossible (water can't be that close), so it's cheaper and more certain
// to drop it outright than to rely on statistics to catch it.
#define DISTANCE_MIN_PLAUSIBLE_CM 10.0f

#define DISTANCE_SENTINEL_CM    -1.0f  // "no valid reading this wake" per device.md

// --- Internal chip temperature sensor (ESP32C6 die/package, not ambient) ---
// -10..80C is the driver's most accurate hardware sub-range (+-1C); also
// comfortably covers an outdoor enclosure plus self-heating headroom. See
// chip_temp.h for why this isn't ambient temperature.
#define CHIP_TEMP_RANGE_MIN_C  -10
#define CHIP_TEMP_RANGE_MAX_C   80
#define CHIP_TEMP_SENTINEL_C  -999.0f  // sensor read failed

// --- Pending-measurement ring buffer (survives WiFi outages) ---
#define PENDING_BUFFER_SIZE     192  // 3.2h of outage at DEFAULT_WAKEUP_PERIOD_MIN's 1 min/measurement
