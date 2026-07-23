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

// --- HC-SR04 ultrasonic sensor ---
// VCC wired to BAT+ (not the 5V/VUSB pin -- that's unpowered on battery).
// ECHO divider is 1k (top, from ECHO) + 3k (bottom, to GND): sized for
// ECHO's HIGH level tracking BAT+ (~3.0-4.2V), not a fixed 5V.
#define HCSR04_TRIG_GPIO   GPIO_NUM_16  // D6
#define HCSR04_ECHO_GPIO   GPIO_NUM_17  // D7
// V2: MOSFET-switched VCC gated by GPIO21 (D3), reserved/unused in V1.

// --- Battery voltage divider (~1MOhm / 1MOhm), tapped from BAT+ ---
// Ratio is calibrated empirically against a multimeter (folds in both
// resistor tolerance and ADC error), not just the nominal 1:1 divider math.
#define BATTERY_ADC_CHANNEL     0        // D0 / GPIO0 / ADC1_CH0
#define BATTERY_DIVIDER_RATIO   2.051f

// --- Timing ---
#define DEFAULT_WAKEUP_PERIOD_MIN      15
#define MIN_WAKEUP_PERIOD_MIN           1
#define MAX_WAKEUP_PERIOD_MIN           1440  // 24h

// --- Measurement ---
#define DEFAULT_AVG_SAMPLE_COUNT 30  // median+3sigma-filtered mean of N HC-SR04 readings per wake
#define MIN_AVG_SAMPLE_COUNT     1
#define MAX_AVG_SAMPLE_COUNT     1000  // config-field validation range; see HCSR04_MAX_SAMPLES
                                        // in sensor_hcsr04.c for the firmware's tighter sampling cap

// Mirrors the server's plausibility check (server-handoff/device.md): a
// reading outside this range can never be a real tank distance, so it's
// swapped for the sentinel below instead of being sent/queued as-is. Without
// this, a single implausible reading gets stuck at the head of the pending
// buffer forever -- the server rejects the whole batch atomically, so it's
// never acknowledged/cleared, and every later reading piles up behind it.
#define HCSR04_MAX_PLAUSIBLE_CM 500.0f
#define DISTANCE_SENTINEL_CM    -1.0f  // "no valid reading this wake" per device.md

// --- Internal chip temperature sensor (ESP32C6 die/package, not ambient) ---
// -10..80C is the driver's most accurate hardware sub-range (+-1C); also
// comfortably covers an outdoor enclosure plus self-heating headroom. See
// chip_temp.h for why this isn't ambient temperature.
#define CHIP_TEMP_RANGE_MIN_C  -10
#define CHIP_TEMP_RANGE_MAX_C   80
#define CHIP_TEMP_SENTINEL_C  -999.0f  // sensor read failed

// --- Pending-measurement ring buffer (survives WiFi outages) ---
#define PENDING_BUFFER_SIZE     192  // 48h of outage at 15 min/measurement
