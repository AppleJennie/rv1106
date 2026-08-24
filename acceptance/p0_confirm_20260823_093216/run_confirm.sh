#!/bin/bash
set -e
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"; URL="http://127.0.0.1:8090"
phase() {
  local name="$1"; local seconds="$2"; local hint="$3"
  echo "==== PHASE ${name} ${seconds}s: ${hint} ====" | tee -a "$ACC/phases.log"
  local end=$(( $(date +%s) + seconds )); local n=0
  while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1))
    curl -s --max-time 2 "$URL/status.json" > "$ACC/status_${name}_${n}.json" 2>/dev/null || true
    sleep 1
  done
  ffmpeg -hide_banner -loglevel error -timeout 8000000 -y -i "$URL/stream_debug.mjpg" -frames:v 1 "$ACC/${name}_debug.jpg" 2>/dev/null || true
}
phase bg_window 20 "纯背景：确保画面里没有人脸，转向窗户/窗帘/强光"
phase front_face 25 "正脸：人脸居中看镜头，保持不动"
adb shell tail -120 /mnt/sdcard/dms/live/console.log > "$ACC/console_final.log" 2>/dev/null || true
echo "DONE $ACC" | tee -a "$ACC/phases.log"
