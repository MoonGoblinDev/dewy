#!/bin/bash
cd ~/Development/obs-beauty
if pgrep -f "OBS.app/Contents/MacOS/OBS" >/dev/null; then
	for o in GetStreamStatus GetRecordStatus GetVirtualCamStatus; do
		r=$(python3 tools/obsctl.py raw $o 2>/dev/null) || { echo "can't reach OBS ($o) - not restarting"; exit 1; }
		grep -q '"outputActive": true' <<<"$r" && { echo "$o ACTIVE - not restarting"; exit 1; }
	done
fi
osascript -e 'quit app "OBS"' 2>/dev/null; for i in $(seq 1 30); do pgrep -f "OBS.app/Contents/MacOS/OBS" >/dev/null || break; sleep 1; done
open -a OBS; for i in $(seq 1 40); do python3 tools/obsctl.py raw GetVersion >/dev/null 2>&1 && break; sleep 1; done; sleep 8
