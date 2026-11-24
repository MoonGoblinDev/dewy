"""burst.py <outdir> <start_ms> <n> [k=v ...]: play the streamer clip from start_ms on air and grab n
screenshots as fast as OBS returns them (with the clip time of each), then switch back.
The clip has keyframes only at 0 and 10.4 s, so OBS can't seek into it: it is restarted and the
grabs begin once playback reaches start_ms."""
import sys, time, json, base64, asyncio, os, glob
sys.path.insert(0, os.path.expanduser('~/Development/obs-beauty/tools'))
import obsctl
out, start, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
settings = {k: obsctl.parse_value(v) for k, v in (a.split('=', 1) for a in sys.argv[4:])}
V = "Dewy Test Clip"
async def main():
    os.makedirs(out, exist_ok=True)
    for p in glob.glob(out + '/b*.png'): os.remove(p)
    ws = await obsctl.connect()
    orig = (await obsctl.request(ws, "GetCurrentProgramScene"))["sceneName"]
    if orig == "Dewy Test": orig = "Scene 3"
    await obsctl.request(ws, "SetCurrentProgramScene", {"sceneName": "Dewy Test"})
    if settings:
        await obsctl.request(ws, "SetSourceFilterSettings", {"sourceName": V, "filterName": "Dewy", "filterSettings": settings, "overlay": True})
    await obsctl.request(ws, "TriggerMediaInputAction", {"inputName": V, "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PLAY"})
    await asyncio.sleep(1.0)
    await obsctl.request(ws, "TriggerMediaInputAction", {"inputName": V, "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
    await asyncio.sleep(0.5)
    while ((await obsctl.request(ws, "GetMediaInputStatus", {"inputName": V})).get("mediaCursor") or 0) < start:
        await asyncio.sleep(0.02)
    log = []
    for i in range(n):
        r = await obsctl.request(ws, "GetSourceScreenshot", {"sourceName": V, "imageFormat": "png"})
        cur = (await obsctl.request(ws, "GetMediaInputStatus", {"inputName": V})).get("mediaCursor")
        open(f"{out}/b{i:02d}.png", "wb").write(base64.b64decode(r["imageData"].split(",", 1)[1]))
        log.append(cur)
    await obsctl.request(ws, "SetCurrentProgramScene", {"sceneName": orig})
    json.dump(log, open(out + "/times.json", "w"))
    print("clip ms:", log)
    await ws.close()
asyncio.run(main())
