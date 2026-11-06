"""Independent-model campaign, source comparisons, and retained counterexamples."""
from __future__ import annotations
from collections import Counter
import json
import time

from generation import EXPECTED, alpha_rename, core_examples, enumerate_programs, enumerate_structured, seeded_programs, structured_count
from model import check, execute
from runner import analyze, native, shrink
from source_bridge import Unsupported, render

INPUTS = ({'choice': False, 'count': 0}, {'choice': True, 'count': 0},
          {'choice': False, 'count': 2}, {'choice': True, 'count': 2})


def model_comparison(program):
    static = check(program)
    concrete = [execute(program, inputs=i) for i in INPUTS]
    renamed = check(alpha_rename(program))
    failure = None
    if static.status == 'accepted' and any(r.status == 'rejected' for r in concrete):
        failure = 'model-accepted-invalid-execution'
    elif static.status != renamed.status:
        failure = 'alpha-renaming-disagreement'
    return static, concrete, failure


def compiler_comparison(program, args, directory, runtime=True):
    static = check(program)
    if static.status not in ('accepted', 'rejected'):
        return {'classification': 'unsupported-model', 'model': static.to_dict()}
    try:
        source = render(program, INPUTS[-1])
    except Unsupported as error:
        return {'classification': 'unsupported-source', 'reason': str(error)}
    record = analyze(source, args, directory)
    record['model'] = static.to_dict()
    actual = record['classification']
    if actual == 'syntax-error':
        record['classification'] = 'source-adapter-error'
    elif static.status == 'rejected' and actual == 'accepted':
        record['classification'] = 'accepted-model-violation'
    elif static.status == 'accepted' and actual == 'rejected':
        record['classification'] = 'conservative-rejection-or-model-error'
    elif actual == 'rejected':
        # Arbitrary type/name errors do not validate a borrowing rejection.
        codes = {d['code'] for d in record['probe']['diagnostics']}
        record['classification'] = ('agreement-reject' if codes.intersection({'GTI-S2002', 'GTI-S2017', 'GTI-S2018', 'GTI-S2045'})
                                    else 'wrong-diagnostic-or-adapter-error')
    elif actual == 'accepted':
        record['classification'] = 'agreement-accept'
        if runtime and not args.no_native:
            execution = execute(program, inputs=INPUTS[-1])
            if execution.status != 'accepted' or execution.violation == 'defined-failure':
                record['classification'] = 'unsupported-execution'
            else:
                stdout = ''.join(str(n) + '\n' for n in execution.observations)
                record['native'] = native(directory / 'program.gti', args, directory, 0, stdout)
                if not all(r['passed'] for r in record['native']):
                    record['classification'] = 'native-disagreement-or-adapter-error'
    return record


def phase2(args):
    if args.max_length < 0 or args.max_length > 4 or args.count > 100000:
        raise ValueError('Campaign bounds: max-length 0..4, seeded count <=100000')
    counts, compiler_counts = Counter(), Counter()
    records, compiler_records = [], []
    failures, sampled, eligible, generated = 0, 0, 0, 0
    named = core_examples()
    streams = [((name, p, EXPECTED[name]) for name, p in named.items()),
               (('exhaustive-' + str(i), p, None) for i, p in enumerate(enumerate_programs(args.max_length))),
               (('structured-' + str(i), p, None) for i, p in enumerate(enumerate_structured(args.structured_length))),
               (('seeded-' + str(i), p, None) for i, p in enumerate(seeded_programs(args.seed, args.count)))]
    model_start = time.perf_counter()
    for stream in streams:
        for name, program, expected in stream:
            generated += 1
            static, concrete, failure = model_comparison(program)
            counts[static.status] += 1
            if expected is not None and expected != static.status:
                failure = 'hand-derived-expectation-mismatch'
            record = {'name': name, 'program': program, 'static': static.to_dict(),
                      'concrete': [r.to_dict() for r in concrete], 'failure': failure}
            if failure:
                failures += 1
                minimized, attempts = shrink(program, lambda p: model_comparison(p)[2] == failure, budget=100)
                record.update(minimized=minimized, reduction_attempts=attempts)
            records.append(record)
    model_seconds = time.perf_counter() - model_start
    with (args.out / 'model-records.jsonl').open('w') as stream:
        for record in records:
            stream.write(json.dumps(record) + '\n')
    # Named constructs first, then a deterministic generated sample. Unsupported
    # translations are counted, never reported as successful source verification.
    if args.build:
        for record in records:
            name, program = record['name'], record['program']
            if name not in named:
                if sampled >= args.compiler_samples:
                    break
                try:
                    render(program, INPUTS[-1])
                except Unsupported:
                    eligible += 1
                    continue
                sampled += 1
            result = compiler_comparison(program, args, args.out / name)
            result['name'] = name
            compiler_counts[result['classification']] += 1
            if not result['classification'].startswith(('agreement-', 'unsupported-')):
                failures += 1
                directory = args.out / name
                directory.mkdir(parents=True, exist_ok=True)
                (directory / 'model.json').write_text(json.dumps(program, indent=2) + '\n')
                (directory / 'finding.json').write_text(json.dumps(result, indent=2) + '\n')
                if args.reduce_compiler and result['classification'] in ('accepted-model-violation', 'internal-error', 'conservative-rejection-or-model-error'):
                    original_class = result['classification']
                    original_violation = (check(program).violation or '').split(':')[0]
                    attempts = [0]
                    def preserves(trial):
                        if (check(trial).violation or '').split(':')[0] != original_violation:
                            return False
                        attempts[0] += 1
                        reduced = compiler_comparison(trial, args, directory / ('reduce-' + str(attempts[0])), runtime=False)
                        return reduced['classification'] == original_class
                    reduced, attempts_count = shrink(program, preserves, budget=args.reduce_compiler)
                    (directory / 'reduced-model.json').write_text(json.dumps(reduced, indent=2) + '\n')
                    result['reduction_attempts'] = attempts_count
                print('Candidate finding ' + name + ': ' + result['classification'], flush=True)
            compiler_records.append(result)
    return {'processed': generated, 'expectation_matches': generated - sum(bool(r['failure']) for r in records),
            'model_safety_checked': sum(r['static']['status'] == 'accepted' and all(x['status'] == 'accepted' for x in r['concrete']) for r in records),
            'failures': failures, 'model_counts': dict(counts), 'model_seconds': model_seconds,
            'linear_grammar_alphabet': 10, 'linear_max_length': args.max_length,
            'linear_exact_count': sum(10 ** n for n in range(args.max_length + 1)),
            'structured_max_body_length': args.structured_length, 'structured_exact_count': structured_count(args.structured_length),
            'structured_seed': args.seed, 'structured_count': args.count,
            'concrete_inputs': INPUTS, 'compiler_requested': bool(args.build),
            'compiler_samples': sampled, 'generated_source_unsupported_before_samples': eligible,
            'compiler_counts': dict(compiler_counts), 'compiler_records': compiler_records,
            'assurance': 'experimental bounded evidence; not a proof or complete RFC phase acceptance'}
