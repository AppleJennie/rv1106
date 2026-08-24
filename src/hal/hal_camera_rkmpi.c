#include "hal_camera.h"
#include "sys_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_comm_vi.h"
#include "rk_comm_venc.h"
#include "rk_comm_video.h"
#include "rk_common.h"

#ifdef RV1126_RV1109
#include <rk_aiq_user_api_camgroup.h>
#include <rk_aiq_user_api_imgproc.h>
#include <rk_aiq_user_api_sysctl.h>
#else
#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>
#endif

#include <stdatomic.h>

#include "common.h"
#if DMS_HW_PREPROCESS
#include "dms_rga_preprocess.h"
#endif

/*
 * 修改要点：
 * 1. 不再 kill rkisp / rkaiq_3A_server，避免破坏 ISP/AIQ/白平衡链路。
 * 2. camera_force_cleanup() 不再强行重复调用 MPI 释放接口，避免初始化失败后二次释放导致段错误。
 * 3. VENC 创建成功后，预热失败会走 fail_venc，确保销毁 VENC。
 * 4. VENC vir_width / vir_height / buf_size 做 16 对齐，避免非 16 对齐尺寸踩坑。
 * 5. 首帧打印 VI 实际 width / height / vir_width / vir_height / fmt，方便排查颜色/stride 问题。
 */

#ifndef ALIGN_UP
#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))
#endif

#ifndef CAMERA_IQ_DIR
#define CAMERA_IQ_DIR            "/etc/iqfiles"
#endif

#ifndef CAMERA_AIQ_WARMUP_MS
#define CAMERA_AIQ_WARMUP_MS     1500
#endif

#ifndef CAMERA_WARMUP_FRAMES
#define CAMERA_WARMUP_FRAMES     CAMERA_FPS
#endif

#define MAX_AIQ_CTX              8
#define MAX_JPEG_SIZE_BYTES      (3 * 1024 * 1024)

/* ==================== 摄像头运行时上下文 ==================== */
typedef struct {
    bool initialized;
    int grab_ok_count;
    int grab_fail_count;       /* 当前连续失败计数 */
    int total_fail_count;
    int timeout_count;
    int restart_count;
    int restart_fail_count;
    int last_fps;
    uint64_t last_stat_ms;
    uint64_t last_frame_ms;
    size_t last_jpeg_size;
    size_t avg_jpeg_size;
    int stat_frames;
    pthread_mutex_t mutex;
} camera_ctx_t;

