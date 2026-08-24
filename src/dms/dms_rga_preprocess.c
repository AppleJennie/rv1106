#include "dms_rga_preprocess.h"

#if DMS_HW_PREPROCESS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "sys_logger.h"

#include "rk_comm_video.h"
#include "rk_comm_mb.h"
#include "rk_mpi_mb.h"

#include "rga.h"
#include "im2d.h"

/*
 * DMS Hardware Video Pipeline V2-A：RGA 硬件预处理实现。
 *
 * RGA API 用法（RV1106 uClibc 版 librga，im2d API 1.10.1，RGA2 硬件）：
 *   - src：VI 帧 NV12 DMABUF，每帧 importbuffer_fd(fd, &param) +
 *          wrapbuffer_handle_t()，用完 releasebuffer_handle()；
 *          stride 必须用 vi_frame.stVFrame 的 u32VirWidth/u32VirHeight。
 *   - dst：RK MPI MB 池（MB_ALLOC_TYPE DMA）分配的物理连续 buffer。
 *          实测 RV1106 RGA2 上 importbuffer_virtualaddr() 导入 malloc
 *          虚拟地址失败，因此 dst 必须与 src 一样走 importbuffer_fd()。
 *          MB_BLK 在 init/首帧时一次性 GetMB 并缓存 fd/vir_addr/handle，
 *          每帧仅 wrapbuffer_handle_t()，deinit 时 ReleaseMB + DestroyPool。
 *   - 转换：imresize_t(src, dst, 0, 0, INTER_LINEAR, 1)，同步执行，
 *          NV12->BGR888 / NV12->RGB888 的 CSC 由 RGA 在 resize 中一并完成。
 *          CSC 色彩空间通过 src_img.color_space_mode = IM_YUV_BT601_FULL_RANGE
 *          显式指定 BT601 full-range（与 JPEG JFIF / stb 解码输出一致；
 *          imsetColorSpace() 在本版 uClibc librga 只有 C++ 符号，故直写字段）。
 *          默认 limited-range 实测会导致 106 点模型 EAR 关键点退化。
 *          source 输出为全分辨率 RGB（与已验收软件路径的 crop 源完全等价；
 *          半幅源图实测会导致 106 点眼部精度不足、EAR 分离度退化）。
 *   消费侧（AI 线程）仍通过 RK_MPI_MB_Handle2VirAddr 拿到的虚拟地址 memcpy。
 */

#define RGA_SLOT_NUM            3
#define RGA_FAIL_DISABLE_THRESH 30      /* 连续失败超过该值后自锁，AI 回退软件路径 */
#define RGA_OP_WARN_MS          20      /* 单次 RGA 操作超过该耗时打 WARN */

#ifndef ALIGN_UP
#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#endif

/* RV1106 RGA2 要求 stride 4 像素对齐 */
#define RGA_STRIDE_ALIGN        4

typedef enum {
    RGA_SLOT_FREE = 0,
    RGA_SLOT_READY,
    RGA_SLOT_IN_USE
} rga_slot_state_t;

typedef struct {
    MB_BLK                retina_mb;    /* 池化 DMA buffer：retina_wstride x model_h BGR */
    MB_BLK                src_mb;       /* 池化 DMA buffer：src_wstride x src_h RGB */
    void                 *retina_vir;   /* RK_MPI_MB_Handle2VirAddr */
    void                 *src_vir;
    rga_buffer_handle_t   retina_handle;/* importbuffer_fd 持久句柄 */
    rga_buffer_handle_t   src_handle;
    rga_slot_state_t      state;
    uint64_t              seq;          /* READY 排序用 */

    /* READY 时写入的帧元数据（take 侧读取） */
    int                   orig_w;
    int                   orig_h;
    uint64_t              timestamp_us;
    uint64_t              vi_get_us;
    uint64_t              rga_retina_us;
    uint64_t              rga_source_us;
} rga_slot_t;

