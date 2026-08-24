#include "dms_fatigue_features.h"

#include <math.h>
#include <string.h>

#include "sys_logger.h"

#define IDX_FACE_A_START   33
#define IDX_FACE_A_END     42
#define IDX_FACE_B_START   87
#define IDX_FACE_B_END     96
#define IDX_NOSE_START     72
#define IDX_NOSE_END       86
#define IDX_MOUTH_START    52
#define IDX_MOUTH_END      71
#define IDX_JAW_START      0
#define IDX_JAW_END        32

typedef struct {
    int inited;
    int calibrated;
    uint64_t calib_start_us;
    uint64_t last_face_us;

    double sum_ear, sum_mar, sum_head;
    int calib_count;

    float ear_ema, mar_ema, head_ema;
    float ear_baseline, mar_baseline, head_baseline;

    uint64_t eye_enter_us, yawn_enter_us, head_enter_us;
    int eye_active, yawn_active, head_active;
} fatigue_feat_ctx_t;

static fatigue_feat_ctx_t g_ff;

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float ema(float oldv, float newv)
{
    return oldv + DMS_FEATURE_EMA_ALPHA * (newv - oldv);
}

#if DMS_ENABLE_LANDMARK_106
static void get_xy(const dms_result_t *r, int idx, float *x, float *y)
{
    *x = r->landmark_106.points[idx * 2 + 0];
    *y = r->landmark_106.points[idx * 2 + 1];
}

static float eye_ear(const dms_result_t *r, int s, int e)
{
    float minx = 1e9f, maxx = -1e9f;
    float ys[16]; int n = 0;
    for (int i = s; i <= e && n < 16; i++) {
        float x, y; get_xy(r, i, &x, &y);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        ys[n++] = y;
    }
    float w = maxx - minx;
    if (w < 1.0f || n < 6) return 0.0f;
    /* 简单排序取上下眼睑：最小两个 y 为上，最大两个 y 为下 */
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (ys[j] < ys[i]) { float t=ys[i]; ys[i]=ys[j]; ys[j]=t; }
    float upper = (ys[0] + ys[1]) * 0.5f;
    float lower = (ys[n-1] + ys[n-2]) * 0.5f;
    return clampf((lower - upper) / w, 0.0f, 1.0f);
}

static float mouth_mar(const dms_result_t *r)
{
    float minx = 1e9f, maxx = -1e9f;
    float xs[32], ys[32]; int n = 0;
    for (int i = IDX_MOUTH_START; i <= IDX_MOUTH_END && n < 32; i++) {
        get_xy(r, i, &xs[n], &ys[n]);
        if (xs[n] < minx) minx = xs[n];
        if (xs[n] > maxx) maxx = xs[n];
        n++;
    }
    float w = maxx - minx;
    if (w < 1.0f || n < 8) return 0.0f;
    float cx0 = minx + w * 0.25f, cx1 = minx + w * 0.75f;
    float miny = 1e9f, maxy = -1e9f; int cn = 0;
    for (int i = 0; i < n; i++) {
        if (xs[i] >= cx0 && xs[i] <= cx1) {
            if (ys[i] < miny) miny = ys[i];
            if (ys[i] > maxy) maxy = ys[i];
            cn++;
        }
    }
    if (cn < 4) { miny = ys[0]; maxy = ys[0]; for (int i=1;i<n;i++){ if(ys[i]<miny)miny=ys[i]; if(ys[i]>maxy)maxy=ys[i]; } }
    return clampf((maxy - miny) / w, 0.0f, 2.0f);
}

static float head_ratio(const dms_result_t *r)
{
    float ax=0, ay=0, bx=0, by=0; int an=0, bn=0;
    for (int i=IDX_FACE_A_START;i<=IDX_FACE_A_END;i++){float x,y;get_xy(r,i,&x,&y);ax+=x;ay+=y;an++;}
    for (int i=IDX_FACE_B_START;i<=IDX_FACE_B_END;i++){float x,y;get_xy(r,i,&x,&y);bx+=x;by+=y;bn++;}
    if (!an || !bn) return 0.0f;
    float eye_y = (ay/an + by/bn) * 0.5f;

    float nose_y = -1e9f;
    for (int i=IDX_NOSE_START;i<=IDX_NOSE_END;i++){float x,y;get_xy(r,i,&x,&y); if(y>nose_y)nose_y=y;}
    float chin_y = -1e9f;
    for (int i=IDX_JAW_START;i<=IDX_JAW_END;i++){float x,y;get_xy(r,i,&x,&y); if(y>chin_y)chin_y=y;}
    float denom = chin_y - eye_y;
    if (denom < 1.0f) return 0.0f;
    return clampf((nose_y - eye_y) / denom, 0.0f, 1.0f);
}
#endif /* DMS_ENABLE_LANDMARK_106 */

