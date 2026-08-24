#!/bin/bash
# 全量数据采集：状态 + EAR/MAR + 106点全坐标，用于离线分析判定逻辑
# 用法: record_full.sh <输出目录> [秒数]
OUT="$1"; DUR="${2:-90}"
mkdir -p "$OUT"
echo "t,status,face,lm,ear,left,right,mar,head,eye,yawn,hd,points" > "$OUT/full.csv"
T=$(date +%s)
while :; do
  el=$(( $(date +%s) - T ))
  [ $el -ge "$DUR" ] && break
  curl -s --max-time 2 http://127.0.0.1:8090/status.json | python3 -c "
import json,sys,time
d=json.load(sys.stdin)
lm=d.get('landmark_106',{})
pts=lm.get('points',[])
pstr=';'.join(f'{p:.1f}' for p in pts) if pts else ''
print(f\"{time.time():.3f},{d.get('status')},{d.get('face_found')},{lm.get('found')},{d.get('ear'):.4f},{d.get('left_ear'):.4f},{d.get('right_ear'):.4f},{d.get('mar'):.4f},{d.get('head_down_score'):.4f},{d.get('eye_closed')},{d.get('yawn')},{d.get('head_down')},{pstr}\")
" >> "$OUT/full.csv" 2>/dev/null
  sleep 0.2
done
echo "DONE $OUT $(wc -l < "$OUT/full.csv") rows"
