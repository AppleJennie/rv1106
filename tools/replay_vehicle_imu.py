#!/usr/bin/env python3
"""
replay_vehicle_imu.py - 车辆 IMU CSV 回归工具（PC 离线，无硬件依赖）。

读取真实采集或仿真的 100Hz IMU CSV，驱动 C 版 vehicle_mcu 完整流水线
（标定/姿态/重力补偿/jerk → 运动事件状态机 → 舒适度），输出事件与行程汇总。

输入 CSV：
  t_ms,ax,ay,az,gx,gy,gz     （加速度 m/s²，角速度 rad/s，传感器坐标系）
注意：流水线开头需约 2s 静止数据用于开机标定（默认 200 个静止样本）。

用法：python3 tools/replay_vehicle_imu.py imu.csv
      python3 tools/replay_vehicle_imu.py --demo   # 生成一段仿真数据演示
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY_BIN = os.path.join(PROJECT_ROOT, "tools", "imu_replay")


def compile_replay():
    src = os.path.join(PROJECT_ROOT, "tools", "imu_replay.c")
    if os.path.exists(REPLAY_BIN) and os.path.getmtime(REPLAY_BIN) > os.path.getmtime(src):
        return
    subprocess.run([
        "gcc", "-Wall", "-Wextra", "-std=c11",
        "-I", "vehicle_mcu/include", "-o", REPLAY_BIN, src,
        "vehicle_mcu/src/vehicle_imu.c", "vehicle_mcu/src/vehicle_can.c",
        "vehicle_mcu/src/vehicle_motion.c", "vehicle_mcu/src/passenger_comfort.c",
        "-lm",
    ], cwd=PROJECT_ROOT, check=True)


def gen_demo():
    """生成仿真 CSV：2s 静止标定 → 2s 匀速 → 1s 急刹(-2.5m/s²) → 2s 正常 → 一次颠簸。"""
    rows = []
    t = 0
    # 静止标定段（2.5s）：a = (0, 0, g)，gyro = 0
    for _ in range(250):
        rows.append((t, 0.0, 0.0, 9.81, 0.0, 0.0, 0.0)); t += 10
    # 匀速段（无加速度）
    for _ in range(200):
        rows.append((t, 0.0, 0.0, 9.81, 0.0, 0.0, 0.0)); t += 10
    # 急刹 0.5s：纵向 -2.5
    for _ in range(50):
        rows.append((t, -2.5, 0.0, 9.81, 0.0, 0.0, 0.0)); t += 10
    # 正常 2s
    for _ in range(200):
        rows.append((t, 0.0, 0.0, 9.81, 0.0, 0.0, 0.0)); t += 10
    # 颠簸：垂向 3.0 持续 50ms
    for _ in range(5):
        rows.append((t, 0.0, 0.0, 9.81 + 3.0, 0.0, 0.0, 0.0)); t += 10
    # 收尾 1s
    for _ in range(100):
        rows.append((t, 0.0, 0.0, 9.81, 0.0, 0.0, 0.0)); t += 10
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="IMU CSV 文件")
    ap.add_argument("--demo", action="store_true", help="生成内置仿真数据并回放")
    args = ap.parse_args()

    if not args.input and not args.demo:
        ap.error("需要输入文件或 --demo")

    input_path = args.input
    if args.demo:
        tf = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
        tf.write("t_ms,ax,ay,az,gx,gy,gz\n")
        for r in gen_demo():
            tf.write(",".join(str(x) for x in r) + "\n")
        tf.close()
        input_path = tf.name

    compile_replay()
    proc = subprocess.run([REPLAY_BIN, input_path], capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)

    if args.demo:
        os.unlink(input_path)
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
