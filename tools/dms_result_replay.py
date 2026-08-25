#!/usr/bin/env python3
"""
dms_result_replay.py - DMS 结果回放工具（PC 离线，无硬件依赖）。

读取 CSV / JSONL 的模拟 AI 结果，驱动 C 版 dms_product_bridge（唯一事实来源），
观察 risk level / risk score / alarm / event logger / MCU packet。

输入格式：
  CSV:   t_ms,status,face_score,ear,mar,head_down_score
  JSONL: {"t_ms": 100000, "status": "NORMAL", "face_score": 0.95, ...}

status: NORMAL / EYE_CLOSED / LONG_EYE_CLOSED / YAWN / HEAD_DOWN / NO_FACE

用法：
  python3 tools/dms_result_replay.py input.csv
  python3 tools/dms_result_replay.py input.jsonl --event-dir /tmp/xx
  python3 tools/dms_result_replay.py --scenario fatigue   # 生成内置演示场景
  python3 tools/dms_result_replay.py --scenario fatigue --dump-only  # 只生成输入不运行
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY_BIN = os.path.join(PROJECT_ROOT, "tools", "bridge_replay")

VALID_STATUS = {"NORMAL", "EYE_CLOSED", "LONG_EYE_CLOSED", "YAWN", "HEAD_DOWN", "NO_FACE"}

# 内置演示场景：司机先正常 10s → 哈欠 → 长闭眼 2s → 恢复 → 又一次长闭眼 → HIGH
SCENARIOS = {
    "fatigue": [
        (0, "NORMAL"), (10000, "YAWN"), (11200, "NORMAL"),
        (20000, "LONG_EYE_CLOSED"), (22000, "NORMAL"),
        (30000, "LONG_EYE_CLOSED"), (32500, "NORMAL"), (40000, "NORMAL"),
    ],
    "normal": [(0, "NORMAL"), (60000, "NORMAL")],
    "yawn_combo": [
        (0, "NORMAL"), (5000, "YAWN"), (6200, "NORMAL"),
        (10000, "YAWN"), (11200, "NORMAL"),
        (15000, "YAWN"), (16200, "NORMAL"), (30000, "NORMAL"),
    ],
}


def compile_replay():
    """编译 C 版 replay（如未编译或源码更新）。"""
    src = os.path.join(PROJECT_ROOT, "tools", "bridge_replay.c")
    if os.path.exists(REPLAY_BIN) and os.path.getmtime(REPLAY_BIN) > os.path.getmtime(src):
        return
    cmd = [
        "gcc", "-Wall", "-Wextra", "-std=c11", "-DDMS_HW_PREPROCESS=0",
        "-I", "include", "-o", REPLAY_BIN, src,
        "src/dms/dms_product_bridge.c", "src/dms/dms_risk_manager.c",
        "src/dms/dms_event_logger.c", "src/dms/dms_alarm_policy.c",
        "src/dms/dms_mcu_protocol.c",
    ]
    subprocess.run(cmd, cwd=PROJECT_ROOT, check=True)


def load_input(path):
    """读取 CSV/JSONL，统一成 (t_ms, status, face_score, ear, mar, head_down_score) 列表。"""
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        first = f.read(1)
        f.seek(0)
        if path.endswith(".jsonl") or first == "{":
            for line in f:
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                rows.append((int(d["t_ms"]), d["status"],
                             d.get("face_score", 0.95), d.get("ear", 0.28),
                             d.get("mar", 0.20), d.get("head_down_score", 0.10)))
        else:
            for r in csv.reader(f):
                if not r or r[0].startswith("#") or not r[0].strip().isdigit():
                    continue
                vals = list(r) + [""] * (6 - len(r))
                rows.append((int(vals[0]), vals[1].strip(),
                             float(vals[2] or 0.95), float(vals[3] or 0.28),
                             float(vals[4] or 0.20), float(vals[5] or 0.10)))
    for _, st, *_ in rows:
        if st not in VALID_STATUS:
            raise ValueError(f"unknown status: {st} (valid: {sorted(VALID_STATUS)})")
    return rows


def expand_scenario(name):
    """把 (时间点, 状态) 列表按 15FPS 展开成逐帧序列。"""
    keyframes = SCENARIOS[name]
    rows = []
    for i, (t_start, status) in enumerate(keyframes):
        t_end = keyframes[i + 1][0] if i + 1 < len(keyframes) else t_start + 66
        t = t_start
        while t < t_end:
            rows.append((t, status, 0.95, 0.28, 0.20, 0.10))
            t += 66
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="CSV/JSONL 输入文件")
    ap.add_argument("--scenario", choices=sorted(SCENARIOS), help="使用内置演示场景")
    ap.add_argument("--event-dir", default=None, help="事件日志输出目录")
    ap.add_argument("--no-heartbeat", action="store_true")
    ap.add_argument("--dump-only", action="store_true", help="只生成归一化输入，不运行")
    args = ap.parse_args()

    if not args.input and not args.scenario:
        ap.error("需要输入文件或 --scenario")

    rows = expand_scenario(args.scenario) if args.scenario else load_input(args.input)

    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as tf:
        tf.write("t_ms,status,face_score,ear,mar,head_down_score\n")
        for r in rows:
            tf.write(",".join(str(x) for x in r) + "\n")
        norm_path = tf.name

    if args.dump_only:
        print(norm_path)
        return

    event_dir = args.event_dir or tempfile.mkdtemp(prefix="dms_replay_events_")
    compile_replay()

    cmd = [REPLAY_BIN, norm_path, "--event-dir", event_dir]
    if args.no_heartbeat:
        cmd.append("--no-heartbeat")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    print(f"\n事件日志: {event_dir}/events.csv")
    if os.path.exists(os.path.join(event_dir, "events.csv")):
        with open(os.path.join(event_dir, "events.csv")) as f:
            print(f.read())
    os.unlink(norm_path)
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
