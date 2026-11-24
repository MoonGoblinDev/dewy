#!/bin/bash
# usage: run.sh <outdir> <frames> <interval_s> [settings...]
#   Puts the looping streamer clip on air for a moment, applies the Dewy settings to its filter,
#   grabs <frames> screenshots <interval_s> apart, then switches back to the scene that was on.
OUT="$1"; NF="$2"; IV="$3"; shift 3
cd ~/Development/obs-beauty; V="Dewy Test Clip"; mkdir -p "$OUT"; rm -f "$OUT"/f*.png
ORIG=$(python3 tools/obsctl.py raw GetCurrentProgramScene | python3 -c "import json,sys; print(json.load(sys.stdin)['sceneName'])")
[ "$ORIG" = "Dewy Test" ] && ORIG="Scene 3"
python3 tools/obsctl.py raw SetCurrentProgramScene '{"sceneName":"Dewy Test"}' >/dev/null
[ $# -gt 0 ] && python3 tools/obsctl.py set "$V" "$@" >/dev/null
sleep 2
for i in $(seq -w 1 $NF); do python3 tools/obsctl.py shot "$V" "$OUT/f$i.png" >/dev/null; sleep "$IV"; done
python3 tools/obsctl.py raw SetCurrentProgramScene "{\"sceneName\":\"$ORIG\"}" >/dev/null
echo "captured $NF frames, back on $ORIG"
