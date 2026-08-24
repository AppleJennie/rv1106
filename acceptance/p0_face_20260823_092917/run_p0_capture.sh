#!/bin/bash
set -e
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"
URL="http://127.0.0.1:8090"
phase() {
  local name="$1"; local seconds="$2"; local hint="$3"
  echo "==== PHASE ${name} ${seconds}s: ${hint} ====" | tee -a "$ACC/phases.log"
  local end=$(( $(date +%s) + seconds ))
  local n=0
  while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1))
    curl -s --max-time 2 "$URL/status.json" > "$ACC/status_${name}_${n}.json" 2>/dev/null || true
    adb shell tail -5 /mnt/sdcard/dms/live/console.log 2>/dev/null | grep -E 'DMS PERF|DMS status' >> "$ACC/perf_${name}.log" || true
    sleep 2
  done
  ffmpeg -hide_banner -loglevel error -timeout 8000000 -y -i "$URL/stream_debug.mjpg" -frames:v 1 "$ACC/${name}_debug.jpg" 2>/dev/null || true
  ffmpeg -hide_banner -loglevel error -timeout 8000000 -y -i "$URL/stream.mjpg" -frames:v 1 "$ACC/${name}_raw.jpg" 2>/dev/null || true
}
phase front 25 "正脸居中，保持不动，看镜头"
phase move_lr 25 "人脸缓慢左右移动，再回中"
phase move_nf 25 "人脸缓慢走近/走远"
phase window 25 "镜头转向窗户/窗帘/强光背景，检查误检"
adb shell tail -200 /mnt/sdcard/dms/live/console.log > "$ACC/console_final.log" 2>/dev/null || true
echo "DONE $ACC" | tee -a "$ACC/phases.log"
