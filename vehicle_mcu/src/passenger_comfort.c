/*
 * passenger_comfort.c - 乘客舒适度引擎实现
 *
 * 原理：
 *   - 每拍对三轴线加速度/三轴 jerk 的平方做 EMA，得到二阶矩估计；
 *     开方后得 RMS，加 jerk 折算项合成"等效不舒适指标"：
 *       metric = rms(accel) + jerk_weight * rms(jerk)
 *   - 各向得分 = 100 * (1 - metric / metric_full)，截断到 [0,100]
 *   - 行程指数 = 三向得分加权合成，再按事件计数做向下调整
 *     （内部工程指标，有总量上限；非考核非处罚）
 *
 * 详见 passenger_comfort.h 头部注释。仅依赖 C 标准库 + libm。
 */
#include "passenger_comfort.h"

#include <math.h>
#include <string.h>

/* ==================== 内部状态 ==================== */

typedef struct {
    passenger_comfort_config_t cfg;

    float ea2[3];   /* 线加速度二阶矩 EMA：0=纵向 1=横向 2=垂向 */
    float ej2[3];   /* jerk 二阶矩 EMA */

    uint32_t hard_brake_count;
    uint32_t hard_accel_count;
    uint32_t hard_turn_count;
    uint32_t bump_count;
    uint32_t high_jerk_count;

    uint32_t first_ts;
    uint32_t last_ts;
    bool     has_ts;
    uint32_t sample_count;

    bool initialized;
} comfort_state_t;

static comfort_state_t s_cf;

/* ==================== 默认配置 ==================== */

void passenger_comfort_get_default_config(passenger_comfort_config_t *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));

    /* 仅工程初始值，需实车与乘客反馈标定 */
    config->ema_alpha        = 0.05f;

    config->long_metric_full = 2.0f;
    config->lat_metric_full  = 2.0f;
    config->vert_metric_full = 3.0f;

    config->jerk_weight      = 0.5f;

    config->w_long = 0.4f;
    config->w_lat  = 0.3f;
    config->w_vert = 0.3f;

    config->impact_hard_brake = 1.5f;
    config->impact_hard_accel = 1.0f;
    config->impact_hard_turn  = 1.2f;
    config->impact_bump       = 0.8f;
    config->impact_high_jerk  = 0.5f;
    config->impact_max_total  = 30.0f;
}

/* ==================== 内部辅助函数 ==================== */

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* 单方向得分：RMS + jerk 折算 → 0~100 */
static float axis_score(float rms_a, float rms_j, float jerk_weight, float metric_full)
{
    float metric = rms_a + jerk_weight * rms_j;
    return clamp01(1.0f - metric / metric_full) * 100.0f;
}

/* ==================== 对外接口 ==================== */

int passenger_comfort_init(const passenger_comfort_config_t *config)
{
    passenger_comfort_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        passenger_comfort_get_default_config(&cfg);
    }

    /* 参数合法性检查 */
    if (cfg.ema_alpha <= 0.0f || cfg.ema_alpha >= 1.0f) return -1;
    if (cfg.long_metric_full <= 0.0f || cfg.lat_metric_full <= 0.0f
        || cfg.vert_metric_full <= 0.0f) return -1;
    if (cfg.jerk_weight < 0.0f) return -1;
    if (cfg.w_long < 0.0f || cfg.w_lat < 0.0f || cfg.w_vert < 0.0f) return -1;
    if (cfg.impact_max_total < 0.0f) return -1;

    memset(&s_cf, 0, sizeof(s_cf));
    s_cf.cfg = cfg;
    s_cf.initialized = true;
    return 0;
}

void passenger_comfort_update(const vehicle_imu_output_t *imu_out)
{
    if (!s_cf.initialized || !imu_out) return;

    const float a[3] = {imu_out->longitudinal, imu_out->lateral,
                        imu_out->vertical_accel};
    const float j[3] = {imu_out->longitudinal_jerk, imu_out->lateral_jerk,
                        imu_out->vertical_jerk};
    float al = s_cf.cfg.ema_alpha;

    for (int i = 0; i < 3; i++) {
        s_cf.ea2[i] += al * (a[i] * a[i] - s_cf.ea2[i]);
        s_cf.ej2[i] += al * (j[i] * j[i] - s_cf.ej2[i]);
    }

    if (!s_cf.has_ts) {
        s_cf.first_ts = imu_out->timestamp_ms;
        s_cf.has_ts = true;
    }
    s_cf.last_ts = imu_out->timestamp_ms;
    s_cf.sample_count++;
}

