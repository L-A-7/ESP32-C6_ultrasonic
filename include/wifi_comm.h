#pragma once

#include <stdbool.h>
#include "storage.h"

// Connects to WiFi with a bounded timeout. Returns false on failure.
bool wifi_connect(void);
void wifi_disconnect(void);

// RSSI in dBm of the current AP connection. Only valid after wifi_connect()
// returns true. Typical range: -30 (excellent) to -90 (unusable).
int wifi_get_rssi(void);

// POSTs the current reading + backlog (oldest first, current last -- the
// server infers each entry's timestamp by counting back from receipt time
// in current_cfg->wakeup_period_min steps) plus current RSSI and config, as JSON.
// If the response updates wakeup_period_min/avg_sample_count (individually
// range-validated against config.h's MIN/MAX_*), out_cfg is filled with
// current_cfg overlaid by the valid changes and out_cfg_updated is set true;
// caller decides whether to persist it.
bool wifi_post_measurement(const pending_measurement_t *current,
                            const pending_measurement_t *backlog, int backlog_count,
                            const tank_config_t *current_cfg,
                            tank_config_t *out_cfg, bool *out_cfg_updated);
