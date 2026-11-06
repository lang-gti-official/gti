#!/usr/bin/env python3
"""Optional Lean feasibility check, including deliberately invalid rule edits."""
import argparse
import json
from pathlib import Path
import time

from runner import command, digest, resource_metrics, text

VERSION = '4.35.0'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lean', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--out', type=lambda x: Path(x).resolve(), required=True)
    args = p.parse_args()
    if (args.out / 'report.json').exists():
        p.error('Choose a fresh evidence directory')
    args.out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_suffix('.lean')
    started = time.perf_counter()
    report = {'source_sha256': digest(source), 'lean_sha256': digest(args.lean),
              'scope': 'four feasibility lemmas only; no executable-model correspondence or core soundness theorem',
              'checks': [], 'passed': False}
    try:
        version = command([args.lean, '--version'], args.out, 'version')
        report['version'] = text(version['stdout']).strip()
        assert version['exit_code'] == 0 and 'version ' + VERSION + ',' in report['version'], report['version']
        baseline = command([args.lean, source], args.out, 'baseline')
        report['checks'].append(baseline)
        assert baseline['status'] == 'completed' and baseline['exit_code'] == 0, baseline
        output = text(baseline['stdout'])
        assert output.count('does not depend on any axioms') == 4, output
        original = source.read_text()
        mutations = {
            'uninitialized-read': ('s.live p ∧ s.initialized p', 's.live p'),
            'join-loses-predecessor': ('a.initialized p ∧ b.initialized p', 'a.initialized p ∨ b.initialized p'),
        }
        for name, (before, after) in mutations.items():
            assert original.count(before) == 1, name
            changed = args.out / (name + '.lean')
            changed.write_text(original.replace(before, after))
            run = command([args.lean, changed], args.out, name)
            report['checks'].append(run)
            output = text(run['stdout']) + text(run['stderr'])
            assert run['status'] == 'completed' and run['exit_code'] != 0, run
            assert 'error:' in output and 'unexpected token' not in output, output
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    report['seconds'] = time.perf_counter() - started
    report['resources'] = resource_metrics(args.out)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'report': str(args.out / 'report.json')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
