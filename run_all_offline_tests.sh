#!/usr/bin/env bash
# run_all_offline_tests.sh - 一条命令跑完全部离线测试。
#
# 覆盖：
#   1. RV1106 产品化模块单测（risk_manager / mcu_protocol / product_bridge）
#   2. MCU 协议 10000 包压力测试
#   3. vehicle_mcu 车辆运动节点单测（IMU/运动状态机/CAN/舒适度）
#   4. bus_event_fusion 事件融合单测
#   5. server FastAPI + 排班风险 + 司机关怀 pytest
#
# 用法：./run_all_offline_tests.sh
# 退出码：全部通过 0；任何一项失败 1。

set -u
cd "$(dirname "$0")"

TOTAL_PASS=0
TOTAL_FAIL=0
SUITE_FAIL=0

# run_suite <名称> <命令...>
run_suite() {
    local name="$1"; shift
    echo "================================================================"
    echo "[suite] $name"
    echo "================================================================"
    if "$@"; then
        echo "[suite] $name: OK"
    else
        echo "[suite] $name: FAILED"
        SUITE_FAIL=$((SUITE_FAIL + 1))
    fi
    echo
}

HOST_CFLAGS="-Wall -Wextra -std=c11 -DDMS_HW_PREPROCESS=0 -I include"

# ---------- 1. RV1106 产品化模块单测 ----------
build_and_run() {
    local name="$1" bin="$2"; shift 2
    gcc $HOST_CFLAGS -o "$bin" "$@" || return 1
    "$bin"
}

run_suite "risk_manager unit"     bash -c "gcc $HOST_CFLAGS -o tests/test_dms_risk_manager tests/test_dms_risk_manager.c src/dms/dms_risk_manager.c && ./tests/test_dms_risk_manager"
run_suite "mcu_protocol unit"     bash -c "gcc $HOST_CFLAGS -o tests/test_dms_mcu_protocol tests/test_dms_mcu_protocol.c src/dms/dms_mcu_protocol.c && ./tests/test_dms_mcu_protocol"
run_suite "product_bridge unit"   bash -c "gcc $HOST_CFLAGS -o tests/test_dms_product_bridge tests/test_dms_product_bridge.c src/dms/dms_product_bridge.c src/dms/dms_risk_manager.c src/dms/dms_event_logger.c src/dms/dms_alarm_policy.c src/dms/dms_mcu_protocol.c && ./tests/test_dms_product_bridge"

# ---------- 2. MCU 协议压力测试 ----------
run_suite "mcu protocol stress"   bash -c "gcc -Wall -Wextra -std=c11 -I mcu/include -o mcu/tests/test_dms_protocol_stress mcu/tests/test_dms_protocol_stress.c mcu/src/dms_protocol.c && ./mcu/tests/test_dms_protocol_stress"

# ---------- 3. vehicle_mcu 车辆运动节点 ----------
if [ -f vehicle_mcu/tests/run_tests.sh ]; then
    run_suite "vehicle_mcu unit"  bash vehicle_mcu/tests/run_tests.sh
else
    echo "[suite] vehicle_mcu unit: SKIP (vehicle_mcu/tests/run_tests.sh 不存在)"
    SUITE_FAIL=$((SUITE_FAIL + 1))
fi

# ---------- 4. bus_event_fusion 事件融合 ----------
run_suite "bus_event_fusion unit" bash bus_event_fusion/tests/run_tests.sh

# ---------- 5. server pytest ----------
run_suite "server pytest"         bash server/run_tests.sh

# ---------- 汇总 ----------
echo "================================================================"
if [ "$SUITE_FAIL" -eq 0 ]; then
    echo "ALL OFFLINE TEST SUITES PASSED"
else
    echo "FAILED SUITES: $SUITE_FAIL"
fi
echo "================================================================"
exit "$SUITE_FAIL"
