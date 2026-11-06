#!/usr/bin/env python3
"""Replay an original or reduced JSON model and optional compiler comparison."""
import argparse
import json
from pathlib import Path
import time

from campaign import INPUTS, compiler_comparison
from model import check, execute
from runner import provenance, resource_metrics


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--program', type=Path, required=True)
    p.add_argument('--build', type=lambda x: Path(x).resolve())
    p.add_argument('--probe', type=lambda x: Path(x).resolve())
    p.add_argument('--out', type=lambda x: Path(x).resolve(), required=True)
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--no-native', action='store_true')
    args = p.parse_args()
    if bool(args.build) != bool(args.probe):
        p.error('--build and --probe must be supplied together')
    if args.timeout <= 0:
        p.error('timeout must be positive')
    if (args.out / 'report.json').exists():
        p.error('Choose a fresh evidence directory')
    program = json.loads(args.program.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    result = {'identity': provenance(args), 'program': program,
              'static': check(program).to_dict(),
              'executions': [{'inputs': i, 'result': execute(program, inputs=i).to_dict()} for i in INPUTS]}
    if args.build:
        result['compiler'] = compiler_comparison(program, args, args.out / 'compiler')
    result['seconds'] = time.perf_counter() - start
    result['resources'] = resource_metrics(args.out)
    (args.out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    classification = result.get('compiler', {}).get('classification', result['static']['status'])
    print(json.dumps({'classification': classification, 'report': str(args.out / 'report.json')}))
    # A replay that reproduces a disagreement remains nonzero. Unsupported and
    # timeout outcomes also remain nonzero, not successfully checked programs.
    return 0 if classification in ('agreement-accept', 'agreement-reject', 'accepted', 'rejected') else 1


if __name__ == '__main__':
    raise SystemExit(main())
