#pragma once

#include <stdbool.h>

// Firmware sampling ceiling (see sensor_hcsr04.c) -- exposed so callers can
// size a buffer for hcsr04_measure_filtered_cm()'s optional out_samples.
#define HCSR04_MAX_SAMPLES 100

// Triggers a measurement and returns the distance in cm.
// Returns false if no echo was received (timeout).
bool hcsr04_measure_cm(float *distance_cm);

// Rejects samples more than 3 sigma from the median (RMS deviation from the
// mean), then returns the mean of what remains. count must be >= 1.
// If out_std_cm is non-NULL, it's set to the RMS deviation from the mean of
// the samples that survived outlier rejection (0 if fewer than 2 survive).
float hcsr04_filter_samples(const float *samples, int count, float *out_std_cm);

// Takes up to n readings and applies hcsr04_filter_samples.
// out_std_cm may be NULL if the caller doesn't need it.
// If out_samples is non-NULL, the raw per-echo distances (pre-outlier-
// rejection, timeouts excluded) are copied into it -- buffer must hold
// HCSR04_MAX_SAMPLES floats -- and *out_sample_count is set to how many
// were written. Pass NULL/NULL to skip (no extra cost either way).
// Returns false only if every reading timed out.
bool hcsr04_measure_filtered_cm(int n, float *distance_cm, float *out_std_cm,
                                 float *out_samples, int *out_sample_count);
