"""Independent-model campaign, source comparisons, and retained counterexamples."""
from __future__ import annotations
from collections import Counter
from itertools import combinations
import json
import time

from generation import EXPECTED, alpha_rename, core_examples, enumerate_programs, enumerate_structured, seeded_programs, structured_count
from model import check, execute
from runner import ROOT, analyze, native, shrink
from source_bridge import Unsupported, render

INPUTS = ({'choice': False, 'count': 0}, {'choice': True, 'count': 0},
          {'choice': False, 'count': 2}, {'choice': True, 'count': 2})


def operations(program):
    """Syntactic presence only; never described as executed branch coverage."""
    found = set()
    def walk(nodes):
        for node in nodes:
            found.add(node['op'])
            for key in ('body', 'then', 'else', 'cleanup'):
                walk(node.get(key, []))
    walk(program['body'])
    for declaration in program.get('functions', {}).values():
        walk(declaration.get('body', []))
    return sorted(found)


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


REDUCIBLE_COMPILER_FINDINGS = frozenset({
    'accepted-model-violation', 'conservative-rejection-or-model-error',
    'lowering-rejection', 'internal-error',
})


def violation_rule(result):
    return (result.violation or '').split(':')[0]


def reduce_compiler_finding(program, record, args, directory, budget):
    """Use the campaign's actual eligibility and invariant-preserving predicate.

    Native-only disagreements require execution-aware reduction and are not
    silently reduced using this semantic/lowering-only path. Unsupported shapes
    and a different model rule can never replace the original counterexample.
    """
    classification = record['classification']
    if budget <= 0 or classification not in REDUCIBLE_COMPILER_FINDINGS:
        return None
    original = check(program)
    signature = original.status, violation_rule(original)
    compiler_attempts = [0]
    def preserves(trial):
        candidate = check(trial)
        if (candidate.status, violation_rule(candidate)) != signature:
            return False
        compiler_attempts[0] += 1
        observed = compiler_comparison(trial, args,
            directory / ('reduce-' + str(compiler_attempts[0])), runtime=False)
        return observed['classification'] == classification
    return shrink(program, preserves, budget=budget)


def reduce_model_finding(program, failure, budget=100, expected=None):
    """Preserve the disagreement class and concrete violated rules under faults."""
    def signature(trial):
        static, concrete, classification = model_comparison(trial)
        if expected is not None and static.status != expected:
            classification = 'hand-derived-expectation-mismatch'
        violated = tuple(sorted({violation_rule(r) for r in concrete if r.status == 'rejected'}))
        return classification, static.status, violated
    original = signature(program)
    if original[0] != failure:
        return program, 0
    return shrink(program, lambda p: signature(p) == original, budget=budget)


def phase2(args):
    if args.max_length < 0 or args.max_length > 4 or args.count > 100000:
        raise ValueError('Campaign bounds: max-length 0..4, seeded count <=100000')
    counts, compiler_counts = Counter(), Counter()
    operation_counts, interaction_counts, execution_counts = Counter(), Counter(), Counter()
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
            vocabulary = operations(program)
            operation_counts.update(vocabulary)
            interaction_counts.update('+'.join(pair) for pair in combinations(vocabulary, 2))
            execution_counts.update(r.status for r in concrete)
            if expected is not None and expected != static.status:
                failure = 'hand-derived-expectation-mismatch'
            record = {'name': name, 'program': program, 'static': static.to_dict(),
                      'concrete': [r.to_dict() for r in concrete], 'failure': failure}
            if failure:
                failures += 1
                minimized, attempts = reduce_model_finding(program, failure, budget=100, expected=expected)
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
                result['replay'] = ['python3', str(ROOT / 'experiments/borrow_verification/replay.py'),
                                    '--program', str(directory / 'model.json'),
                                    '--build', str(args.build), '--probe', str(args.probe),
                                    '--out', str(directory / 'replayed')]
                reduction = reduce_compiler_finding(program, result, args, directory, args.reduce_compiler)
                if reduction is not None:
                    reduced, attempts_count = reduction
                    (directory / 'reduced-model.json').write_text(json.dumps(reduced, indent=2) + '\n')
                    result['reduction_attempts'] = attempts_count
                    result['reduced_replay'] = [str(directory / 'reduced-model.json') if a == str(directory / 'model.json')
                                                else str(directory / 'reduced-replayed') if a == str(directory / 'replayed') else a
                                                for a in result['replay']]
                (directory / 'finding.json').write_text(json.dumps(result, indent=2) + '\n')
                print('Candidate finding ' + name + ': ' + result['classification'], flush=True)
            compiler_records.append(result)
    return {'processed': generated, 'expectation_matches': generated - sum(bool(r['failure']) for r in records),
            'model_safety_checked': sum(r['static']['status'] == 'accepted' and all(x['status'] == 'accepted' for x in r['concrete']) for r in records),
            'failures': failures, 'model_counts': dict(counts), 'model_seconds': model_seconds,
            'concrete_execution_counts': dict(execution_counts),
            'syntactic_operation_program_counts': dict(operation_counts),
            'syntactic_cooccurrence_program_counts': dict(interaction_counts),
            'named_case_operations': {name: operations(p) for name, p in named.items()},
            'linear_grammar_alphabet': 10, 'linear_max_length': args.max_length,
            'linear_exact_count': sum(10 ** n for n in range(args.max_length + 1)),
            'structured_max_body_length': args.structured_length, 'structured_exact_count': structured_count(args.structured_length),
            'structured_seed': args.seed, 'structured_count': args.count,
            'concrete_inputs': INPUTS, 'compiler_requested': bool(args.build),
            'native_sampled_inputs': [INPUTS[-1]] if args.build and not args.no_native else [],
            'compiler_samples': sampled, 'generated_source_unsupported_before_samples': eligible,
            'compiler_counts': dict(compiler_counts), 'compiler_records': compiler_records,
            'assurance': 'experimental bounded evidence; not a proof or complete RFC phase acceptance'}
