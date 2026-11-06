#!/usr/bin/env python3
"""Opt-in borrow-verification lab; never imported or registered by CI."""
from __future__ import annotations
import argparse
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]

MUTATION_WITNESSES = (
    ('shared_aliases', 'missing_endpoint', r'remains active|loan.*end'),
    ('shared_aliases', 'wrong_origin', r'conflicts with active'),
    ('shared_aliases', 'permission_upgrade', r'conflicts with active'),
    ('temporary_non_escape', 'temporary_escape', r'non-escaping temporary'),
    ('multiple_temporary_endpoints', 'failure_end_order', r'temporary argument loans.*reverse order'),
    ('shared_aliases', 'missing_initialization', r'place access requires a definitely initialized scalar place'),
)


def mutation_gate(records):
    by_name = {r['name']: r for r in records}
    results = []
    for case, mutation, reason in MUTATION_WITNESSES:
        if case not in by_name:
            results.append({'case': case, 'mutation': mutation, 'status': 'not-selected'})
            continue
        probe = by_name[case].get('probe', {})
        found = next((m for m in probe.get('mutations', []) if m['mutation'] == mutation), None)
        status = 'missing-witness'
        if probe.get('baseline_verified') is not True:
            status = 'baseline-not-verified'
        elif found:
            status = found['status']
            if status == 'rejected':
                status = ('intended-kill' if re.search(reason, '\n'.join(found.get('errors', [])))
                          else 'shape-only-or-unrelated-rejection')
        results.append({'case': case, 'mutation': mutation, 'status': status,
                        'expected_reason': reason, 'observation': found})
    return results


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def resource_metrics(directory):
    """High-water observations, not a sum of simultaneous process memory."""
    own = resource.getrusage(resource.RUSAGE_SELF)
    children = resource.getrusage(resource.RUSAGE_CHILDREN)
    factor = 1 if platform.system() == 'Darwin' else 1024
    return {'runner_peak_rss_bytes': own.ru_maxrss * factor,
            'waited_children_peak_rss_bytes': children.ru_maxrss * factor,
            'runner_cpu_seconds': own.ru_utime + own.ru_stime,
            'waited_children_cpu_seconds': children.ru_utime + children.ru_stime,
            'retained_artifact_bytes': sum(p.stat().st_size for p in directory.rglob('*') if p.is_file()),
            'scope': 'getrusage high-water marks; child maximum is not concurrent total RSS; artifacts exclude final report'}


def command(argv, directory, name, timeout=30):
    """Retain complete logs, bound time and displayed output, kill child group."""
    directory.mkdir(parents=True, exist_ok=True)
    stdout, stderr = directory / (name + '.out'), directory / (name + '.err')
    started = time.perf_counter()
    with stdout.open('wb') as out, stderr.open('wb') as err:
        proc = subprocess.Popen([str(x) for x in argv], stdout=out, stderr=err,
                                start_new_session=True, cwd=ROOT,
                                env={**os.environ, 'GTI_STDLIB_PATH': str(ROOT / 'stdlib'), 'NO_COLOR': '1'})
        try:
            code = proc.wait(timeout=timeout)
            status = 'completed'
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            code, status = None, 'timeout'
    return {'argv': [str(x) for x in argv], 'status': status, 'exit_code': code,
            'seconds': time.perf_counter() - started, 'stdout': str(stdout),
            'stderr': str(stderr), 'stdout_bytes': stdout.stat().st_size,
            'stderr_bytes': stderr.stat().st_size}


def text(path, limit=2_000_000):
    if Path(path).stat().st_size > limit:
        raise ValueError('output limit exceeded: ' + str(path))
    return Path(path).read_text(errors='replace')


def classify_probe(raw):
    if not raw.get('syntax_valid'):
        return 'syntax-error'
    if any(d['code'].startswith('GTI-B000') for d in raw['diagnostics']):
        return 'internal-error'
    if raw['semantic_valid'] != raw['full_semantic_valid']:
        return 'phase-disagreement'
    if raw['accepted']:
        return 'accepted'
    if not raw['semantic_valid']:
        return 'rejected'
    return 'lowering-rejection'


