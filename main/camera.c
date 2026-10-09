#include "camera.h"
#include "bsp_i2c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *TAG = "CAMERA";

#define CAM_BUF_COUNT      2
#define CAM_SCCB_FREQ_HZ   100000      /* sensor on the shared touch/RTC/codec bus */
#define CAM_TASK_STACK     12288   /* face recognition + attendance + UI events run here */
#define CAM_TASK_PRIO      4

static int s_fd = -1;
static uint8_t *s_buf[CAM_BUF_COUNT];
static int s_width, s_height;
static TaskHandle_t s_task;

static bool s_video_inited;              /* esp_video_init() may only run once */
static volatile bool s_want_run;
static volatile bool s_streaming;
static volatile camera_state_t s_state = CAMERA_IDLE;
static const char *s_error = "";
static volatile uint32_t s_frames;
static camera_frame_cb_t s_cb;
static void *s_cb_user;

/* ── One-time hardware bring-up ─────────────────────────────────────────── */

#define OV02C10_ADDR        0x36
#define OV02C10_PID         0x5602
#define PROBE_ATTEMPTS      10
#define PROBE_INTERVAL_MS   50

/*
 * The OV02C10 sometimes answers its first SCCB reads with zeros (seen as
 * "Camera sensor is not OV02C10, PID=0x0"). esp_video_init() only probes once
 * and cannot be re-run, so poll the chip ID here until the sensor is ready.
 */
static esp_err_t sensor_wake_probe(void)
{
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OV02C10_ADDR,
        .scl_speed_hz    = CAM_SCCB_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 1; i <= PROBE_ATTEMPTS; i++) {
        uint8_t reg_h[2] = { 0x30, 0x0A }, reg_l[2] = { 0x30, 0x0B };
        uint8_t id_h = 0, id_l = 0;
        esp_err_t e1 = i2c_master_transmit_receive(dev, reg_h, 2, &id_h, 1, 100);
        esp_err_t e2 = i2c_master_transmit_receive(dev, reg_l, 2, &id_l, 1, 100);
        uint16_t pid = (id_h << 8) | id_l;
        if (e1 == ESP_OK && e2 == ESP_OK && pid == OV02C10_PID) {
            ESP_LOGI(TAG, "sensor ready (PID=0x%04X) after %d attempt(s)", pid, i);
            ret = ESP_OK;
            break;
        }
        ESP_LOGW(TAG, "sensor probe %d: %s PID=0x%04X", i,
                 (e1 == ESP_OK && e2 == ESP_OK) ? "read ok" : "i2c error", pid);
        vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
    i2c_master_bus_rm_device(dev);
    return ret;
}

static esp_err_t camera_hw_init(void)
{
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb  = false,
            .i2c_handle = bsp_i2c_get_handle(),
            .freq       = CAM_SCCB_FREQ_HZ,
        },
        .reset_pin = -1,     /* module has no reset / power-down lines */
        .pwdn_pin  = -1,
    };
    esp_video_init_config_t cfg = { .csi = &csi_cfg };

    if (!s_video_inited) {
        if (sensor_wake_probe() != ESP_OK) {
            ESP_LOGE(TAG, "camera sensor not responding");
            s_error = "camera sensor not responding (press start again)";
            return ESP_ERR_NOT_FOUND;
        }
        esp_err_t ret = esp_video_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
            s_error = "camera init failed (sensor not detected?)";
            return ret;
        }
        s_video_inited = true;
    }

    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        s_error = "cannot open camera device";
        return ESP_FAIL;
    }

    struct v4l2_capability cap = {0};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "driver=%s card=%s", cap.driver, cap.card);
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "G_FMT failed");
        goto fail;
    }
    s_width  = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format f = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = s_width,
            .fmt.pix.height = s_height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        if (ioctl(s_fd, VIDIOC_S_FMT, &f) != 0) {
            ESP_LOGE(TAG, "S_FMT RGB565 failed");
            goto fail;
        }
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed");
        goto fail;
    }
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) {
            ESP_LOGE(TAG, "QUERYBUF %d failed", i);
            goto fail;
        }
        s_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "mmap %d failed", i);
            goto fail;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "QBUF %d failed", i);
            goto fail;
        }
    }

    ESP_LOGI(TAG, "Camera ready: %dx%d RGB565, %d buffers", s_width, s_height, CAM_BUF_COUNT);
    return ESP_OK;

fail:
    s_error = "camera format/buffer setup failed";
    close(s_fd);
    s_fd = -1;
    return ESP_FAIL;
}

/* ── Stream task ────────────────────────────────────────────────────────── */

static void camera_task(void *arg)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool first = true;

    for (;;) {
        while (!s_want_run) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        }
        s_state = CAMERA_STARTING;

        if (s_fd < 0) {
            int64_t t0 = esp_timer_get_time();
            if (camera_hw_init() != ESP_OK) {
                s_state = CAMERA_ERROR;
                s_want_run = false;
                continue;
            }
            first = true;   /* fresh buffers are already queued */
            ESP_LOGI(TAG, "hardware init took %lld ms", (esp_timer_get_time() - t0) / 1000);
        }

        /* STREAMOFF returns every buffer to the application (V4L2 semantics),
         * so they must all be queued again before the next STREAMON. */
        if (!first) {
            for (int i = 0; i < CAM_BUF_COUNT; i++) {
                struct v4l2_buffer b = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory = V4L2_MEMORY_MMAP,
                    .index = i,
                };
                if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
                    ESP_LOGW(TAG, "re-QBUF %d failed", i);
                }
            }
        }
        first = false;

        if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
            ESP_LOGE(TAG, "STREAMON failed");
            s_error = "stream start failed";
            s_state = CAMERA_ERROR;
            s_want_run = false;
            continue;
        }
        s_streaming = true;
        s_state = CAMERA_STREAMING;
        s_frames = 0;
        ESP_LOGI(TAG, "Stream on");

        while (s_want_run) {
            struct v4l2_buffer b = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
            };
            if (ioctl(s_fd, VIDIOC_DQBUF, &b) != 0) {
                ESP_LOGE(TAG, "DQBUF failed");
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            s_frames++;
            camera_frame_cb_t cb = s_cb;
            if (cb && b.index < CAM_BUF_COUNT) cb(s_buf[b.index], s_width, s_height, s_cb_user);
            if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
                ESP_LOGE(TAG, "QBUF failed");
            }
        }

        if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGE(TAG, "STREAMOFF failed");
        }
        s_streaming = false;
        s_state = CAMERA_IDLE;
        ESP_LOGI(TAG, "Stream off (%lu frames)", (unsigned long)s_frames);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t camera_start(camera_frame_cb_t cb, void *user)
{
    if (!s_task) {
        if (xTaskCreatePinnedToCore(camera_task, "camera", CAM_TASK_STACK, NULL,
                                    CAM_TASK_PRIO, &s_task, 1) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_cb = cb;
    s_cb_user = user;
    if (s_state != CAMERA_STREAMING) s_state = CAMERA_STARTING;
    s_want_run = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

camera_state_t camera_state(void)
{
    return s_state;
}

const char *camera_error(void)
{
    return s_error;
}

void camera_stop(void)
{
    s_want_run = false;
}

bool camera_is_streaming(void)
{
    return s_streaming;
}

int camera_width(void)  { return s_width; }
int camera_height(void) { return s_height; }

uint32_t camera_frame_count(void)
{
    return s_frames;
}
