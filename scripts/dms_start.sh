#!/bin/sh
# ============================================================
# DMS - 产品级启动脚本
# 功能：开机等待 -> 杀旧服务 -> 清理 socket -> 有限次数重启
#
# 与 hand_capture_start.sh 的区别：
#   - 有限次数重启（避免疯狂重启）
#   - 重启间隔递增
#   - DMS 日志路径
#   - 最大重启次数可配置
# ============================================================

export LD_LIBRARY_PATH=/oem/usr/lib:/usr/lib:/lib:$LD_LIBRARY_PATH

LOG_DIR="/mnt/sdcard/dms/log"
LOG_FILE="$LOG_DIR/console.log"

# 重启策略配置
MAX_RESTART_COUNT=10        # 最大连续重启次数
RESTART_WINDOW_SEC=300      # 在此时间窗口内统计重启次数（5分钟）
BASE_RESTART_DELAY=5        # 基础重启延迟（秒）
MAX_RESTART_DELAY=60        # 最大重启延迟（秒）

mkdir -p "$LOG_DIR"

# 开机给系统一点时间稳定
echo "[DMS] 等待系统稳定 (10s)..."
sleep 10

# 限制日志文件大小（保留最近 2MB）
if [ -f "$LOG_FILE" ]; then
    SIZE=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 2097152 ]; then
        mv "$LOG_FILE" "${LOG_FILE}.old"
    fi
fi

# 重启计数器
restart_count=0
window_start=$(date +%s)

while true; do
    current_time=$(date +%s)

    # 检查是否需要重置重启计数窗口
    if [ $((current_time - window_start)) -gt $RESTART_WINDOW_SEC ]; then
        restart_count=0
        window_start=$current_time
    fi

    # 检查是否超过最大重启次数
    if [ $restart_count -ge $MAX_RESTART_COUNT ]; then
        echo "[DMS] $(date '+%Y-%m-%d %H:%M:%S') 超过最大重启次数($MAX_RESTART_COUNT)，等待${MAX_RESTART_DELAY}s后重置计数"
        sleep $MAX_RESTART_DELAY
        restart_count=0
        window_start=$(date +%s)
        continue
    fi

    # 计算重启延迟（递增）
    restart_delay=$((BASE_RESTART_DELAY + restart_count * 2))
    if [ $restart_delay -gt $MAX_RESTART_DELAY ]; then
        restart_delay=$MAX_RESTART_DELAY
    fi

    echo "[DMS] $(date '+%Y-%m-%d %H:%M:%S') 启动 hand_capture_right (第$((restart_count+1))次)"

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

    restart_count=$((restart_count + 1))
    echo "[DMS] 程序退出，exit_code=$EXIT_CODE，${restart_delay}秒后进行第$((restart_count+1))次重启"
    sleep $restart_delay
done