/* mutex 静态初始化，程序生命周期不 destroy */
static camera_ctx_t g_cam = {
    .initialized = false,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/*
 * 运行时分辨率参数。
 * DMS 模式下先尝试 1280x720@15，失败则回退到原始 1920x1536@30。
 * 非 DMS 模式下等于 common.h 里的宏。
 */
static int g_cam_width = CAMERA_WIDTH;
static int g_cam_height = CAMERA_HEIGHT;
static int g_cam_fps = CAMERA_FPS;

/* kill 系统服务只做一次 */
static bool g_service_killed = false;

/* 首帧 VI 信息只打印一次 */
static int g_vi_frame_printed = 0;

/* RKAIQ / IQ：MIS5001 颜色、曝光、白平衡依赖这部分 */
static rk_aiq_sys_ctx_t *g_aiq_ctx[MAX_AIQ_CTX];
static rk_aiq_working_mode_t g_WDRMode[MAX_AIQ_CTX];
static atomic_int g_sof_cnt = 0;
static atomic_bool g_should_quit = false;
static bool g_aiq_started = false;

/*
 * 安全清理残留：
 * 注意：这里不再直接调用 RK_MPI_VENC_DestroyChn / RK_MPI_SYS_Exit。
 * 原因：初始化失败路径里可能已经释放过一部分 MPI 资源，二次释放容易导致 Segmentation fault。
 */
static void camera_force_cleanup(void)
{
    log_warn("清理 RKMPI 临时残留文件，不做 MPI 二次释放");

    system("rm -f /tmp/UNIX.domain* 2>/dev/null");
    system("rm -f /tmp/rk* 2>/dev/null");
    system("rm -f /tmp/rt* 2>/dev/null");
    system("sync 2>/dev/null");

    usleep(500000);  /* 500ms，等驱动释放 */
}

static XCamReturn camera_aiq_sof_cb(rk_aiq_metas_t *meta)
{
    g_sof_cnt++;
    if (g_sof_cnt <= 2) {
        log_info("[AIQ] SOF frame=%u", meta->frame_id);
    }
    return XCAM_RETURN_NO_ERROR;
}

static XCamReturn camera_aiq_err_cb(rk_aiq_err_msg_t *msg)
{
    if (msg->err_code == XCAM_RETURN_BYPASS) {
        g_should_quit = true;
        log_warn("[AIQ] bypass/error callback: code=%d", msg->err_code);
    }
    return XCAM_RETURN_NO_ERROR;
}

static int camera_aiq_start(int cam_id, const char *iq_file_dir)
{
    if (g_aiq_started) {
        return 0;
    }

    if (cam_id < 0 || cam_id >= MAX_AIQ_CTX) {
        log_error("[AIQ] CamId 超限: %d", cam_id);
        return -1;
    }

    if (!iq_file_dir) {
        iq_file_dir = CAMERA_IQ_DIR;
    }

    setlinebuf(stdout);

    g_sof_cnt = 0;
    g_should_quit = false;
    g_WDRMode[cam_id] = RK_AIQ_WORKING_MODE_NORMAL;

    /* must set HDR_MODE before init */
    char hdr_str[16];
    snprintf(hdr_str, sizeof(hdr_str), "%d", (int)RK_AIQ_WORKING_MODE_NORMAL);
    setenv("HDR_MODE", hdr_str, 1);

    rk_aiq_static_info_t aiq_static_info;
    memset(&aiq_static_info, 0, sizeof(aiq_static_info));

#ifdef RV1126_RV1109
    rk_aiq_uapi_sysctl_enumStaticMetas(cam_id, &aiq_static_info);

    log_info("[AIQ] CamId=%d sensor=%s iq_dir=%s",
             cam_id, aiq_static_info.sensor_info.sensor_name, iq_file_dir);

    g_aiq_ctx[cam_id] =
        rk_aiq_uapi_sysctl_init(aiq_static_info.sensor_info.sensor_name,
                                iq_file_dir,
                                camera_aiq_err_cb,
                                camera_aiq_sof_cb);

    if (!g_aiq_ctx[cam_id]) {
        log_error("[AIQ] rk_aiq_uapi_sysctl_init 失败");
        return -1;
    }

    if (rk_aiq_uapi_sysctl_prepare(g_aiq_ctx[cam_id], 0, 0, g_WDRMode[cam_id])) {
        log_error("[AIQ] prepare 失败");
        rk_aiq_uapi_sysctl_deinit(g_aiq_ctx[cam_id]);
        g_aiq_ctx[cam_id] = NULL;
        return -1;
    }

    log_info("[AIQ] prepare 成功");

    if (rk_aiq_uapi_sysctl_start(g_aiq_ctx[cam_id])) {
        log_error("[AIQ] start 失败");
        rk_aiq_uapi_sysctl_deinit(g_aiq_ctx[cam_id]);
        g_aiq_ctx[cam_id] = NULL;
        return -1;
    }
#else
    rk_aiq_uapi2_sysctl_enumStaticMetas(cam_id, &aiq_static_info);

    log_info("[AIQ] CamId=%d sensor=%s iq_dir=%s",
             cam_id, aiq_static_info.sensor_info.sensor_name, iq_file_dir);

    g_aiq_ctx[cam_id] =
        rk_aiq_uapi2_sysctl_init(aiq_static_info.sensor_info.sensor_name,
                                 iq_file_dir,
                                 camera_aiq_err_cb,
                                 camera_aiq_sof_cb);

    if (!g_aiq_ctx[cam_id]) {
        log_error("[AIQ] rk_aiq_uapi2_sysctl_init 失败");
        return -1;
    }

    if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx[cam_id], 0, 0, g_WDRMode[cam_id])) {
        log_error("[AIQ] prepare 失败");
        rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[cam_id]);
        g_aiq_ctx[cam_id] = NULL;
        return -1;
    }

    log_info("[AIQ] prepare 成功");

    if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx[cam_id])) {
        log_error("[AIQ] start 失败");
        rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[cam_id]);
        g_aiq_ctx[cam_id] = NULL;
        return -1;
    }
#endif

    g_aiq_started = true;
    log_info("[AIQ] start 成功");
    return 0;
}