typedef struct {
    bool            inited;
    bool            disabled;           /* 连续失败后自锁 */
    int             consecutive_fail;

    int             model_w;
    int             model_h;
    int             retina_wstride;     /* 4 对齐 */
    MB_POOL         retina_pool;
    bool            retina_pool_ready;

    /* 半幅源图尺寸，首帧根据 VI 实际分辨率确定 */
    int             src_w;
    int             src_h;
    int             src_wstride;        /* 4 对齐 */
    MB_POOL         src_pool;
    bool            src_pool_ready;

    rga_slot_t      slots[RGA_SLOT_NUM];

    /* 消费侧拷贝缓冲（AI 线程唯一消费者），有效期到下一次 take */
    uint8_t        *cons_retina;
    uint8_t        *cons_src;

    uint64_t        seq_counter;

    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    /* 统计（写侧持有 mutex 更新）。produced/dropped 另有累计总值，
     * reset_stats 只清周期值，避免 perf 日志里计数口径与主 PERF 行不一致。 */
    dms_rga_preprocess_stats_t stats;
} rga_preprocess_ctx_t;

static rga_preprocess_ctx_t g_rga = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond  = PTHREAD_COND_INITIALIZER,
};

static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* ==================== MB 池辅助 ==================== */

static MB_POOL create_mb_pool(uint64_t blk_size, uint32_t blk_cnt, const char *tag)
{
    MB_POOL_CONFIG_S cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.u64MBSize = blk_size;
    cfg.u32MBCnt = blk_cnt;
    cfg.enRemapMode = MB_REMAP_MODE_NONE;
    cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    cfg.enDmaType = MB_DMA_TYPE_CMA;    /* RGA 需要物理连续内存 */
    cfg.bPreAlloc = RK_FALSE;

    MB_POOL pool = RK_MPI_MB_CreatePool(&cfg);
    if (pool == MB_INVALID_POOLID) {
        log_error("rga_preprocess: RK_MPI_MB_CreatePool(%s) 失败 size=%llu cnt=%u",
                  tag, (unsigned long long)blk_size, blk_cnt);
        return MB_INVALID_POOLID;
    }
    log_info("rga_preprocess: MB pool(%s) 创建成功 blk=%llu bytes (%.2f MB) cnt=%u total=%.2f MB",
             tag, (unsigned long long)blk_size,
             (double)blk_size / (1024.0 * 1024.0), blk_cnt,
             (double)blk_size * blk_cnt / (1024.0 * 1024.0));
    return pool;
}

/*
 * 从池里取一个块并缓存 vir/fd/RGA handle。
 * RGA2 上 dst 必须走 importbuffer_fd（virtualaddr 导入 malloc 内存会失败）。
 */
static bool prepare_slot_mb(MB_POOL pool, uint64_t blk_size,
                            int wstride, int hstride, uint32_t format,
                            MB_BLK *out_mb, void **out_vir,
                            rga_buffer_handle_t *out_handle,
                            const char *tag)
{
    MB_BLK mb = RK_MPI_MB_GetMB(pool, blk_size, RK_TRUE);
    if (mb == MB_INVALID_HANDLE) {
        log_error("rga_preprocess: RK_MPI_MB_GetMB(%s) 失败", tag);
        return false;
    }

    void *vir = RK_MPI_MB_Handle2VirAddr(mb);
    int fd = RK_MPI_MB_Handle2Fd(mb);
    if (!vir || fd < 0) {
        log_error("rga_preprocess: MB(%s) Handle2VirAddr=%p Handle2Fd=%d 异常",
                  tag, vir, fd);
        RK_MPI_MB_ReleaseMB(mb);
        return false;
    }

    im_handle_param_t param;
    param.width = (uint32_t)wstride;
    param.height = (uint32_t)hstride;
    param.format = format;

    rga_buffer_handle_t handle = importbuffer_fd(fd, &param);
    if (handle <= 0) {
        log_error("rga_preprocess: importbuffer_fd(%s) 失败 ret=%d: %s",
                  tag, (int)handle, imStrError((IM_STATUS)handle));
        RK_MPI_MB_ReleaseMB(mb);
        return false;
    }

    *out_mb = mb;
    *out_vir = vir;
    *out_handle = handle;
    return true;
}

static void release_slot_mb(MB_BLK *mb, rga_buffer_handle_t *handle)
{
    if (*handle > 0) {
        releasebuffer_handle(*handle);
        *handle = 0;
    }
    if (*mb != MB_INVALID_HANDLE && *mb != 0) {
        RK_MPI_MB_ReleaseMB(*mb);
        *mb = MB_INVALID_HANDLE;
    }
}

/* ==================== 各池/槽位一次性分配 ==================== */

