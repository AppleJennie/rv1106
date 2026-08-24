#!/bin/sh
# ============================================================
# Hand Capture Right - 产品级启动脚本
# 功能：开机等待 -> 杀旧服务 -> 清理 socket -> 循环启动程序
# ============================================================

export LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib:/lib:$LD_LIBRARY_PATH

LOG_DIR="/mnt/sdcard/right/log"
LOG_FILE="$LOG_DIR/console.log"

mkdir -p "$LOG_DIR"

# 开机给系统一点时间稳定
echo "[START] 等待系统稳定 (10s)..."
sleep 10

# 限制日志文件大小（保留最近 2MB）
if [ -f "$LOG_FILE" ]; then
    SIZE=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 2097152 ]; then
        mv "$LOG_FILE" "${LOG_FILE}.old"
    fi
fi

while true; do
    echo "[START] $(date '+%Y-%m-%d %H:%M:%S') 启动 hand_capture_right"

    # 停止可能占用摄像头的默认服务
    killall rkipc      2>/dev/null
    killall rkisp      2>/dev/null
    killall mpp_service 2>/dev/null
    killall rkaiq_3A_server 2>/dev/null
    usleep 300000
    killall -9 rkipc   2>/dev/null
    killall -9 rkisp   2>/dev/null
    killall -9 mpp_service 2>/dev/null
    killall -9 rkaiq_3A_server 2>/dev/null

    # 清理 Rockit 残留 socket
    rm -f /tmp/UNIX.domain* /tmp/rk* /tmp/rt*
    sync

    # 确保我们的程序没有在跑（防止残留）
    killall hand_capture_right 2>/dev/null
    usleep 200000
    killall -9 hand_capture_right 2>/dev/null

    cd /root || exit 1
    ./hand_capture_right >> "$LOG_FILE" 2>&1
    EXIT_CODE=$?

    echo "[START] 程序退出，exit_code=$EXIT_CODE，5秒后重启"
    sleep 5
done
