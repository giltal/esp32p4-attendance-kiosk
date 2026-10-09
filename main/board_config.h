#pragma once

/*******************************************************************************
 * Board: Guition JC1060P470C_I_W_Y
 * MCU:   ESP32-P4 + ESP32-C6 (WiFi/BT via ESP-Hosted)
 * App:   Fingerprint attendance (student entry/exit) system
 ******************************************************************************/

#define APP_VERSION "0.1.0"

/* Feature flags — set to 0 to disable a peripheral */
#define ENABLE_DISPLAY      1
#define ENABLE_TOUCH        1
#define ENABLE_AUDIO        1
#define ENABLE_RTC          1
#define ENABLE_SD_CARD      1
#define ENABLE_WIFI         1   /* WiFi via ESP-Hosted (ESP32-C6 SDIO) */
#define ENABLE_FINGERPRINT  1

/*──────────────────────────────────────────────────────────────────────────────
 *  LCD (JD9165 MIPI-DSI, 1024×600)
 *────────────────────────────────────────────────────────────────────────────*/
#define LCD_H_RES               1024
#define LCD_V_RES               600
#define LCD_BIT_PER_PIXEL       16          /* RGB565 */
#define LCD_BK_LIGHT_GPIO       23          /* Backlight enable */
#define LCD_BK_LIGHT_ON_LEVEL   1           /* Active HIGH */
#define LCD_RST_GPIO            27          /* Display reset (also powers touch AVDD) */

/* MIPI-DSI DPI timing (from panel vendor datasheet) */
#define LCD_DPI_CLK_MHZ         51.2
#define LCD_HSYNC_BACK_PORCH    136
#define LCD_HSYNC_PULSE_WIDTH   24
#define LCD_HSYNC_FRONT_PORCH   160
#define LCD_VSYNC_BACK_PORCH    21
#define LCD_VSYNC_PULSE_WIDTH   2
#define LCD_VSYNC_FRONT_PORCH   12

/* MIPI-DSI PHY LDO */
#define LCD_MIPI_DSI_PHY_LDO_CHAN   3
#define LCD_MIPI_DSI_PHY_LDO_MV    2500

/*──────────────────────────────────────────────────────────────────────────────
 *  I2C Bus (shared: touch, audio codec, RTC)
 *────────────────────────────────────────────────────────────────────────────*/
#define I2C_NUM                 0
#define I2C_SDA_GPIO            7
#define I2C_SCL_GPIO            8
#define I2C_FREQ_HZ             400000

/*──────────────────────────────────────────────────────────────────────────────
 *  Touch (GT911 capacitive, I2C addr 0x5D)
 *  NOTE: RST/INT set to -1 (NC) — matches stock BSP.
 *  DO NOT assign GPIOs without verifying the schematic first;
 *  driving wrong pins as outputs can damage the power circuit.
 *────────────────────────────────────────────────────────────────────────────*/
#define TOUCH_RST_GPIO          -1
#define TOUCH_INT_GPIO          -1

/*──────────────────────────────────────────────────────────────────────────────
 *  Audio (ES8311 codec, I2C addr 0x18)
 *────────────────────────────────────────────────────────────────────────────*/
#define I2S_MCLK_GPIO           13
#define I2S_BCLK_GPIO           12
#define I2S_WS_GPIO             10
#define I2S_DOUT_GPIO           9       /* ESP32-P4  → ES8311 */
#define I2S_DIN_GPIO            48      /* ES8311 → ESP32-P4  */
#define AUDIO_PA_GPIO           11      /* Power-amp enable (active HIGH) */
#define AUDIO_CODEC_I2C_ADDR    0x30  /* ES8311 8-bit address (7-bit 0x18 << 1) */
#define AUDIO_I2S_NUM           0
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_MCLK_MULTIPLE     256

/*──────────────────────────────────────────────────────────────────────────────
 *  SD Card (SDMMC Slot 0 — fixed GPIOs on ESP32-P4)
 *────────────────────────────────────────────────────────────────────────────*/
#define SD_MMC_CLK_GPIO         43
#define SD_MMC_CMD_GPIO         44
#define SD_MMC_D0_GPIO          39
#define SD_MMC_D1_GPIO          40
#define SD_MMC_D2_GPIO          41
#define SD_MMC_D3_GPIO          42
#define SD_POWER_GPIO           36      /* SD card power enable */
#define SD_MOUNT_POINT          "/sd"

/*──────────────────────────────────────────────────────────────────────────────
 *  WiFi (ESP-Hosted via ESP32-C6, SDMMC Slot 1)
 *────────────────────────────────────────────────────────────────────────────*/
#define WIFI_C6_RST_GPIO        54
#define WIFI_HANDSHAKE_GPIO     6

/*──────────────────────────────────────────────────────────────────────────────
 *  RTC (RX8025T, I2C addr 0x32)
 *────────────────────────────────────────────────────────────────────────────*/
#define RTC_I2C_ADDR            0x32

/*──────────────────────────────────────────────────────────────────────────────
 *  Fingerprint sensor: driver speaks the ID809 protocol (DFRobot SEN0348 type,
 *  115200 baud, AA 55 frames). The first two modules (AliExpress, sold as ID809,
 *  R503-style white housing) never answered — replacements on order.
 *
 *  Pinout of those modules (SH1.0 6-pin, connector latch up, pin 1 on the left),
 *  matches the R503 datasheet — verify against the new sensor's documentation:
 *    1 Red    = VCC 3.3V           2 Black = GND
 *    3 Yellow = TX (out of sensor) 4 Green = RX (into sensor)
 *    5 Blue   = WAKEUP / touch     6 White = 3.3VT (touch power)
 *  !! An AliExpress table claiming red = GND / green = VCC was WRONG for these
 *  !! modules — wiring by it overheated one sensor.
 *
 *  Wiring:  3V3 -> red + white,  GND -> black,
 *           GPIO3 -> green (sensor RX),  GPIO4 <- yellow (sensor TX),
 *           GPIO5 <- blue (optional, diagnostics only)
 *  The driver auto-detects swapped TX/RX and the baud rate at startup.
 *  All header GPIOs (1-5, 20, 32, 33, 45-47) passed a drive/readback test.
 *────────────────────────────────────────────────────────────────────────────*/
#define FP_UART_NUM             UART_NUM_1
#define FP_UART_TX_GPIO         3       /* P4 TX -> sensor RX (black)   */
#define FP_UART_RX_GPIO         4       /* P4 RX <- sensor TX (yellow)  */
#define FP_UART_BAUD            115200  /* ID809 factory default */
