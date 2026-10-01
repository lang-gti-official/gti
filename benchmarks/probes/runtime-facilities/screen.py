#!/usr/bin/env python3
"""Disposable candidate screen, not a compiler optimization or shipping tool."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('command', choices=['build', 'measure'])
parser.add_argument('--gti', required=True, type=Path)
parser.add_argument('--cxx', default='/usr/bin/clang++')
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
OUT = args.output.resolve()
GTI = str(args.gti.resolve())
CXX = args.cxx
if args.command == 'build':
    OUT.mkdir(parents=True, exist_ok=False)
elif not OUT.is_dir():
    parser.error('measure requires an existing build output directory')
COMMON = ['-O3', '-std=c++23', '-fno-fast-math', '-ffp-contract=off']
KINDS = ['callback', 'interface', 'owner', 'soa', 'cross-unit']

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def invoke(argv, name):
    start = time.perf_counter_ns()
    result = subprocess.run(['/usr/bin/time', '-l', *argv], capture_output=True, text=True, timeout=180)
    (OUT / (name + '.stdout')).write_text(result.stdout)
    (OUT / (name + '.stderr')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name}: exit {result.returncode}: {result.stderr[:3000]}')
    return {'argv': argv, 'duration_ns': time.perf_counter_ns()-start, 'log': str(OUT / (name + '.stderr'))}

def compile_one(kind, variant):
    name = kind + '-' + variant
    exe = OUT / name
    if variant.startswith('gti'):
        source = ROOT / (kind + ('-proxy' if variant == 'gti-proxy' and kind != 'cross-unit' else '') + '.gti')
        cmd = [GTI, str(source), '-O3', '--std', 'c++23', '--cxx', CXX, '--keep-cpp',
               '--build-report', str(OUT / (name + '.build.json')),
               '--optimization-report', str(OUT / (name + '.opt.json')),
               '--dump-mir-before', str(OUT / (name + '.before.mir')),
               '--dump-mir-after', str(OUT / (name + '.after.mir')), '-o', str(exe)]
        if kind == 'cross-unit':
            cmd += ['--', str(OUT / ('native-lto.o' if variant == 'gti-proxy' else 'native.o'))]
            if variant == 'gti-proxy': cmd += ['-flto=thin']
    else:
        source = ROOT / 'reference.cpp'
        cmd = [CXX, *COMMON, '-D'+kind.upper().replace('-','_'), str(source), '-o', str(exe)]
        if variant == 'cpp-idiomatic': cmd += ['-DIDIOMATIC']
        if kind == 'cross-unit': cmd += [str(OUT / 'native.o')]
    build = invoke(cmd, name + '.compile')
    build.update(name=name, kind=kind, variant=variant, source=str(source), source_sha256=sha(source),
                 executable=str(exe), executable_sha256=sha(exe), executable_bytes=exe.stat().st_size)
    inspect = subprocess.run(['/usr/bin/otool', '-tvV', str(exe)], capture_output=True, text=True)
    (OUT / (name + '.asm')).write_text(inspect.stdout)
    memory = invoke([str(exe)], name + '.memory')
    build['memory'] = memory
    build['output_sha256'] = sha(OUT / (name + '.memory.stdout'))
    build['output_lines'] = len((OUT / (name + '.memory.stdout')).read_text().splitlines())
    return build

def build():
    native = invoke([CXX, *COMMON, '-c', str(ROOT/'native.cpp'), '-o', str(OUT/'native.o')], 'native')
    native_lto = invoke([CXX, *COMMON, '-flto=thin', '-c', str(ROOT/'native.cpp'), '-o', str(OUT/'native-lto.o')], 'native-lto')
    tasks = [(k,v) for k in KINDS for v in ['gti', 'gti-proxy', 'cpp-semantic', 'cpp-idiomatic']]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        builds = list(pool.map(lambda item: compile_one(*item), tasks))
    for kind in KINDS:
        group = [b for b in builds if b['kind'] == kind]
        if len({b['output_sha256'] for b in group}) != 1 or any(b['output_lines'] != 1024 for b in group):
            raise RuntimeError('complete-state mismatch: ' + kind)
    report = {'compiler':GTI, 'compiler_sha256':sha(GTI), 'native_compiler_sha256':sha(CXX), 'native': [native, native_lto], 'builds':builds}
    (OUT/'builds.json').write_text(json.dumps(report,indent=2))

def measure():
    report = json.loads((OUT/'builds.json').read_text())
    if sha(GTI) != report['compiler_sha256']:
        raise RuntimeError('selected compiler changed since the probe build')
    for build in report['builds']:
        if sha(build['executable']) != build['executable_sha256']:
            raise RuntimeError('probe executable changed: ' + build['name'])
        if sha(build['source']) != build['source_sha256']:
            raise RuntimeError('probe source changed: ' + build['name'])
    rng = random.Random(1001)
    results = []
    for kind in KINDS:
        builds = [b for b in report['builds'] if b['kind'] == kind]
        samples = {b['name']:[] for b in builds}
        warmups = {b['name']:[] for b in builds}
        order = []
        for r in range(25):
            batch = builds[:]
            rng.shuffle(batch)
            order.append([b['name'] for b in batch])
            for b in batch:
                start=time.perf_counter_ns()
                process=subprocess.run([b['executable']],capture_output=True,timeout=180)
                elapsed=time.perf_counter_ns()-start
                if process.returncode or hashlib.sha256(process.stdout).hexdigest()!=b['output_sha256']:
                    raise RuntimeError('changed complete state: '+b['name'])
                (warmups if r<5 else samples)[b['name']].append(elapsed)
        result={'kind':kind,'warmup_samples_ns':warmups,'samples_ns':samples,'order':order,
                'median_ms':{n:statistics.median(v)/1e6 for n,v in samples.items()}}
        results.append(result)
        print(json.dumps(result['median_ms']), flush=True)
        report['measurements']=results
        (OUT/'report.json').write_text(json.dumps(report,indent=2))

if __name__ == '__main__':
    {'build':build,'measure':measure}[args.command]()
