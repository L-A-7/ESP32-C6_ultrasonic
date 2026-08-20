#include "sensor_hcsr04.h"
#include "config.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define ECHO_START_TIMEOUT_US   30000  // max wait for echo to go high
#define ECHO_PULSE_TIMEOUT_US   38000  // ~6.5 m round trip, longer = no object/out of range
#define SAMPLE_SPACING_MS       60     // let echoes settle before retriggering
// Firmware sampling ceiling -- deliberately tighter than the server-facing
// config validation range (MAX_AVG_SAMPLE_COUNT in config.h, up to 1000):
// at ~65ms/sample, 100 samples is ~6.5s of ranging, which is already a lot
// relative to the WiFi phase. n is clamped to this regardless of config.
#define MAX_SAMPLES              HCSR04_MAX_SAMPLES
#define OUTLIER_SIGMA_MULT      3.0f

static bool s_initialized = false;

static void hcsr04_init(void) {
    gpio_reset_pin(HCSR04_TRIG_GPIO);
    gpio_set_direction(HCSR04_TRIG_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(HCSR04_TRIG_GPIO, 0);

    gpio_reset_pin(HCSR04_ECHO_GPIO);
    gpio_set_direction(HCSR04_ECHO_GPIO, GPIO_MODE_INPUT);

    s_initialized = true;
}

bool hcsr04_measure_cm(float *distance_cm) {
    if (!s_initialized) {
        hcsr04_init();
    }

    gpio_set_level(HCSR04_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(HCSR04_TRIG_GPIO, 0);

    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(HCSR04_ECHO_GPIO) == 0) {
        if (esp_timer_get_time() - wait_start > ECHO_START_TIMEOUT_US) {
            return false;  // sensor did not respond
        }
    }

    int64_t pulse_start = esp_timer_get_time();
    while (gpio_get_level(HCSR04_ECHO_GPIO) == 1) {
        if (esp_timer_get_time() - pulse_start > ECHO_PULSE_TIMEOUT_US) {
            return false;  // out of range
        }
    }
    int64_t pulse_us = esp_timer_get_time() - pulse_start;

    *distance_cm = pulse_us / 58.0f;  // standard HC-SR04 formula
    return true;
}

float hcsr04_filter_samples(const float *samples, int count, float *out_std_cm) {
    if (count <= 1) {
        if (out_std_cm) {
            *out_std_cm = 0.0f;
        }
        return count == 1 ? samples[0] : 0.0f;
    }
    if (count > MAX_SAMPLES) {
        count = MAX_SAMPLES;
    }

    // median: robust center, unaffected by the outliers we're about to reject
    float sorted[MAX_SAMPLES];
    memcpy(sorted, samples, count * sizeof(float));
    for (int i = 1; i < count; i++) {
        float key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    float median = sorted[count / 2];

    // sigma: RMS deviation from the mean
    float sum = 0;
    for (int i = 0; i < count; i++) {
        sum += samples[i];
    }
    float mean = sum / count;

    float sq_sum = 0;
    for (int i = 0; i < count; i++) {
        float diff = samples[i] - mean;
        sq_sum += diff * diff;
    }
    float sigma = sqrtf(sq_sum / count);

    // reject anything more than 3 sigma from the median, average the rest
    float clean_sum = 0;
    int clean_count = 0;
    for (int i = 0; i < count; i++) {
        if (fabsf(samples[i] - median) <= OUTLIER_SIGMA_MULT * sigma) {
            clean_sum += samples[i];
            clean_count++;
        }
    }

    float clean_mean = clean_count > 0 ? clean_sum / clean_count : median;

    if (out_std_cm) {
        float clean_sq_sum = 0;
        int clean_sq_count = 0;
        for (int i = 0; i < count; i++) {
            if (fabsf(samples[i] - median) <= OUTLIER_SIGMA_MULT * sigma) {
                float diff = samples[i] - clean_mean;
                clean_sq_sum += diff * diff;
                clean_sq_count++;
            }
        }
        *out_std_cm = clean_sq_count > 1 ? sqrtf(clean_sq_sum / clean_sq_count) : 0.0f;
    }

    return clean_mean;
}

bool hcsr04_measure_filtered_cm(int n, float *distance_cm, float *out_std_cm,
                                 float *out_samples, int *out_sample_count) {
    if (n < 1) {
        n = 1;
    } else if (n > MAX_SAMPLES) {
        n = MAX_SAMPLES;
    }

    float samples[MAX_SAMPLES];
    int valid = 0;

    for (int i = 0; i < n; i++) {
        float d;
        if (hcsr04_measure_cm(&d)) {
            samples[valid++] = d;
        }
        if (i < n - 1) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_SPACING_MS));
        }
    }

    if (out_samples) {
        memcpy(out_samples, samples, valid * sizeof(float));
        *out_sample_count = valid;
    }

    if (valid == 0) {
        return false;
    }

    *distance_cm = hcsr04_filter_samples(samples, valid, out_std_cm);
    return true;
}
