#!/usr/bin/env python3
"""Bounded before/after source-storage measurement; no timing acceptance gate.

Uses existing optimized binaries and frozen, independently selected libraries.
Run only while other compiler builds, tests and benchmarks are paused.
"""
from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import tarfile
import time

import benchmark_compare as benchmark

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gti', type=Path, required=True)
    parser.add_argument('--baseline-gti', type=Path, required=True)
    parser.add_argument('--baseline-revision', required=True)
    parser.add_argument('--cxx', default='/usr/bin/clang++')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compile-runs', type=int, default=5)
    parser.add_argument('--runtime-runs', type=int, default=9)
    args = parser.parse_args()
    if args.compile_runs < 1 or args.runtime_runs < 1:
        parser.error('sample counts must be positive')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'benchmarks/probes/storage-migration.gti'
    fixture = output / source.name
    shutil.copy2(source, fixture)
    revision = subprocess.check_output(
        ['git', 'rev-parse', '--verify', args.baseline_revision + '^{commit}'],
        cwd=ROOT, text=True).strip()
    report = {'schema': 1, 'scope': 'warm filesystem; sequential direct builds; O2 C++23',
              'warning': 'descriptive local samples, not a statistical performance guarantee',
              'baseline_revision': revision, 'source': benchmark.file_evidence(fixture),
              'harness': benchmark.file_evidence(Path(__file__).resolve()),
              'repository': benchmark.repository_facts(), 'variants': {}, 'order': []}
    report['tools'] = {'cxx': benchmark.tool_record(
        benchmark.resolved_tool(args.cxx, ('--version',)))}
    report['working_diff_sha256'] = benchmark.sha256_bytes(subprocess.check_output(
        ['git', 'diff', 'HEAD', '--'], cwd=ROOT))
    environments, commands, binaries = {}, {}, {}
    for name, original in [('baseline', args.baseline_gti), ('candidate', args.gti)]:
        original = original.resolve()
        directory = output / name
        directory.mkdir()
        snapshot = directory / 'source'
        snapshot.mkdir()
        resources = ('stdlib', 'runtime/include', 'vendor/expected_lite/include')
        if name == 'baseline':
            archive = subprocess.check_output(['git', 'archive', revision, *resources], cwd=ROOT)
            with tarfile.open(fileobj=io.BytesIO(archive)) as bundle:
                bundle.extractall(snapshot, filter='data')
        else:
            for resource in resources:
                shutil.copytree(ROOT / resource, snapshot / resource)
        compiler = directory / 'gti'
        runtime = directory / 'libgti_runtime.a'
        shutil.copy2(original, compiler)
        shutil.copy2(original.parent / 'libgti_runtime.a', runtime)
        cache = (original.parent / 'CMakeCache.txt').read_text()
        if not any('CMAKE_BUILD_TYPE:STRING=' + mode in cache
                   for mode in ('Release', 'RelWithDebInfo', 'MinSizeRel')):
            raise RuntimeError('compiler must have an optimized host build: ' + str(original))
        identity = (original.parent / 'generated/gti/build_identity_config.h').read_text()
        if name == 'baseline' and (revision not in identity or '"clean"' not in identity):
            raise RuntimeError('baseline binary build identity does not match clean snapshot')
        env = dict(os.environ, GTI_STDLIB_PATH=str(snapshot / 'stdlib'),
                   GTI_RUNTIME_INCLUDE=str(snapshot / 'runtime/include'),
                   GTI_VENDOR_INCLUDE=str(snapshot / 'vendor/expected_lite/include'),
                   GTI_RUNTIME_LIBRARY=str(runtime))
        environments[name] = env
        executable = directory / 'workload'
        binaries[name] = executable
        base = [str(compiler), str(fixture), '-O2', '--std', 'c++23', '--cxx', args.cxx]
        commands[name + '-full'] = base + ['--keep-cpp', '-o', str(executable)]
        commands[name + '-emit'] = base + ['--emit-cpp', '-o', str(directory / 'emitted.cpp')]
        report['variants'][name] = {
            'compiler': benchmark.file_evidence(compiler), 'compiler_build': identity,
            'runtime': benchmark.file_evidence(runtime),
            'resources': [benchmark.file_evidence(path)
                          for path in sorted(snapshot.rglob('*')) if path.is_file()],
            'environment': {key: env[key] for key in ('GTI_STDLIB_PATH',
                'GTI_RUNTIME_INCLUDE', 'GTI_VENDOR_INCLUDE', 'GTI_RUNTIME_LIBRARY')},
            'compile_samples': {}, 'runtime_samples': {}, 'runtime_warmups': {}}

    def invoke(command: list[str], label: str, name: str) -> dict:
        start = time.perf_counter_ns()
        result = subprocess.run(command, cwd=output, env=environments[name],
                                capture_output=True, timeout=300)
        elapsed = time.perf_counter_ns() - start
        (output / (label + '.stdout')).write_bytes(result.stdout)
        (output / (label + '.stderr')).write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(label + ': ' + result.stderr.decode(errors='replace'))
        return {'argv': command, 'duration_ns': elapsed,
                'stdout': result.stdout.decode(), 'stderr': result.stderr.decode()}

    def save() -> None:
        (output / 'report.json').write_text(json.dumps(report, indent=2))

    save()
    print('Frozen compiler, runtime and source snapshots: ' + str(output), flush=True)
    for name in binaries:
        invoke(commands[name + '-full'] + ['--build-report', str(output / name / 'build.json'),
               '--optimization-report', str(output / name / 'optimization.json')],
               name + '-prepare', name)
        report['variants'][name]['provenance'] = json.loads(
            (output / name / 'build.json').read_text())
        report['variants'][name]['emitted_cpp'] = benchmark.file_evidence(
            binaries[name].with_suffix('.gti.cpp'))
        report['variants'][name]['executable'] = benchmark.file_evidence(binaries[name])
        save()
        print(name + ' prepared', flush=True)
    rng = random.Random(1001)
    for run in range(args.compile_runs + 1):
        batch = list(commands)
        rng.shuffle(batch)
        report['order'].append({'compile': run, 'commands': batch})
        for label in batch:
            name = label.split('-')[0]
            sample = invoke(commands[label], f'{label}-{run}', name)
            destination = 'compile_warmup' if run == 0 else 'compile_samples'
            report['variants'][name].setdefault(destination, {}).setdefault(label, []).append(sample)
        save()
        print('Compile batch ' + str(run) + ' complete', flush=True)
    for mode, arguments in [('process', []), ('resource', ['resource'])]:
        seeds = sum(round % 97 for round in range(25000))
        total = 128 * seeds + 25000 * 8448
        expected = list(map(str, [total * (2 if mode == 'resource' else 1),
                                 25000 * 192, 192 * seeds + 25000 * 12480]))
        for run in range(args.runtime_runs + 2):
            batch = list(binaries)
            rng.shuffle(batch)
            report['order'].append({'runtime': mode, 'run': run, 'variants': batch})
            for name in batch:
                sample = invoke([str(binaries[name]), *arguments], f'{name}-{mode}-{run}', name)
                lines = sample['stdout'].splitlines()
                if len(lines) != 5 or not all(line.isdecimal() for line in lines):
                    raise RuntimeError('malformed complete-state output')
                state = lines[:3]
                if state != expected:
                    raise RuntimeError('complete semantic state differs from arithmetic oracle')
                counts = list(map(int, lines[3:]))
                required = 0 if mode == 'process' else (50000 if name == 'baseline' else 100000)
                if counts != [required, required]:
                    raise RuntimeError('unexpected resource allocation/release counts: ' + str(counts))
                key = 'runtime_warmups' if run < 2 else 'runtime_samples'
                report['variants'][name][key].setdefault(mode, []).append(sample)
            save()
        print(mode + ' runtime samples complete', flush=True)
    for value in report['variants'].values():
        value['medians_ms'] = {
            key: statistics.median(sample['duration_ns'] for sample in samples) / 1e6
            for family in ('compile_samples', 'runtime_samples')
            for key, samples in value[family].items()}
        value['runtime_ranges_ms'] = {
            key: [min(sample['duration_ns'] for sample in samples) / 1e6,
                  max(sample['duration_ns'] for sample in samples) / 1e6]
            for key, samples in value['runtime_samples'].items()}
    save()
    print(json.dumps({key: value['medians_ms'] for key, value in report['variants'].items()}, indent=2))


if __name__ == '__main__':
    main()
