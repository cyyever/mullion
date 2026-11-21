#!/usr/bin/env python3
"""Run the project's clang-tidy checks on staged C++ changes, serially."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = {'.cpp', '.cc', '.cxx', '.mm'}
HEADER = {'.h', '.hpp', '.hh', '.hxx', '.inc', '.inl'}
FIXTURE = ROOT / 'tests/host/air_tidy.cpp'


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def owned(path):
    return path.is_relative_to(ROOT) and path.relative_to(ROOT).parts[0] in ('src', 'tests') and 'engines' not in path.relative_to(ROOT).parts


def build_directory(value):
    if value:
        candidates = [Path(value).resolve()]
    else:
        candidates = [ROOT / 'build', ROOT.parent / 'mullion-build-llvm23-stl', *ROOT.glob('build*'),
                      *ROOT.parent.glob(ROOT.name + '-build*')]
    for path in candidates:
        if (path / 'compile_commands.json').is_file():
            return path
    sys.exit('clang-tidy: configure a build with compile_commands.json; set MULLION_TIDY_BUILD to its directory.')


def commands(build, clang):
    entries = {}
    includes = None
    for entry in json.loads((build / 'compile_commands.json').read_text()):
        source = (Path(entry['directory']) / entry['file']).resolve()
        if not owned(source) or source.suffix not in SOURCE or source == FIXTURE:
            continue
        args = entry.get('arguments') or shlex.split(entry['command'])
        native = 'mingw' not in args[0]
        if source in entries and (not native or entries[source]['native']):
            continue
        output = entry.get('output') or args[args.index('-o') + 1]
        if not native:
            if includes is None:
                result = subprocess.run([args[0], '-E', '-x', 'c++', '-v', os.devnull], capture_output=True, text=True, check=True)
                search = result.stderr.split('#include <...> search starts here:')[1].split('End of search list.')[0]
                includes = [str(Path(line.strip()).resolve()) for line in search.splitlines() if line.strip()]
                # Use Clang's own intrinsics, not GCC's intrinsic headers.
                includes = [path for path in includes if '/lib/gcc/' not in path]
            args[0] = str(clang)
            args[1:1] = ['--target=x86_64-w64-windows-gnu', '-nostdinc++',
                         *[arg for path in includes for arg in ('-isystem', path)]]
        entries[source] = dict(file=str(source), directory=entry['directory'], arguments=args,
                               native=native, output=str((Path(entry['directory']) / output).resolve()))
    return entries


def affected(entries, changed, build):
    selected = set(entries) & changed
    dependent = changed - selected
    if dependent:
        objects = {entry['output']: path for path, entry in entries.items()}
        found = set()
        process = subprocess.Popen(['ninja', '-C', str(build), '-t', 'deps'], stdout=subprocess.PIPE, text=True)
        target = None
        for line in process.stdout:
            if ': #deps ' in line or ': #deps=' in line:
                target = objects.get(str((build / line.split(': #deps', 1)[0]).resolve()))
            elif line.startswith('    '):
                path = (build / line.strip()).resolve()
                if path in dependent and target:
                    selected.add(target)
                    found.add(path)
        if process.wait():
            sys.exit('clang-tidy: could not read Ninja dependencies; rebuild before committing.')
        uncovered = dependent - found
        sources = [path for path in uncovered if path.suffix in SOURCE]
        if sources:
            sys.exit('clang-tidy: no compile command or include dependency for: ' +
                     ', '.join(str(path.relative_to(ROOT)) for path in sources) + '; update the build configuration.')
        # A new header or an absent dependency record cannot safely narrow the check.
        if uncovered:
            selected.update(entries)
    return sorted(selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=os.environ.get('MULLION_TIDY_BUILD'))
    parser.add_argument('--list', action='store_true', help='show affected compilation units without running checks')
    args = parser.parse_args()
    staged = {ROOT / os.fsdecode(path) for path in git('diff', '--cached', '--name-only', '--diff-filter=ACMRD', '-z').split(b'\0') if path}
    changed = {path for path in staged if owned(path) and path.suffix in SOURCE | HEADER and path != FIXTURE}
    if not changed:
        return 0
    build = build_directory(args.build)
    name = os.environ.get('CLANG_TIDY') or shutil.which('clang-tidy') or '/opt/homebrew/opt/llvm/bin/clang-tidy'
    tidy = Path(name).resolve()
    if not tidy.is_file():
        sys.exit('clang-tidy: executable missing; install LLVM or set CLANG_TIDY.')
    entries = commands(build, tidy.with_name('clang++'))
    selected = affected(entries, changed, build)
    if not selected:
        sys.exit('clang-tidy: no compilation unit covers the staged C++ changes; update the build configuration.')
    print(f'clang-tidy: {len(selected)} affected compilation units (serial)', flush=True)
    if args.list:
        for path in selected:
            print(path.relative_to(ROOT))
        return 0
    with tempfile.TemporaryDirectory(prefix='mullion-staged-tidy-') as temporary:
        temp = Path(temporary)
        database = [{k: v for k, v in entries[path].items() if k not in ('native', 'output')} for path in selected]
        (temp / 'compile_commands.json').write_text(json.dumps(database))
        # Overlay both staged and unstaged edits with their index versions. The worktree is never rewritten.
        dirty = staged | {ROOT / os.fsdecode(path) for path in git('diff', '--name-only', '-z').split(b'\0') if path}
        roots = []
        for i, path in enumerate(sorted(dirty)):
            if not owned(path) or path.suffix not in SOURCE | HEADER:
                continue
            result = subprocess.run(['git', '-C', str(ROOT), 'show', ':' + str(path.relative_to(ROOT))], capture_output=True)
            snapshot = temp / str(i)
            if result.returncode == 0:
                snapshot.write_bytes(result.stdout)
            roots.append({'type': 'file', 'name': str(path), 'external-contents': str(snapshot), 'use-external-name': False})
        overlay = temp / 'overlay.json'
        overlay.write_text(json.dumps({'version': 0, 'roots': roots}))
        config = temp / 'air-tidy.yaml'
        config.write_bytes(git('show', ':tools/air-tidy.yaml'))
        failed = False
        for path in selected:
            print(f'clang-tidy: {path.relative_to(ROOT)}', flush=True)
            try:
                result = subprocess.run([str(tidy), '-p', str(temp), '--experimental-custom-checks',
                                         '--config-file=' + str(config), '--vfsoverlay=' + str(overlay),
                                         '--header-filter=^' + str(ROOT) + '/(src|tests)/',
                                         '--line-filter=' + json.dumps([{'name': str(file.relative_to(ROOT))} for file in sorted(changed)]),
                                         '--warnings-as-errors=*', str(path)], cwd=ROOT, timeout=180)
                failed |= result.returncode != 0
            except subprocess.TimeoutExpired:
                print(f'clang-tidy: timeout: {path.relative_to(ROOT)}', file=sys.stderr)
                failed = True
        if failed:
            print('clang-tidy: commit blocked; fix the diagnostics and stage the changes.', file=sys.stderr)
        return int(failed)


if __name__ == '__main__':
    sys.exit(main())