static bool alloc_retina_buffers_locked(void)
{
    if (g_rga.retina_pool_ready) {
        return true;
    }

    uint64_t blk_size = (uint64_t)g_rga.retina_wstride * g_rga.model_h * 3;

    g_rga.retina_pool = create_mb_pool(blk_size, RGA_SLOT_NUM, "retina");
    if (g_rga.retina_pool == MB_INVALID_POOLID) {
        return false;
    }

    for (int i = 0; i < RGA_SLOT_NUM; i++) {
        if (!prepare_slot_mb(g_rga.retina_pool, blk_size,
                             g_rga.retina_wstride, g_rga.model_h,
                             RK_FORMAT_BGR_888,
                             &g_rga.slots[i].retina_mb,
                             &g_rga.slots[i].retina_vir,
                             &g_rga.slots[i].retina_handle,
                             "retina")) {
            return false;
        }
    }

    g_rga.cons_retina = (uint8_t *)malloc((size_t)g_rga.model_w * g_rga.model_h * 3);
    if (!g_rga.cons_retina) {
        log_error("rga_preprocess: 消费侧 retina buffer 分配失败");
        return false;
    }

    g_rga.retina_pool_ready = true;
    log_info("rga_preprocess: retina %dx%d BGR MB buffer 分配完成 (wstride=%d)",
             g_rga.model_w, g_rga.model_h, g_rga.retina_wstride);
    return true;
}

static bool alloc_source_buffers_locked(int orig_w, int orig_h)
{
    if (g_rga.src_pool_ready) {
        return true;
    }

    /*
     * 全分辨率源图：106 点 crop 源与已验收软件路径（stb 解码全幅 RGB）完全等价。
     * 不要用半幅：实测半幅 + 最近邻 crop 导致眼部关键点精度不足，EAR 分离度退化。
     */
    g_rga.src_w = orig_w;
    g_rga.src_h = orig_h;
    g_rga.src_wstride = ALIGN_UP(g_rga.src_w, RGA_STRIDE_ALIGN);
    if (g_rga.src_w <= 0 || g_rga.src_h <= 0) {
        log_error("rga_preprocess: 非法源图尺寸 %dx%d", orig_w, orig_h);
        return false;
    }

    uint64_t blk_size = (uint64_t)g_rga.src_wstride * g_rga.src_h * 3;

    g_rga.src_pool = create_mb_pool(blk_size, RGA_SLOT_NUM, "source");
    if (g_rga.src_pool == MB_INVALID_POOLID) {
        return false;
    }

    for (int i = 0; i < RGA_SLOT_NUM; i++) {
        if (!prepare_slot_mb(g_rga.src_pool, blk_size,
                             g_rga.src_wstride, g_rga.src_h,
                             RK_FORMAT_RGB_888,
                             &g_rga.slots[i].src_mb,
                             &g_rga.slots[i].src_vir,
                             &g_rga.slots[i].src_handle,
                             "source")) {
            return false;
        }
    }

    g_rga.cons_src = (uint8_t *)malloc((size_t)g_rga.src_w * g_rga.src_h * 3);
    if (!g_rga.cons_src) {
        log_error("rga_preprocess: 消费侧源图 buffer 分配失败");
        return false;
    }

    g_rga.src_pool_ready = true;
    log_info("rga_preprocess: 全幅源图 %dx%d RGB MB buffer 分配完成 (wstride=%d)",
             g_rga.src_w, g_rga.src_h, g_rga.src_wstride);
    return true;
}

/* ==================== Public API ==================== */

bool dms_rga_preprocess_init(int model_w, int model_h)
{
    if (g_rga.inited) {
        return true;
    }

    memset(&g_rga.slots, 0, sizeof(g_rga.slots));
    memset(&g_rga.stats, 0, sizeof(g_rga.stats));
    g_rga.disabled = false;
    g_rga.consecutive_fail = 0;
    g_rga.retina_pool_ready = false;
    g_rga.src_pool_ready = false;
    g_rga.retina_pool = MB_INVALID_POOLID;
    g_rga.src_pool = MB_INVALID_POOLID;
    g_rga.seq_counter = 0;
    for (int i = 0; i < RGA_SLOT_NUM; i++) {
        g_rga.slots[i].retina_mb = MB_INVALID_HANDLE;
        g_rga.slots[i].src_mb = MB_INVALID_HANDLE;
        g_rga.slots[i].state = RGA_SLOT_FREE;
    }

    const char *rga_ver = querystring(RGA_VERSION);
    const char *rga_vendor = querystring(RGA_VENDOR);
    log_info("rga_preprocess: RGA vendor=%s version=%s",
             rga_vendor ? rga_vendor : "(null)", rga_ver ? rga_ver : "(null)");

    if (!rga_ver || strstr(rga_ver, "unknown") || model_w <= 0 || model_h <= 0) {
        log_error("rga_preprocess: RGA 不可用或模型尺寸非法 (%dx%d)，回退软件路径",
                  model_w, model_h);
        return false;
    }

    g_rga.model_w = model_w;
    g_rga.model_h = model_h;
    g_rga.retina_wstride = ALIGN_UP(model_w, RGA_STRIDE_ALIGN);

    g_rga.inited = true;
    log_info("RGA preprocess enabled: retina %dx%d BGR, slots=%d (MB pool DMA dst)",
             model_w, model_h, RGA_SLOT_NUM);
    return true;
}

