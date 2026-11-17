#!/usr/bin/env python3
"""Run an ABBA comparison of the fixed D3D11 sampling probe; no game assets are stored here."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--wine", required=True, type=Path)
p.add_argument("--prefix", required=True, type=Path)
p.add_argument("--exe", required=True, type=Path)
p.add_argument("--shader", type=Path)
p.add_argument("--output", required=True, type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
env = os.environ.copy()
for k in list(env):
    if k.startswith(("MULLION_", "DXMT_", "MTL_")):
        del env[k]
env.update(WINEPREFIX=str(a.prefix.resolve()), WINEDEBUG="-all", WINEMSYNC="0",
           WINEDLLOVERRIDES="d3dcompiler_47=n", MULLION_SHADER_CACHE="0",
           MULLION_CONFIG="d3d11.timestampQueries=True")
results = []
# ABBA reduces the bias of monotonically changing temperature and clock speed.
for i, mode in enumerate(["default", "no-minmax", "no-minmax", "default"]):
    child = env.copy()
    if mode == "no-minmax":
        child["MULLION_AIRCONV_SKIP"] = "minmax-filters"
    cmd = [str(a.wine.resolve()), str(a.exe.resolve())]
    if a.shader:
        cmd.append("Z:" + str(a.shader.resolve()))
    run = subprocess.run(cmd, env=child, capture_output=True, text=True, timeout=180)
    (a.output / f"{i}-{mode}.log").write_text(run.stdout + run.stderr)
    match = re.search(r"gpu_median_ms=([\d.]+) gpu_p95_ms=([\d.]+) pixel_hash=([0-9a-f]+)", run.stdout)
    if run.returncode or not match:
        raise SystemExit(f"{mode} failed ({run.returncode}); see logs")
    result = dict(mode=mode, median_ms=float(match[1]), p95_ms=float(match[2]), pixel_hash=match[3])
    results.append(result)
    print(result, flush=True)
    (a.output / "results.json").write_text(json.dumps(results, indent=2))
if len({r["pixel_hash"] for r in results}) != 1:
    raise SystemExit("Output differs between modes; timing is not a valid equivalent-work comparison")
means = {m: statistics.mean(r["median_ms"] for r in results if r["mode"] == m) for m in ["default", "no-minmax"]}
print("Identical pixel hashes. Mean of run medians:", means)
print("This isolates minmax fallback overhead with an ordinary linear sampler; it is not a game FPS result.")
