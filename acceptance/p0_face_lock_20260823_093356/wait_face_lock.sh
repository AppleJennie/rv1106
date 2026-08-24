#!/bin/bash
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"; URL="http://127.0.0.1:8090"
echo "等待连续5次 face_found=1，最多90秒。请让 status.json 出现 FACE 后保持不动。" | tee "$ACC/phases.log"
lock=0
for i in $(seq 1 90); do
  s=$(curl -s --max-time 2 "$URL/status.json" || true)
  echo "$i $s" | tee -a "$ACC/status_stream.log"
  if echo "$s" | grep -q '"face_found":1'; then lock=$((lock+1)); else lock=0; fi
  if [ "$lock" -ge 5 ]; then
    echo "$s" > "$ACC/status_locked.json"
    adb shell tail -80 /mnt/sdcard/dms/live/console.log > "$ACC/console_locked.log" 2>/dev/null || true
    ffmpeg -hide_banner -loglevel error -timeout 8000000 -y -i "$URL/stream_debug.mjpg" -frames:v 1 "$ACC/locked_debug.jpg" 2>/dev/null || true
    echo "LOCKED at sample $i" | tee -a "$ACC/phases.log"
    exit 0
  fi
  sleep 1
done
echo "NOT_LOCKED timeout" | tee -a "$ACC/phases.log"
exit 2