/*
 * 选一个可写槽位：优先 FREE；没有 FREE 时覆盖最旧 READY（latest-only）。
 * 返回时槽位已置为 IN_USE（写侧持有），调用方写完后由本文件置 READY。
 */
static rga_slot_t *acquire_write_slot(void)
{
    pthread_mutex_lock(&g_rga.mutex);

    rga_slot_t *slot = NULL;
    for (int i = 0; i < RGA_SLOT_NUM; i++) {
        if (g_rga.slots[i].state == RGA_SLOT_FREE) {
            slot = &g_rga.slots[i];
            break;
        }
    }

    if (!slot) {
        /* 没有 FREE：偷最旧的 READY（seq 最小），AI 正在读的 IN_USE 绝不覆盖 */
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < RGA_SLOT_NUM; i++) {
            if (g_rga.slots[i].state == RGA_SLOT_READY && g_rga.slots[i].seq < oldest) {
                oldest = g_rga.slots[i].seq;
                slot = &g_rga.slots[i];
            }
        }
        if (slot) {
            g_rga.stats.dropped_frames++;
            g_rga.stats.dropped_total++;
        }
    }

    if (slot) {
        slot->state = RGA_SLOT_IN_USE;
    }

    pthread_mutex_unlock(&g_rga.mutex);
    return slot;
}

static void release_write_slot(rga_slot_t *slot)
{
    pthread_mutex_lock(&g_rga.mutex);
    slot->state = RGA_SLOT_FREE;
    pthread_mutex_unlock(&g_rga.mutex);
}

