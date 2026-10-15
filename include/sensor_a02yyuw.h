#pragma once

#include <stdbool.h>

// Firmware sampling ceiling (see sensor_a02yyuw.c) -- exposed so callers can
// size a buffer for a02_measure_filtered_cm()'s optional out_samples.
#define A02_MAX_SAMPLES 100

// Waits for the next valid frame from the sensor and returns its distance
// in cm. Returns false if no valid frame arrived in time (sensor absent or
// silent, bad checksum, or out of the sensor's range).
bool a02_measure_cm(float *distance_cm);

// Rejects samples more than 3 sigma from the median (MAD-derived), then
// returns the mean of what remains. count must be >= 1.
// If out_std_cm is non-NULL, it's set to the RMS deviation from the mean of
// the samples that survived outlier rejection (0 if fewer than 2 survive).
float a02_filter_samples(const float *samples, int count, float *out_std_cm);

// Takes up to n readings and applies a02_filter_samples.
// out_std_cm may be NULL if the caller doesn't need it.
// If out_samples is non-NULL, one entry per attempted reading (n total, fully
// raw -- pre-outlier-rejection, nothing dropped) is copied into it: a valid
// distance, or DISTANCE_SENTINEL_CM (config.h) if that reading failed.
// Buffer must hold A02_MAX_SAMPLES floats. *out_sample_count is set to n.
// Pass NULL/NULL to skip (no extra cost either way).
// If the very first reading gets no frame at all, the sensor is treated as
// absent and the remaining readings are skipped (recorded as sentinels)
// rather than each waiting out its own timeout.
// Returns false only if every reading failed.
bool a02_measure_filtered_cm(int n, float *distance_cm, float *out_std_cm,
                             float *out_samples, int *out_sample_count);
