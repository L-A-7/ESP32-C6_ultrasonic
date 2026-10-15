#pragma once

// Reads the ESP32C6's internal die/package temperature sensor. This is chip
// temperature, not ambient -- self-heating from WiFi TX and CPU load pushes
// it several degC above actual ambient. Diagnostic/device-health signal
// only, not suitable for ultrasonic speed-of-sound compensation.
// Returns CHIP_TEMP_SENTINEL_C (config.h) if the sensor read fails.
float chip_temp_read_c(void);
