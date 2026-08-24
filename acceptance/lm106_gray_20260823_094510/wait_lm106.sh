#!/bin/bash
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"
echo "等待 LANDMARK_106_OK，最多90秒。请让人脸进入画面并保持。" | tee "$ACC/phases.log"
for i in $(seq 1 90); do
  line=$(adb shell tail -40 /mnt/sdcard/dms/live/console.log 2>/dev/null | grep -E 'LANDMARK_106_OK|landmark_106|DMS PERF|DMS status' | tail -8)
  echo "--- $i ---" >> "$ACC/monitor.log"
  echo "$line" >> "$ACC/monitor.log"
  if echo "$line" | grep -q 'LANDMARK_106_OK'; then
    adb shell tail -160 /mnt/sdcard/dms/live/console.log > "$ACC/console_locked.log" 2>/dev/null || true
    curl -s --max-time 2 http://127.0.0.1:8090/status.json > "$ACC/status_locked.json" 2>/dev/null || true
    echo "LM106_OK at $i" | tee -a "$ACC/phases.log"
    exit 0
  fi
  sleep 1
done
echo "LM106_NOT_SEEN timeout" | tee -a "$ACC/phases.log"
exit 2
