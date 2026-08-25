#include "dms_product_bridge.h"
#include "sys_logger.h"

#include <string.h>

/*
 * DMS Product Bridge - Integration V1 胶水层实现。
 * 详见 include/dms_product_bridge.h 注释。
 */

/* ==================== 内部常量 ==================== */

/* MCU 待发队列深度（每帧最大 DMS_MCU_ENCODE_BUF_SIZE 字节） */
#define PB_QUEUE_CAPACITY   16
#define PB_PACKET_BUF_SIZE  96

/* 事件标志索引 */
enum {
    PB_FLAG_EYE_CLOSED = 0,
    PB_FLAG_LONG_EYE_CLOSED,
    PB_FLAG_YAWN,
    PB_FLAG_HEAD_DOWN,
    PB_FLAG_COUNT
};

/* ==================== 内部状态 ==================== */

typedef struct {
    bool initialized;

    /* 各事件标志的上一帧状态与开始时间（算持续时间用） */
    int      flag_active[PB_FLAG_COUNT];
    uint64_t flag_start_ms[PB_FLAG_COUNT];

    /* 上一帧风险等级（检测升级/降级边沿） */
    dms_risk_level_t prev_level;

    /* MCU 待发队列（环形） */
    uint8_t  queue_buf[PB_QUEUE_CAPACITY][PB_PACKET_BUF_SIZE];
    size_t   queue_len[PB_QUEUE_CAPACITY];
    int      queue_head;   /* 最旧帧 */
    int      queue_tail;   /* 下一个写入位置 */
    size_t   queue_count;
    uint32_t queue_dropped;
} pb_state_t;

static pb_state_t s_pb;

/* ==================== 内部辅助 ==================== */

/*
 * 从 dms_result_t 解析事件标志。
 * status 字符串是权威来源（由疲劳特征状态机设置）；
 * status 为空时回退到布尔字段。
 */
static void pb_parse_flags(const dms_result_t *dms, int flags[PB_FLAG_COUNT])
{
    memset(flags, 0, sizeof(int) * PB_FLAG_COUNT);

    if (!dms->face_found) return;   /* NO_FACE / AI_ERROR：无事件 */

    if (dms->status[0] != '\0') {
        if (strcmp(dms->status, "LONG_EYE_CLOSED") == 0) {
            flags[PB_FLAG_LONG_EYE_CLOSED] = 1;
            flags[PB_FLAG_EYE_CLOSED]      = 1;
        } else if (strcmp(dms->status, "EYE_CLOSED") == 0) {
            flags[PB_FLAG_EYE_CLOSED] = 1;
        } else if (strcmp(dms->status, "YAWN") == 0) {
            flags[PB_FLAG_YAWN] = 1;
        } else if (strcmp(dms->status, "HEAD_DOWN") == 0) {
            flags[PB_FLAG_HEAD_DOWN] = 1;
        }
        /* NORMAL / FACE / 其他：无标志 */
    } else {
        flags[PB_FLAG_EYE_CLOSED] = dms->eye_closed;
        flags[PB_FLAG_YAWN]       = dms->yawn;
        flags[PB_FLAG_HEAD_DOWN]  = dms->head_down;
    }
}

/* 跟踪单个标志的持续时间（0→1 边沿记录开始时间） */
static uint64_t pb_track_duration(int flag_idx, int active, uint64_t now_ms)
{
    if (active) {
        if (!s_pb.flag_active[flag_idx]) {
            s_pb.flag_start_ms[flag_idx] = now_ms;
            s_pb.flag_active[flag_idx]   = 1;
            return 0;
        }
        return now_ms - s_pb.flag_start_ms[flag_idx];
    }
    s_pb.flag_active[flag_idx] = 0;
    return 0;
}

/* 入队一帧已编码数据；队列满时丢弃最旧帧 */
static void pb_enqueue_packet(const uint8_t *data, size_t len)
{
    if (s_pb.queue_count >= PB_QUEUE_CAPACITY) {
        s_pb.queue_head = (s_pb.queue_head + 1) % PB_QUEUE_CAPACITY;
        s_pb.queue_count--;
        s_pb.queue_dropped++;
        log_warn("[ProductBridge] mcu queue full, dropped oldest frame");
    }

    if (len > PB_PACKET_BUF_SIZE) len = PB_PACKET_BUF_SIZE;
    memcpy(s_pb.queue_buf[s_pb.queue_tail], data, len);
    s_pb.queue_len[s_pb.queue_tail] = len;
    s_pb.queue_tail = (s_pb.queue_tail + 1) % PB_QUEUE_CAPACITY;
    s_pb.queue_count++;
}