bool dms_rga_preprocess_from_vi(const struct rkVIDEO_FRAME_INFO_S *vi_frame_in,
                                uint64_t vi_get_us)
{
    if (!g_rga.inited || g_rga.disabled || !vi_frame_in) {
        return false;
    }

    const VIDEO_FRAME_INFO_S *vi_frame = (const VIDEO_FRAME_INFO_S *)vi_frame_in;
    const VIDEO_FRAME_S *vf = &vi_frame->stVFrame;

    uint32_t w = vf->u32Width;
    uint32_t h = vf->u32Height;
    uint32_t vir_w = vf->u32VirWidth;
    uint32_t vir_h = vf->u32VirHeight;

    if (w == 0 || h == 0 || vir_w < w || vir_h < h ||
        vf->enPixelFormat != RK_FMT_YUV420SP) {
        log_warn("rga_preprocess: VI 帧参数异常 %ux%u vir=%ux%u fmt=%d",
                 w, h, vir_w, vir_h, (int)vf->enPixelFormat);
        return false;
    }

    /* MB 池一次性分配（需要 RK_MPI_SYS_Init 已完成；capture 线程满足该条件） */
    pthread_mutex_lock(&g_rga.mutex);
    bool alloc_ok = alloc_retina_buffers_locked() && alloc_source_buffers_locked((int)w, (int)h);
    pthread_mutex_unlock(&g_rga.mutex);
    if (!alloc_ok) {
        g_rga.disabled = true;
        log_error("rga_preprocess: MB buffer 分配失败，RGA 预处理自锁，AI 回退软件路径");
        return false;
    }

    rga_slot_t *slot = acquire_write_slot();
    if (!slot) {
        /* 全部 IN_USE（理论上 AI 单消费者最多占 1 个，不会发生） */
        return false;
    }

    /* 从 VI MB 取 dma fd（无需拷贝 NV12 数据） */
    int fd = RK_MPI_MB_Handle2Fd(vf->pMbBlk);
    if (fd < 0) {
        log_warn("rga_preprocess: RK_MPI_MB_Handle2Fd 失败");
        release_write_slot(slot);
        return false;
    }

    /* stride 必须用 vir_width/vir_height，否则花屏/斜图 */
    im_handle_param_t src_param;
    src_param.width = vir_w;
    src_param.height = vir_h;
    src_param.format = RK_FORMAT_YCbCr_420_SP;

    rga_buffer_handle_t src_handle = importbuffer_fd(fd, &src_param);
    if (src_handle <= 0) {
        log_warn("rga_preprocess: importbuffer_fd(src) 失败 ret=%d: %s",
                 (int)src_handle, imStrError((IM_STATUS)src_handle));
        release_write_slot(slot);
        return false;
    }

    rga_buffer_t src_img = wrapbuffer_handle_t(src_handle, (int)w, (int)h,
                                               (int)vir_w, (int)vir_h,
                                               RK_FORMAT_YCbCr_420_SP);
    /*
     * CSC 显式指定 BT601 full-range（与 stb JPEG 解码输出一致）。
     * RGA 默认 limited-range，实测会让 106 点模型输入偏色、EAR 关键点退化。
     * 注意：这版 uClibc librga 的 imsetColorSpace() 只导出 C++ mangled 符号，
     * C 代码直接写 rga_buffer_t.color_space_mode 字段（与该 helper 实现等价），
     * 对后续 imresize 一步到位生效，retina(BGR) 与 source(RGB) 两次转换共用，均生效。
     */
    src_img.color_space_mode = IM_YUV_BT601_FULL_RANGE;

    rga_buffer_t dst_retina = wrapbuffer_handle_t(slot->retina_handle,
                                                  g_rga.model_w, g_rga.model_h,
                                                  g_rga.retina_wstride, g_rga.model_h,
                                                  RK_FORMAT_BGR_888);
    rga_buffer_t dst_src = wrapbuffer_handle_t(slot->src_handle,
                                               g_rga.src_w, g_rga.src_h,
                                               g_rga.src_wstride, g_rga.src_h,
                                               RK_FORMAT_RGB_888);

    bool ok = true;
    uint64_t rga_retina_us = 0;
    uint64_t rga_source_us = 0;

    /* 1. NV12 -> model_w x model_h BGR（RetinaFace 输入，stretch + CSC 一次完成） */
    uint64_t t0 = get_mono_time_us();
    IM_STATUS st = imresize_t(src_img, dst_retina, 0, 0, INTER_LINEAR, 1);
    rga_retina_us = get_mono_time_us() - t0;
    if (st != IM_STATUS_SUCCESS) {
        log_warn("rga_preprocess: imresize retina 失败: %s", imStrError(st));
        ok = false;
    } else if (rga_retina_us > (uint64_t)RGA_OP_WARN_MS * 1000ULL) {
        log_warn("rga_preprocess: RGA retina 耗时过长 %llums",
                 (unsigned long long)(rga_retina_us / 1000ULL));
    }

    /* 2. NV12 -> 全幅 RGB（106 点 crop 源图） */
    if (ok) {
        t0 = get_mono_time_us();
        st = imresize_t(src_img, dst_src, 0, 0, INTER_LINEAR, 1);
        rga_source_us = get_mono_time_us() - t0;
        if (st != IM_STATUS_SUCCESS) {
            log_warn("rga_preprocess: imresize source 失败: %s", imStrError(st));
            ok = false;
        } else if (rga_source_us > (uint64_t)RGA_OP_WARN_MS * 1000ULL) {
            log_warn("rga_preprocess: RGA source 耗时过长 %llums",
                     (unsigned long long)(rga_source_us / 1000ULL));
        }
    }

    releasebuffer_handle(src_handle);

    pthread_mutex_lock(&g_rga.mutex);
    if (ok) {
        slot->seq = ++g_rga.seq_counter;
        slot->orig_w = (int)w;
        slot->orig_h = (int)h;
        slot->timestamp_us = get_mono_time_us();
        slot->vi_get_us = vi_get_us;
        slot->rga_retina_us = rga_retina_us;
        slot->rga_source_us = rga_source_us;
        slot->state = RGA_SLOT_READY;
        g_rga.consecutive_fail = 0;
        g_rga.stats.produced_frames++;
        g_rga.stats.produced_total++;
        g_rga.stats.vi_get_us += vi_get_us;
        g_rga.stats.rga_retina_us += rga_retina_us;
        g_rga.stats.rga_source_us += rga_source_us;
        pthread_cond_signal(&g_rga.cond);
    } else {
        slot->state = RGA_SLOT_FREE;
        g_rga.consecutive_fail++;
        if (g_rga.consecutive_fail >= RGA_FAIL_DISABLE_THRESH) {
            g_rga.disabled = true;
            log_error("rga_preprocess: 连续失败 %d 次，RGA 预处理自锁，AI 回退软件路径",
                      g_rga.consecutive_fail);
        }
    }
    pthread_mutex_unlock(&g_rga.mutex);
    return ok;
}

