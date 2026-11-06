"""Strict mandatory-core source -> independent MIR -> LLVM comparisons."""
import json

from campaign import INPUTS
from generation import compiler_core_examples
from mir_adapter import verify_core_mir_dump
from model import check, execute
from runner import analyze, command, text
from source_bridge import Unsupported, render_core


def compare_core(witness, inputs, args, directory):
    program = witness['program']
    static, concrete = check(program), execute(program, inputs=inputs)
    record = {'model': static.to_dict(), 'execution': concrete.to_dict(),
              'inputs': inputs, 'declared_features': witness['features'],
              'heap_owners': witness['heap_owners'], 'passed': False}
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'model.json').write_text(json.dumps(program, indent=2) + '\n')
    if static.status != 'accepted' or concrete.status != 'accepted':
        record['classification'] = 'mandatory-model-witness-failed'
        return record
    try:
        source = render_core(program, concrete.observations, inputs, witness['heap_owners'])
    except Unsupported as error:
        record.update(classification='mandatory-source-unsupported', error=str(error))
        return record
    probe = analyze(source, args, directory)
    record['probe'] = probe
    if probe['classification'] != 'accepted':
        record['classification'] = probe['classification']
        return record
    source_path = directory / 'program.gti'
    failure = concrete.violation == 'defined-failure'
    record['modes'] = []
    for mode in ('O0', 'O3'):
        output = directory / ('program-' + mode)
        call = command([args.build / 'gti', source_path, '--backend', 'llvm', '-' + mode,
                        '--dump-mir-before', directory / (mode + '-before.mir'),
                        '--dump-mir-after', directory / (mode + '-after.mir'), '-o', output],
                       directory, mode + '-compile', args.timeout)
        result = {'mode': mode, 'compile': call, 'passed': False}
        record['modes'].append(result)
        if call['status'] != 'completed' or call['exit_code'] != 0:
            result['classification'] = 'compiler-failure'
            continue
        stages = {}
        for stage in ('before', 'after'):
            checked = verify_core_mir_dump(text(directory / (mode + '-' + stage + '.mir'), limit=64_000_000))
            stages[stage] = {**checked.to_dict(), 'core_evidence': checked.core_evidence}
        result['independent_mir'] = stages
        expected = [] if failure else [0]
        result['mir_agrees'] = all(s['status'] == 'accepted'
                                  and s['observations'] == expected
                                  and (s['violation'] == 'defined-failure') == failure
                                  and s['core_evidence'].get('observations') == concrete.observations
                                  and s['core_evidence'].get('failure_cause') == ('division_by_zero' if failure else None)
                                  for s in stages.values())
        # A declared heap witness must actually exercise allocation and free in
        # both artifacts. Labels alone cannot establish heap coverage.
        if witness['heap_owners']:
            result['mir_agrees'] &= all({'owned-allocation', 'owned-allocation-free'}.issubset(
                s['core_evidence']['features']) for s in stages.values())
        run = command([output], directory, mode + '-run', args.timeout)
        result['execution'] = run
        result['binary_bytes'] = output.stat().st_size
        result['native_agrees'] = (run['status'] == 'completed'
                                  and run['exit_code'] == (70 if failure else 0)
                                  and text(run['stdout']) == '')
        if failure:
            result['native_agrees'] &= 'division_by_zero' in text(run['stderr'])
        else:
            result['native_agrees'] &= text(run['stderr']) == ''
        result['passed'] = bool(result['mir_agrees'] and result['native_agrees'])
        result['classification'] = 'agreement' if result['passed'] else 'core-or-native-disagreement'
    record['passed'] = all(m['passed'] for m in record['modes'])
    record['classification'] = 'agreement' if record['passed'] else 'core-or-native-disagreement'
    return record


def core_campaign(args):
    witnesses = compiler_core_examples()
    if args.case:
        if set(args.case) - set(witnesses):
            raise ValueError('Unknown core witness')
        witnesses = {n: w for n, w in witnesses.items() if n in args.case}
    results = []
    covered = set()
    for name, witness in witnesses.items():
        runs = []
        for i, inputs in enumerate(INPUTS):
            directory = args.out / name / ('input-' + str(i))
            result = compare_core(witness, inputs, args, directory)
            result['name'] = name
            (directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            runs.append(result)
            results.append(result)
            print(name + '/input-' + str(i) + ': ' + result['classification'], flush=True)
        if all(r['passed'] for r in runs):
            covered.update(witness['features'])
    return {'cases': results, 'passed': sum(r['passed'] for r in results), 'total': len(results),
            'failures': sum(not r['passed'] for r in results),
            'scope': 'selected-witnesses' if args.case else 'mandatory-core-witnesses',
            'witness_count': len(witnesses), 'inputs': INPUTS,
            'covered_features': sorted(covered),
            'assurance': 'finite source/MIR/native correspondence for these witnesses, not a soundness proof'}