/* 事件类型字符串 → MCU 事件码 */
static dms_mcu_event_t pb_map_event_code(const char *event_type,
                                         dms_risk_level_t level)
{
    if (strcmp(event_type, "EYE_CLOSED") == 0)      return DMS_MCU_EVENT_EYE_CLOSED;
    if (strcmp(event_type, "LONG_EYE_CLOSED") == 0) return DMS_MCU_EVENT_LONG_EYE_CLOSED;
    if (strcmp(event_type, "YAWN") == 0)            return DMS_MCU_EVENT_YAWN;
    if (strcmp(event_type, "HEAD_DOWN") == 0)       return DMS_MCU_EVENT_HEAD_DOWN;
    if (strcmp(event_type, "FACE_LOST") == 0)       return DMS_MCU_EVENT_FACE_LOST;

    /* COMBO_FATIGUE / NONE（等级边沿触发）等：按等级映射 */
    return (level >= DMS_RISK_HIGH) ? DMS_MCU_EVENT_FATIGUE_HIGH
                                    : DMS_MCU_EVENT_FATIGUE_WARNING;
}

/* 构造并编码一帧事件报文，入队 */
static bool pb_queue_event_frame(const char *event_type,
                                 const dms_risk_result_t *risk,
                                 const dms_result_t *dms,
                                 uint64_t monotonic_ms)
{
    dms_mcu_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    uint8_t confidence = 0;
    if (dms->face_found && dms->face_score > 0.0f) {
        float c = dms->face_score * 100.0f;
        confidence = (c > 100.0f) ? 100 : (uint8_t)c;
    }

    uint16_t duration = (risk->event_duration_ms > 65535ULL)
                        ? 65535 : (uint16_t)risk->event_duration_ms;

    dms_mcu_build_event_frame(&frame,
                              pb_map_event_code(event_type, risk->level),
                              risk->level,
                              confidence,
                              duration,
                              (uint32_t)(monotonic_ms / 1000));

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t len = dms_mcu_encode(&frame, buf, sizeof(buf));
    if (len == 0) {
        log_error("[ProductBridge] mcu encode failed");
        return false;
    }

    pb_enqueue_packet(buf, len);
    return true;
}

/* ==================== 公开 API ==================== */

bool dms_product_bridge_init(void)
{
    return dms_product_bridge_init_with_dir(NULL);
}

bool dms_product_bridge_init_with_dir(const char *event_dir)
{
    memset(&s_pb, 0, sizeof(s_pb));
    s_pb.prev_level = DMS_RISK_NORMAL;

    if (!dms_risk_manager_init()) {
        log_error("[ProductBridge] risk manager init failed");
        return false;
    }

    bool logger_ok = event_dir ? dms_event_logger_init_with_dir(event_dir)
                               : dms_event_logger_init();
    if (!logger_ok) {
        log_error("[ProductBridge] event logger init failed");
        return false;
    }

    if (!dms_alarm_policy_init()) {
        log_error("[ProductBridge] alarm policy init failed");
        return false;
    }

    s_pb.initialized = true;
    log_info("[ProductBridge] initialized (event_dir=%s)",
             event_dir ? event_dir : "default-sd");
    return true;
}

