#include "face_detect.h"
#include "human_face_detect.hpp"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "FACE_DETECT";

static HumanFaceDetect *s_detect;

esp_err_t face_detect_init(void)
{
    if (s_detect) return ESP_OK;
    int64_t t0 = esp_timer_get_time();
    s_detect = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false /* load now */);
    if (!s_detect) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Model loaded in %lld ms", (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

int face_detect_run(const uint8_t *rgb565, int width, int height,
                    face_box_t *out, int max, int *ms)
{
    if (!s_detect && face_detect_init() != ESP_OK) return -1;

    dl::image::img_t img = {
        .data     = (void *)rgb565,
        .width    = (uint16_t)width,
        .height   = (uint16_t)height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> &res = s_detect->run(img);
    if (ms) *ms = (int)((esp_timer_get_time() - t0) / 1000);

    int n = 0;
    for (auto &r : res) {
        if (n >= max) break;
        r.limit_box(width, height);
        face_box_t *f = &out[n++];
        f->x0 = r.box[0]; f->y0 = r.box[1];
        f->x1 = r.box[2]; f->y1 = r.box[3];
        f->score = r.score;
        for (int i = 0; i < FACE_KEYPOINTS * 2; i++) {
            f->kp[i] = (i < (int)r.keypoint.size()) ? r.keypoint[i] : -1;
        }
    }
    return n;
}
