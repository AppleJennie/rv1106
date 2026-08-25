#!/usr/bin/env python3
"""
replay_bus_events.py - 公交事件融合回放工具（PC 离线，无硬件依赖）。

读取 DMS / 车辆运动 / 车速混合事件流（CSV 或 JSONL），驱动 C 版 bus_event_fusion
（唯一事实来源），输出触发的 Bus Safety Event。

输入 CSV（同一时间基准，毫秒）：
  t_ms,DMS,HEAD_DOWN,WARNING,1500
  t_ms,MOTION,HARD_BRAKE,80,-2.5,0.3
  t_ms,SPEED,8.3
JSONL：{"t_ms":..,"source":"DMS","event_type":"HEAD_DOWN","risk_level":"WARNING","duration_ms":1500}
       {"t_ms":..,"source":"MOTION","event_type":"HARD_BRAKE","confidence":80,"longitudinal_accel":-2.5}
       {"t_ms":..,"source":"SPEED","speed_mps":8.3}

用法：python3 tools/replay_bus_events.py input.csv|input.jsonl
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY_BIN = os.path.join(PROJECT_ROOT, "tools", "fusion_replay")


def compile_replay():
    src = os.path.join(PROJECT_ROOT, "tools", "fusion_replay.c")
    if os.path.exists(REPLAY_BIN) and os.path.getmtime(REPLAY_BIN) > os.path.getmtime(src):
        return
    subprocess.run([
        "gcc", "-Wall", "-Wextra", "-std=c11",
        "-I", "bus_event_fusion/include", "-o", REPLAY_BIN, src,
        "bus_event_fusion/src/bus_event_fusion.c",
    ], cwd=PROJECT_ROOT, check=True)


def jsonl_to_csv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            t = int(d["t_ms"])
            src = d["source"].upper()
            if src == "DMS":
                rows.append((t, "DMS", d["event_type"], d.get("risk_level", "NORMAL"),
                             d.get("duration_ms", 0)))
            elif src == "MOTION":
                rows.append((t, "MOTION", d["event_type"], d.get("confidence", 0),
                             d.get("longitudinal_accel", 0.0), d.get("lateral_accel", 0.0)))
            elif src == "SPEED":
                rows.append((t, "SPEED", d["speed_mps"]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="CSV 或 JSONL 事件流文件")
    args = ap.parse_args()

    if args.input.endswith(".jsonl"):
        rows = jsonl_to_csv(args.input)
        tf = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
        for r in rows:
            tf.write(",".join(str(x) for x in r) + "\n")
        tf.close()
        input_path = tf.name
    else:
        input_path = args.input

    compile_replay()
    proc = subprocess.run([REPLAY_BIN, input_path], capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)

    if input_path != args.input:
        os.unlink(input_path)
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
