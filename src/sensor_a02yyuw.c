#include "sensor_a02yyuw.h"
#include "config.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "a02yyuw";

// A02YYUW UART protocol: 9600 8N1, the sensor streams 4-byte frames on its
// own (no trigger): 0xFF, distance_mm high byte, low byte, checksum =
// (0xFF + high + low) & 0xFF.
#define A02_BAUD                9600
#define A02_FRAME_HEADER        0xFF
#define A02_RX_BUF_SIZE         256   // driver minimum is > the 128-byte HW FIFO
// Datasheet range. Frames outside it are the sensor's own "no target" /
// blind-zone values, not distances.
#define A02_MIN_RANGE_MM        30
#define A02_MAX_RANGE_MM        4500
// Frames arrive every ~100ms in real-time mode (~100-300ms processed), so
// a missing frame is declared after a generous multiple of that.
#define FRAME_TIMEOUT_MS        500
// Mode change on the sensor's RX pin needs a moment to take effect; frames
// already in flight were produced under the old mode and are discarded.
#define MODE_SETTLE_MS          150
// Firmware sampling ceiling -- deliberately tighter than the server-facing
// config validation range (MAX_AVG_SAMPLE_COUNT in config.h): at ~100ms per
// frame, 100 samples is ~10s of ranging, which is already a lot relative to
// the WiFi phase. n is clamped to this regardless of config.
#define MAX_SAMPLES              A02_MAX_SAMPLES
// Rejection threshold width, in MAD-derived sigma-equivalents (see
// a02_filter_samples -- MAD_TO_SIGMA/MAD_ABS_FLOOR_CM there).
#define OUTLIER_SIGMA_MULT      3.0f

static bool s_initialized = false;

static void a02_init(void) {
    // GPIO16/17 are UART0's default console pins on the ESP32-C6. Resetting
    // GPIO16 to a plain output detaches the console TX from it, so log
    // output doesn't toggle the sensor's mode pin.
    gpio_reset_pin(A02_MODE_GPIO);
    gpio_set_direction(A02_MODE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(A02_MODE_GPIO, A02_REALTIME_MODE ? 0 : 1);

    const uart_config_t uart_cfg = {
        .baud_rate = A02_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(A02_UART_NUM, A02_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(A02_UART_NUM, &uart_cfg));
    // RX only: the sensor's own RX pin is the mode select, driven as a GPIO above.
    ESP_ERROR_CHECK(uart_set_pin(A02_UART_NUM, UART_PIN_NO_CHANGE, A02_TX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // UART idles high. The pull-up keeps the line quiet when the sensor is
    // unplugged, instead of a floating input decoding noise as bytes.
    gpio_pullup_en(A02_TX_GPIO);

    vTaskDelay(pdMS_TO_TICKS(MODE_SETTLE_MS));
    uart_flush_input(A02_UART_NUM);

    s_initialized = true;
}

// Reads one byte, giving up at deadline_us (esp_timer time base).
static bool read_byte(uint8_t *b, int64_t deadline_us) {
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return false;
    }
    TickType_t ticks = pdMS_TO_TICKS(remaining_us / 1000);
    return uart_read_bytes(A02_UART_NUM, b, 1, ticks > 0 ? ticks : 1) == 1;
}

// Waits up to timeout_ms for a checksum-valid frame. Resyncs on the 0xFF
// header byte, so a frame joined mid-way or a corrupted one is skipped
// rather than misread. *got_frame tells "no data at all" (sensor absent)
// apart from "data, but out of range".
static bool read_frame_cm(float *distance_cm, int timeout_ms, bool *got_frame) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    *got_frame = false;

    uint8_t b;
    while (read_byte(&b, deadline)) {
        if (b != A02_FRAME_HEADER) {
            continue;
        }
        uint8_t hi, lo, sum;
        if (!read_byte(&hi, deadline) || !read_byte(&lo, deadline) ||
            !read_byte(&sum, deadline)) {
            return false;
        }
        if (((A02_FRAME_HEADER + hi + lo) & 0xFF) != sum) {
            continue;  // corrupt, or the 0xFF was a data byte: resync
        }
        *got_frame = true;
        int mm = (hi << 8) | lo;
        if (mm < A02_MIN_RANGE_MM || mm > A02_MAX_RANGE_MM) {
            return false;
        }
        *distance_cm = mm / 10.0f;
        return true;
    }
    return false;
}

bool a02_measure_cm(float *distance_cm) {
    if (!s_initialized) {
        a02_init();
    }
    bool got_frame;
    return read_frame_cm(distance_cm, FRAME_TIMEOUT_MS, &got_frame);
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
// Floor on the rejection threshold. The sensor's real per-reading noise is
// around its 1mm resolution, so MAD alone can collapse to ~0 on a clean
// wake -- without a floor that would reject everything but an exact median
// match.
#define MAD_ABS_FLOOR_CM   1.0f

float a02_filter_samples(const float *samples, int count, float *out_std_cm) {
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

bool a02_measure_filtered_cm(int n, float *distance_cm, float *out_std_cm,
                             float *out_samples, int *out_sample_count) {
    if (!s_initialized) {
        a02_init();
    }
    if (n < 1) {
        n = 1;
    } else if (n > MAX_SAMPLES) {
        n = MAX_SAMPLES;
    }

    float samples[MAX_SAMPLES];
    int valid = 0;
    bool sensor_absent = false;

    for (int i = 0; i < n; i++) {
        float d = 0;
        bool ok = false;
        if (!sensor_absent) {
            bool got_frame;
            ok = read_frame_cm(&d, FRAME_TIMEOUT_MS, &got_frame);
            if (i == 0 && !got_frame) {
                ESP_LOGW(TAG, "no frame from sensor -- unplugged or unpowered?");
                sensor_absent = true;
            }
        }
        // DISTANCE_MIN_PLAUSIBLE_CM: a reading below this is physically
        // impossible for this tank's mounting, so it's dropped before it ever
        // reaches outlier rejection -- cheaper and more certain than relying
        // on statistics to catch it. out_samples stays fully raw regardless
        // (diagnostic data shouldn't have this filtering baked in).
        if (ok && d >= DISTANCE_MIN_PLAUSIBLE_CM) {
            samples[valid++] = d;
        }
        if (out_samples) {
            out_samples[i] = ok ? d : DISTANCE_SENTINEL_CM;
        }
    }

    if (out_sample_count) {
        *out_sample_count = n;
    }

    if (valid == 0) {
        return false;
    }

    *distance_cm = a02_filter_samples(samples, valid, out_std_cm);
    return true;
}
