#!/bin/bash
set -e
cd /home/jennie/hhh/embed_complication/hand_capture_right
ACC="$1"
echo "等待 FACE 后自动抓索引图，最多120秒" | tee "$ACC/phases.log"
for i in $(seq 1 120); do
  s=$(curl -s --max-time 2 http://127.0.0.1:8090/status.json || true)
  echo "$i $s" >> "$ACC/status_stream.log"
  if echo "$s" | grep -q '"face_found":1'; then
    echo "$s" > "$ACC/status.json"
    ffmpeg -hide_banner -loglevel error -timeout 8000000 -y -i http://127.0.0.1:8090/stream_debug.mjpg -frames:v 1 "$ACC/index_full.jpg" 2>/dev/null || true
    python3 - "$ACC" <<'PY'
import json,sys,subprocess,os
acc=sys.argv[1]
d=json.load(open(os.path.join(acc,'status.json')))
x,y,w,h=map(int,(d['face_x'],d['face_y'],d['face_w'],d['face_h']))
full=os.path.join(acc,'index_full.jpg')
pad=int(max(w,h)*0.45)
X=max(0,x-pad); Y=max(0,y-pad); X2=min(1280,x+w+pad); Y2=min(720,y+h+pad)
cw=max(1,X2-X); ch=max(1,Y2-Y)
subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',full,'-vf',f'crop={cw}:{ch}:{X}:{Y},scale={cw*4}:{ch*4}:flags=neighbor',os.path.join(acc,'face_x4.jpg')],check=True)
# rough eye/mouth regions from bbox
regions={
 'eyes':(x-int(w*0.05), y-int(h*0.05), int(w*1.10), int(h*0.45)),
 'mouth':(x+int(w*0.10), y+int(h*0.52), int(w*0.80), int(h*0.48)),
 'nose_chin':(x+int(w*0.25), y+int(h*0.25), int(w*0.50), int(h*0.75)),
}
for name,(Xr,Yr,Wr,Hr) in regions.items():
    Xr=max(0,Xr); Yr=max(0,Yr); Wr=max(1,min(1280-Xr,Wr)); Hr=max(1,min(720-Yr,Hr))
    subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-i',full,'-vf',f'crop={Wr}:{Hr}:{Xr}:{Yr},scale={Wr*6}:{Hr*6}:flags=neighbor',os.path.join(acc,f'{name}_x6.jpg')],check=True)
print('cropped',x,y,w,h,file=sys.stderr)
PY
    echo "CAPTURED at $i" | tee -a "$ACC/phases.log"
    exit 0
  fi
  sleep 1
done
echo "TIMEOUT no face" | tee -a "$ACC/phases.log"
exit 2