bool dms_rga_preprocess_take(dms_prepared_frame_t *out, int timeout_ms)
{
    if (!g_rga.inited || !out) {
        return false;
    }

    pthread_mutex_lock(&g_rga.mutex);

    /* 等待任一 READY 槽位（带超时，便于 AI 线程检查退出标志） */
    rga_slot_t *slot = NULL;
    for (;;) {
        uint64_t newest = 0;
        slot = NULL;
        for (int i = 0; i < RGA_SLOT_NUM; i++) {
            if (g_rga.slots[i].state == RGA_SLOT_READY && g_rga.slots[i].seq > newest) {
                newest = g_rga.slots[i].seq;
                slot = &g_rga.slots[i];
            }
        }
        if (slot) {
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_cond_timedwait(&g_rga.cond, &g_rga.mutex, &ts) != 0) {
            pthread_mutex_unlock(&g_rga.mutex);
            return false;   /* 超时，由调用方决定是否继续 */
        }
    }

    slot->state = RGA_SLOT_IN_USE;
    pthread_mutex_unlock(&g_rga.mutex);

    /* 拷贝到消费侧缓冲（AI 单消费者），持锁时间短，capture 不被阻塞。
     * wstride == width 时整块拷贝，否则按行跳过 stride padding。 */
    if (g_rga.retina_wstride == g_rga.model_w) {
        memcpy(g_rga.cons_retina, slot->retina_vir,
               (size_t)g_rga.model_w * g_rga.model_h * 3);
    } else {
        for (int y = 0; y < g_rga.model_h; y++) {
            memcpy(g_rga.cons_retina + (size_t)y * g_rga.model_w * 3,
                   (const uint8_t *)slot->retina_vir + (size_t)y * g_rga.retina_wstride * 3,
                   (size_t)g_rga.model_w * 3);
        }
    }

    if (g_rga.src_wstride == g_rga.src_w) {
        memcpy(g_rga.cons_src, slot->src_vir,
               (size_t)g_rga.src_w * g_rga.src_h * 3);
    } else {
        for (int y = 0; y < g_rga.src_h; y++) {
            memcpy(g_rga.cons_src + (size_t)y * g_rga.src_w * 3,
                   (const uint8_t *)slot->src_vir + (size_t)y * g_rga.src_wstride * 3,
                   (size_t)g_rga.src_w * 3);
        }
    }

    out->retina_bgr = g_rga.cons_retina;
    out->retina_w = g_rga.model_w;
    out->retina_h = g_rga.model_h;
    out->src_rgb = g_rga.cons_src;
    out->src_w = g_rga.src_w;
    out->src_h = g_rga.src_h;
    out->orig_w = slot->orig_w;
    out->orig_h = slot->orig_h;
    out->frame_id = (int)slot->seq;
    out->timestamp_us = slot->timestamp_us;
    out->vi_get_us = slot->vi_get_us;
    out->rga_retina_us = slot->rga_retina_us;
    out->rga_source_us = slot->rga_source_us;

    pthread_mutex_lock(&g_rga.mutex);
    slot->state = RGA_SLOT_FREE;
    pthread_mutex_unlock(&g_rga.mutex);

    return true;
}

bool dms_rga_preprocess_is_ready(void)
{
    return g_rga.inited && !g_rga.disabled;
}

void dms_rga_preprocess_get_stats(dms_rga_preprocess_stats_t *out)
{
    if (!out) {
        return;
    }
    pthread_mutex_lock(&g_rga.mutex);
    *out = g_rga.stats;
    pthread_mutex_unlock(&g_rga.mutex);
}

void dms_rga_preprocess_reset_stats(void)
{
    pthread_mutex_lock(&g_rga.mutex);
    /* 只清周期值，保留累计总值 produced_total/dropped_total */
    g_rga.stats.produced_frames = 0;
    g_rga.stats.dropped_frames = 0;
    g_rga.stats.vi_get_us = 0;
    g_rga.stats.rga_retina_us = 0;
    g_rga.stats.rga_source_us = 0;
    pthread_mutex_unlock(&g_rga.mutex);
}