static void camera_aiq_stop(int cam_id)
{
    if (!g_aiq_started) {
        return;
    }

    if (cam_id < 0 || cam_id >= MAX_AIQ_CTX || !g_aiq_ctx[cam_id]) {
        g_aiq_started = false;
        return;
    }

#ifdef RV1126_RV1109
    rk_aiq_uapi_sysctl_stop(g_aiq_ctx[cam_id], false);
    rk_aiq_uapi_sysctl_deinit(g_aiq_ctx[cam_id]);
#else
    rk_aiq_uapi2_sysctl_stop(g_aiq_ctx[cam_id], false);
    rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[cam_id]);
#endif

    g_aiq_ctx[cam_id] = NULL;
    g_aiq_started = false;

    log_info("[AIQ] stop/deinit 完成");
}

/* ==================== 工具函数 ==================== */
static uint64_t get_mono_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void camera_reset_runtime_stats_locked(bool keep_restart)
{
    int saved_restart = g_cam.restart_count;
    int saved_restart_fail = g_cam.restart_fail_count;

    g_cam.initialized = false;
    g_cam.grab_ok_count = 0;
    g_cam.grab_fail_count = 0;
    g_cam.total_fail_count = 0;
    g_cam.timeout_count = 0;
    g_cam.last_fps = 0;
    g_cam.last_stat_ms = get_time_ms();
    g_cam.last_frame_ms = 0;
    g_cam.last_jpeg_size = 0;
    g_cam.avg_jpeg_size = 0;
    g_cam.stat_frames = 0;

    if (keep_restart) {
        g_cam.restart_count = saved_restart;
        g_cam.restart_fail_count = saved_restart_fail;
    } else {
        g_cam.restart_count = 0;
        g_cam.restart_fail_count = 0;
    }
}

static void camera_update_stats(bool ok, size_t jpeg_size)
{
    pthread_mutex_lock(&g_cam.mutex);

    uint64_t now_ms = get_time_ms();

    if (ok) {
        g_cam.grab_ok_count++;
        g_cam.grab_fail_count = 0;
        g_cam.last_jpeg_size = jpeg_size;
        g_cam.last_frame_ms = now_ms;

        /* 滑动平均 JPEG 大小 */
        if (g_cam.avg_jpeg_size == 0) {
            g_cam.avg_jpeg_size = jpeg_size;
        } else {
            g_cam.avg_jpeg_size = (g_cam.avg_jpeg_size * 15 + jpeg_size) / 16;
        }

        g_cam.stat_frames++;
    } else {
        g_cam.grab_fail_count++;
        g_cam.total_fail_count++;
    }

    /* 每秒打印一次统计 */
    if (now_ms - g_cam.last_stat_ms >= STAT_PRINT_INTERVAL_MS) {
        if (g_cam.last_stat_ms > 0) {
            log_info("[CAM] fps=%d, ok=%d, fail_cur=%d, fail_total=%d, "
                     "timeout=%d, restart=%d, restart_fail=%d, jpeg=%zu/%zu bytes",
                     g_cam.stat_frames,
                     g_cam.grab_ok_count,
                     g_cam.grab_fail_count,
                     g_cam.total_fail_count,
                     g_cam.timeout_count,
                     g_cam.restart_count,
                     g_cam.restart_fail_count,
                     g_cam.last_jpeg_size,
                     g_cam.avg_jpeg_size);
        }
        g_cam.last_fps = g_cam.stat_frames;
        g_cam.last_stat_ms = now_ms;
        g_cam.stat_frames = 0;
    }

    pthread_mutex_unlock(&g_cam.mutex);
}

/* ==================== VI dev 初始化 ==================== */
static int vi_dev_init(void)
{
    int ret;
    int devId = RK_VI_DEV_ID;
    int pipeId = devId;

    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    memset(&stDevAttr, 0, sizeof(stDevAttr));
    memset(&stBindPipe, 0, sizeof(stBindPipe));

    ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
        if (ret != RK_SUCCESS) {
            log_error("RK_MPI_VI_SetDevAttr 失败: %#x", ret);
            return -1;
        }
    }

    ret = RK_MPI_VI_GetDevIsEnable(devId);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(devId);
        if (ret != RK_SUCCESS) {
            log_error("RK_MPI_VI_EnableDev 失败: %#x", ret);
            return -1;
        }

        stBindPipe.u32Num = 1;
        stBindPipe.PipeId[0] = pipeId;
        ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
        if (ret != RK_SUCCESS) {
            log_error("RK_MPI_VI_SetDevBindPipe 失败: %#x", ret);
            return -1;
        }
    }

    return 0;
}