def analyze(source, args, directory, mutations=False):
    path = directory / 'program.gti'
    directory.mkdir(parents=True, exist_ok=True)
    path.write_text(source)
    call = [args.probe, path, ROOT / 'stdlib']
    if mutations:
        call.append('--mutate')
    run = command(call, directory, 'probe', args.timeout)
    if run['status'] != 'completed':
        return {'classification': 'timeout', 'run': run}
    if run['exit_code'] != 0:
        return {'classification': 'probe-crash', 'run': run}
    try:
        raw = json.loads(text(run['stdout']))
        return {'classification': classify_probe(raw), 'probe': raw, 'run': run}
    except (ValueError, KeyError, TypeError) as error:
        return {'classification': 'probe-protocol-error', 'error': str(error), 'run': run}


def native(source_path, args, directory, expected_code, expected_stdout, stderr_contains=''):
    records = []
    for mode in ('O0', 'O3'):
        output = directory / ('program-' + mode)
        compile_run = command([args.build / 'gti', source_path, '--backend', 'llvm', '-' + mode,
                               '--dump-mir-before', directory / (mode + '-before.mir'),
                               '--dump-mir-after', directory / (mode + '-after.mir'), '-o', output],
                              directory, mode + '-compile', args.timeout)
        record = {'mode': mode, 'compile': compile_run, 'passed': False}
        if compile_run['status'] == 'completed' and compile_run['exit_code'] == 0:
            run = command([output], directory, mode + '-run', args.timeout)
            record['execution'] = run
            record['binary_bytes'] = output.stat().st_size
            record['passed'] = (run['status'] == 'completed' and run['exit_code'] == expected_code
                                and text(run['stdout']) == expected_stdout
                                and stderr_contains in text(run['stderr']).lower())
            from mir_adapter import verify_mir_dump
            record['independent_mir'] = {}
            for stage in ('before', 'after'):
                result = verify_mir_dump(text(directory / (mode + '-' + stage + '.mir'), limit=64_000_000))
                record['independent_mir'][stage] = result.to_dict()
                if result.status in ('rejected', 'timeout'):
                    record['passed'] = False
            # Independent admission is reported separately; unsupported MIR is
            # neither proof nor a substitute for the native execution check.
            record['independent_mir_checked'] = all(r['status'] == 'accepted' for r in record['independent_mir'].values())
            if record['independent_mir_checked']:
                before = set(record['independent_mir']['before']['observations'])
                after = set(record['independent_mir']['after']['observations'])
                record['mir_correspondence'] = before == after
                record['mir_native_return_agrees'] = all(n % 256 == expected_code for n in after)
                record['passed'] &= record['mir_correspondence'] and record['mir_native_return_agrees']
        records.append(record)
    return records


def provenance(args):
    result = {'schema': 1, 'platform': platform.platform(), 'python': platform.python_version(),
              'source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
              'lab_hashes': {p.name: digest(p) for p in Path(__file__).parent.glob('*.py')}}
    if args.build:
        cache = (args.build / 'CMakeCache.txt').read_text()
        profile = next((p for p in ('Release', 'RelWithDebInfo', 'MinSizeRel')
                        if 'CMAKE_BUILD_TYPE:STRING=' + p in cache), None)
        if not profile:
            raise ValueError('An optimized compiler is required; pass its --build directory')
        home = next(s.split('=', 1)[1] for s in cache.splitlines() if s.startswith('CMAKE_HOME_DIRECTORY:'))
        if Path(home).resolve() != ROOT:
            raise ValueError('Compiler build must belong to this source checkout')
        result.update(compiler=str(args.build / 'gti'), compiler_sha256=digest(args.build / 'gti'),
                      compiler_profile=profile, probe_sha256=digest(args.probe))
    return result


