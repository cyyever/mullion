"""Check that the AIR tidy rule diagnoses discarded errors without flagging handled ones."""
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
source = root / "tests/host/air_tidy.cpp"
result = subprocess.run(
    [sys.argv[1], "--experimental-custom-checks", "--config-file=" + str(root / "tools/air-tidy.yaml"),
     str(source), "--", "-std=c++23"],
    capture_output=True, text=True, timeout=60,
)
output = result.stdout + result.stderr
print(output, end="")
expected = {(i, line.split("// EXPECT: ")[1].strip())
            for i, line in enumerate(source.read_text().splitlines(), 1) if "// EXPECT: " in line}
actual = {(int(line), check) for line, check in
          re.findall(r"air_tidy.cpp:(\d+):\d+: warning:.*\[([^\]]+)\]", output)}
if result.returncode or actual != expected or "error:" in output:
    raise SystemExit(f"AIR tidy failed: expected {expected}, got {actual}, exit {result.returncode}")
print("PASS: expected clang-tidy diagnostics matched")
