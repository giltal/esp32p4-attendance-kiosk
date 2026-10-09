#pragma once

/*
 * Face "signature" (feature vector) extraction — ESP-DL human_face_recognition,
 * MFN S8 v1 model. Signatures are L2-normalised float vectors; the similarity of
 * two signatures is their dot product (1.0 = identical, ~0 = unrelated).
 * Not thread-safe: call from the camera task only.
 */

#include "esp_err.h"
#include "face_detect.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_FEAT_MAX_LEN  512

/* Load the model (lazy; ~once). */
esp_err_t face_recog_init(void);

/* Length of a signature (valid after init), <= FACE_FEAT_MAX_LEN. */
int face_recog_feat_len(void);

/* Compute the signature of `face` in an RGB565LE image. */
esp_err_t face_recog_extract(const uint8_t *rgb565, int width, int height,
                             const face_box_t *face, float *out_feat, int *ms);

/* Dot-product similarity of two signatures. */
float face_recog_similarity(const float *a, const float *b, int len);

#ifdef __cplusplus
}
#endif
