#!/usr/bin/env python3
"""Partition a named-test executable and require complete, same-source evidence."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def inventory(executable):
    result = subprocess.run([str(executable), '--list'], text=True, capture_output=True,
                            check=True, timeout=60)
    names = result.stdout.splitlines()
    if not names or any(not n.strip() or n != n.strip() for n in names) or len(set(names)) != len(names):
        raise ValueError('empty, malformed or duplicate named-test inventory')
    return sorted(names)


def selection(names, index, count):
    if count < 1 or not 1 <= index <= count or len(names) < count:
        raise ValueError('invalid or empty shard')
    return names[index - 1::count]


def save(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)


def run(executable, index, count, source, report_path, timeout):
    if timeout <= 0 or not source:
        raise ValueError('positive timeout and source identity required')
    names = inventory(executable)
    selected = selection(names, index, count)
    report = dict(source=source, inventory=names, index=index, count=count,
                  selected=selected, results=[])
    save(report_path, report)
    deadline = time.monotonic() + timeout
    for name in selected:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        print(f'RUN {name}', flush=True)
        started = time.monotonic()
        process = subprocess.Popen([str(executable), '--test', name], start_new_session=True)
        try:
            code = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            # Kill descendants too: a timed-out fixture may have spawned a compiler.
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            code = 'timeout'
        report['results'].append(dict(name=name, code=code, seconds=time.monotonic() - started))
        save(report_path, report)
        print(f'END {name}: {code}', flush=True)
    return 0 if len(report['results']) == len(selected) and all(r['code'] == 0 for r in report['results']) else 1


def verify(paths, source, count):
    reports = [json.loads(p.read_text()) for p in paths]
    if len(reports) != count or {r['index'] for r in reports} != set(range(1, count + 1)):
        raise ValueError('missing or duplicate shard report')
    names = reports[0]['inventory']
    if not names or names != sorted(set(names)) or any(not n.strip() or n != n.strip() for n in names):
        raise ValueError('invalid inventory')
    for r in reports:
        expected = selection(names, r['index'], count)
        if r['source'] != source or r['count'] != count or r['inventory'] != names:
            raise ValueError('source, count or inventory mismatch')
        if r['selected'] != expected or [x['name'] for x in r['results']] != expected:
            raise ValueError('missing, duplicate or misassigned named case')
        if any(type(x['code']) is not int or x['code'] != 0 for x in r['results']):
            raise ValueError('failed or timed-out named case')
    print(f'All {len(names)} named cases passed exactly once across {count} shards')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    runner = sub.add_parser('run')
    runner.add_argument('--executable', type=Path, required=True)
    runner.add_argument('--index', type=int, required=True)
    runner.add_argument('--count', type=int, required=True)
    runner.add_argument('--source', required=True)
    runner.add_argument('--report', type=Path, required=True)
    runner.add_argument('--timeout', type=float, required=True)
    checker = sub.add_parser('verify')
    checker.add_argument('--reports', type=Path, required=True)
    checker.add_argument('--source', required=True)
    checker.add_argument('--count', type=int, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'run':
            return run(args.executable.resolve(), args.index, args.count, args.source, args.report, args.timeout)
        verify(sorted(args.reports.rglob('lifecycle-*.json')), args.source, args.count)
        return 0
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
