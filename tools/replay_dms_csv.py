#!/usr/bin/env python3
"""
replay_dms_csv.py - DMS 结果 CSV 回归工具入口。

功能与 tools/dms_result_replay.py 完全一致（它支持 CSV/JSONL/内置场景），
本脚本只是按 TASK O 约定提供的固定入口名。

用法：python3 tools/replay_dms_csv.py input.csv|input.jsonl [--event-dir DIR]
      python3 tools/replay_dms_csv.py --scenario fatigue
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

if __name__ == "__main__":
    sys.exit(subprocess.run(
        [sys.executable, os.path.join(HERE, "dms_result_replay.py")] + sys.argv[1:]
    ).returncode)
