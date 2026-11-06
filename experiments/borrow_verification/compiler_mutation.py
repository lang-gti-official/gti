#!/usr/bin/env python3
"""Disable one production conflict reporter in an isolated source/object copy.

Never edits production sources, archives, compiler executables or installed tools.
Demonstrates that stage-specific tests detect a missing production check.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time

from build_probe import link_probe
from corpus import cases
from runner import ROOT, analyze, digest

ANCHOR = '''auto SemanticVisitor::Impl::reportBorrowConflict(
    const Token &token, const std::vector<SemanticLoanId> &loans,
    std::string message, bool rawAudit) -> void {'''

REQUIRED_WITNESSES = ('shared_write_conflict', 'mutable_alias_conflict', 'move_while_borrowed')


def mandatory_witnesses_killed(records):
    killed = {c['name'] for c in records if c['killed_by_semantic_stage_expectation']}
    return all(name in killed for name in REQUIRED_WITNESSES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--probe-dir', type=lambda p: Path(p).resolve(), required=True)
    parser.add_argument('--out', type=lambda p: Path(p).resolve(), required=True)
    args = parser.parse_args()
    args.timeout = 45
    args.probe = args.probe_dir / 'borrow_probe'
    if (args.out / 'report.json').exists():
        parser.error('Choose a fresh output directory; evidence is never overwritten')
    args.out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'src/compiler/semantic_ownership_analysis.cpp'
    original = source.read_text()
    if original.count(ANCHOR) != 1:
        raise RuntimeError('Mutation anchor changed; review before adapting')
    cache = (args.build / 'CMakeCache.txt').read_text()
    if not any('CMAKE_BUILD_TYPE:STRING=' + p in cache for p in ('Release', 'RelWithDebInfo', 'MinSizeRel')):
        raise RuntimeError('Optimized compiler required')
    if f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}' not in cache:
        raise RuntimeError('Mutation source and compiler build must use the same checkout')
    mutant = args.out / 'semantic_ownership_analysis.cpp'
    mutant.write_text(original.replace(ANCHOR, ANCHOR + '\n  return; // laboratory counterfactual only\n'))
    entries = json.loads((args.build / 'compile_commands.json').read_text())
    entry = next(e for e in entries if Path(e['file']) == source)
    cmd = list(entry.get('arguments') or shlex.split(entry['command']))
    obj = args.out / 'semantic_ownership_mutant.o'
    cmd[cmd.index('-o') + 1] = str(obj)
    cmd[cmd.index('-c') + 1] = str(mutant)
    cmd.extend(['-I' + str(source.parent)])
    start = time.perf_counter()
    subprocess.run(cmd, cwd=entry['directory'], check=True)
    replacement = args.out / 'mutant_probe'
    link_probe(args.build, args.probe_dir / 'borrow_probe.o', replacement, [obj])
    build_seconds = time.perf_counter() - start
    result = {'mutation': 'disable-reportBorrowConflict', 'source_sha256': digest(source),
              'mutant_source_sha256': digest(mutant), 'build_seconds': build_seconds, 'cases': []}
    for case in cases():
        if case.name not in ('shared_write_conflict', 'mutable_alias_conflict', 'move_while_borrowed', 'call_overlap'):
            continue
        baseline = analyze(case.source, args, args.out / case.name / 'baseline')
        args.probe = replacement
        changed = analyze(case.source, args, args.out / case.name / 'mutant')
        args.probe = args.probe_dir / 'borrow_probe'
        killed = (baseline.get('probe', {}).get('semantic_valid') is False
                  and changed.get('probe', {}).get('semantic_valid') is True)
        result['cases'].append({'name': case.name, 'baseline': baseline, 'mutant': changed,
                                'killed_by_semantic_stage_expectation': killed})
    result['required_witnesses'] = list(REQUIRED_WITNESSES)
    result['killed'] = mandatory_witnesses_killed(result['cases'])
    result['production_source_unchanged'] = source.read_text() == original
    (args.out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'killed': result['killed'], 'build_seconds': build_seconds, 'report': str(args.out / 'report.json')}))
    return 0 if result['killed'] and result['production_source_unchanged'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
