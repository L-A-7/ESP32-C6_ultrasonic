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
// Rejection threshold width, in MAD-derived sigma-equivalents (see
// hcsr04_filter_samples -- MAD_TO_SIGMA/MAD_ABS_FLOOR_CM there).
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

// Sorts arr in place (insertion sort -- count is small, <= MAX_SAMPLES) and
// returns its middle element (upper median for even count).
static float median_inplace(float *arr, int count) {
    for (int i = 1; i < count; i++) {
        float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    return arr[count / 2];
}

// Consistency constant that scales MAD (median absolute deviation) to be
// comparable to a normal distribution's sigma, for well-behaved data.
#define MAD_TO_SIGMA       1.4826f
// Floor on the rejection threshold. The sensor's real per-ping noise is
// sub-mm, so MAD alone can collapse to ~0 on a clean wake -- without a
// floor that would reject everything but an exact median match.
#define MAD_ABS_FLOOR_CM   1.0f

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
    float median = median_inplace(sorted, count);

    // MAD: robust spread, computed from the median rather than the mean, so
    // (unlike RMS-from-mean sigma) it isn't itself inflated by the outliers
    // it's used to screen for -- a minority cluster of bad samples barely
    // moves it, since it only shifts where those samples land in sort order,
    // not by how far off they are. RMS-from-mean sigma has a ~12.7%
    // contamination breakdown point (a shared deviation among >=~13% of
    // samples inflates sigma enough that all of them survive rejection,
    // regardless of magnitude); MAD's breakdown point is 50%.
    float deviations[MAX_SAMPLES];
    for (int i = 0; i < count; i++) {
        deviations[i] = fabsf(samples[i] - median);
    }
    float mad = median_inplace(deviations, count);
    float threshold = OUTLIER_SIGMA_MULT * MAD_TO_SIGMA * mad;
    if (threshold < MAD_ABS_FLOOR_CM) {
        threshold = MAD_ABS_FLOOR_CM;
    }

    // reject anything farther than threshold from the median, average the rest
    float clean_sum = 0;
    int clean_count = 0;
    for (int i = 0; i < count; i++) {
        if (fabsf(samples[i] - median) <= threshold) {
            clean_sum += samples[i];
            clean_count++;
        }
    }

    float clean_mean = clean_count > 0 ? clean_sum / clean_count : median;

    if (out_std_cm) {
        float clean_sq_sum = 0;
        int clean_sq_count = 0;
        for (int i = 0; i < count; i++) {
            if (fabsf(samples[i] - median) <= threshold) {
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
        bool ok = hcsr04_measure_cm(&d);
        // HCSR04_MIN_PLAUSIBLE_CM: a ping below this is physically impossible
        // for this tank's mounting, so it's dropped before it ever reaches
        // outlier rejection -- cheaper and more certain than relying on
        // statistics to catch it. out_samples stays fully raw regardless
        // (diagnostic data shouldn't have this filtering baked in).
        if (ok && d >= HCSR04_MIN_PLAUSIBLE_CM) {
            samples[valid++] = d;
        }
        if (out_samples) {
            out_samples[i] = ok ? d : DISTANCE_SENTINEL_CM;
        }
        if (i < n - 1) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_SPACING_MS));
        }
    }

    if (out_sample_count) {
        *out_sample_count = n;
    }

    if (valid == 0) {
        return false;
    }

    *distance_cm = hcsr04_filter_samples(samples, valid, out_std_cm);
    return true;
}
