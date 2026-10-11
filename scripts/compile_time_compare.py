#!/usr/bin/env python3
"""Measure clean GTI/C++ compilation, not runtime or cache-hit latency.

Reuses the runtime harness's source, tool, and native-provenance validation.
Timing bands are descriptive only: never use wall time as a shared CI gate.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time

import benchmark_compare as benchmark


def invoke(command: list[str], directory: Path, label: str) -> dict:
    start = time.perf_counter_ns()
    result = subprocess.run(command, cwd=benchmark.REPOSITORY_ROOT,
                            capture_output=True, timeout=180)
    elapsed = time.perf_counter_ns() - start
    (directory / (label + '.stdout')).write_bytes(result.stdout)
    (directory / (label + '.stderr')).write_bytes(result.stderr)
    if result.returncode:
        raise benchmark.BenchmarkError(
            f'{label}: exit {result.returncode}: {result.stderr.decode(errors="replace")}')
    return {'argv': command, 'duration_ns': elapsed,
            'stdout_sha256': benchmark.sha256_bytes(result.stdout)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gti', required=True)
    parser.add_argument('--baseline-gti')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--workload', action='append', required=True)
    parser.add_argument('--optimization', action='append', choices=('0', '1', '2', '3'))
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--warmup', type=int, default=1)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        parser.error('runs must be positive and warmup nonnegative')
    workloads = [benchmark.load_workload(name) for name in args.workload]
    if args.output.exists():
        parser.error('output must be a new directory')
    output = benchmark.canonical_output_root(str(args.output), workloads)
    compilers = {'gti': benchmark.resolved_tool(args.gti, ('--version',))}
    if args.baseline_gti:
        compilers['baseline-gti'] = benchmark.resolved_tool(args.baseline_gti, ('--version',))
    cxx = benchmark.resolved_tool(args.cxx, ('--version',))
    report = {'schema': 1, 'scope': 'clean builds; warm filesystem; no compiler cache',
              'seed': 1001, 'runs': args.runs, 'warmup': args.warmup,
              'comparison_band': {'cpp_multiplier': 2, 'fixed_allowance_ms': 250,
                                  'status': 'provisional; not a CI timing gate'},
              'environment': {'platform': platform.platform(), 'machine': platform.machine()},
              'tools': {k: benchmark.tool_record(v) for k, v in {**compilers, 'cxx': cxx}.items()},
              'rows': []}
    rng = random.Random(1001)
    for workload in workloads:
        variants = {v.name: v for v in workload.variants}
        if 'gti' not in variants or 'cpp-idiomatic' not in variants:
            raise benchmark.BenchmarkError('requires gti and cpp-idiomatic variants')
        gti_source = variants['gti'].source
        for level in args.optimization or ['0', '2']:
            directory = output / (workload.name + '-O' + level)
            directory.mkdir()
            commands = {}
            artifacts = {}
            provenance = {}
            for name, compiler in compilers.items():
                executable = directory / name
                base = [str(compiler.resolved), str(gti_source), '-O' + level,
                        '--std', 'c++23', '--cxx', str(cxx.command_path)]
                profile = variants['gti'].execution_profile
                if profile:
                    base += ['--execution-profile', profile]
                sidecar = directory / (name + '.build.json')
                invoke(base + ['--keep-cpp', '--build-report', str(sidecar), '-o', str(executable)],
                       directory, name + '-prepare')
                provenance[name] = benchmark.load_build_provenance(
                    sidecar, compiler, cxx, gti_source, benchmark.REPOSITORY_ROOT)
                generated = executable.with_suffix('.gti.cpp')
                artifacts[name] = benchmark.file_evidence(generated)
                commands[name + '-full'] = base + ['--keep-cpp', '-o', str(executable)]
                commands[name + '-emit'] = base + ['--emit-cpp', '-o', str(directory / (name + '-emit.cpp'))]
                commands[name + '-native'] = benchmark.fixed_native_rebuild_command(
                    provenance[name]['build']['native']['argv'], generated, executable,
                    directory / (name + '-native'), directory / (name + '.d'),
                    'c++23', level, level, benchmark.REPOSITORY_ROOT)
            for name in ('cpp-idiomatic', 'cpp-semantic'):
                if name in variants:
                    commands[name] = [str(cxx.command_path), '-O' + level, '-std=c++23',
                                      *benchmark.STRICT_NATIVE_FLAGS, str(variants[name].source),
                                      '-o', str(directory / name)]
            samples = {name: [] for name in commands}
            warmups = {name: [] for name in commands}
            order = []
            for iteration in range(args.warmup + args.runs):
                batch = list(commands)
                rng.shuffle(batch)
                order.append(batch)
                for name in batch:
                    measurement = invoke(commands[name], directory, f'{iteration}-{name}')
                    (warmups if iteration < args.warmup else samples)[name].append(measurement)
            # Compare complete generated code when measuring an internal-only change.
            emitted_equal = (None if 'baseline-gti' not in compilers else
                             artifacts['gti']['sha256'] == artifacts['baseline-gti']['sha256'])
            # Large runtime loops at O0 are deliberately not part of this compile-time run.
            outputs = {}
            if level in ('2', '3'):
                for name in [*compilers, 'cpp-idiomatic', *(['cpp-semantic'] if 'cpp-semantic' in variants else [])]:
                    outputs[name] = invoke([str(directory / name)], directory, name + '-validate')['stdout_sha256']
                if len(set(outputs.values())) != 1:
                    raise benchmark.BenchmarkError('runtime result mismatch: ' + workload.name)
            medians = {k: statistics.median(x['duration_ns'] for x in v) / 1e6 for k, v in samples.items()}
            row = {'workload': workload.name, 'optimization': level, 'samples': samples,
                   'warmups': warmups, 'order': order, 'median_ms': medians,
                   'provenance': provenance, 'emitted_cpp': artifacts,
                   'baseline_emission_identical': emitted_equal, 'runtime_outputs': outputs,
                   'sources': {k: {**benchmark.file_evidence(v.source),
                                   'lines': len(v.source.read_text().splitlines())}
                               for k, v in variants.items() if k in ('gti', 'cpp-idiomatic', 'cpp-semantic')},
                   'provisional_band_met': medians['gti-full'] <= 2 * medians['cpp-idiomatic'] + 250}
            report['rows'].append(row)
            (output / 'report.json').write_text(json.dumps(report, indent=2))
            print(workload.name, 'O' + level, json.dumps(medians), flush=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (benchmark.BenchmarkError, OSError, subprocess.TimeoutExpired) as error:
        print(f'compile-time comparison: {error}', file=sys.stderr)
        raise SystemExit(1)
