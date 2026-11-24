import subprocess, json, time, sys
def stats():
    return json.loads(subprocess.run(['python3','tools/obsctl.py','raw','GetStats'],capture_output=True,text=True).stdout)
s0=stats(); t0=time.time(); rt=[]; cpu=[]
for _ in range(int(sys.argv[1]) if len(sys.argv)>1 else 15):
    time.sleep(1); s=stats(); rt.append(s['averageFrameRenderTime']); cpu.append(s['cpuUsage'])
s1=stats()
print(f"render {sum(rt)/len(rt):.2f} ms (min {min(rt):.2f} max {max(rt):.2f})  OBS cpu {sum(cpu)/len(cpu):.1f}%  "
      f"skipped render frames {s1['renderSkippedFrames']-s0['renderSkippedFrames']}")
