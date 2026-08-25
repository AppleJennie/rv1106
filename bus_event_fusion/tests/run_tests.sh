#!/bin/bash
# run_tests.sh - 编译并运行 bus_event_fusion 单元测试
# 要求：gcc -Wall -Wextra -std=c11 零警告

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"

echo "[build] compiling with gcc -Wall -Wextra -std=c11 ..."
gcc -Wall -Wextra -std=c11 \
    -I"$MODULE_DIR/include" \
    "$MODULE_DIR/src/bus_event_fusion.c" \
    "$SCRIPT_DIR/test_bus_event_fusion.c" \
    -o "$BUILD_DIR/test_bus_event_fusion"

echo "[build] OK (zero warnings)"
echo "[run] executing tests ..."
"$BUILD_DIR/test_bus_event_fusion"