/* ==================== VI chn 初始化 ==================== */
static int vi_chn_init(int chnId, int width, int height)
{
    int ret;
    int buf_cnt = 3;

    VI_CHN_ATTR_S vi_chn_attr;
    memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));

    vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
    vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi_chn_attr.stSize.u32Width = width;
    vi_chn_attr.stSize.u32Height = height;
    vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    vi_chn_attr.u32Depth = 2;   /* 手动取帧模式必须 > 0 */
    vi_chn_attr.stFrameRate.s32SrcFrameRate = g_cam_fps;
    vi_chn_attr.stFrameRate.s32DstFrameRate = g_cam_fps;

    ret = RK_MPI_VI_SetChnAttr(RK_VI_DEV_ID, chnId, &vi_chn_attr);
    ret |= RK_MPI_VI_EnableChn(RK_VI_DEV_ID, chnId);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VI_SetChnAttr/EnableChn 失败: %#x", ret);
        return -1;
    }

    return 0;
}

/* ==================== VENC JPEG 初始化 ==================== */
static int venc_jpeg_init(int chnId, int width, int height)
{
    VENC_CHN_ATTR_S stAttr;
    memset(&stAttr, 0, sizeof(stAttr));

    int vir_width  = ALIGN_UP(width, 16);
    int vir_height = ALIGN_UP(height, 16);

    stAttr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
    stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    stAttr.stVencAttr.u32PicWidth = width;
    stAttr.stVencAttr.u32PicHeight = height;
    stAttr.stVencAttr.u32VirWidth = vir_width;
    stAttr.stVencAttr.u32VirHeight = vir_height;
    stAttr.stVencAttr.u32StreamBufCnt = 2;
    stAttr.stVencAttr.u32BufSize = vir_width * vir_height * 3 / 2;
    stAttr.stVencAttr.enMirror = MIRROR_NONE;

    stAttr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
    stAttr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
    stAttr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;

    int ret = RK_MPI_VENC_CreateChn(chnId, &stAttr);
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_VENC_CreateChn 失败: %#x", ret);
        return -1;
    }

    VENC_JPEG_PARAM_S stJpegParam;
    memset(&stJpegParam, 0, sizeof(stJpegParam));
    stJpegParam.u32Qfactor = 85;
    RK_MPI_VENC_SetJpegParam(chnId, &stJpegParam);

    log_info("VENC JPEG 初始化完成: pic=%dx%d, vir=%dx%d, q=%u",
             width, height, vir_width, vir_height, stJpegParam.u32Qfactor);

    return 0;
}

/* ==================== 单次抓图：VI GetFrame → VENC SendFrame → GetStream ==================== */
static bool venc_grab_one_frame(uint8_t **jpeg_buf, int *jpeg_size, int timeout_ms)
{
    int ret;
    VIDEO_FRAME_INFO_S vi_frame;
    memset(&vi_frame, 0, sizeof(vi_frame));

    /* 1. 从 VI 取一帧 */
    uint64_t t_vi_0 = get_mono_time_us();
    ret = RK_MPI_VI_GetChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame, timeout_ms);
    uint64_t vi_get_us = get_mono_time_us() - t_vi_0;
    if (ret != RK_SUCCESS) {
        log_warn("RK_MPI_VI_GetChnFrame 失败/超时: %#x", ret);
        return false;
    }

    if (!g_vi_frame_printed) {
        log_info("VI actual: width=%u height=%u vir_width=%u vir_height=%u fmt=%d",
                 vi_frame.stVFrame.u32Width,
                 vi_frame.stVFrame.u32Height,
                 vi_frame.stVFrame.u32VirWidth,
                 vi_frame.stVFrame.u32VirHeight,
                 vi_frame.stVFrame.enPixelFormat);
        g_vi_frame_printed = 1;
    }

#if DMS_HW_PREPROCESS
    /*
     * V2-A：VI 帧 Release 之前先做 RGA 硬件预处理（NV12 DMABUF 直转
     * 640x640 BGR + 半幅 RGB），AI 主路径不再依赖软件 JPEG 解码。
     * RGA 失败不影响 VENC JPEG 主链路。
     */
    dms_rga_preprocess_from_vi(&vi_frame, vi_get_us);
