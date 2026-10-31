#!/usr/bin/env python3
"""Standalone comparative study; deliberately not registered with CTest or CI."""
import argparse, base64, hashlib, json, math, os, platform, random, re, shutil, statistics, subprocess, sys, time, zlib
from pathlib import Path
LANGUAGES = ('gti', 'cpp', 'rust')
RECORD = re.compile('GTI-COMPARE-1 ([0-9]+) ([0-9]+)\\n?\\Z')

class SampleError(ValueError):

    def __init__(self, message, status='correctness_failed'):
        super().__init__(message)
        self.status = status

def tree_hash(path):
    root = Path(path)
    h = hashlib.sha256()
    count = 0
    for p in sorted(root.rglob('*')):
        if p.is_file():
            h.update(str(p.relative_to(root)).encode() + b'\x00')
            h.update(p.read_bytes())
            h.update(b'\x00')
            count += 1
    return {'path': str(root), 'sha256': h.hexdigest(), 'files': count}

def calibration_rounds(rounds, fastest, slowest, target, timeout):
    desired = max(rounds + 1, math.ceil(rounds * target / fastest * 1.1))
    safe = max(rounds, int(rounds * (timeout * 1000000000.0 / 2) / slowest))
    return min(1000000, desired, safe)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def command(argv, env=None, timeout=180):
    started = time.monotonic()
    try:
        p = subprocess.run(list(map(str, argv)), env=env, capture_output=True, text=True, timeout=timeout)
        return {'argv': list(map(str, argv)), 'exit': p.returncode, 'seconds': time.monotonic() - started, 'stdout': p.stdout[-16000:], 'stderr': p.stderr[-16000:]}
    except subprocess.TimeoutExpired as e:
        return {'argv': list(map(str, argv)), 'exit': None, 'seconds': time.monotonic() - started, 'error': 'timeout', 'stdout': str(e.stdout or '')[-16000:], 'stderr': str(e.stderr or '')[-16000:]}

def sample(binary, rounds, seed, timeout):
    env = dict(os.environ, BENCH_ROUNDS=str(rounds), BENCH_SEED=str(seed))
    r = command([binary], env, timeout)
    if r['exit'] != 0:
        raise SampleError(json.dumps(r), 'measurement_failed' if r.get('error') == 'timeout' else 'correctness_failed')
    m = RECORD.fullmatch(r['stdout'])
    if not m or r['stderr']:
        raise SampleError('malformed output: ' + json.dumps(r))
    digest, ns = map(int, m.groups())
    if ns <= 0:
        raise SampleError('nonpositive duration', 'measurement_failed')
    return {'digest': digest, 'ns': ns, 'process_seconds': r['seconds']}

def compare(gti, other, band=1.1):
    rng = random.Random(8103)
    n = len(gti)
    ratios = []
    for _ in range(2000):
        ix = [rng.randrange(n) for _ in range(n)]
        ratios.append(statistics.median([gti[i] for i in ix]) / statistics.median([other[i] for i in ix]))
    ratios.sort()
    low, high = (ratios[49], ratios[1949])
    ratio = statistics.median(gti) / statistics.median(other)
    verdict = 'competitive' if high <= band else 'slower' if low > band else 'inconclusive'
    return {'ratio': ratio, 'bootstrap_95_interval': [low, high], 'verdict': verdict}

def summary(cases):
    counts = {k: 0 for k in ('competitive', 'slower', 'inconclusive', 'compile_failed', 'correctness_failed', 'measurement_failed', 'too_short', 'noisy')}
    comparisons = {l: {k: 0 for k in ('competitive', 'slower', 'inconclusive')} for l in ('cpp', 'rust')}
    for c in cases:
        counts[c['status']] = counts.get(c['status'], 0) + 1
        if c['status'] in ('competitive', 'slower', 'inconclusive'):
            for l, r in c.get('comparisons', {}).items():
                comparisons[l][r['verdict']] += 1
    total = len(cases)
    comparable = sum((counts[k] for k in ('competitive', 'slower', 'inconclusive')))
    return {'total_corpus': total, 'comparable_cases': comparable, 'counts': counts, 'versus': comparisons, 'competitive_percent_of_comparable': 100 * counts['competitive'] / comparable if comparable else None, 'competitive_percent_of_entire_corpus': 100 * counts['competitive'] / total if total else None}

