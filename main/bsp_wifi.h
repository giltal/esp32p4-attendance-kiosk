#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize WiFi via ESP-Hosted (ESP32-C6 SDIO co-processor).
 * Starts the WiFi STA interface and connects using credentials from settings.
 * After connection, starts SNTP and updates the RTC when time is synced.
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t bsp_wifi_init(void);

/** Returns true if WiFi STA is connected and has an IP address. */
bool bsp_wifi_is_connected(void);

/** Returns true if WiFi has been initialized (even if not connected). */
bool bsp_wifi_is_initialized(void);

/** Trigger an immediate SNTP time sync (re-syncs from NTP servers). */
void bsp_wifi_sync_time(void);

/**
 * Reconnect WiFi with new credentials (hot-reconnect, no reboot needed).
 * If WiFi is already initialized, disconnects and reconfigures STA.
 * If WiFi was never initialized, performs full init.
 *
 * @param ssid     New SSID (must not be empty).
 * @param password New password (empty string for open networks).
 * @return ESP_OK on success (connection attempt started), error code on failure.
 */
esp_err_t bsp_wifi_reconnect(const char *ssid, const char *password);