#endif

    /* 2. 启动 VENC 接收 */
    VENC_RECV_PIC_PARAM_S recv_param;
    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = 1;

    ret = RK_MPI_VENC_StartRecvFrame(RK_VENC_CHN_ID, &recv_param);
    if (ret != RK_SUCCESS) {
        log_warn("RK_MPI_VENC_StartRecvFrame 失败: %#x", ret);
        RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);
        return false;
    }

    /* 3. 把 VI 帧送给 VENC 编码 */
    ret = RK_MPI_VENC_SendFrame(RK_VENC_CHN_ID, &vi_frame, timeout_ms);
    if (ret != RK_SUCCESS) {
        log_warn("RK_MPI_VENC_SendFrame 失败: %#x", ret);
        RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
        RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);
        return false;
    }

    /* 4. 取 JPEG 码流 */
    VENC_PACK_S pack;
    VENC_STREAM_S stream;
    memset(&pack, 0, sizeof(pack));
    memset(&stream, 0, sizeof(stream));
    stream.pstPack = &pack;

    ret = RK_MPI_VENC_GetStream(RK_VENC_CHN_ID, &stream, timeout_ms);
    if (ret != RK_SUCCESS) {
        log_warn("RK_MPI_VENC_GetStream 失败/超时: %#x", ret);
        RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
        RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);
        return false;
    }

    /* 5. 拷贝 JPEG 数据 */
    void *vir_addr = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
    RK_U32 len = stream.pstPack->u32Len;

    if (!vir_addr || len == 0 || len > MAX_JPEG_SIZE_BYTES) {
        log_error("VENC 返回异常 JPEG 数据: vir=%p, len=%u", vir_addr, len);
        RK_MPI_VENC_ReleaseStream(RK_VENC_CHN_ID, &stream);
        RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
        RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        log_error("JPEG malloc 失败, size=%u", len);
        RK_MPI_VENC_ReleaseStream(RK_VENC_CHN_ID, &stream);
        RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
        RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);
        return false;
    }

    memcpy(buf, vir_addr, len);
    *jpeg_buf = buf;
    *jpeg_size = (int)len;

    /* 6. 释放资源 */
    RK_MPI_VENC_ReleaseStream(RK_VENC_CHN_ID, &stream);
    RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
    RK_MPI_VI_ReleaseChnFrame(RK_VI_DEV_ID, RK_VI_CHN_ID, &vi_frame);

    return true;
}

/* ==================== 内部 locked 初始化 ==================== */
static bool camera_init_locked(void)
{
    int ret;

    if (g_cam.initialized) {
        return true;
    }

    camera_reset_runtime_stats_locked(true);
    g_cam.last_stat_ms = get_time_ms();
    g_vi_frame_printed = 0;

    log_info("初始化 RKMPI 摄像头链路 (VI + VENC JPEG, 独立模式)");
    log_info("CAMERA BUILD CONFIG: width=%d height=%d fps=%d interval=%d us",
             g_cam_width, g_cam_height, g_cam_fps, CAPTURE_INTERVAL_US);

    /*
     * 只停止真正可能占用 VENC/VI 的业务进程。
     * 不杀 rkisp / rkaiq_3A_server，避免破坏 ISP/AIQ/白平衡链路。
     */
    if (!g_service_killed) {
        system("killall rkipc mpp_service > /dev/null 2>&1");
        sleep(1);
        system("killall -9 rkipc mpp_service > /dev/null 2>&1");
        sleep(1);
        g_service_killed = true;
    }

    if (camera_aiq_start(0, CAMERA_IQ_DIR) != 0) {
        log_error("RKAIQ 启动失败");
        goto fail;
    }

    /* 等待 AE/AWB/颜色校正稳定；后面仍会再丢 CAMERA_WARMUP_FRAMES 帧 */
    usleep(CAMERA_AIQ_WARMUP_MS * 1000);

    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        log_error("RK_MPI_SYS_Init 失败: %#x", ret);
        goto fail_aiq;
    }

    if (vi_dev_init() != 0) {
        goto fail_sys;
    }

    if (vi_chn_init(RK_VI_CHN_ID, g_cam_width, g_cam_height) != 0) {
        goto fail_sys;
    }

    if (venc_jpeg_init(RK_VENC_CHN_ID, g_cam_width, g_cam_height) != 0) {
        goto fail_vi;
    }

    /* 不 Bind VI→VENC，采用独立 GetFrame + SendFrame 模式 */

    log_info("RKMPI 预热中 (%d 帧)...", g_cam_fps);
    int warmup_ok = 0;
    for (int i = 0; i < g_cam_fps; i++) {
        uint8_t *tmp_buf = NULL;
        int tmp_size = 0;
        if (venc_grab_one_frame(&tmp_buf, &tmp_size, 700)) {
            free(tmp_buf);
            warmup_ok++;
        }
        usleep(10000);
    }

    if (warmup_ok < 2) {
        log_error("RKMPI 预热失败，仅成功 %d/%d 帧", warmup_ok, CAMERA_WARMUP_FRAMES);
        goto fail_venc;
    }

    log_info("RKMPI 预热完成，成功 %d/%d 帧", warmup_ok, CAMERA_FPS);
    g_cam.initialized = true;
    g_cam.grab_fail_count = 0;
    log_info("RKMPI 摄像头初始化完成: %dx%d @%dFPS JPEG",
             g_cam_width, g_cam_height, g_cam_fps);
    return true;

