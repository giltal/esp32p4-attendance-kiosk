#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_touch.h"
#include "bsp_audio.h"
#include "bsp_sdcard.h"
#include "bsp_rtc.h"
#include "bsp_wifi.h"
#include "settings_store.h"
#include "student_db.h"
#include "attendance.h"
#include "fp_service.h"
#include "ui_common.h"
#include "ui_main.h"
#include "ui_admin.h"
#include "ui_camera.h"
#include "face_db.h"
#include "face_service.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define PIN_DIAG 0   /* 1 = run sensor-line diagnostic (pin_diag.c) at boot */
#define CAMERA_SELFTEST 0   /* 1 = headless camera test at boot (opens admin WITHOUT PIN — never ship) */
#if PIN_DIAG
#include "pin_diag.h"
#endif

static const char *TAG = "MAIN";

/* ── LVGL setup (same as HebClock) ──────────────────────────────────────── */

static esp_err_t lvgl_init(esp_lcd_panel_handle_t panel,
                           esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_touch_handle_t  touch)
{
    ESP_LOGI(TAG, "Initializing LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 16384;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = panel_io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H_RES * 50,
        .double_buffer = false,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .swap_bytes  = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }

    if (touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = touch,
        };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGW(TAG, "Failed to add LVGL touch input");
        }
    }
    return ESP_OK;
}

/* ── Fingerprint events → UI (called from the fp task) ──────────────────── */

static void on_fp_event(const fp_event_t *e)
{
    if (!lvgl_port_lock(0)) return;

    switch (e->type) {
    case FP_EVT_SENSOR_STATUS:
        ui_main_handle_event(e);
        ui_admin_handle_event(e);
        break;
    case FP_EVT_SCAN_MATCH:
    case FP_EVT_SCAN_UNKNOWN:
    case FP_EVT_SCAN_BAD_IMAGE:
        ui_main_handle_event(e);
        break;
    default:
        ui_admin_handle_event(e);
        break;
    }

    lvgl_port_unlock();
}

/* Face enrollment progress → admin UI (called from the camera task) */
static void on_face_enroll(const face_enroll_evt_t *e)
{
    if (!lvgl_port_lock(0)) return;
    ui_admin_handle_face_enroll(e);
    lvgl_port_unlock();
}

/* ── Main entry ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Fingerprint Attendance v%s — Guition JC1060P470C ===", APP_VERSION);

    /* Why did we (re)start? Helps diagnose crashes that happen while nobody watches the log. */
    static const char *const reasons[] = {
        [ESP_RST_UNKNOWN] = "unknown", [ESP_RST_POWERON] = "power-on", [ESP_RST_EXT] = "external pin",
        [ESP_RST_SW] = "software restart", [ESP_RST_PANIC] = "CRASH (panic)", [ESP_RST_INT_WDT] = "interrupt watchdog",
        [ESP_RST_TASK_WDT] = "task watchdog", [ESP_RST_WDT] = "other watchdog", [ESP_RST_DEEPSLEEP] = "deep sleep",
        [ESP_RST_BROWNOUT] = "BROWN-OUT (power dip)", [ESP_RST_SDIO] = "SDIO",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "Reset reason: %s (%d)",
             (rr < sizeof(reasons) / sizeof(reasons[0]) && reasons[rr]) ? reasons[rr] : "other", rr);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* I2C bus (touch, audio codec, RTC) */
    ESP_ERROR_CHECK(bsp_i2c_init());

    /* Display + touch + LVGL */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    ESP_ERROR_CHECK(bsp_display_init(&panel, &dsi_bus, &panel_io));

    esp_lcd_touch_handle_t touch = NULL;
#if ENABLE_TOUCH
    if (bsp_touch_init(&touch) != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed — continuing without touch");
        touch = NULL;
    }
#endif
    ESP_ERROR_CHECK(lvgl_init(panel, panel_io, touch));

#if ENABLE_AUDIO
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed — continuing without audio");
    }
#endif

#if ENABLE_RTC
    if (bsp_rtc_init() != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed — timestamps will be wrong");
    }
#endif

    bool sd_ok = false;
#if ENABLE_SD_CARD
    sd_ok = (bsp_sdcard_init() == ESP_OK);
    if (!sd_ok) ESP_LOGE(TAG, "SD card init failed — students/logs will NOT be saved");
#endif

    /* Application data */
    settings_load(settings_get());
    bsp_audio_set_volume(settings_get()->volume);
    student_db_load();
    attendance_init();
    face_db_load();
    face_service_init(on_fp_event, on_face_enroll);   /* before the UI registers its view */

#if ENABLE_WIFI
    if (bsp_wifi_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not started (no SSID configured?) — NTP unavailable");
    }
#endif

    /* UI */
    if (lvgl_port_lock(0)) {
        ui_fonts_init();
        ui_main_create(sd_ok);
        ui_admin_create();
        lvgl_port_unlock();
    }

#if PIN_DIAG
    pin_diag_sensor_lines(FP_UART_TX_GPIO, FP_UART_RX_GPIO);   /* TX pad readback + RX edge count */
    pin_diag_watch(5);                                         /* log sensor WAKEUP (blue) changes */
#endif

    /* Fingerprint sensor service */
#if ENABLE_FINGERPRINT
    ESP_ERROR_CHECK(fp_service_start(on_fp_event));
#endif

    /* Face recognition at the door (camera runs while the main screen is shown) */
    if (settings_get()->face_enabled && lvgl_port_lock(0)) {
        face_service_set_kiosk(true);
        ui_main_show();          /* switch the idle card to the live camera view */
        lvgl_port_unlock();
    }

#if CAMERA_SELFTEST
    ui_camera_selftest();
#endif

    bsp_audio_play_tone(880, 120);
    ESP_LOGI(TAG, "=== System ready ===");
}
