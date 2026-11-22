#!/usr/bin/env python3
"""Standalone owned-copy/view runtime comparison; never a CI gate."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=15)
    args = parser.parse_args()
    if args.samples < 3:
        parser.error('--samples must be at least 3')
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    for key in ('SDKROOT', 'DEVELOPER_DIR', 'MACOSX_DEPLOYMENT_TARGET',
                'DYLD_INSERT_LIBRARIES'):
        environment.pop(key, None)
    compiler = args.compiler.resolve()
    support = output / 'support.o'
    subprocess.run(['clang', '-O3', '-c', str(root.parents[1] / 'comparative/support.c'),
                    '-o', str(support)], env=environment, check=True)
    result = {'compiler': str(compiler), 'load_start': os.getloadavg(),
              'builds': {}, 'samples_ns': {'owned': [], 'views': []}}
    for name in ('owned', 'views'):
        start = time.monotonic()
        build = subprocess.run([str(compiler), str(root / (name + '.gti')),
                                '--backend', 'llvm', '-O3', '-o', str(output / name),
                                '--', str(support)], env=environment,
                               capture_output=True, text=True, timeout=240)
        (output / (name + '-build.log')).write_text(build.stdout + build.stderr)
        build.check_returncode()
        result['builds'][name] = {'seconds': time.monotonic() - start,
                                  'binary_bytes': (output / name).stat().st_size}

    def run(name, rounds):
        execution = subprocess.run([str(output / name)],
                                   env=dict(environment, BENCH_ROUNDS=str(rounds)),
                                   capture_output=True, text=True, timeout=120)
        execution.check_returncode()
        fields = execution.stdout.split()
        if len(fields) != 3 or fields[0] != 'GTI-COMPARE-1':
            raise RuntimeError('unexpected benchmark output: ' + execution.stdout)
        return int(fields[1]), int(fields[2])

    rounds = 1
    for _ in range(8):
        probes = {name: run(name, rounds) for name in ('owned', 'views')}
        if probes['owned'][0] != probes['views'][0]:
            raise RuntimeError('owned and borrowed results differ')
        fastest = min(value[1] for value in probes.values())
        if fastest >= 100_000_000:
            break
        rounds = min(1_000_000, max(rounds + 1,
                                   int(rounds * 120_000_000 / max(1, fastest))))
    result['rounds'] = rounds
    expected_digest = probes['owned'][0]
    # Recompute after the last calibration increase, if any.
    expected_digest = run('owned', rounds)[0]
    for repetition in range(args.samples):
        order = ('owned', 'views') if repetition % 2 == 0 else ('views', 'owned')
        for name in order:
            digest, elapsed = run(name, rounds)
            if digest != expected_digest:
                raise RuntimeError('benchmark digest changed')
            result['samples_ns'][name].append(elapsed)
    result['digest'] = expected_digest
    result['medians_ns'] = {name: statistics.median(values)
                            for name, values in result['samples_ns'].items()}
    result['view_over_owned'] = (result['medians_ns']['views'] /
                                 result['medians_ns']['owned'])
    result['load_end'] = os.getloadavg()
    (output / 'evidence.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