fail_venc:
    RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
    RK_MPI_VENC_DestroyChn(RK_VENC_CHN_ID);

fail_vi:
    RK_MPI_VI_DisableChn(RK_VI_DEV_ID, RK_VI_CHN_ID);
    RK_MPI_VI_DisableDev(RK_VI_DEV_ID);

fail_sys:
    RK_MPI_SYS_Exit();

fail_aiq:
    camera_aiq_stop(0);

fail:
    g_cam.initialized = false;
    return false;
}

/* ==================== 内部 locked 反初始化 ==================== */
static void camera_deinit_locked(void)
{
    if (!g_cam.initialized) {
        return;
    }

    log_info("关闭 RKMPI 摄像头");

    /* 独立模式不需要 UnBind */
    RK_MPI_VENC_StopRecvFrame(RK_VENC_CHN_ID);
    RK_MPI_VENC_DestroyChn(RK_VENC_CHN_ID);
    RK_MPI_VI_DisableChn(RK_VI_DEV_ID, RK_VI_CHN_ID);
    RK_MPI_VI_DisableDev(RK_VI_DEV_ID);
    RK_MPI_SYS_Exit();

    camera_aiq_stop(0);

    g_cam.initialized = false;
    log_info("RKMPI 摄像头已关闭");
}

/* ==================== 对外接口：查询当前分辨率 ==================== */
void camera_get_resolution(int *width, int *height, int *fps)
{
    pthread_mutex_lock(&g_cam.mutex);
    if (width)  *width  = g_cam_width;
    if (height) *height = g_cam_height;
    if (fps)    *fps    = g_cam_fps;
    pthread_mutex_unlock(&g_cam.mutex);
}

/* ==================== 对外接口：初始化 ==================== */
bool camera_init(void)
{
    bool ok = false;

    pthread_mutex_lock(&g_cam.mutex);

    for (int attempt = 1; attempt <= 3; attempt++) {
        log_info("camera_init 尝试 %d/3", attempt);

        ok = camera_init_locked();
        if (ok) {
            break;
        }

        log_error("camera_init 尝试 %d 失败", attempt);

        if (!g_run_flag) {
            log_warn("g_run_flag=0，终止重试");
            break;
        }

        if (attempt < 3) {
            /*
             * 不再调用 camera_deinit_locked()。
             * 因为 initialized=false 时它不会释放半初始化资源；
             * 半初始化资源已经由 camera_init_locked() 的 fail_* 路径释放。
             */
            pthread_mutex_unlock(&g_cam.mutex);

            camera_force_cleanup();

            pthread_mutex_lock(&g_cam.mutex);
        }
    }

#ifdef DMS_MODE
    /*
     * DMS 模式下，如果建议分辨率初始化失败，自动回退到原始分辨率。
     * 这样在 1280x720 不支持的模组上也能跑起来。
     */
    if (!ok &&
        (g_cam_width != ORIGINAL_CAMERA_WIDTH ||
         g_cam_height != ORIGINAL_CAMERA_HEIGHT ||
         g_cam_fps != ORIGINAL_CAMERA_FPS)) {
        log_warn("DMS 分辨率 %dx%d@%d 初始化失败，回退到 %dx%d@%d",
                 g_cam_width, g_cam_height, g_cam_fps,
                 ORIGINAL_CAMERA_WIDTH, ORIGINAL_CAMERA_HEIGHT,
                 ORIGINAL_CAMERA_FPS);

        g_cam_width = ORIGINAL_CAMERA_WIDTH;
        g_cam_height = ORIGINAL_CAMERA_HEIGHT;
        g_cam_fps = ORIGINAL_CAMERA_FPS;

        for (int attempt = 1; attempt <= 3; attempt++) {
            log_info("camera_init 回退分辨率尝试 %d/3", attempt);
            ok = camera_init_locked();
            if (ok) {
                break;
            }

            log_error("camera_init 回退分辨率尝试 %d 失败", attempt);

            if (!g_run_flag) {
                log_warn("g_run_flag=0，终止重试");
                break;
            }

            if (attempt < 3) {
                pthread_mutex_unlock(&g_cam.mutex);
                camera_force_cleanup();
                pthread_mutex_lock(&g_cam.mutex);
            }
        }
    }
#endif

    pthread_mutex_unlock(&g_cam.mutex);
    return ok;
}

