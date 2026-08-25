#!/bin/bash
# run_tests.sh - vehicle_mcu 全部单元测试：编译 + 运行 + 统计
#
# 用法：vehicle_mcu/tests/run_tests.sh
# 编译选项：gcc -Wall -Wextra -Werror -std=c11（-Werror 强制零警告）
# 末尾打印 PASS/FAIL 总数；有失败时退出码非 0。

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -Werror -std=c11"
INCLUDES="-I$ROOT_DIR/include -I$SCRIPT_DIR"
LIBM="-lm"

SRCS="$ROOT_DIR/src/vehicle_imu.c \
$ROOT_DIR/src/vehicle_can.c \
$ROOT_DIR/src/vehicle_motion.c \
$ROOT_DIR/src/passenger_comfort.c"

TESTS="test_vehicle_imu test_vehicle_can test_vehicle_motion test_passenger_comfort"

total_pass=0
total_fail=0

for t in $TESTS; do
    bin="$SCRIPT_DIR/$t"
    echo "======== 编译 $t ========"
    if ! $CC $CFLAGS $INCLUDES -o "$bin" "$SCRIPT_DIR/$t.c" $SRCS $LIBM; then
        echo "FAIL: $t 编译失败"
        total_fail=$((total_fail + 1))
        continue
    fi
    echo "======== 运行 $t ========"
    out="$("$bin" 2>&1)"
    rc=$?
    printf '%s\n' "$out"
    p=$(printf '%s\n' "$out" | grep -c '^PASS:' || true)
    f=$(printf '%s\n' "$out" | grep -c '^FAIL:' || true)
    total_pass=$((total_pass + p))
    total_fail=$((total_fail + f))
    if [ "$rc" -ne 0 ]; then
        echo "!! $t 退出码 $rc"
    fi
done

echo "========================================"
echo "总 PASS: $total_pass"
echo "总 FAIL: $total_fail"
if [ "$total_fail" -eq 0 ]; then
    echo "RESULT: ALL TESTS PASSED"
    exit 0
else
    echo "RESULT: SOME TESTS FAILED"
    exit 1
fi
