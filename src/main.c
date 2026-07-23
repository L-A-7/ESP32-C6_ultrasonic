// Production firmware: wake -> measure -> connect -> send (+ flush backlog)
// -> deep sleep for wakeup_period_min. Deep sleep is effectively a full reset, so
// app_main() runs once per wake and never returns (ends in deep sleep).

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "config.h"
#include "battery.h"
#include "chip_temp.h"
#include "sensor_hcsr04.h"
#include "wifi_comm.h"
#include "storage.h"

static const char *TAG = "main";

static void deep_sleep_for(uint32_t wakeup_period_min) {
    ESP_LOGI(TAG, "sleeping for %u min", wakeup_period_min);
    esp_sleep_enable_timer_wakeup((uint64_t)wakeup_period_min * 60ULL * 1000000ULL);
    esp_deep_sleep_start();
}

void app_main(void) {
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 0 : 1);  // LED on: awake

    storage_init();

    tank_config_t cfg;
    storage_load_config(&cfg);
    ESP_LOGI(TAG, "config: wakeup_period_min=%u avg_sample_count=%u | wr_count=%u",
             cfg.wakeup_period_min, cfg.avg_sample_count, storage_get_wr_count());

    pending_measurement_t m = {0};
    m.battery_mv = battery_read_mv();
    m.chip_temp_c = chip_temp_read_c();
    if (!hcsr04_measure_filtered_cm(cfg.avg_sample_count, &m.distance_cm, &m.distance_std_cm)) {
        ESP_LOGW(TAG, "no echo (timeout) -- sending sentinel distance");
        m.distance_cm = DISTANCE_SENTINEL_CM;
        m.distance_std_cm = DISTANCE_SENTINEL_CM;
    } else if (m.distance_cm < 0 || m.distance_cm > HCSR04_MAX_PLAUSIBLE_CM) {
        ESP_LOGW(TAG, "distance %.2f cm out of plausible range -- sending sentinel distance",
                 m.distance_cm);
        m.distance_cm = DISTANCE_SENTINEL_CM;
        m.distance_std_cm = DISTANCE_SENTINEL_CM;
    }
    ESP_LOGI(TAG, "battery = %d mV | distance = %.2f cm (std %.2f) | chip_temp = %.2f C",
             m.battery_mv, m.distance_cm, m.distance_std_cm, m.chip_temp_c);

    bool posted = false;
    if (wifi_connect()) {
        int rssi = wifi_get_rssi();
        const char *quality = rssi >= -60 ? "good" : rssi >= -75 ? "fair" : "weak";
        ESP_LOGI(TAG, "WiFi RSSI = %d dBm (%s)", rssi, quality);

        static pending_measurement_t backlog[PENDING_BUFFER_SIZE];  // too big for the stack
        int backlog_count = storage_pop_all_pending(backlog, PENDING_BUFFER_SIZE);
        if (backlog_count > 0) {
            ESP_LOGI(TAG, "flushing %d backlogged measurement(s)", backlog_count);
        }

        tank_config_t new_cfg;
        bool cfg_updated = false;
        posted = wifi_post_measurement(&m, backlog, backlog_count, &cfg, &new_cfg, &cfg_updated);
        ESP_LOGI(TAG, "POST %s", posted ? "ok" : "failed");

        if (posted) {
            storage_clear_pending();
        }
        if (cfg_updated) {
            ESP_LOGI(TAG, "config updated by server: wakeup_period_min=%u avg_sample_count=%u",
                     new_cfg.wakeup_period_min, new_cfg.avg_sample_count);
            storage_save_config(&new_cfg);
            cfg = new_cfg;
        }

        wifi_disconnect();
    } else {
        ESP_LOGW(TAG, "WiFi connect failed, skipping POST");
    }

    if (!posted) {
        storage_push_pending(&m);
        ESP_LOGI(TAG, "measurement queued (wr_count=%u)", storage_get_wr_count());
    }

    gpio_set_level(LED_GPIO, LED_ACTIVE_LOW ? 1 : 0);  // LED off: about to sleep

    deep_sleep_for(cfg.wakeup_period_min);
}