void passenger_comfort_on_motion_events(uint32_t entered_flags)
{
    if (!s_cf.initialized) return;

    if (entered_flags & (1u << VEHICLE_HARD_BRAKE))      s_cf.hard_brake_count++;
    if (entered_flags & (1u << VEHICLE_HARD_ACCEL))      s_cf.hard_accel_count++;
    if (entered_flags & (1u << VEHICLE_HARD_TURN_LEFT))  s_cf.hard_turn_count++;
    if (entered_flags & (1u << VEHICLE_HARD_TURN_RIGHT)) s_cf.hard_turn_count++;
    if (entered_flags & (1u << VEHICLE_BUMP))            s_cf.bump_count++;
    if (entered_flags & (1u << VEHICLE_HIGH_LONG_JERK))  s_cf.high_jerk_count++;
    if (entered_flags & (1u << VEHICLE_HIGH_LAT_JERK))   s_cf.high_jerk_count++;
}

void passenger_comfort_get_metrics(passenger_comfort_metrics_t *out)
{
    if (!out) return;

    memset(out, 0, sizeof(*out));
    if (!s_cf.initialized) return;

    const passenger_comfort_config_t *c = &s_cf.cfg;

    float rms_al = sqrtf(s_cf.ea2[0]);
    float rms_aa = sqrtf(s_cf.ea2[1]);
    float rms_av = sqrtf(s_cf.ea2[2]);
    float rms_jl = sqrtf(s_cf.ej2[0]);
    float rms_ja = sqrtf(s_cf.ej2[1]);
    float rms_jv = sqrtf(s_cf.ej2[2]);

    out->longitudinal_smoothness = axis_score(rms_al, rms_jl, c->jerk_weight,
                                              c->long_metric_full);
    out->lateral_smoothness      = axis_score(rms_aa, rms_ja, c->jerk_weight,
                                              c->lat_metric_full);
    out->vertical_comfort        = axis_score(rms_av, rms_jv, c->jerk_weight,
                                              c->vert_metric_full);

    out->hard_brake_count = s_cf.hard_brake_count;
    out->hard_accel_count = s_cf.hard_accel_count;
    out->hard_turn_count  = s_cf.hard_turn_count;
    out->bump_count       = s_cf.bump_count;
    out->high_jerk_count  = s_cf.high_jerk_count;

    /* 三向加权合成基准分 */
    float base = c->w_long * out->longitudinal_smoothness
               + c->w_lat  * out->lateral_smoothness
               + c->w_vert * out->vertical_comfort;

    /* 事件影响（向下调整，有总量上限；内部工程指标，非考核非处罚） */
    float impact = (float)s_cf.hard_brake_count * c->impact_hard_brake
                 + (float)s_cf.hard_accel_count * c->impact_hard_accel
                 + (float)s_cf.hard_turn_count  * c->impact_hard_turn
                 + (float)s_cf.bump_count       * c->impact_bump
                 + (float)s_cf.high_jerk_count  * c->impact_high_jerk;
    if (impact > c->impact_max_total) {
        impact = c->impact_max_total;
    }

    float index = base - impact;
    if (index < 0.0f) index = 0.0f;
    if (index > 100.0f) index = 100.0f;
    out->trip_comfort_index = index;

    out->trip_duration_ms = s_cf.has_ts ? (s_cf.last_ts - s_cf.first_ts) : 0;
    out->sample_count = s_cf.sample_count;
}

void passenger_comfort_reset_trip(void)
{
    if (!s_cf.initialized) return;

    passenger_comfort_config_t keep = s_cf.cfg;
    memset(&s_cf, 0, sizeof(s_cf));
    s_cf.cfg = keep;
    s_cf.initialized = true;
}
