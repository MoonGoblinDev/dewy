#!/bin/bash
# copycost.sh <label>: alternates Dewy off/on on the camera (clip's Dewy off), 3 s samples, and
# prints OBS's camera-frame copy cost and ONNX Runtime / Dewy CPU for each.
cd ~/Development/obs-beauty; T=$(dirname "$0"); PID=$(pgrep -f "OBS.app/Contents/MacOS/OBS" | head -1); V="Video Capture Device"
python3 tools/obsctl.py enable "Dewy Test Clip" off
for state in off on off on; do
  python3 tools/obsctl.py enable "$V" $state; sleep 2.5
  sample $PID 3 -file $T/cc.txt >/dev/null 2>&1
  python3 - $T/cc.txt "$1 Dewy $state" <<'PY'
import sys,re
txt=open(sys.argv[1]).read(); lines=txt.split('Call graph:')[1].split('Total number in stack')[0].splitlines()
stack=[]; cp=0
for ln in lines:
    m=re.match(r'^([ +!:|]*)(\d+) (.*)$', ln)
    if not m: continue
    d=len(m.group(1)); n=int(m.group(2)); fn=m.group(3)
    while stack and stack[-1][0]>=d: stack.pop()
    stack.append((d,fn,n))
    if 'obs_source_output_video_internal' in fn and not any('obs_source_output_video_internal' in s[1] for s in stack[:-1]): cp+=n
top=txt.split('Sort by top of stack',1)[1]
lib=lambda l: sum(int(m.group(1)) for m in re.finditer(r'\(in '+l+r'[^)]*\)\s+(\d+)', top))
print(f"{sys.argv[2]:22} camera frame handling {cp/30:5.1f}%   onnxruntime {lib('libonnxruntime')/30:5.1f}%   dewy {lib('dewy')/30:4.1f}%")
PY
done
python3 tools/obsctl.py enable "$V" on; python3 tools/obsctl.py enable "Dewy Test Clip" on