void dms_rga_preprocess_deinit(void)
{
    if (!g_rga.inited) {
        return;
    }

    /* 唤醒可能阻塞在 take 的 AI 线程 */
    pthread_mutex_lock(&g_rga.mutex);
    pthread_cond_broadcast(&g_rga.cond);
    pthread_mutex_unlock(&g_rga.mutex);

    for (int i = 0; i < RGA_SLOT_NUM; i++) {
        release_slot_mb(&g_rga.slots[i].retina_mb, &g_rga.slots[i].retina_handle);
        release_slot_mb(&g_rga.slots[i].src_mb, &g_rga.slots[i].src_handle);
        g_rga.slots[i].state = RGA_SLOT_FREE;
    }

    if (g_rga.retina_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(g_rga.retina_pool);
        g_rga.retina_pool = MB_INVALID_POOLID;
    }
    if (g_rga.src_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(g_rga.src_pool);
        g_rga.src_pool = MB_INVALID_POOLID;
    }

    SAFE_FREE(g_rga.cons_retina);
    SAFE_FREE(g_rga.cons_src);

    g_rga.inited = false;
    g_rga.disabled = false;
    g_rga.retina_pool_ready = false;
    g_rga.src_pool_ready = false;
    g_rga.consecutive_fail = 0;

    log_info("rga_preprocess: 已关闭");
}

/* ==================== 一次性 dump 调试（任务：肉眼验证 RGA 输出） ==================== */

#define RGA_DUMP_FLAG_FILE      "/mnt/sdcard/dms/DUMP_RGA"
#define RGA_DUMP_DIR            "/mnt/sdcard/dms/live"
#define RGA_DUMP_CHECK_INTERVAL 50      /* 每 50 帧查一次标志文件 */

/*
 * AI 线程每处理一帧 prepared 调一次。
 * 每 50 帧 access() 检查一次标志文件；存在则删除并返回 true（本帧做 dump）。
 */
bool dms_hw_dump_check_and_consume(void)
{
    static uint32_t s_frame_cnt = 0;

    s_frame_cnt++;
    if (s_frame_cnt % RGA_DUMP_CHECK_INTERVAL != 0) {
        return false;
    }

    if (access(RGA_DUMP_FLAG_FILE, F_OK) != 0) {
        return false;
    }

    unlink(RGA_DUMP_FLAG_FILE);
    log_info("rga_preprocess: 检测到 %s，本帧执行 RGA dump", RGA_DUMP_FLAG_FILE);
    return true;
}

/*
 * 写 PPM P6。is_bgr=true 时输入为 BGR 序，写出前逐行转 RGB
 * （retina 输入是 BGR，PPM 查看器期望 RGB）。
 */
bool dms_hw_dump_ppm(const char *name, const uint8_t *data, int w, int h, bool is_bgr)
{
    if (!name || !data || w <= 0 || h <= 0) {
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", RGA_DUMP_DIR, name);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        log_warn("rga_preprocess: dump 打开失败 %s: %s", path, strerror(errno));
        return false;
    }

    fprintf(fp, "P6\n%d %d\n255\n", w, h);

    bool ok = true;
    if (!is_bgr) {
        if (fwrite(data, 1, (size_t)w * h * 3, fp) != (size_t)w * h * 3) {
            ok = false;
        }
    } else {
        uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
        if (!row) {
            fclose(fp);
            return false;
        }
        for (int y = 0; y < h && ok; y++) {
            const uint8_t *src = data + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                row[x * 3 + 0] = src[x * 3 + 2];    /* B -> R */
                row[x * 3 + 1] = src[x * 3 + 1];
                row[x * 3 + 2] = src[x * 3 + 0];    /* R -> B */
            }
            if (fwrite(row, 1, (size_t)w * 3, fp) != (size_t)w * 3) {
                ok = false;
            }
        }
        free(row);
    }

    fclose(fp);
    if (ok) {
        log_info("rga_preprocess: dump 完成 %s (%dx%d%s)", path, w, h,
                 is_bgr ? " bgr->rgb" : "");
    } else {
        log_warn("rga_preprocess: dump 写入失败 %s", path);
    }
    return ok;
}

#endif /* DMS_HW_PREPROCESS */
