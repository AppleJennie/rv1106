/*
 * imu_replay.c - 车辆 IMU 数据离线回放 CLI（PC 主机运行，无硬件依赖）。
 *
 * 从 stdin 或文件读取 100Hz IMU CSV，驱动完整车辆运动流水线：
 *   vehicle_imu（标定/姿态/重力补偿/jerk）
 *   → vehicle_motion（事件状态机，纯 IMU 无 CAN）
 *   → passenger_comfort（舒适度统计）
 *
 * 输入 CSV（# 注释，表头自动跳过）：
 *   t_ms,ax,ay,az,gx,gy,gz        （加速度 m/s²，角速度 rad/s，传感器坐标系）
 *
 * 用法：./imu_replay [input.csv]
 * 输出：每个新确认的运动事件一行 + 结束时舒适度行程汇总。
 *
 * 编译：gcc -Wall -Wextra -std=c11 -I vehicle_mcu/include -o tools/imu_replay \
 *       tools/imu_replay.c vehicle_mcu/src/vehicle_imu.c \
 *       vehicle_mcu/src/vehicle_can.c vehicle_mcu/src/vehicle_motion.c \
 *       vehicle_mcu/src/passenger_comfort.c -lm
 */

#include "vehicle_imu.h"
#include "vehicle_motion.h"
#include "passenger_comfort.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *fp = stdin;
    if (argc > 1 && !(fp = fopen(argv[1], "r"))) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    vehicle_imu_init(NULL);
    vehicle_motion_init(NULL);
    passenger_comfort_init(NULL);

    char line[256];
    long n_in = 0, n_events = 0, n_bad = 0;
    unsigned ready_ms = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        uint32_t t_ms;
        float ax, ay, az, gx, gy, gz;
        if (sscanf(line, "%u,%f,%f,%f,%f,%f,%f",
                   &t_ms, &ax, &ay, &az, &gx, &gy, &gz) != 7) {
            continue;  /* 表头或坏行 */
        }
        n_in++;

        vehicle_imu_state_t st = vehicle_imu_update(ax, ay, az, gx, gy, gz, t_ms);
        if (st == VEHICLE_IMU_ERROR) {
            fprintf(stderr, "IMU_ERROR at t=%u (标定失败：长期非静止?)\n", t_ms);
            break;
        }
        if (st != VEHICLE_IMU_READY) continue;   /* 标定中 */
        if (ready_ms == 0) {
            ready_ms = t_ms;
            fprintf(stderr, "[imu] calibrated, READY at t=%u\n", t_ms);
        }

        vehicle_imu_output_t imu;
        if (!vehicle_imu_get_output(&imu)) continue;

        vehicle_motion_input_t mi = {
            imu.longitudinal, imu.lateral, imu.vertical_accel,
            imu.longitudinal_jerk, imu.lateral_jerk, imu.vertical_jerk,
            t_ms,
        };
        vehicle_motion_output_t mo;
        vehicle_motion_update(&mi, NULL, &mo);   /* 纯 IMU，无 CAN */

        if (mo.entered_flags) {
            for (int e = VEHICLE_HARD_ACCEL; e <= VEHICLE_HIGH_LAT_JERK; e++) {
                if (mo.entered_flags & (1u << e)) {
                    printf("t=%u EVENT=%-16s conf=%.2f lon=%.2f lat=%.2f vert=%.2f "
                           "jerk_lon=%.2f jerk_lat=%.2f\n",
                           t_ms, vehicle_motion_event_name((vehicle_motion_event_t)e),
                           mo.confidence,
                           imu.longitudinal, imu.lateral, imu.vertical_accel,
                           imu.longitudinal_jerk, imu.lateral_jerk);
                    n_events++;
                }
            }
            passenger_comfort_on_motion_events(mo.entered_flags);
        }
        passenger_comfort_update(&imu);
    }

    if (argc > 1) fclose(fp);

    passenger_comfort_metrics_t m;
    passenger_comfort_get_metrics(&m);
    fprintf(stderr,
            "---\nsummary: samples=%ld events=%ld bad=%ld\n"
            "comfort: long=%.1f lat=%.1f vert=%.1f trip_index=%.1f\n"
            "counts: hard_brake=%u hard_accel=%u hard_turn=%u bump=%u high_jerk=%u\n",
            n_in, n_events, n_bad,
            m.longitudinal_smoothness, m.lateral_smoothness, m.vertical_comfort,
            m.trip_comfort_index,
            m.hard_brake_count, m.hard_accel_count, m.hard_turn_count,
            m.bump_count, m.high_jerk_count);
    return 0;
}