def phase1(args):
    from corpus import cases
    results = []
    for case in cases():
        if args.case and case.name not in args.case:
            continue
        directory = args.out / case.name
        record = analyze(case.source, args, directory, mutations=case.accepted)
        record.update(name=case.name, rule=case.rule, expected=dataclasses.asdict(case))
        codes = {d['code'] for d in record.get('probe', {}).get('diagnostics', [])}
        record['passed'] = (record['classification'] == ('accepted' if case.accepted else 'rejected')
                            and (case.accepted or bool(codes.intersection(case.codes))))
        if record['passed'] and case.accepted and not args.no_native:
            record['native'] = native(directory / 'program.gti', args, directory,
                                      case.exit_code, case.stdout, case.stderr_contains)
            record['passed'] &= all(r['passed'] for r in record['native'])
        results.append(record)
        print(case.name + ': ' + ('pass' if record['passed'] else record['classification'] + ' MISMATCH'), flush=True)
    if not results:
        raise ValueError('No selected cases')
    gates = mutation_gate(results)
    return {'cases': results, 'passed': sum(r['passed'] for r in results), 'total': len(results),
            'mandatory_mutations': gates,
            'failures': sum(g['status'] not in ('intended-kill', 'not-selected') for g in gates),
            'scope': 'selected-fixtures' if args.case else 'complete-source-corpus',
            'native_requested': not args.no_native}


def shrink(program, predicate, budget=100):
    """Bounded deletion reducer; predicate must preserve the original failure class."""
    import copy
    candidate, attempts = copy.deepcopy(program), 0
    def lists(nodes, path=()):
        yield path, nodes
        for i, node in enumerate(nodes):
            for key in ('then', 'else', 'body', 'cleanup'):
                if isinstance(node.get(key), list):
                    yield from lists(node[key], path + (i, key))
    changed = True
    while changed and attempts < budget:
        changed = False
        for path, nodes in list(lists(candidate['body'])):
            for i in range(len(nodes)):
                trial = copy.deepcopy(candidate)
                target = trial['body']
                for key in path:
                    target = target[key]
                del target[i]
                attempts += 1
                if predicate(trial):
                    candidate, changed = trial, True
                    break
                if attempts >= budget:
                    break
            if changed or attempts >= budget:
                break
    return candidate, attempts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase', choices=('phase1', 'phase2', 'core'))
    p.add_argument('--build', type=lambda x: Path(x).resolve())
    p.add_argument('--probe', type=lambda x: Path(x).resolve())
    p.add_argument('--out', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--case', action='append')
    p.add_argument('--no-native', action='store_true', help='Analysis only; explicitly recorded, not runtime evidence')
    p.add_argument('--seed', type=int, default=20261106)
    p.add_argument('--count', type=int, default=100)
    p.add_argument('--max-length', type=int, default=2)
    p.add_argument('--structured-length', type=int, choices=(0, 1, 2), default=1)
    p.add_argument('--reduce-compiler', type=int, default=0, help='Optional maximum compiler reduction attempts per finding')
    p.add_argument('--compiler-samples', type=int, default=10)
    args = p.parse_args()
    if args.timeout <= 0 or args.count < 0 or args.compiler_samples < 0 or not 0 <= args.reduce_compiler <= 100:
        p.error('Bounds must be nonnegative and timeout positive')
    if args.phase in ('phase1', 'core') and (not args.build or not args.probe):
        p.error('phase1/core requires --build and --probe')
    if args.phase == 'core' and args.no_native:
        p.error('The mandatory core comparison requires native execution')
    if bool(args.build) != bool(args.probe):
        p.error('--build and --probe must be supplied together')
    if (args.out / 'report.json').exists():
        p.error('Refusing to overwrite evidence; select a fresh --out directory')
    args.out.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    report = {'identity': provenance(args), 'phase': args.phase, 'arguments': vars(args).copy()}
    if args.phase == 'phase1':
        report['results'] = phase1(args)
    elif args.phase == 'phase2':
        from campaign import phase2
        report['results'] = phase2(args)
    else:
        from core_campaign import core_campaign
        report['results'] = core_campaign(args)
    report['seconds'] = time.perf_counter() - start
    report['resources'] = resource_metrics(args.out)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2, default=str) + '\n')
    print('Evidence: ' + str(args.out / 'report.json'), flush=True)
    if report['results'].get('failures', 0) or report['results'].get('passed') != report['results'].get('total'):
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
