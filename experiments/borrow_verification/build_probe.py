#!/usr/bin/env python3
"""Build the opt-in probe with an existing optimized Ninja compiler build.

No CMake targets or tests are registered. Only the new probe object is linked.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess


def build_probe(build: Path, out: Path) -> Path:
    build = build.resolve()
    root = Path(__file__).resolve().parents[2]
    cache = (build / 'CMakeCache.txt').read_text()
    if not any('CMAKE_BUILD_TYPE:STRING=' + p in cache for p in ('Release', 'RelWithDebInfo', 'MinSizeRel')):
        raise ValueError('An optimized compiler build is required')
    if f'CMAKE_HOME_DIRECTORY:INTERNAL={root}' not in cache:
        raise ValueError('Probe source and compiler build must use the same checkout')
    out.mkdir(parents=True, exist_ok=True)
    entries = json.loads((build / 'compile_commands.json').read_text())
    entry = next(e for e in entries if e['file'].endswith('/tests/scalar_temporary_borrow_tests.cpp'))
    args = list(entry.get('arguments') or shlex.split(entry['command']))
    llvm_dir = next(line.split('=', 1)[1] for line in cache.splitlines() if line.startswith('LLVM_DIR:'))
    args.extend(['-I' + str(root / 'tests'), '-isystem', str(Path(llvm_dir).resolve().parents[2] / 'include')])
    obj = out.resolve() / 'borrow_probe.o'
    target = out.resolve() / 'borrow_probe'
    args[args.index('-o') + 1] = str(obj)
    args[args.index('-c') + 1] = str(root / 'experiments/borrow_verification/probe.cpp')
    subprocess.run(args, cwd=entry['directory'], check=True)
    link_probe(build, obj, target)
    return target


def link_probe(build, obj, target, extra_objects=()):
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'gti_scalar_temporary_borrow_tests'], text=True)
    line = commands.splitlines()[-1]
    if line.startswith(': && ') and line.endswith(' && :'):
        line = line[5:-5]
    link = shlex.split(line)
    if any(x in link for x in ('&&', ';', '|')):
        raise ValueError('Unsupported link command wrapper')
    objects = [i for i, x in enumerate(link) if x.endswith('.o')]
    if len(objects) != 1:
        raise ValueError('Expected exactly one owning test object')
    link[objects[0]] = str(obj)
    link[objects[0]:objects[0]] = [str(x) for x in extra_objects]
    link[link.index('-o') + 1] = str(target)
    subprocess.run(link, cwd=build, check=True)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    print(build_probe(a.build, a.out))
