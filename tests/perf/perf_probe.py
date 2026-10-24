#!/usr/bin/env python3
"""D3D12 performance probe runner (tests/perf/d3d12_perf_probe.cpp).

  tests/perf/perf_probe.py run [--label NAME] [--exe PROBE.exe] [--wine WINE] [--frames N] [--warmup N]
                               [--scenes a,b,...] [--repeat N] [--env K=V ...] [--out FILE.json]
  tests/perf/perf_probe.py compare BEFORE.json AFTER.json

run: runs every scene (or --scenes) in its own process through WINE (default: the MetalSharp test
home's FEX wrapper, so the x86_64 probe goes through FEX, VKD3D-Proton and MoltenVK the way games
do) with GRAPHICS_BACKEND=vkd3d, --repeat times, keeping the run with the lowest median CPU time
(the others are usually disturbed by thermal or scheduling noise). Writes JSON with every value
the probe printed. compare: per scene, the headline values of two runs and their change.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WINE = "/Volumes/AverySSD/fex-test-home/runtime/wine/bin/wine"
DEFAULT_EXE = os.path.join(HERE, "../../artifacts/build/perf/d3d12_perf_probe-x86_64.exe")


def list_scenes(wine, exe, env):
    out = subprocess.run([wine, exe, "list"], env=env, capture_output=True, text=True, timeout=120).stdout
    return [line.split()[0] for line in out.replace("\r", "").splitlines() if line and not line.startswith(("info:", "warn"))
            and re.match(r"^[a-z_0-9]+\s", line)]


def parse(text):
    r = {}
    for kind in ("cpu", "gpu"):
        m = re.search(kind + r" frames (\d+) mean ([\d.]+) median ([\d.]+) p95 ([\d.]+) max ([\d.]+) ms", text)
        if m:
            for name, value in zip(["frames", "mean", "median", "p95", "max"], m.groups()):
                r["%s_%s" % (kind, name)] = float(value)
    m = re.search(r"first ([\d.]+) ms", text)
    if m:
        r["first_ms"] = float(m.group(1))
    for name, value in re.findall(r"^extra (\w+)=([-\d.]+)", text, re.M):
        r[name] = float(value)
    r["ok"] = "D3D12_PERF_PROBE_OK" in text
    return r


def run_scene(wine, exe, scene, frames, warmup, env, timeout):
    t = time.time()
    try:
        p = subprocess.run([wine, exe, scene, str(frames), str(warmup)], env=env, capture_output=True, text=True,
                           timeout=timeout)
        text = (p.stdout + p.stderr).replace("\r", "")
    except subprocess.TimeoutExpired as e:
        text = ((e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")) + "\nTIMEOUT"
    r = parse(text)
    r["wall_s"] = round(time.time() - t, 1)
    if not r["ok"]:
        r["log_tail"] = "\n".join(l for l in text.splitlines() if not l.startswith(("info:", "fixme"))) [-1500:]
    return r


def cmd_run(args):
    env = dict(os.environ)
    env.setdefault("GRAPHICS_BACKEND", "vkd3d")
    env.setdefault("FEX_DISKCACHE", "0")
    for kv in args.env or []:
        k, v = kv.split("=", 1)
        env[k] = v
    exe = os.path.abspath(args.exe)
    scenes = args.scenes.split(",") if args.scenes else list_scenes(args.wine, exe, env)
    data = {"label": args.label, "exe": exe, "wine": args.wine, "frames": args.frames, "warmup": args.warmup,
            "env": {k: env[k] for k in sorted(env) if k.startswith(("MVK_", "VKD3D", "FEX_", "GRAPHICS_", "DXVK"))},
            "time": time.strftime("%Y-%m-%d %H:%M:%S"), "scenes": {}}
    for scene in scenes:
        runs = [run_scene(args.wine, exe, scene, args.frames, args.warmup, env, args.timeout) for _ in range(args.repeat)]
        good = [r for r in runs if r["ok"]]
        best = min(good, key=lambda r: r.get("cpu_median", 1e9)) if good else runs[-1]
        data["scenes"][scene] = best
        if best["ok"]:
            extras = " ".join("%s=%.3f" % (k, v) for k, v in best.items()
                              if k not in ("ok", "wall_s") and not k.startswith(("cpu_", "gpu_")) and isinstance(v, float))
            print("%-18s cpu %7.3f ms (p95 %7.3f)  gpu %7.3f ms  %s" % (scene, best.get("cpu_median", 0), best.get("cpu_p95", 0),
                                                                     best.get("gpu_median", 0), extras), flush=True)
        else:
            print("%-18s FAILED\n%s" % (scene, best.get("log_tail", "")), flush=True)
    out = args.out or os.path.join(HERE, "../../artifacts/perf/%s.json" % args.label)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(data, f, indent=1)
    print("wrote", out)


def cmd_compare(args):
    before, after = (json.load(open(p)) for p in (args.before, args.after))
    print("%-18s %-28s %10s %10s %8s" % ("scene", "value", before["label"][:10], after["label"][:10], "change"))
    for scene in before["scenes"]:
        b, a = before["scenes"][scene], after["scenes"].get(scene)
        if not a:
            continue
        keys = ["cpu_median", "cpu_p95", "gpu_median"] + [k for k in b if k not in ("ok", "wall_s", "frames", "log_tail", "first_ms")
                                                         and not k.startswith(("cpu_", "gpu_"))]
        for k in keys:
            if k in b and k in a and isinstance(b[k], (int, float)) and b[k]:
                print("%-18s %-28s %10.3f %10.3f %+7.1f%%" % (scene, k, b[k], a[k], 100.0 * (a[k] - b[k]) / b[k]))
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    run = sub.add_parser("run")
    run.add_argument("--label", default="baseline")
    run.add_argument("--exe", default=DEFAULT_EXE)
    run.add_argument("--wine", default=DEFAULT_WINE)
    run.add_argument("--frames", type=int, default=200)
    run.add_argument("--warmup", type=int, default=30)
    run.add_argument("--repeat", type=int, default=1)
    run.add_argument("--timeout", type=int, default=240)
    run.add_argument("--scenes")
    run.add_argument("--env", action="append", help="K=V passed to the probe (repeatable)")
    run.add_argument("--out")
    cmp = sub.add_parser("compare")
    cmp.add_argument("before")
    cmp.add_argument("after")
    args = parser.parse_args()
    cmd_run(args) if args.cmd == "run" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