def markdown(report):
    s = report['summary']
    lines = ['# GTI / C++ / Rust comparative study', '', f"Host: {report['host']['platform']}; {report['host']['machine']}. Suite source: `{report['source_commit']}`; compiler source: `{report['compiler_source_commit']}`.", '', f"Corpus: {s['total_corpus']}; comparable: {s['comparable_cases']}. Counts: `{json.dumps(s['counts'], sort_keys=True)}`.", '', 'Ratio = GTI time / comparator time; lower is better. Competitive means the upper bound of the bootstrap interval is at most 1.10 against BOTH comparators. This is a corpus result, not a percentage of all possible programs.', '', '| Program | Status | GTI ms | C++ ms | Rust ms | GTI/C++ | GTI/Rust |', '|---|---|---:|---:|---:|---:|---:|']
    for c in report['cases']:
        med = c.get('medians_ns', {})
        comp = c.get('comparisons', {})
        vals = [f'{med[l] / 1000000.0:.3f}' if l in med else '—' for l in LANGUAGES]
        ratios = [f"{comp[l]['ratio']:.3f} [{comp[l]['bootstrap_95_interval'][0]:.3f}, {comp[l]['bootstrap_95_interval'][1]:.3f}]" if l in comp else '—' for l in ('cpp', 'rust')]
        lines.append('| ' + ' | '.join([c['id'], c['status'], *vals, *ratios]) + ' |')
    families = {}
    for c in report['cases']:
        group = families.setdefault(c['family'], {})
        group[c['status']] = group.get(c['status'], 0) + 1
    lines += ['', '## Family breakdown', '', '| Family | Outcomes |', '|---|---|']
    for family, counts in sorted(families.items()):
        lines.append('| ' + family + ' | ' + ', '.join((str(n) + ' ' + k for k, n in sorted(counts.items()))) + ' |')
    lines += ['', '## Interpretation and limitations', '', 'Build flags, raw timings, randomized orders, validation digests, source hashes, tool versions, binary sizes, compilation durations, and errors are retained in report.json. Build duration is one observation, not a compiler-speed benchmark. Binary size includes executable/runtime differences. Allocations and peak RSS are not measured in this stage.', '', 'GTI retains its checked semantics; Rust uses release overflow behavior and safe indexing, C++ ordinary native indexing. All authored successful arithmetic is bounded. Standard-library algorithms/representations are allowed to differ and must be interpreted using each case contract. Floating-point tests disable C++ contraction and avoid fast-math; integer digests are not a proof of bitwise floating equivalence.', '', 'Timing begins inside each process after runtime inputs are read and ends before reporting, excluding startup/output. Setup and teardown placement is documented per case. This is a single macOS ARM64 host campaign, not Linux/x86 evidence. Bootstrap intervals describe observed sampling uncertainty, not all environmental bias.']
    return '\n'.join(lines) + '\n'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--gti', required=True)
    p.add_argument('--stdlib')
    p.add_argument('--compiler-source-commit', default='unknown')
    p.add_argument('--compiler-build-type', default='unknown')
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--cc', default='clang')
    p.add_argument('--rustc', default='rustc')
    p.add_argument('--output', required=True)
    p.add_argument('--case', action='append')
    p.add_argument('--samples', type=int, default=11)
    p.add_argument('--warmup', type=int, default=2)
    p.add_argument('--target-ms', type=float, default=100)
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--smoke', action='store_true')
    p.add_argument('--emit-report', action='store_true')
    p.add_argument('--reuse-build', action='store_true')
    args = p.parse_args()
    if args.samples < 5 or args.warmup < 1 or args.target_ms <= 0:
        p.error('samples >=5, warmup >=1, target-ms >0 required')
    root = Path(__file__).resolve().parent
    out = Path(args.output).resolve()
    if out.exists() and (not args.reuse_build):
        p.error('output must be new (or explicitly --reuse-build)')
    out.mkdir(parents=True, exist_ok=True)
    if args.stdlib:
        os.environ['GTI_STDLIB_PATH'] = str(Path(args.stdlib).resolve())
    cases = []
    for path in sorted(root.glob('*_cases.json')):
        cases.extend(json.loads(path.read_text()))
    if len({c['id'] for c in cases}) != len(cases):
        raise ValueError('duplicate case id')
    if args.case:
        unknown = set(args.case) - {c['id'] for c in cases}
        if unknown:
            p.error('unknown cases ' + str(unknown))
        cases = [c for c in cases if c['id'] in args.case]
    versions = {l: command([tool, '--version']) for l, tool in [('gti', args.gti), ('cpp', args.cxx), ('rust', args.rustc), ('cc', args.cc)]}
    if any((v['exit'] != 0 for v in versions.values())):
        raise ValueError('toolchain missing ' + str(versions))
    source = command(['git', '-C', root, 'rev-parse', 'HEAD'])['stdout'].strip()
    report = {'schema': 1, 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'source_commit': source, 'compiler_source_commit': args.compiler_source_commit, 'compiler_build_type': args.compiler_build_type, 'source_status': command(['git', '-C', root, 'status', '--short'])['stdout'], 'host': {'platform': platform.platform(), 'machine': platform.machine(), 'cpu_count': os.cpu_count(), 'cpu': command(['sysctl', '-n', 'machdep.cpu.brand_string'])['stdout'].strip(), 'load_start': os.getloadavg()}, 'tools': versions, 'gti_sha256': sha(args.gti), 'harness_sha256': sha(root / 'support.c'), 'runner_sha256': sha(__file__), 'policy': vars(args), 'cases': []}
    report['descriptor_hashes'] = {p.name: sha(p) for p in sorted(root.glob('*_cases.json'))}
    selected_stdlib = args.stdlib or os.environ.get('GTI_STDLIB_PATH')
    report['stdlib'] = tree_hash(selected_stdlib) if selected_stdlib else {'path': 'compiler default', 'sha256': 'unknown'}
    report['host']['thermal_start'] = command(['pmset', '-g', 'therm'])
    support = out / 'support.o'
    c = command([args.cc, '-O3', '-c', root / 'support.c', '-o', support])
    report['support_build'] = c
    if c['exit'] != 0:
        raise ValueError(c)
    for case in cases:
        print('CASE ' + case['id'], flush=True)
        row = dict(case)
        row['builds'] = {}
        row['source_hashes'] = {}
        row['binary_bytes'] = {}
        row['binary_sha256'] = {}
        row['validation'] = []
        row['samples'] = []
        report['cases'].append(row)
        d = out / case['id']
        d.mkdir(exist_ok=True)
        src = root / 'workloads' / case['id']
        binaries = {l: d / l for l in LANGUAGES}
        argv = {'gti': [args.gti, src / 'main.gti', '--backend', 'llvm', '-O3', '-o', binaries['gti'], '--', support], 'cpp': [args.cxx, '-O3', '-std=c++23', '-ffp-contract=off', '-fno-fast-math', '-pthread', src / 'main.cpp', support, '-o', binaries['cpp']], 'rust': [args.rustc, '--edition=2021', '-C', 'opt-level=3', '-C', 'overflow-checks=no', src / 'main.rs', '-C', 'link-arg=' + str(support), '-o', binaries['rust']]}
        if case.get('execution_profile'):
            argv['gti'][1:1] = ['--execution-profile', case['execution_profile']]
        for l, ext in [('gti', 'gti'), ('cpp', 'cpp'), ('rust', 'rs')]:
            row['source_hashes'][l] = sha(src / ('main.' + ext))
            r = command(argv[l])
            row['builds'][l] = r
            (d / (l + '-build.json')).write_text(json.dumps(r, indent=2))
            if r['exit'] == 0:
                row['binary_bytes'][l] = binaries[l].stat().st_size
                row['binary_sha256'][l] = sha(binaries[l])
        row['status'] = 'compile_failed'
        if all((v['exit'] == 0 for v in row['builds'].values())):
            row['status'] = 'correctness_failed'
            try:
                for seed in (17, 29, 101):
                    for rounds in (1, 2):
                        observations = {l: sample(binaries[l], rounds, seed, args.timeout) for l in LANGUAGES}
                        row['validation'].append({'seed': seed, 'rounds': rounds, 'results': observations})
                        if len({v['digest'] for v in observations.values()}) != 1:
                            raise ValueError('checksum disagreement')
                row['seed_sensitive'] = len({v['results']['gti']['digest'] for v in row['validation'] if v['rounds'] == 1}) > 1
                if not row['seed_sensitive']:
                    raise ValueError('digest does not vary across tested input seeds')
                rounds = case.get('initial_rounds', 1)
                row['calibration'] = []
                row['status'] = 'measurement_failed'
                target = (1 if args.smoke else args.target_ms) * 1000000
                for attempt in range(4):
                    observations = {l: sample(binaries[l], rounds, 17, args.timeout) for l in LANGUAGES}
                    row['calibration'].append({'rounds': rounds, 'results': observations})
                    if len({v['digest'] for v in observations.values()}) != 1:
                        row['status'] = 'correctness_failed'
                        raise ValueError('calibration checksum disagreement')
                    fastest = min((v['ns'] for v in observations.values()))
                    slowest = max((v['ns'] for v in observations.values()))
                    if fastest >= target or rounds >= 1000000 or slowest > args.timeout * 1000000000.0 / 4 or (attempt == 3):
                        break
                    next_rounds = calibration_rounds(rounds, fastest, slowest, target, args.timeout)
                    if next_rounds == rounds:
                        break
                    rounds = next_rounds
                row['rounds'] = rounds
                expected = observations['gti']['digest']
                rng = random.Random(9217)
                for n in range(args.warmup + args.samples):
                    order = list(LANGUAGES)
                    rng.shuffle(order)
                    values = {}
                    load = os.getloadavg()
                    for l in order:
                        values[l] = sample(binaries[l], rounds, 17, args.timeout)
                        if values[l]['digest'] != expected:
                            row['status'] = 'correctness_failed'
                            raise ValueError('timed sample checksum disagreement')
                    if n >= args.warmup:
                        row['samples'].append({'order': order, 'load': load, 'results': values})
                times = {l: [s['results'][l]['ns'] for s in row['samples']] for l in LANGUAGES}
                row['medians_ns'] = {l: statistics.median(v) for l, v in times.items()}
                row['relative_mad'] = {l: statistics.median([abs(x - row['medians_ns'][l]) for x in v]) / row['medians_ns'][l] for l, v in times.items()}
                row['comparisons'] = {l: compare(times['gti'], times[l]) for l in ('cpp', 'rust')}
                row['status'] = 'competitive' if all((r['verdict'] == 'competitive' for r in row['comparisons'].values())) else 'slower' if any((r['verdict'] == 'slower' for r in row['comparisons'].values())) else 'inconclusive'
                if min(row['medians_ns'].values()) < target / 2:
                    row['status'] = 'too_short'
                if max(row['relative_mad'].values()) > 0.1:
                    row['status'] = 'noisy'
            except (ValueError, OSError) as e:
                row['error'] = str(e)
                if isinstance(e, SampleError):
                    row['status'] = e.status
        report['summary'] = summary(report['cases'])
        (out / 'report.json').write_text(json.dumps(report, indent=2))
        (out / 'report.md').write_text(markdown(report))
        print('RESULT ' + case['id'] + ' ' + row['status'], flush=True)
    report['host']['thermal_end'] = command(['pmset', '-g', 'therm'])
    report['host']['load_end'] = os.getloadavg()
    report['finished_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    report['summary'] = summary(report['cases'])
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    (out / 'report.md').write_text(markdown(report))
    print('SUMMARY ' + json.dumps(report['summary']), flush=True)
    if args.emit_report:
        print('GTI_COMPARATIVE_REPORT_ZLIB_BASE64 ' + base64.b64encode(zlib.compress(json.dumps(report).encode())).decode(), flush=True)
    return 0
if __name__ == '__main__':
    sys.exit(main())
