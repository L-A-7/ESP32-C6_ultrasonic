#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t wakeup_period_min;
    uint32_t avg_sample_count;  // sensor readings averaged (median) per wake
} tank_config_t;

typedef struct {
    float distance_cm;
    float distance_std_cm;  // spread across the avg_sample_count sensor readings this wake
    int battery_mv;
    float chip_temp_c;
} pending_measurement_t;

void storage_init(void);

// Config (NVS-backed, rarely written).
void storage_load_config(tank_config_t *cfg);
void storage_save_config(const tank_config_t *cfg);

// Exact count of NVS flash writes so far (NVS-backed, piggybacked onto every
// other flash write below -- costs nothing extra since NVS batches all
// nvs_set_* calls before a commit into one transaction).
uint32_t storage_get_wr_count(void);

// Pending-measurement ring buffer (NVS-backed, for WiFi-outage backlog).
bool storage_push_pending(const pending_measurement_t *m);
int storage_pop_all_pending(pending_measurement_t *out, int max_count);
void storage_clear_pending(void);
