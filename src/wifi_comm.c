#include "wifi_comm.h"
#include "secrets.h"
#include "config.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "wifi_comm";

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1

#define HTTP_RESPONSE_BUF_SIZE 1024
static char s_response_buf[HTTP_RESPONSE_BUF_SIZE];
static int s_response_len = 0;

static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ip_event = (ip_event_got_ip_t *)event_data;
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(ip_event->esp_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            ESP_LOGI(TAG, "DNS server: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool s_wifi_stack_initialized = false;

// Selects the external u.FL antenna over the onboard ceramic one via the
// RF switch. Must run before esp_wifi_start() and on every wake, since deep
// sleep resets all GPIO state.
static void select_external_antenna(void) {
    gpio_reset_pin(WIFI_ANT_ENABLE_GPIO);
    gpio_set_direction(WIFI_ANT_ENABLE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_ANT_ENABLE_GPIO, 0);  // activate RF switch control

    vTaskDelay(pdMS_TO_TICKS(100));  // let the RF switch settle before selecting

    gpio_reset_pin(WIFI_ANT_CONFIG_GPIO);
    gpio_set_direction(WIFI_ANT_CONFIG_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_ANT_CONFIG_GPIO, 1);  // route to external antenna
}

static void wifi_stack_init_once(void) {
    if (s_wifi_stack_initialized) {
        return;
    }

    select_external_antenna();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    s_wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    s_wifi_stack_initialized = true;
}

bool wifi_connect(void) {
    wifi_stack_init_once();

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    if (connected) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connect failed/timeout");
    }
    return connected;
}

void wifi_disconnect(void) {
    esp_wifi_stop();
}

int wifi_get_rssi(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int space_left = HTTP_RESPONSE_BUF_SIZE - 1 - s_response_len;
        int copy_len = evt->data_len < space_left ? evt->data_len : space_left;
        if (copy_len > 0) {
            memcpy(s_response_buf + s_response_len, evt->data, copy_len);
            s_response_len += copy_len;
        }
    }
    return ESP_OK;
}

// Minimal growable-buffer JSON writer -- payload is a small, fixed, self
// controlled schema, so a full JSON library isn't warranted.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} json_writer_t;

static void jw_appendf(json_writer_t *w, const char *fmt, ...) {
    if (w->len >= w->cap) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, args);
    va_end(args);
    if (n > 0) {
        size_t written = (size_t)n < (w->cap - w->len) ? (size_t)n : (w->cap - w->len);
        w->len += written;
    }
}

// readings: backlog oldest-first, current last. Server infers each entry's
// time by counting back from receipt time in wakeup_period_min steps.
// raw_samples/raw_sample_count (may be NULL/0) attach a "samples_cm" array
// to the current reading only -- backlog entries never have raw samples
// available (not persisted, see storage.h's pending_measurement_t).
static char *build_payload(const pending_measurement_t *current,
                            const pending_measurement_t *backlog, int backlog_count,
                            const tank_config_t *cfg, int rssi,
                            const float *raw_samples, int raw_sample_count,
                            size_t *out_len) {
    size_t cap = 200 + (size_t)(backlog_count + 1) * 100 + (size_t)raw_sample_count * 8;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    json_writer_t w = { .buf = buf, .cap = cap, .len = 0 };
    jw_appendf(&w, "{\"config\":{\"wakeup_period_min\":%u,\"avg_sample_count\":%u},",
               cfg->wakeup_period_min, cfg->avg_sample_count);
    jw_appendf(&w, "\"rssi\":%d,\"readings\":[", rssi);
    for (int i = 0; i < backlog_count; i++) {
        jw_appendf(&w, "{\"distance_cm\":%.2f,\"distance_std_cm\":%.2f,\"battery_mv\":%d,\"chip_temp_c\":%.2f},",
                   backlog[i].distance_cm, backlog[i].distance_std_cm,
                   backlog[i].battery_mv, backlog[i].chip_temp_c);
    }
    jw_appendf(&w, "{\"distance_cm\":%.2f,\"distance_std_cm\":%.2f,\"battery_mv\":%d,\"chip_temp_c\":%.2f",
               current->distance_cm, current->distance_std_cm,
               current->battery_mv, current->chip_temp_c);
#if DEBUG_SEND_RAW_SAMPLES
    if (raw_samples && raw_sample_count > 0) {
        jw_appendf(&w, ",\"samples_cm\":[");
        for (int i = 0; i < raw_sample_count; i++) {
            jw_appendf(&w, i == 0 ? "%.2f" : ",%.2f", raw_samples[i]);
        }
        jw_appendf(&w, "]");
    }
#endif
    jw_appendf(&w, "}]}");

    *out_len = w.len;
    return buf;
}

// Looks for "key": <integer> anywhere in json (single-level scan, matches
// our flat response schema). Returns false if the key is absent or the
// value isn't a valid non-negative integer.
static bool extract_json_u32(const char *json, const char *key, uint32_t *out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    char *end;
    long val = strtol(p, &end, 10);
    if (end == p || val < 0) {
        return false;
    }
    *out = (uint32_t)val;
    return true;
}

// Applies key from the response to *cfg if present and within
// [min_val, max_val]. Returns true if a valid, in-range value was applied.
static bool apply_validated_u32(const char *json, const char *key,
                                 uint32_t min_val, uint32_t max_val, uint32_t *field) {
    uint32_t val;
    if (!extract_json_u32(json, key, &val)) {
        return false;
    }
    if (val < min_val || val > max_val) {
        ESP_LOGW(TAG, "server sent %s=%u, out of range [%u,%u] -- ignoring",
                 key, val, min_val, max_val);
        return false;
    }
    *field = val;
    return true;
}

bool wifi_post_measurement(const pending_measurement_t *current,
                            const pending_measurement_t *backlog, int backlog_count,
                            const tank_config_t *current_cfg,
                            const float *raw_samples, int raw_sample_count,
                            tank_config_t *out_cfg, bool *out_cfg_updated) {
    size_t payload_len;
    char *payload = build_payload(current, backlog, backlog_count, current_cfg,
                                   wifi_get_rssi(), raw_samples, raw_sample_count,
                                   &payload_len);
    if (!payload) {
        ESP_LOGW(TAG, "payload allocation failed");
        return false;
    }

    s_response_len = 0;

    esp_http_client_config_t http_cfg = {
        .url = POST_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)payload_len);

    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ok = (status >= 200 && status < 300);
        s_response_buf[s_response_len] = '\0';
        ESP_LOGI(TAG, "POST status=%d, response: %s", status, s_response_buf);
    } else {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(payload);

    if (out_cfg) {
        *out_cfg = *current_cfg;
    }
    if (out_cfg_updated) {
        *out_cfg_updated = false;
    }
    if (ok && out_cfg) {
        bool changed = false;
        changed |= apply_validated_u32(s_response_buf, "wakeup_period_min",
                                        MIN_WAKEUP_PERIOD_MIN, MAX_WAKEUP_PERIOD_MIN, &out_cfg->wakeup_period_min);
        changed |= apply_validated_u32(s_response_buf, "avg_sample_count",
                                        MIN_AVG_SAMPLE_COUNT, MAX_AVG_SAMPLE_COUNT,
                                        &out_cfg->avg_sample_count);
        if (out_cfg_updated) {
            *out_cfg_updated = changed;
        }
    }

    return ok;
}
