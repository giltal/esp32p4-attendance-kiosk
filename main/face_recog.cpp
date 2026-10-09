#include "face_recog.h"
#include "human_face_recognition.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>

static const char *TAG = "FACE_RECOG";

static HumanFaceFeat *s_feat;
static int s_len;

esp_err_t face_recog_init(void)
{
    if (s_feat) return ESP_OK;
    int64_t t0 = esp_timer_get_time();
    s_feat = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false /* load now */);
    if (!s_feat) return ESP_ERR_NO_MEM;
    s_len = s_feat->get_feat_len();
    ESP_LOGI(TAG, "Feature model loaded in %lld ms, signature length %d",
             (esp_timer_get_time() - t0) / 1000, s_len);
    if (s_len <= 0 || s_len > FACE_FEAT_MAX_LEN) {
        ESP_LOGE(TAG, "unexpected signature length %d", s_len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int face_recog_feat_len(void)
{
    return s_len;
}

esp_err_t face_recog_extract(const uint8_t *rgb565, int width, int height,
                             const face_box_t *face, float *out_feat, int *ms)
{
    if (!s_feat && face_recog_init() != ESP_OK) return ESP_FAIL;

    dl::image::img_t img = {
        .data     = (void *)rgb565,
        .width    = (uint16_t)width,
        .height   = (uint16_t)height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };
    std::vector<int> landmarks(face->kp, face->kp + FACE_KEYPOINTS * 2);
    for (int v : landmarks) {
        if (v < 0) return ESP_ERR_INVALID_ARG;   /* keypoints missing */
    }

    int64_t t0 = esp_timer_get_time();
    dl::TensorBase *feat = s_feat->run(img, landmarks);
    if (ms) *ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (!feat || feat->dtype != dl::DATA_TYPE_FLOAT || feat->size != s_len) {
        ESP_LOGE(TAG, "bad feature tensor");
        return ESP_FAIL;
    }

    /* The model output is already normalised; normalise again defensively so
     * the dot product is a true cosine similarity. */
    const float *src = (const float *)feat->data;
    float norm = 0;
    for (int i = 0; i < s_len; i++) norm += src[i] * src[i];
    norm = sqrtf(norm);
    if (norm < 1e-6f) return ESP_FAIL;
    for (int i = 0; i < s_len; i++) out_feat[i] = src[i] / norm;
    return ESP_OK;
}

float face_recog_similarity(const float *a, const float *b, int len)
{
    float s = 0;
    for (int i = 0; i < len; i++) s += a[i] * b[i];
    return s;
}