void dms_fatigue_features_reset(void)
{
    memset(&g_ff, 0, sizeof(g_ff));
    g_ff.inited = 1;
}

void dms_fatigue_features_update(dms_result_t *result)
{
    if (!result) return;
    uint64_t t0 = mono_us();
    if (!g_ff.inited) dms_fatigue_features_reset();

    result->landmark_106_found = 0;
    result->feature_calibrated = g_ff.calibrated;

#if DMS_ENABLE_LANDMARK_106
    if (!result->face_found || !result->landmark_106.found) {
        result->feature_cost_us = mono_us() - t0;
        if (mono_us() - g_ff.last_face_us > 1000000ULL) {
            g_ff.eye_active = g_ff.yawn_active = g_ff.head_active = 0;
            g_ff.eye_enter_us = g_ff.yawn_enter_us = g_ff.head_enter_us = 0;
        }
        return;
    }
    g_ff.last_face_us = mono_us();
    result->landmark_106_found = 1;

    float ear_a = eye_ear(result, IDX_FACE_A_START, IDX_FACE_A_END);
    float ear_b = eye_ear(result, IDX_FACE_B_START, IDX_FACE_B_END);
    /* 左右按 x 坐标自动分配，避免镜像/翻转导致写反 */
    float ax=0,bx=0; int an=0,bn=0;
    for (int i=IDX_FACE_A_START;i<=IDX_FACE_A_END;i++){ax+=result->landmark_106.points[i*2];an++;}
    for (int i=IDX_FACE_B_START;i<=IDX_FACE_B_END;i++){bx+=result->landmark_106.points[i*2];bn++;}
    if (an && bn && ax/an > bx/bn) {
        result->left_ear = ear_b;
        result->right_ear = ear_a;
    } else {
        result->left_ear = ear_a;
        result->right_ear = ear_b;
    }
    float ear_raw = (result->left_ear + result->right_ear) * 0.5f;
    /* 单眼关键点退化防护：EAR > 0.6（眼高/眼宽）物理上不可能，判该眼无效。
     * 一眼无效时用另一眼；两眼都无效则保持 EMA 原值，不污染基线/判定。
     * 低值（含 0，轮廓塌缩）保留——交给 fmax 判定逻辑处理。 */
    int lv = result->left_ear  <= DMS_EAR_SANE_MAX;
    int rv = result->right_ear <= DMS_EAR_SANE_MAX;
    float ear_both;
    if (lv && rv)      { ear_both = fmaxf(result->left_ear, result->right_ear); }
    else if (lv)       { ear_both = result->left_ear;  ear_raw = result->left_ear; }
    else if (rv)       { ear_both = result->right_ear; ear_raw = result->right_ear; }
    else               { ear_both = g_ff.ear_ema;      ear_raw = g_ff.ear_ema; }
    float mar_raw = mouth_mar(result);
    float head_raw = head_ratio(result);

    if (g_ff.calib_count == 0) {
        g_ff.ear_ema = ear_raw; g_ff.mar_ema = mar_raw; g_ff.head_ema = head_raw;
        g_ff.calib_start_us = mono_us();
    } else {
        g_ff.ear_ema = ema(g_ff.ear_ema, ear_raw);
        g_ff.mar_ema = ema(g_ff.mar_ema, mar_raw);
        g_ff.head_ema = ema(g_ff.head_ema, head_raw);
    }

    result->ear = g_ff.ear_ema;
    result->mar = g_ff.mar_ema;

    if (!g_ff.calibrated) {
        g_ff.sum_ear += ear_raw; g_ff.sum_mar += mar_raw; g_ff.sum_head += head_raw; g_ff.calib_count++;
        if (mono_us() - g_ff.calib_start_us >= DMS_CALIB_TIME_US && g_ff.calib_count >= 3) {
            g_ff.ear_baseline = (float)(g_ff.sum_ear / g_ff.calib_count);
            g_ff.mar_baseline = (float)(g_ff.sum_mar / g_ff.calib_count);
            g_ff.head_baseline = (float)(g_ff.sum_head / g_ff.calib_count);
            g_ff.calibrated = 1;
            log_info("fatigue features calibrated: ear_base=%.3f mar_base=%.3f head_base=%.3f",
                     g_ff.ear_baseline, g_ff.mar_baseline, g_ff.head_baseline);
        }
    }

    result->ear_baseline = g_ff.ear_baseline;
    result->mar_baseline = g_ff.mar_baseline;
    result->head_baseline = g_ff.head_baseline;
    result->ear_threshold = g_ff.ear_baseline * DMS_EAR_CLOSE_RATIO;
    result->mar_threshold = fmaxf(DMS_MAR_YAWN_MIN, g_ff.mar_baseline * DMS_MAR_YAWN_RATIO);
    result->head_down_score = clampf((g_ff.head_ema - g_ff.head_baseline) / 0.25f, 0.0f, 1.0f);
    result->feature_calibrated = g_ff.calibrated;

    if (g_ff.calibrated) {
        uint64_t now = mono_us();
        float ear_enter = g_ff.ear_baseline * DMS_EAR_CLOSE_RATIO;
        float ear_recover = g_ff.ear_baseline * DMS_EAR_RECOVER_RATIO;
        float mar_enter = result->mar_threshold;
        float mar_recover = fmaxf(DMS_MAR_YAWN_MIN * 0.8f, g_ff.mar_baseline * DMS_MAR_RECOVER_RATIO);
        float head_enter = g_ff.head_baseline + DMS_HEAD_ENTER_DELTA;
        float head_recover = g_ff.head_baseline + DMS_HEAD_RECOVER_DELTA;

        /* 闭眼判定用双眼 raw EAR 的较大值（已经过高值退化过滤）：真闭眼双眼同时低才触发，
         * 防单眼关键点退化（塌 0 或冲 1）造成误判；比 EMA 更跟手 */
        if (!g_ff.eye_active && ear_both < ear_enter) { if (!g_ff.eye_enter_us) g_ff.eye_enter_us = now; }
        if (g_ff.eye_enter_us && ear_both > ear_recover) { g_ff.eye_enter_us = 0; }
        if (g_ff.eye_enter_us && now - g_ff.eye_enter_us >= (uint64_t)DMS_EYE_CLOSED_MS * 1000ULL) g_ff.eye_active = 1;
        if (g_ff.eye_active && ear_both > ear_recover) { g_ff.eye_active = 0; g_ff.eye_enter_us = 0; }

        if (!g_ff.yawn_active && result->mar > mar_enter) { if (!g_ff.yawn_enter_us) g_ff.yawn_enter_us = now; }
        if (g_ff.yawn_enter_us && result->mar < mar_recover) { g_ff.yawn_enter_us = 0; }
        if (g_ff.yawn_enter_us && now - g_ff.yawn_enter_us >= (uint64_t)DMS_YAWN_MS * 1000ULL) g_ff.yawn_active = 1;
        if (g_ff.yawn_active && result->mar < mar_recover) { g_ff.yawn_active = 0; g_ff.yawn_enter_us = 0; }

        if (!g_ff.head_active && result->head_down_score > head_enter) { if (!g_ff.head_enter_us) g_ff.head_enter_us = now; }
        if (g_ff.head_enter_us && result->head_down_score < head_recover) { g_ff.head_enter_us = 0; }
        if (g_ff.head_enter_us && now - g_ff.head_enter_us >= (uint64_t)DMS_HEAD_DOWN_MS * 1000ULL) g_ff.head_active = 1;
        if (g_ff.head_active && result->head_down_score < head_recover) { g_ff.head_active = 0; g_ff.head_enter_us = 0; }

        result->eye_closed = g_ff.eye_active;
        result->yawn = g_ff.yawn_active;
        result->head_down = g_ff.head_active;
        if (g_ff.eye_active && g_ff.eye_enter_us && now - g_ff.eye_enter_us >= (uint64_t)DMS_LONG_EYE_CLOSED_MS * 1000ULL) {
            snprintf(result->status, sizeof(result->status), "LONG_EYE_CLOSED");
        } else if (g_ff.eye_active) {
            snprintf(result->status, sizeof(result->status), "EYE_CLOSED");
        } else if (g_ff.yawn_active) {
            snprintf(result->status, sizeof(result->status), "YAWN");
        } else if (g_ff.head_active) {
            snprintf(result->status, sizeof(result->status), "HEAD_DOWN");
        } else {
            snprintf(result->status, sizeof(result->status), "NORMAL");
        }
    }
#endif

    result->feature_cost_us = mono_us() - t0;
}
