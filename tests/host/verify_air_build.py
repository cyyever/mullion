"""Check generated AIR against the active Apple compiler before completing a build."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="mullion-air-check-") as directory:
    source = pathlib.Path(directory) / "probe.metal"
    output = source.with_suffix(".ll")
    source.write_text("#include <metal_stdlib>\nkernel void probe(device uint *p [[buffer(0)]]) { p[0] = 1; }\n")
    subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-S", "-emit-llvm", "-std=metal4.0",
                    "--target=air64-apple-macos27.2", str(source), "-o", str(output)], check=True, timeout=60)
    triple = re.search(r'target triple = "([^"\n]+)"', output.read_text())
    if not triple:
        raise SystemExit("Apple compiler output has no AIR target triple")
    env = dict(os.environ, MULLION_EXPECTED_AIR_TRIPLE=triple[1])
    subprocess.run([sys.argv[1]], env=env, check=True, timeout=120)
pathlib.Path(sys.argv[2]).write_text("Apple AIR encoding and verification passed\n")
