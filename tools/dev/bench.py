"""bench.py <label>: measures OBS with the running Dewy build in four conditions (12 s each).
Clip = the looping streamer clip with a fixed look; camera = your camera with your own settings."""
import sys, os, json, time, subprocess, asyncio
sys.path.insert(0, os.path.expanduser('~/Development/obs-beauty/tools'))
import obsctl
label = sys.argv[1]
pid = subprocess.run(['pgrep', '-f', 'OBS.app/Contents/MacOS/OBS'], capture_output=True, text=True).stdout.split()[0]
LOOK = {"smooth": 50, "foundation": 20, "makeup_preset": "Smoky", "makeup_amt": 70,
        "light_preset": "Neon", "light_amt": 60, "light_on": True, "enhance_on": True, "debug_view": 0}
async def measure(ws, secs=12):
    s0 = await obsctl.request(ws, "GetStats"); rt = []; cpu = []
    for _ in range(secs):
        await asyncio.sleep(1)
        s = await obsctl.request(ws, "GetStats"); rt.append(s["averageFrameRenderTime"])
        cpu.append(float(subprocess.run(['ps', '-o', '%cpu=', '-p', pid], capture_output=True, text=True).stdout or 0))
    s1 = await obsctl.request(ws, "GetStats")
    return {"render_ms": round(sum(rt) / len(rt), 2), "obs_cpu_pct_of_core": round(sum(cpu) / len(cpu), 1),
            "skipped": s1["renderSkippedFrames"] - s0["renderSkippedFrames"]}
async def main():
    ws = await obsctl.connect(); out = {}
    clip, cam = "Dewy Test Clip", "Video Capture Device"
    orig = (await obsctl.request(ws, "GetCurrentProgramScene"))["sceneName"]
    if orig == "Dewy Test": orig = "Scene 3"
    await obsctl.request(ws, "SetSourceFilterSettings", {"sourceName": clip, "filterName": "Dewy", "filterSettings": {"makeup_preset": "None"}, "overlay": True})
    await obsctl.request(ws, "SetSourceFilterSettings", {"sourceName": clip, "filterName": "Dewy", "filterSettings": LOOK, "overlay": True})
    for scene, src, key in (("Dewy Test", clip, "clip"), (orig, cam, "camera")):
        other = cam if src == clip else clip  # only the measured source's Dewy runs
        await obsctl.request(ws, "SetSourceFilterEnabled", {"sourceName": other, "filterName": "Dewy", "filterEnabled": False})
        await obsctl.request(ws, "SetCurrentProgramScene", {"sceneName": scene})
        for on in (True, False):
            await obsctl.request(ws, "SetSourceFilterEnabled", {"sourceName": src, "filterName": "Dewy", "filterEnabled": on})
            await asyncio.sleep(4)
            out[f"{key}, Dewy {'on' if on else 'off'}"] = await measure(ws)
        await obsctl.request(ws, "SetSourceFilterEnabled", {"sourceName": src, "filterName": "Dewy", "filterEnabled": True})
        await obsctl.request(ws, "SetSourceFilterEnabled", {"sourceName": other, "filterName": "Dewy", "filterEnabled": True})
    await obsctl.request(ws, "SetCurrentProgramScene", {"sceneName": orig})
    await ws.close()
    json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), f"bench_{label}.json"), "w"), indent=1)
    for k, v in out.items(): print(f"{label:8} {k:22} {v}")
asyncio.run(main())
