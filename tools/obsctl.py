#!/usr/bin/env python3
"""Tiny obs-websocket v5 client for developing obs-beauty.

Reads the password from OBS's own obs-websocket config, so nothing secret is passed on the
command line.

  obsctl.py inputs                                  list inputs (sources)
  obsctl.py filters <source>                        list filters on a source
  obsctl.py add <source> [name]                     add the Beauty Enhance filter
  obsctl.py set <source> key=value ... [--filter name]   change filter settings
  obsctl.py enable <source> on|off [--filter name]  toggle the filter
  obsctl.py shot <source> <out.png> [width] [--frames N]  screenshot a source (with filters)
  obsctl.py reload <source> [--filter name]        re-create the filter (reloads .effect files)
  obsctl.py raw <RequestType> '<json>'              any request
"""

import asyncio, base64, hashlib, json, os, sys, uuid

import websockets

CONFIG = os.path.expanduser("~/Library/Application Support/obs-studio/plugin_config/obs-websocket/config.json")
FILTER_KIND = "obs_beauty_filter"


async def connect():
    cfg = json.load(open(CONFIG))
    ws = await websockets.connect(f"ws://127.0.0.1:{cfg.get('server_port', 4455)}", max_size=64 * 1024 * 1024)
    hello = json.loads(await ws.recv())["d"]
    ident = {"rpcVersion": 1}
    if "authentication" in hello:
        a = hello["authentication"]
        secret = base64.b64encode(hashlib.sha256((cfg["server_password"] + a["salt"]).encode()).digest())
        ident["authentication"] = base64.b64encode(
            hashlib.sha256(secret + a["challenge"].encode()).digest()).decode()
    await ws.send(json.dumps({"op": 1, "d": ident}))
    json.loads(await ws.recv())  # Identified
    return ws


async def request(ws, rtype, data=None):
    rid = str(uuid.uuid4())
    await ws.send(json.dumps({"op": 6, "d": {"requestType": rtype, "requestId": rid, "requestData": data or {}}}))
    while True:
        msg = json.loads(await ws.recv())
        if msg["op"] == 7 and msg["d"]["requestId"] == rid:
            st = msg["d"]["requestStatus"]
            if not st["result"]:
                raise SystemExit(f"{rtype} failed: {st.get('comment', st)}")
            return msg["d"].get("responseData", {})


def parse_value(v):
    for conv in (int, float):
        try:
            return conv(v)
        except ValueError:
            pass
    return {"true": True, "false": False}.get(v.lower(), v)


async def main(argv):
    if not argv:
        print(__doc__)
        return
    filt = "Beauty Enhance"
    frames = 1
    if "--frames" in argv:
        i = argv.index("--frames")
        frames = int(argv[i + 1])
        del argv[i:i + 2]
    if "--filter" in argv:
        i = argv.index("--filter")
        filt = argv[i + 1]
        del argv[i:i + 2]
    ws = await connect()
    cmd = argv[0]
    if cmd == "inputs":
        for i in (await request(ws, "GetInputList"))["inputs"]:
            print(f"{i['inputName']!r:40} {i['inputKind']}")
    elif cmd == "filters":
        for f in (await request(ws, "GetSourceFilterList", {"sourceName": argv[1]}))["filters"]:
            print(f"{f['filterName']!r:30} {f['filterKind']:30} enabled={f['filterEnabled']}")
    elif cmd == "add":
        name = argv[2] if len(argv) > 2 else filt
        await request(ws, "CreateSourceFilter",
                      {"sourceName": argv[1], "filterName": name, "filterKind": FILTER_KIND, "filterSettings": {}})
        print("added", name)
    elif cmd == "set":
        settings = {k: parse_value(v) for k, v in (a.split("=", 1) for a in argv[2:])}
        await request(ws, "SetSourceFilterSettings",
                      {"sourceName": argv[1], "filterName": filt, "filterSettings": settings, "overlay": True})
        print("set", settings)
    elif cmd == "enable":
        await request(ws, "SetSourceFilterEnabled",
                      {"sourceName": argv[1], "filterName": filt, "filterEnabled": argv[2] == "on"})
    elif cmd == "shot":
        data = {"sourceName": argv[1], "imageFormat": "png"}
        if len(argv) > 3:
            data["imageWidth"] = int(argv[3])
        # Sources outside the active scenes only render when screenshotted, so grab a few
        # frames back to back to let tracking and fades settle (--frames N, default 1).
        for _ in range(frames - 1):
            await request(ws, "GetSourceScreenshot", data)
        img = (await request(ws, "GetSourceScreenshot", data))["imageData"]
        open(argv[2], "wb").write(base64.b64decode(img.split(",", 1)[1]))
        print("wrote", argv[2])
    elif cmd == "reload":
        f = await request(ws, "GetSourceFilter", {"sourceName": argv[1], "filterName": filt})
        await request(ws, "RemoveSourceFilter", {"sourceName": argv[1], "filterName": filt})
        await request(ws, "CreateSourceFilter", {"sourceName": argv[1], "filterName": filt, "filterKind": FILTER_KIND,
                                                 "filterSettings": f["filterSettings"]})
        await request(ws, "SetSourceFilterIndex", {"sourceName": argv[1], "filterName": filt, "filterIndex": f["filterIndex"]})
        print("reloaded", filt)
    elif cmd == "raw":
        print(json.dumps(await request(ws, argv[1], json.loads(argv[2]) if len(argv) > 2 else {}), indent=2))
    await ws.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
