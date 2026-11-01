#!/usr/bin/env python3
"""Manual paired before/after GTI study using the unchanged comparative corpus."""
import argparse
import base64
import json
import os
from pathlib import Path
import random
import re
import statistics
import sys
import time
import zlib

import run as corpus


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', required=True, type=Path)
    parser.add_argument('--baseline-stdlib', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--candidate-stdlib', required=True, type=Path)
    parser.add_argument('--baseline-source', required=True)
    parser.add_argument('--candidate-source', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--samples', type=int, default=11)
    parser.add_argument('--case', action='append')
    parser.add_argument('--target-ms', type=float, default=100)
    args = parser.parse_args()
    if args.samples < 5 or args.target_ms <= 0:
        parser.error('samples >=5 and positive target-ms required')
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parent
    cases = sum((json.loads(p.read_text()) for p in sorted(root.glob('*_cases.json'))), [])
    if args.case:
        unknown = set(args.case) - {c['id'] for c in cases}
        if unknown:
            parser.error(f'unknown cases: {unknown}')
        cases = [c for c in cases if c['id'] in args.case]
    support = args.output / 'support.o'
    built = corpus.command(['clang', '-O3', '-c', root / 'support.c', '-o', support])
    if built['exit'] != 0:
        raise RuntimeError(built)
    report = {
        'schema': 1, 'kind': 'paired-gti-compilers',
        'baseline_source': args.baseline_source, 'candidate_source': args.candidate_source,
        'baseline_sha256': corpus.sha(args.baseline), 'candidate_sha256': corpus.sha(args.candidate),
        'baseline_stdlib': corpus.tree_hash(args.baseline_stdlib),
        'candidate_stdlib': corpus.tree_hash(args.candidate_stdlib),
        'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'host': corpus.platform.platform(), 'load_start': os.getloadavg(),
        'versions': {name: corpus.command([getattr(args, name), '--version'])
                     for name in ('baseline', 'candidate')},
        'support_compiler': corpus.command(['clang', '--version']),
        'policy': {'samples': args.samples, 'warmups': 2, 'target_ms': args.target_ms,
                   'comparison': 'candidate/baseline; paired bootstrap 95% interval',
                   'practical_band': 0.05},
        'cases': []}
    failures = 0
    for case in cases:
        row = dict(case, builds={}, binary_bytes={}, binary_sha256={}, validation=[], samples=[])
        report['cases'].append(row)
        directory = args.output / case['id']
        directory.mkdir()
        source = root / 'workloads' / case['id'] / 'main.gti'
        row['source_sha256'] = corpus.sha(source)
        bins = {name: directory / name for name in ('baseline', 'candidate')}
        print('CASE', case['id'], flush=True)
        for name in bins:
            compiler = getattr(args, name)
            stdlib = getattr(args, name + '_stdlib')
            argv = [compiler, source, '--backend', 'llvm', '-O3', '-o', bins[name]]
            if case.get('execution_profile'):
                argv += ['--execution-profile', case['execution_profile']]
            argv += ['--', support]
            # These are build-time observations, not a compiler-latency claim.
            env = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
            command = ['/usr/bin/time', '-l', *argv] if sys.platform == 'darwin' else argv
            result = corpus.command(command, env=env)
            peak = re.search(r'^\s*(\d+)\s+maximum resident set size\s*$',
                             result.get('stderr', ''), re.MULTILINE)
            if peak:
                result['peak_rss_bytes'] = int(peak.group(1))
            row['builds'][name] = result
            if result['exit'] == 0:
                row['binary_bytes'][name] = bins[name].stat().st_size
                row['binary_sha256'][name] = corpus.sha(bins[name])
        try:
            if any(v['exit'] != 0 for v in row['builds'].values()):
                raise RuntimeError('compile failure')
            for seed in (17, 29, 101):
                for rounds in (1, 2):
                    observations = {name: corpus.sample(binary, rounds, seed, 30) for name, binary in bins.items()}
                    row['validation'].append({'seed': seed, 'rounds': rounds, 'results': observations})
                    if len({v['digest'] for v in observations.values()}) != 1:
                        raise RuntimeError('correctness disagreement')
            if len({v['results']['baseline']['digest'] for v in row['validation']
                    if v['rounds'] == 1}) == 1:
                raise RuntimeError('seed-independent digest')
            rounds = 1
            row['calibration'] = []
            target = args.target_ms * 1e6
            for attempt in range(4):
                observations = {name: corpus.sample(binary, rounds, 17, 30) for name, binary in bins.items()}
                row['calibration'].append({'rounds': rounds, 'results': observations})
                if len({v['digest'] for v in observations.values()}) != 1:
                    raise RuntimeError('calibration disagreement')
                fastest = min(v['ns'] for v in observations.values())
                slowest = max(v['ns'] for v in observations.values())
                if fastest >= target or rounds >= 1000000 or attempt == 3:
                    break
                new_rounds = corpus.calibration_rounds(rounds, fastest, slowest, target, 30)
                if new_rounds == rounds:
                    break
                rounds = new_rounds
            expected = observations['baseline']['digest']
            row['rounds'] = rounds
            rng = random.Random(7151)
            for n in range(2 + args.samples):
                order = list(bins)
                rng.shuffle(order)
                observations = {name: corpus.sample(bins[name], rounds, 17, 30) for name in order}
                if any(v['digest'] != expected for v in observations.values()):
                    raise RuntimeError('sample correctness disagreement')
                if n >= 2:
                    row['samples'].append({'order': order, 'results': observations, 'load': os.getloadavg()})
            times = {name: [s['results'][name]['ns'] for s in row['samples']] for name in bins}
            row['medians_ns'] = {name: statistics.median(v) for name, v in times.items()}
            row['relative_mad'] = {name: statistics.median(abs(x - row['medians_ns'][name]) for x in v) / row['medians_ns'][name] for name, v in times.items()}
            result = corpus.compare(times['candidate'], times['baseline'])
            row['comparison'] = result
            lo, hi = result['bootstrap_95_interval']
            row['status'] = 'improved' if hi < .95 else 'regressed' if lo > 1.05 else 'within-band-or-inconclusive'
            if max(row['relative_mad'].values()) > .10:
                row['status'] = 'noisy'
            elif min(row['medians_ns'].values()) < target / 2:
                row['status'] = 'too-short'
        except (RuntimeError, ValueError) as error:
            row['status'] = 'failed'
            row['error'] = str(error)
            failures += 1
        (args.output / 'paired.json').write_text(json.dumps(report, indent=2))
        print('RESULT', case['id'], row['status'], row.get('comparison', {}).get('ratio'), flush=True)
    report['load_end'] = os.getloadavg()
    report['counts'] = {status: sum(c['status'] == status for c in report['cases']) for status in sorted({c['status'] for c in report['cases']})}
    (args.output / 'paired.json').write_text(json.dumps(report, indent=2))
    print('GTI_PAIRED_REPORT_ZLIB_BASE64', base64.b64encode(zlib.compress(json.dumps(report).encode())).decode(), flush=True)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
