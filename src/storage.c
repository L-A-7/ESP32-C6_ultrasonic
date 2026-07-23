#include "storage.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "storage";

#define NVS_NAMESPACE "tank"

// NVS key names must fit NVS_KEY_NAME_MAX_SIZE-1 (15 chars) -- longer keys
// fail every nvs_set_*/nvs_get_* call with ESP_ERR_NVS_KEY_TOO_LONG, and that
// return value is easy to leave unchecked (NVS calls "always work" in
// testing against a fresh, empty flash). This bit us once already: the
// original "wakeup_period_min" (17 chars) and "avg_sample_count" (16 chars)
// keys silently no-op'd on every load/save, so the device ran at
// DEFAULT_WAKEUP_PERIOD_MIN (1 min) forever instead of the configured value.
// Route every NVS key literal through NVS_KEY_STATIC_ASSERT so a future key
// that's too long fails the build instead of failing silently at runtime.
#define NVS_KEY_MAX_LEN 15  // NVS_KEY_NAME_MAX_SIZE (16) - 1 for the null terminator
#define NVS_KEY_STATIC_ASSERT(key) \
    _Static_assert(sizeof(key) - 1 <= NVS_KEY_MAX_LEN, #key " exceeds NVS's 15-char key limit")

#define NVS_KEY_WAKEUP_PERIOD_MIN "wake_period_min"
#define NVS_KEY_AVG_SAMPLE_COUNT  "sample_count"
#define NVS_KEY_WR_COUNT          "wr_count"
#define NVS_KEY_PENDING_BUF       "pending_buf"

NVS_KEY_STATIC_ASSERT(NVS_KEY_WAKEUP_PERIOD_MIN);
NVS_KEY_STATIC_ASSERT(NVS_KEY_AVG_SAMPLE_COUNT);
NVS_KEY_STATIC_ASSERT(NVS_KEY_WR_COUNT);
NVS_KEY_STATIC_ASSERT(NVS_KEY_PENDING_BUF);

typedef struct {
    uint32_t count;       // valid entries, 0..PENDING_BUFFER_SIZE
    uint32_t next_index;  // ring cursor: where the next push lands
    pending_measurement_t entries[PENDING_BUFFER_SIZE];
} pending_buffer_t;

static uint32_t s_wr_count = 0;

static void nvs_init_safe(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void storage_init(void) {
    nvs_init_safe();

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, NVS_KEY_WR_COUNT, &s_wr_count);
        nvs_close(handle);
    }
}

uint32_t storage_get_wr_count(void) {
    return s_wr_count;
}

// Writes a blob together with the updated wr_count in a single NVS
// transaction, so tracking write-count costs nothing extra.
static bool commit_blob(const char *key, const void *data, size_t len) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    nvs_set_blob(handle, key, data, len);
    nvs_set_u32(handle, NVS_KEY_WR_COUNT, s_wr_count + 1);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    s_wr_count++;
    return true;
}

void storage_load_config(tank_config_t *cfg) {
    cfg->wakeup_period_min = DEFAULT_WAKEUP_PERIOD_MIN;
    cfg->avg_sample_count = DEFAULT_AVG_SAMPLE_COUNT;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    esp_err_t err;
    err = nvs_get_u32(handle, NVS_KEY_WAKEUP_PERIOD_MIN, &cfg->wakeup_period_min);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "load %s failed: %s -- falling back to default",
                 NVS_KEY_WAKEUP_PERIOD_MIN, esp_err_to_name(err));
    }
    err = nvs_get_u32(handle, NVS_KEY_AVG_SAMPLE_COUNT, &cfg->avg_sample_count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "load %s failed: %s -- falling back to default",
                 NVS_KEY_AVG_SAMPLE_COUNT, esp_err_to_name(err));
    }
    nvs_close(handle);
}

void storage_save_config(const tank_config_t *cfg) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    esp_err_t err;
    err = nvs_set_u32(handle, NVS_KEY_WAKEUP_PERIOD_MIN, cfg->wakeup_period_min);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save %s failed: %s", NVS_KEY_WAKEUP_PERIOD_MIN, esp_err_to_name(err));
    }
    err = nvs_set_u32(handle, NVS_KEY_AVG_SAMPLE_COUNT, cfg->avg_sample_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save %s failed: %s", NVS_KEY_AVG_SAMPLE_COUNT, esp_err_to_name(err));
    }
    nvs_set_u32(handle, NVS_KEY_WR_COUNT, s_wr_count + 1);
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        s_wr_count++;
    }
}

static bool load_pending_buffer(pending_buffer_t *buf) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        memset(buf, 0, sizeof(*buf));
        return false;
    }
    size_t len = sizeof(*buf);
    esp_err_t err = nvs_get_blob(handle, NVS_KEY_PENDING_BUF, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(*buf)) {
        memset(buf, 0, sizeof(*buf));
        return false;
    }
    return true;
}

static bool save_pending_buffer(const pending_buffer_t *buf) {
    return commit_blob(NVS_KEY_PENDING_BUF, buf, sizeof(*buf));
}

bool storage_push_pending(const pending_measurement_t *m) {
    pending_buffer_t buf;
    load_pending_buffer(&buf);  // not found -> zeroed, treated as empty

    buf.entries[buf.next_index] = *m;
    buf.next_index = (buf.next_index + 1) % PENDING_BUFFER_SIZE;
    if (buf.count < PENDING_BUFFER_SIZE) {
        buf.count++;
    }

    return save_pending_buffer(&buf);
}

int storage_pop_all_pending(pending_measurement_t *out, int max_count) {
    pending_buffer_t buf;
    if (!load_pending_buffer(&buf) || buf.count == 0) {
        return 0;
    }

    int available = (int)buf.count;
    int n = available < max_count ? available : max_count;
    uint32_t oldest = (buf.next_index + PENDING_BUFFER_SIZE - buf.count) % PENDING_BUFFER_SIZE;
    for (int i = 0; i < n; i++) {
        out[i] = buf.entries[(oldest + i) % PENDING_BUFFER_SIZE];
    }
    return n;
}

void storage_clear_pending(void) {
    pending_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    save_pending_buffer(&buf);
}
