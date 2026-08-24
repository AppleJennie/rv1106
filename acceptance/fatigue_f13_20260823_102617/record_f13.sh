#!/bin/bash
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"
echo "time,status,face_found,lm106,calib,ear,left,right,ear_base,ear_th,mar,mar_base,mar_th,head,head_base,eye,yawn,head_down,feature_ms" > "$ACC/status.csv"
echo "开始记录180秒。先正常睁眼2秒校准，再按提示做动作。" | tee "$ACC/README.txt"
last_state=""
for i in $(seq 1 900); do
  s=$(curl -s --max-time 2 http://127.0.0.1:8090/status.json || true)
  [ -z "$s" ] && { sleep 0.2; continue; }
  echo "$s" > "$ACC/last.json"
  python3 - "$s" >> "$ACC/status.csv" <<'PY'
import json,sys,time
d=json.loads(sys.argv[1]); lm=d.get('landmark_106',{})
f=lambda k: d.get(k) if d.get(k) is not None else 0.0
print(f"{time.time():.3f},{d.get('status')},{f('face_found')},{lm.get('found') or 0},{f('feature_calibrated')},{f('ear'):.4f},{f('left_ear'):.4f},{f('right_ear'):.4f},{f('ear_baseline'):.4f},{f('ear_threshold'):.4f},{f('mar'):.4f},{f('mar_baseline'):.4f},{f('mar_threshold'):.4f},{f('head_down_score'):.4f},{f('head_baseline'):.4f},{f('eye_closed')},{f('yawn')},{f('head_down')},{f('feature_cost_ms'):.3f}")
PY
  st=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("status"))' "$ACC/last.json" 2>/dev/null || echo '?')
  if [ "$st" != "$last_state" ]; then
    echo "$(date +%H:%M:%S) state=$st" | tee -a "$ACC/state_changes.log"
    ffmpeg -hide_banner -loglevel error -timeout 5000000 -y -i http://127.0.0.1:8090/stream_debug.mjpg -frames:v 1 "$ACC/state_${st}_$(date +%H%M%S).jpg" 2>/dev/null || true
    last_state="$st"
  fi
  sleep 0.2
done
adb shell tail -200 /mnt/sdcard/dms/live/console.log > "$ACC/console_final.log" 2>/dev/null || true
echo "DONE $ACC" | tee -a "$ACC/README.txt"
