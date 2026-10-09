idf_component_register(
    SRCS
        "main.c"
        "bsp_i2c.c"
        "bsp_display.c"
        "bsp_touch.c"
        "bsp_audio.c"
        "bsp_sdcard.c"
        "bsp_rtc.c"
        "bsp_wifi.c"
        "fp_sensor.c"
        "fp_service.c"
        "pin_diag.c"
        "student_db.c"
        "attendance.c"
        "report.c"
        "settings_store.c"
        "ui_common.c"
        "ui_main.c"
        "ui_admin.c"
        "ui_camera.c"
        "camera.c"
        "face_detect.cpp"
        "face_recog.cpp"
        "face_db.c"
        "face_service.c"
        "fonts/font_clock_80.c"
        "fonts/font_heb_48.c"
        "fonts/font_heb_32.c"
        "fonts/font_heb_22.c"
    INCLUDE_DIRS
        "."
        "fonts"
    REQUIRES
        driver esp_lcd esp_timer nvs_flash fatfs sdmmc
        esp_wifi esp_netif esp_event
        esp_driver_ppa esp_video human_face_detect human_face_recognition
)
