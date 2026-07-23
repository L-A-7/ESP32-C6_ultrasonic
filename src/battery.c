#include "battery.h"
#include "config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

int battery_read_mv(void) {
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg));

    adc_cali_handle_t cali_handle = NULL;
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    bool calibrated = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK);

    int raw = 0, pin_mv = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw));

    if (calibrated) {
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw, &pin_mv));
        adc_cali_delete_scheme_curve_fitting(cali_handle);
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, using uncalibrated approximation");
        pin_mv = raw * 3300 / 4095;
    }

    adc_oneshot_del_unit(adc_handle);

    return (int)(pin_mv * BATTERY_DIVIDER_RATIO);
}
