cmake_minimum_required(VERSION 3.5)

set(CMAKE_C_COMPILER "/home/jennie/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "/home/jennie/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")

project(hand_capture_right)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)

# Runtime library search path on the target board
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "/oem/usr/lib;/usr/lib")

# RKMPI SDK 路径（根据实际环境修改）
set(RKMPI_DIR "/home/jennie/hhh/luckfox-pico/media/rockit/out")
set(MPP_DIR    "/home/jennie/hhh/luckfox-pico/media/out")
set(RKAIQ_DIR  "/home/jennie/hhh/luckfox-pico/media/isp/out")
set(RKNN_DIR   "/home/jennie/hhh/luckfox-pico/media/out")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb_image
    ${RKMPI_DIR}/include
    ${MPP_DIR}/include
    ${RKAIQ_DIR}/include
    ${RKAIQ_DIR}/include/rkaiq
    ${RKAIQ_DIR}/include/rkaiq/uAPI2
    ${RKAIQ_DIR}/include/rkaiq/xcore
    ${RKAIQ_DIR}/include/rkaiq/algos
    ${RKAIQ_DIR}/include/rkaiq/common
    ${RKAIQ_DIR}/include/rkaiq/iq_parser
    ${RKAIQ_DIR}/include/rkaiq/iq_parser_v2
)

add_executable(hand_capture_right
    src/app/main.c
    src/app/state_machine.c
    src/sys/sys_logger.c
    src/sys/sys_storage.c
    src/sys/sys_time.c
    src/sys/sys_health.c
    src/sys/sys_dms_storage.c
    src/dms/dms_infer.c
    src/dms/dms_face_detect.c
    src/dms/dms_retinaface.c
    src/dms/dms_face_landmark.c
    src/dms/dms_face_landmark_106.c
    src/dms/dms_fatigue_logic.c
    src/dms/dms_image_utils.c
    src/dms/dms_stream_server.c
    src/dms/dms_ai_thread.c
    src/dms/dms_visualize.c
    src/hal/hal_gpio.cpp
    src/hal/hal_led.c
    src/hal/hal_encoder.c
    src/hal/hal_beep.c
    src/hal/hal_camera_rkmpi.c
    src/hal/hal_power.c
    src/net/net_transfer.c
)

target_compile_definitions(hand_capture_right PRIVATE RKAIQ)

find_library(RKAIQ_LIB
    NAMES rkaiq
    PATHS
        ${MPP_DIR}/lib
        ${RKAIQ_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT RKAIQ_LIB)
    message(FATAL_ERROR "librkaiq.so not found. Check MPP_DIR/RKAIQ_DIR in CMakeLists.txt")
endif()

find_library(RKNNMRT_LIB
    NAMES rknnmrt
    PATHS
        ${RKNN_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT RKNNMRT_LIB)
    message(FATAL_ERROR "librknnmrt.so not found. Check RKNN_DIR in CMakeLists.txt")
endif()

target_link_libraries(hand_capture_right
    ${RKMPI_DIR}/lib/librockit.so
    ${MPP_DIR}/lib/librockchip_mpp.so
    ${MPP_DIR}/lib/librga.so
    ${RKAIQ_LIB}
    ${RKNNMRT_LIB}
    pthread
    dl
    m
)

install(TARGETS hand_capture_right DESTINATION bin)
