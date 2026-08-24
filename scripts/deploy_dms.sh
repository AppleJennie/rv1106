#!/bin/bash
# DMS 模式一键部署脚本（RV1106，自动启动 + HTTP 推流）
# 用法：./scripts/deploy_dms.sh

set -e

BIN="build/hand_capture_right"
MODEL_DIR="models"
REMOTE_BIN="/root/hand_capture_right"
REMOTE_MODEL_DIR="/mnt/sdcard/dms/models"
REMOTE_LOG="/mnt/sdcard/dms/live/console.log"
REMOTE_START_SCRIPT="/root/start_hand_capture.sh"
STREAM_PORT=8090

echo "[0/6] 停止旧进程并释放 ${STREAM_PORT} 端口..."
adb shell "/etc/init.d/S99hand_capture stop 2>/dev/null || true"
adb shell "killall -9 hand_capture_start.sh 2>/dev/null || true"
adb shell "killall -9 hand_capture_right 2>/dev/null || true"
sleep 2
adb shell "netstat -an 2>/dev/null | grep ':${STREAM_PORT} ' || echo PORT_${STREAM_PORT}_FREE"

echo "[1/6] 推送可执行文件..."
adb push "${BIN}" "${REMOTE_BIN}"
adb shell chmod +x "${REMOTE_BIN}"

echo "[2/6] 创建 SD 卡目录..."
adb shell "mkdir -p ${REMOTE_MODEL_DIR} /mnt/sdcard/dms/sessions /mnt/sdcard/dms/live /mnt/sdcard/dms/events /mnt/sdcard/dms/alarms"

echo "[3/6] 推送 RKNN 模型..."
adb push "${MODEL_DIR}/retinaface.rknn"        "${REMOTE_MODEL_DIR}/retinaface.rknn"
adb push "${MODEL_DIR}/face_landmark.rknn"     "${REMOTE_MODEL_DIR}/face_landmark.rknn"
adb push "${MODEL_DIR}/2d106det.rknn"          "${REMOTE_MODEL_DIR}/2d106det.rknn"

echo "[4/6] 推送并安装启动脚本..."
adb shell "cat > ${REMOTE_START_SCRIPT} << 'EOF'
#!/bin/sh
# hand_capture_right DMS 启动脚本
# 由 deploy_dms.sh 生成，使用 start-stop-daemon 后台守护

cd /root || exit 1
rm -f /mnt/sdcard/dms/live/console.log
start-stop-daemon -S -b -m -p /var/run/hand_capture_right.pid -x /root/hand_capture_right
EOF
chmod +x ${REMOTE_START_SCRIPT}"

echo "[5/6] 启动程序..."
adb shell "rm -f ${REMOTE_LOG}; ${REMOTE_START_SCRIPT}"
sleep 6

echo "[6/6] 检查进程是否启动..."
PID=$(adb shell "pidof hand_capture_right" 2>/dev/null | tr -d '\\r\\n')
if [ -z "${PID}" ]; then
    echo "ERROR: hand_capture_right 启动失败，没有进程在运行"
    echo "--- 启动日志 ---"
    adb shell "cat ${REMOTE_LOG} 2>/dev/null | tail -80 || echo '无法读取日志'"
    exit 1
fi
echo "hand_capture_right 正在运行，PID=${PID}"

echo "[7/6] 设置端口转发..."
adb forward --remove tcp:${STREAM_PORT} 2>/dev/null || true
adb forward tcp:${STREAM_PORT} tcp:${STREAM_PORT}

echo ""
echo "部署完成。"
echo "ffplay 观看: ffplay -fflags nobuffer -flags low_delay -framedrop http://127.0.0.1:${STREAM_PORT}/stream.mjpg"
echo "网页查看:   http://127.0.0.1:${STREAM_PORT}/"
echo "查看日志:   adb shell tail -100 ${REMOTE_LOG}"
echo ""
