#include "hal_camera.h"
#include "sys_logger.h"
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

static cv::VideoCapture cap;
static bool initialized = false;

bool camera_init(void)
{
    // 1. 释放 RV1106 系统占用的摄像头资源（与您验证代码一致）
    log_info("释放摄像头系统资源...");
    system("killall -9 rkipc rkisp mpp_service > /dev/null 2>&1");
    sleep(3);
    system("killall -9 rkipc rkisp mpp_service > /dev/null 2>&1");
    sleep(2);

    // 2. 打开摄像头
    log_info("打开摄像头设备: %s", CAMERA_DEVICE);
    cap.open(0);  // /dev/video0
    if (!cap.isOpened()) {
        log_error("摄像头打开失败");
        return false;
    }

    // 3. 设置分辨率
    cap.set(cv::CAP_PROP_FRAME_WIDTH, CAMERA_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT);
    log_info("摄像头分辨率设置为: %dx%d", CAMERA_WIDTH, CAMERA_HEIGHT);

    // 4. ISP 预热（抓取100帧丢弃，确保图像稳定）
    log_info("摄像头 ISP 预热中...");
    cv::Mat dummy;
    for (int i = 0; i < 100; i++) {
        cap >> dummy;
        usleep(10000);
    }
    log_info("摄像头预热完成");

    initialized = true;
    return true;
}

bool camera_grab_frame(cv::Mat &frame)
{
    if (!initialized || !cap.isOpened()) {
        log_error("摄像头未初始化");
        return false;
    }
    cap >> frame;
    if (frame.empty()) {
        log_warn("抓取到空帧");
        return false;
    }
    return true;
}

bool camera_grab_jpeg(uint8_t **jpeg_buf, int *jpeg_size)
{
    cv::Mat frame;
    if (!camera_grab_frame(frame)) {
        return false;
    }

    // JPEG 编码，质量 85（平衡画质与大小）
    std::vector<uint8_t> jpeg_vec;
    std::vector<int> encode_params;
    encode_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    encode_params.push_back(85);

    if (!cv::imencode(".jpg", frame, jpeg_vec, encode_params)) {
        log_error("JPEG 编码失败");
        return false;
    }

    *jpeg_size = jpeg_vec.size();
    *jpeg_buf = (uint8_t *)malloc(*jpeg_size);
    if (!*jpeg_buf) {
        log_error("JPEG 缓冲区内存分配失败");
        return false;
    }
    memcpy(*jpeg_buf, jpeg_vec.data(), *jpeg_size);
    return true;
}

void camera_deinit(void)
{
    if (cap.isOpened()) {
        cap.release();
    }
    initialized = false;
    log_info("摄像头已关闭");
}


