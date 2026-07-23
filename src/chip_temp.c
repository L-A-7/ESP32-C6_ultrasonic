#include "chip_temp.h"
#include "config.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "chip_temp";

float chip_temp_read_c(void) {
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tsens_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(CHIP_TEMP_RANGE_MIN_C, CHIP_TEMP_RANGE_MAX_C);

    if (temperature_sensor_install(&tsens_config, &tsens) != ESP_OK) {
        ESP_LOGW(TAG, "install failed -- sending sentinel");
        return CHIP_TEMP_SENTINEL_C;
    }

    float temp_c = CHIP_TEMP_SENTINEL_C;
    if (temperature_sensor_enable(tsens) == ESP_OK) {
        if (temperature_sensor_get_celsius(tsens, &temp_c) != ESP_OK) {
            ESP_LOGW(TAG, "read failed -- sending sentinel");
            temp_c = CHIP_TEMP_SENTINEL_C;
        }
        temperature_sensor_disable(tsens);
    } else {
        ESP_LOGW(TAG, "enable failed -- sending sentinel");
    }

    temperature_sensor_uninstall(tsens);
    return temp_c;
}
