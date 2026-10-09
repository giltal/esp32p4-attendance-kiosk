#include "bsp_wifi.h"
#include "board_config.h"
#include "settings_store.h"
#include "bsp_rtc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include <string.h>
#include <time.h>

static const char *TAG = "BSP_WIFI";

static bool s_wifi_initialized = false;
static bool s_wifi_connected = false;
static bool s_force_ntp_sync = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started — connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "Disconnected — reason %d (SSID: %.*s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x)",
                     disc->reason,
                     disc->ssid_len, disc->ssid,
                     disc->bssid[0], disc->bssid[1], disc->bssid[2],
                     disc->bssid[3], disc->bssid[4], disc->bssid[5]);
            s_wifi_connected = false;
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_wifi_connect();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_wifi_connected = true;
    }
}

static void sntp_sync_cb(struct timeval *tv)
{
    /* If auto NTP is disabled and not a manual force, ignore the sync */
    const app_settings_t *s = settings_get();
    if (!s->auto_ntp && !s_force_ntp_sync) {
        ESP_LOGI(TAG, "SNTP sync received but auto_ntp is disabled — ignoring");
        return;
    }
    s_force_ntp_sync = false;

    ESP_LOGI(TAG, "SNTP time synced");

    /* Convert UTC to local struct tm */
    time_t now = tv->tv_sec;
    struct tm local;

    /* Apply timezone offset from settings */
    now += s->timezone * 3600;
    if (s->summer_time) {
        now += 3600;
    }
    gmtime_r(&now, &local);

    ESP_LOGI(TAG, "NTP time: %04d-%02d-%02d %02d:%02d:%02d (UTC%+d%s)",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec,
             s->timezone, s->summer_time ? "+DST" : "");

    /* Update the hardware RTC */
    esp_err_t ret = bsp_rtc_set_time(&local);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated from NTP");
    } else {
        ESP_LOGW(TAG, "Failed to update RTC: %s", esp_err_to_name(ret));
    }
}

static void start_sntp(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.windows.com");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
}

esp_err_t bsp_wifi_init(void)
{
    const app_settings_t *s = settings_get();

    if (s->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "No WiFi SSID configured — skipping WiFi");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing WiFi (SSID: \"%s\", pass len: %d)", s->wifi_ssid, (int)strlen(s->wifi_password));

    /* Network interface & event loop */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();

    /* WiFi init — esp_hosted provides the transport via SDIO to C6 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    /* Register event handlers */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL),
        TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, NULL),
        TAG, "ip event handler");

    /* Configure STA with credentials from settings */
    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, s->wifi_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, s->wifi_password, sizeof(wifi_cfg.sta.password));
    /* Use WPA_PSK as minimum threshold for wider router compatibility */
    wifi_cfg.sta.threshold.authmode = s->wifi_password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    /* SNTP will be started once we get an IP — but we can init it now,
       it will wait for connectivity internally */
    start_sntp();

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized — connection in progress");
    return ESP_OK;
}

bool bsp_wifi_is_connected(void)
{
    return s_wifi_connected;
}

void bsp_wifi_sync_time(void)
{
    if (!s_wifi_connected) {
        ESP_LOGW(TAG, "Cannot sync time — WiFi not connected");
        return;
    }
    ESP_LOGI(TAG, "Forcing SNTP re-sync (manual)");
    s_force_ntp_sync = true;
    esp_sntp_restart();
}

bool bsp_wifi_is_initialized(void)
{
    return s_wifi_initialized;
}

esp_err_t bsp_wifi_reconnect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "Cannot reconnect — empty SSID");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_initialized) {
        ESP_LOGI(TAG, "WiFi not yet initialized — doing full init");
        return bsp_wifi_init();
    }

    ESP_LOGI(TAG, "Hot-reconnect: SSID=\"%s\"", ssid);

    esp_wifi_disconnect();
    s_wifi_connected = false;

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password ? password : "", sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");

    ESP_LOGI(TAG, "Reconnect initiated with new credentials");
    return ESP_OK;
}
