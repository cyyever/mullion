"""Compile the literal HLSL fixtures in changed tests with the profiles used by their test helpers."""
import argparse
import ast
from collections import defaultdict
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

DXC_TREE = "tests/engines/directx-shader-compiler"
XML = "tools/clang/unittests/HLSLExec/ShaderOpArithTable.xml"
SMOKE = ["tests/d3d11/d3d11_integer_rare.cpp", "tests/d3d12/d3d12_shared_shaders.cpp"]
RAW = re.compile(r'\bchar\s+(\w+)\s*\[\s*\]\s*=\s*R"([^\s\\()"]{0,16})\((.*?)\)\2"\s*;', re.S)
CALL = re.compile(r'\bcompile\(\s*((?:\w+::)?\w+)\s*,\s*"(\w*)"\s*,\s*"([a-z]+(?:_\d_\d)?)"\s*'
                  r'(?:,\s*\{\s*((?:"\w+(?:=[\w.+-]*)?"\s*,?\s*)*)\}\s*)?\)')


def git(*args):
    return subprocess.check_output(["git", *args])


def changed_paths(base):
    # A force-push can remove the previous tip from the checkout's history.
    if subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
                      stdout=subprocess.DEVNULL).returncode == 1:
        print(f"Diff base {base} is unavailable; checking all tracked fixtures")
        names = git("ls-tree", "-r", "--name-only", "-z", "HEAD", "--", "tests")
    else:
        names = git("diff", "--name-only", "--diff-filter=ACMR", "-z", base, "HEAD", "--")
    return {os.fsdecode(path) for path in names.split(b"\0") if path}


def shader_cases(path, front_ends):
    text = path.read_text(encoding="utf-8")
    sources = defaultdict(list)
    for m in RAW.finditer(text):
        sources[m[1]].append(m[3])
    # Shared fixtures use headers named for their namespace, directly included by the caller.
    for include in re.findall(r'^#include "(\.\./\w+\.hpp)"', text, re.M):
        header = path.parent / include
        if not header.is_file():  # Some headers are generated during the C++ build.
            continue
        shared = header.read_text(encoding="utf-8")
        if re.search(rf"\bnamespace\s+{re.escape(header.stem)}\s*\{{", shared):
            for m in RAW.finditer(shared):
                sources[f"{header.stem}::{m[1]}"].append(m[3])
    # Omit comments and HLSL bodies when looking for C++ calls.
    code = re.sub(r'//[^\n]*|/\*.*?\*/', '', RAW.sub('', text), flags=re.S)
    matches = list(CALL.finditer(code))
    seen = set()
    duplicates = 0
    skipped = len(re.findall(r"\bcompile\(", code)) - len(matches)
    for match in matches:
        name, entry, stage, macros = match.groups()
        if len(sources[name]) != 1:
            skipped += 1
            continue
        defines = tuple(re.findall(r'"([^"]*)"', macros or ""))
        if "_" in stage:
            profiles = [stage]
        elif path.parent.name == "d3d11":
            profiles = [stage + "_5_0"]
        else:
            profiles = [stage + ("_6_0" if fe == "dxil" else "_5_1") for fe in front_ends.get(path.stem, ())]
        if not profiles:
            skipped += 1
        for profile in profiles:
            key = (name, entry, profile, defines)
            if key in seen:
                duplicates += 1
                continue
            seen.add(key)
            yield sources[name][0], entry, profile, defines
    if duplicates:
        print(f"{path}: skipped {duplicates} duplicate variants")
    if skipped:
        print(f"{path}: skipped {skipped} calls without a unique literal source, entry, profile or definitions")


def xml_cases(directory):
    path = Path(DXC_TREE) / XML
    if not path.exists():
        revision = git("rev-parse", f"HEAD:{DXC_TREE}").decode().strip()
        path = directory / "ShaderOpArithTable.xml"
        url = f"https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/{revision}/{XML}"
        with urllib.request.urlopen(url) as response:
            path.write_bytes(response.read())
    # Check the same target, row count and data invariants as the conformance test.
    subprocess.run([sys.executable, "tests/dxc_cases.py", path], stdout=subprocess.DEVNULL, check=True)
    for row in ET.parse(path).findall("./Table[@Id='UnaryIntOpTable']/Row"):
        yield row.findtext("./Parameter[@Name='ShaderOp.Text']"), "main", row.findtext("./Parameter[@Name='ShaderOp.Target']"), ()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="compile fixtures in the diff from this commit to HEAD")
    parser.add_argument("--fxc", default="fxc")
    parser.add_argument("--dxc", default="dxc")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    paths = set(args.paths)
    if args.base:
        paths.update(changed_paths(args.base))
    if (not args.base and not paths) or paths & {"tests/lint_hlsl.py", ".github/workflows/hlsl.yml"}:
        paths.update([*SMOKE, "tests/dxc_cases.py"])
    if paths & {"tests/d3d11/d3d11_test.hpp", "tests/d3d12/d3d12_test.hpp",
                "tests/d3d11/meson.build", "tests/d3d12/meson.build"}:
        paths.update(SMOKE)
    for name in list(paths):
        if re.fullmatch(r"tests/\w+\.hpp", name):
            include = f'#include "../{Path(name).name}"'
            callers = [p.as_posix() for p in Path("tests").glob("d3d1[12]/*.cpp")
                       if include in p.read_text(encoding="utf-8")]
            paths.update(callers)
            print(f"{name}: checking {len(callers)} direct C++ callers")
    # Meson's test rows name the front ends when a D3D12 test only runs one of them.
    table = Path("tests/d3d12/meson.build").read_text().split("foreach t : ", 1)[1].splitlines()[0]
    front_ends = {row[0]: row[2] if len(row) > 2 else ["dxil", "dxbc"] for row in ast.literal_eval(table)}
    failures = total = 0
    with tempfile.TemporaryDirectory() as folder:
        directory = Path(folder)
        paths_to_check = [p for p in sorted(paths) if re.fullmatch(r"tests/d3d1[12]/[^/]+\.(cpp|hpp)", p)]
        if paths & {DXC_TREE, "tests/dxc_cases.py", "tests/d3d12/d3d12_dxc_official.cpp"}:
            paths_to_check.append(DXC_TREE)
        for p in paths_to_check:
            cases = xml_cases(directory) if p == DXC_TREE else shader_cases(Path(p), front_ends)
            count = 0
            for source, entry, profile, defines in cases:
                shader = directory / "shader.hlsl"
                shader.write_text(source, encoding="utf-8")
                modern = int(profile.split("_")[1]) >= 6
                compiler = args.dxc if modern else args.fxc
                print(f"{p}: {entry or '(library)'} {profile} {' '.join(defines)}", flush=True)
                cmd = [compiler, "-WX", "-T", profile, "-Fo", str(directory / "shader.bin")]
                if entry:
                    cmd += ["-E", entry]
                for define in defines:
                    # Compiler::compile passes bare definitions to D3DCompile as NAME=NAME.
                    if not modern and p.startswith("tests/d3d12/") and "=" not in define:
                        define = f"{define}={define}"
                    cmd += ["-D", define]
                failures += subprocess.run([*cmd, str(shader)]).returncode != 0
                count += 1
            total += count
            if not count:
                print(f"{p}: no statically identified shader variants")
    print(f"Checked {total} shader variants; {failures} failed")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