dms_product_bridge_output_t dms_product_bridge_update(const dms_result_t *dms,
                                                      uint64_t monotonic_ms)
{
    dms_product_bridge_output_t out;
    memset(&out, 0, sizeof(out));
    out.risk.level  = DMS_RISK_NORMAL;
    out.alarm_level = ALARM_NONE;

    if (!s_pb.initialized || !dms) return out;

    /* 1. 解析事件标志 + 跟踪持续时间（先存上一帧状态，用于边沿检测） */
    int flags[PB_FLAG_COUNT];
    pb_parse_flags(dms, flags);

    int prev_flags[PB_FLAG_COUNT];
    memcpy(prev_flags, s_pb.flag_active, sizeof(prev_flags));

    dms_risk_input_t input;
    memset(&input, 0, sizeof(input));
    input.face_found     = dms->face_found;
    input.eye_closed      = flags[PB_FLAG_EYE_CLOSED];
    input.long_eye_closed = flags[PB_FLAG_LONG_EYE_CLOSED];
    input.yawn            = flags[PB_FLAG_YAWN];
    input.head_down       = flags[PB_FLAG_HEAD_DOWN];
    input.ear            = dms->ear;
    input.mar            = dms->mar;
    input.head_down_score = dms->head_down_score;
    input.eye_closed_duration_ms =
        pb_track_duration(PB_FLAG_EYE_CLOSED, flags[PB_FLAG_EYE_CLOSED], monotonic_ms);
    input.long_eye_closed_duration_ms =
        pb_track_duration(PB_FLAG_LONG_EYE_CLOSED, flags[PB_FLAG_LONG_EYE_CLOSED], monotonic_ms);
    input.yawn_duration_ms =
        pb_track_duration(PB_FLAG_YAWN, flags[PB_FLAG_YAWN], monotonic_ms);
    input.head_down_duration_ms =
        pb_track_duration(PB_FLAG_HEAD_DOWN, flags[PB_FLAG_HEAD_DOWN], monotonic_ms);
    input.timestamp_ms = monotonic_ms;

    /* 2. 风险评估 */
    out.risk = dms_risk_manager_update(&input);

    /* 3. 报警策略（内部带冷却） */
    out.alarm_level = dms_alarm_policy_update(&out.risk, monotonic_ms);

    /* 4. 事件边沿 / 等级变化检测（记日志和发 MCU 帧都只在边沿做，不按帧做）
     * 注意：LONG_EYE_CLOSED 激活期间 EYE_CLOSED 也成立，只有"长闭眼"自身的
     * 上升沿才算新事件，避免一长闭眼同时记两条日志。 */
    bool event_edge = (flags[PB_FLAG_LONG_EYE_CLOSED] && !prev_flags[PB_FLAG_LONG_EYE_CLOSED])
                      || (flags[PB_FLAG_EYE_CLOSED] && !prev_flags[PB_FLAG_EYE_CLOSED]
                          && !flags[PB_FLAG_LONG_EYE_CLOSED])
                      || (flags[PB_FLAG_YAWN]      && !prev_flags[PB_FLAG_YAWN])
                      || (flags[PB_FLAG_HEAD_DOWN] && !prev_flags[PB_FLAG_HEAD_DOWN]);
    bool level_changed = (out.risk.level != s_pb.prev_level);
    s_pb.prev_level  = out.risk.level;

    /* 5. 事件日志：事件边沿或升级时记录（NORMAL 由 logger 内部忽略） */
    if ((event_edge || level_changed) && out.risk.save_event_requested) {
        dms_event_record_t record;
        memset(&record, 0, sizeof(record));
        record.timestamp_ms   = monotonic_ms;
        snprintf(record.event_type, sizeof(record.event_type), "%s", out.risk.event_type);
        record.risk_level     = out.risk.level;
        record.risk_score     = out.risk.risk_score;
        record.duration_ms    = out.risk.event_duration_ms;
        record.ear            = dms->ear;
        record.ear_baseline   = dms->ear_baseline;
        record.mar            = dms->mar;
        record.mar_baseline   = dms->mar_baseline;
        record.head_down_score = dms->head_down_score;
        record.face_score     = dms->face_score;
        /* vehicle_speed / route_id / driver_id / shift_id：预留字段，
         * 当前无数据，保持 0 和空串，不编造。 */
        out.event_logged = dms_event_logger_write(&record);
    }

    /* 6. MCU 事件帧：等级变化必发（含恢复降级，MCU 侧要解除报警）；
     *      事件边沿在 ATTENTION 及以上时发送 */
    if (level_changed || (event_edge && out.risk.level >= DMS_RISK_ATTENTION)) {
        out.mcu_event_queued = pb_queue_event_frame(out.risk.event_type,
                                                    &out.risk, dms,
                                                    monotonic_ms);
    }

    return out;
}

bool dms_product_bridge_queue_heartbeat(bool camera_alive, bool ai_alive,
                                        uint32_t timestamp_sec)
{
    if (!s_pb.initialized) return false;

    dms_mcu_heartbeat_t hb;
    hb.dms_alive    = 1;
    hb.camera_alive = camera_alive ? 1 : 0;
    hb.ai_alive     = ai_alive ? 1 : 0;
    hb.risk_level   = (uint8_t)dms_risk_manager_get_level();

    dms_mcu_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    dms_mcu_build_heartbeat_frame(&frame, &hb, timestamp_sec);

    uint8_t buf[DMS_MCU_ENCODE_BUF_SIZE];
    size_t len = dms_mcu_encode(&frame, buf, sizeof(buf));
    if (len == 0) {
        log_error("[ProductBridge] heartbeat encode failed");
        return false;
    }

    pb_enqueue_packet(buf, len);
    return true;
}

bool dms_product_bridge_get_mcu_packet(uint8_t *buf, size_t capacity, size_t *size)
{
    if (!buf || !size) return false;
    if (s_pb.queue_count == 0) return false;

    size_t len = s_pb.queue_len[s_pb.queue_head];
    if (len > capacity) return false;   /* 调用方缓冲区太小，不出队 */

    memcpy(buf, s_pb.queue_buf[s_pb.queue_head], len);
    *size = len;

    s_pb.queue_head = (s_pb.queue_head + 1) % PB_QUEUE_CAPACITY;
    s_pb.queue_count--;
    return true;
}

size_t dms_product_bridge_pending_packets(void)
{
    return s_pb.queue_count;
}

uint32_t dms_product_bridge_dropped_packets(void)
{
    return s_pb.queue_dropped;
}

void dms_product_bridge_reset(void)
{
    int keep_initialized = s_pb.initialized;
    memset(&s_pb, 0, sizeof(s_pb));
    s_pb.initialized = keep_initialized;
    s_pb.prev_level  = DMS_RISK_NORMAL;

    dms_risk_manager_reset();
    dms_alarm_policy_reset();
    log_info("[ProductBridge] reset");
}

void dms_product_bridge_deinit(void)
{
    dms_event_logger_deinit();
    dms_risk_manager_deinit();
    memset(&s_pb, 0, sizeof(s_pb));
    log_info("[ProductBridge] deinitialized");
}
