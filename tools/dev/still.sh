#!/bin/bash
# usage: still.sh <outdir> <frames> <interval_s> <cursor_ms> [settings...]
#   Like run.sh, but pauses the clip at <cursor_ms> first, so every frame is the same picture:
#   any change between captured frames is Dewy moving, not her.
OUT="$1"; NF="$2"; IV="$3"; CUR="$4"; shift 4
cd ~/Development/obs-beauty; V="Dewy Test Clip"; mkdir -p "$OUT"; rm -f "$OUT"/f*.png
ORIG=$(python3 tools/obsctl.py raw GetCurrentProgramScene | python3 -c "import json,sys; print(json.load(sys.stdin)['sceneName'])")
[ "$ORIG" = "Dewy Test" ] && ORIG="Scene 3"
python3 tools/obsctl.py raw SetCurrentProgramScene '{"sceneName":"Dewy Test"}' >/dev/null
python3 tools/obsctl.py raw TriggerMediaInputAction "{\"inputName\":\"$V\",\"mediaAction\":\"OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PAUSE\"}" >/dev/null
python3 tools/obsctl.py raw SetMediaInputCursor "{\"inputName\":\"$V\",\"mediaCursor\":$CUR}" >/dev/null
[ $# -gt 0 ] && python3 tools/obsctl.py set "$V" "$@" >/dev/null
sleep 2.5
for i in $(seq -w 1 $NF); do python3 tools/obsctl.py shot "$V" "$OUT/f$i.png" >/dev/null; sleep "$IV"; done
python3 tools/obsctl.py raw TriggerMediaInputAction "{\"inputName\":\"$V\",\"mediaAction\":\"OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PLAY\"}" >/dev/null
python3 tools/obsctl.py raw SetCurrentProgramScene "{\"sceneName\":\"$ORIG\"}" >/dev/null