/* ==================== 对外接口：抓取 JPEG ==================== */
bool camera_grab_jpeg(uint8_t **jpeg_buf, int *jpeg_size)
{
    if (!jpeg_buf || !jpeg_size) {
        return false;
    }

    *jpeg_buf = NULL;
    *jpeg_size = 0;

    bool initialized = false;

    pthread_mutex_lock(&g_cam.mutex);
    initialized = g_cam.initialized;
    pthread_mutex_unlock(&g_cam.mutex);

    if (!initialized) {
        log_error("RKMPI 摄像头未初始化");
        return false;
    }

    uint8_t *buf = NULL;
    int size = 0;

    bool ok = venc_grab_one_frame(&buf, &size, RK_GETSTREAM_TIMEOUT_MS);

    if (!ok) {
        /* 锁外统计 timeout（避免和 update_stats 二次锁冲突） */
        pthread_mutex_lock(&g_cam.mutex);
        g_cam.timeout_count++;
        pthread_mutex_unlock(&g_cam.mutex);

        camera_update_stats(false, 0);
        return false;
    }

    *jpeg_buf = buf;
    *jpeg_size = size;
    camera_update_stats(true, (size_t)size);
    return true;
}

/* ==================== 对外接口：重启 ==================== */
bool camera_restart(void)
{
    bool ok;

    pthread_mutex_lock(&g_cam.mutex);
    g_cam.restart_count++;
    log_warn("重启 RKMPI 摄像头链路 (restart_count=%d)", g_cam.restart_count);
    camera_deinit_locked();
    pthread_mutex_unlock(&g_cam.mutex);

    usleep(300000);  /* 300ms，让硬件释放 */

    pthread_mutex_lock(&g_cam.mutex);
    ok = camera_init_locked();
    if (!ok) {
        g_cam.restart_fail_count++;
        log_error("camera_init_locked 失败，restart_fail_count=%d", g_cam.restart_fail_count);
    }
    pthread_mutex_unlock(&g_cam.mutex);

    return ok;
}

/* ==================== 对外接口：健康检查 ==================== */
bool camera_health_ok(void)
{
    bool ok;
    pthread_mutex_lock(&g_cam.mutex);
    ok = (g_cam.grab_fail_count < GRAB_FAIL_FATAL_THRESHOLD);
    pthread_mutex_unlock(&g_cam.mutex);
    return ok;
}

/* ==================== 对外接口：获取统计 ==================== */
void camera_get_stats(camera_stats_t *out)
{
    if (!out) return;

    pthread_mutex_lock(&g_cam.mutex);
    out->initialized        = g_cam.initialized;
    out->grab_ok_count      = g_cam.grab_ok_count;
    out->grab_fail_count    = g_cam.grab_fail_count;
    out->total_fail_count   = g_cam.total_fail_count;
    out->timeout_count      = g_cam.timeout_count;
    out->restart_count      = g_cam.restart_count;
    out->restart_fail_count = g_cam.restart_fail_count;
    out->last_fps           = g_cam.last_fps;
    out->last_jpeg_size     = g_cam.last_jpeg_size;
    out->avg_jpeg_size      = g_cam.avg_jpeg_size;
    pthread_mutex_unlock(&g_cam.mutex);
}

/* ==================== 对外接口：反初始化 ==================== */
void camera_deinit(void)
{
    pthread_mutex_lock(&g_cam.mutex);
    camera_deinit_locked();
    pthread_mutex_unlock(&g_cam.mutex);
}
