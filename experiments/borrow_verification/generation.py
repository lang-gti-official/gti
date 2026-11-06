"""Reproducible finite grammars and structured model programs.

Enumeration is exhaustive only for the named linear grammar and length bound;
core_examples covers structured constructs, not an exhaustive structured grammar.
"""
from __future__ import annotations
import copy
import itertools
import json
import random


def program(body, functions=None):
    out = {'schema': 1, 'body': body}
    if functions:
        out['functions'] = functions
    # JSON programs have tree semantics, not Python object-identity semantics.
    # itertools.product may repeat the same alphabet dict; detach every occurrence.
    return json.loads(json.dumps(out))


def alloc(name='o', value=1):
    return {'op': 'alloc', 'name': name, 'fields': {'value': value}}


def borrow(name='r', target='o', mode='shared'):
    return {'op': 'borrow', 'name': name, 'target': target, 'mode': mode}


def read(target='r'):
    return {'op': 'read', 'target': target}


def end(name='r'):
    return {'op': 'end', 'name': name}


def core_examples():
    """Named expected programs. Outcomes are hand-derived, not model queries."""
    identity = {'params': [{'name': 'p', 'mode': 'shared'}],
                'body': [{'op': 'return', 'target': 'p'}]}
    mut_identity = {'params': [{'name': 'p', 'mode': 'mut'}],
                    'body': [{'op': 'return', 'target': 'p'}]}
    consume = {'params': [{'name': 'p', 'mode': 'shared'}], 'body': [read('p')]}
    examples = {
        'shared_read': program([alloc(), borrow(), read(), end()]),
        'mutable_write': program([alloc(), borrow(mode='mut'),
            {'op': 'write', 'target': 'r', 'value': 8}, read(), end()]),
        'overlapping_mutable': program([alloc(), borrow(mode='mut'), borrow('s'), read(), read('s')]),
        'shared_write': program([alloc(), borrow(), {'op': 'write', 'target': 'r', 'value': 8}]),
        'disjoint_fields': program([{'op': 'alloc', 'name': 'o', 'fields': {'x': 2, 'y': 3}},
            borrow('a', 'o.x', 'mut'), borrow('b', 'o.y', 'mut'), read('a'), read('b'), end('b'), end('a')]),
        'reborrow_restore': program([alloc(), borrow(mode='mut'), borrow('s', 'r', 'mut'),
            {'op': 'write', 'target': 's', 'value': 4}, read('s'), end('s'), read(), end()]),
        'parent_suspended': program([alloc(), borrow(mode='mut'), borrow('s', 'r', 'mut'), read(), read('s')]),
        'child_before_parent': program([alloc(), borrow(mode='mut'), borrow('s', 'r'), end(), end('s')]),
        'borrowed_drop': program([alloc(), borrow(), {'op': 'drop', 'target': 'o'}, read()]),
        'move_then_read': program([alloc(), {'op': 'move', 'source': 'o', 'name': 'm'}, read('m')]),
        'whole_move_reinitialize': program([alloc(), {'op': 'move', 'source': 'o', 'name': 'm'},
            {'op': 'init', 'target': 'o', 'value': 9}, read('o'), read('m')]),
        'use_after_move': program([alloc(), {'op': 'move', 'source': 'o', 'name': 'm'}, read('o')]),
        'partial_move_reinitialize': program([{'op': 'alloc', 'name': 'o', 'fields': {'x': 5, 'y': 7}},
            {'op': 'move', 'source': 'o.x', 'name': 'm'}, {'op': 'init', 'target': 'o.x', 'value': 9}, read('o')]),
        'partial_move_read': program([{'op': 'alloc', 'name': 'o', 'fields': {'x': 5, 'y': 7}},
            {'op': 'move', 'source': 'o.x', 'name': 'm'}, read('o')]),
        'branch_owner_dead': program([alloc(), {'op': 'if', 'condition': 'choice',
            'then': [{'op': 'drop', 'target': 'o'}], 'else': []}, read('o')]),
        'branch_reinitialize': program([alloc(), {'op': 'if', 'condition': 'choice',
            'then': [{'op': 'write', 'target': 'o', 'value': 3}],
            'else': [{'op': 'write', 'target': 'o', 'value': 4}]}, read('o')]),
        'loop_borrow_end': program([alloc(), {'op': 'loop', 'condition': 'count', 'iterations': 3,
            'body': [borrow(), read(), end()]}, read('o')]),
        'loop_repeated_move': program([alloc(), {'op': 'loop', 'condition': 'count', 'iterations': 2,
            'body': [{'op': 'move', 'source': 'o', 'name': 'm'}]}]),
        'returned_reference': program([alloc(), {'op': 'call', 'function': 'identity', 'args': ['o'], 'name': 'r'},
            read(), end()], {'identity': identity}),
        'returned_mutable_reference': program([alloc(), {'op': 'call', 'function': 'identity', 'args': ['o'], 'name': 'r'},
            {'op': 'write', 'target': 'r', 'value': 6}, read(), end()], {'identity': mut_identity}),
        'return_through_cleanup': program([alloc(), {'op': 'call', 'function': 'relay', 'args': ['o'], 'name': 'r'}, read(), end()],
            {'relay': {'params': [{'name': 'p', 'mode': 'shared'}], 'body': [borrow('local', 'p'),
                {'op': 'scope', 'body': [{'op': 'return', 'target': 'local'}], 'cleanup': [end('local')]}]}}),
        'return_cleanup_conflict': program([alloc(), {'op': 'call', 'function': 'relay', 'args': ['o'], 'name': 'r'}],
            {'relay': {'params': [{'name': 'p', 'mode': 'mut'}], 'body': [borrow('local', 'p'),
                {'op': 'scope', 'body': [{'op': 'return', 'target': 'local'}],
                 'cleanup': [end('local'), {'op': 'write', 'target': 'p', 'value': 9}]}]}}),
        'callee_local_escape': program([{'op': 'call', 'function': 'bad', 'args': [], 'name': 'r'}],
            {'bad': {'params': [], 'body': [alloc(), {'op': 'return', 'target': 'o'}]}}),
        'temporary_call': program([{'op': 'call', 'function': 'consume', 'args': [42]}], {'consume': consume}),
        'temporary_return': program([{'op': 'call', 'function': 'identity', 'args': [42], 'name': 'r'}],
            {'identity': identity}),
        'temporary_mutable': program([{'op': 'call', 'function': 'identity', 'args': [42], 'name': 'r'}],
            {'identity': mut_identity}),
        'normal_cleanup': program([alloc(), borrow(), {'op': 'scope', 'body': [read()],
            'cleanup': [end(), {'op': 'drop', 'target': 'o'}]}]),
        'failure_cleanup': program([alloc(), borrow(), {'op': 'scope', 'body': [read(), {'op': 'fail'}],
            'cleanup': [end(), {'op': 'drop', 'target': 'o'}]}, {'op': 'write', 'target': 'o', 'value': 2}]),
        'failure_cleanup_wrong_order': program([alloc(), borrow(), {'op': 'scope', 'body': [{'op': 'fail'}],
            'cleanup': [{'op': 'drop', 'target': 'o'}, end()]}]),
        'allocation_reuse': program([alloc(), borrow(), read(), end(), {'op': 'drop', 'target': 'o'},
            alloc(value=7), borrow(), read(), end()]),
        'freed_reference': program([alloc(), borrow(), {'op': 'drop', 'target': 'o'}, alloc(value=7), read()]),
        'unknown_call': program([{'op': 'call', 'function': 'unknown', 'args': []}]),
        'recursive_call': program([{'op': 'call', 'function': 'f', 'args': []}],
            {'f': {'params': [], 'body': [{'op': 'call', 'function': 'f', 'args': []}]}}),
        'raw_pointer': program([{'op': 'raw_load', 'target': 'o'}]),
    }
    return examples


EXPECTED = {
    'shared_read': 'accepted', 'mutable_write': 'accepted',
    'overlapping_mutable': 'rejected', 'shared_write': 'rejected',
    'disjoint_fields': 'accepted', 'reborrow_restore': 'accepted',
    'parent_suspended': 'rejected', 'child_before_parent': 'rejected',
    'borrowed_drop': 'rejected', 'move_then_read': 'accepted',
    'whole_move_reinitialize': 'accepted', 'use_after_move': 'rejected', 'partial_move_reinitialize': 'accepted',
    'partial_move_read': 'rejected', 'branch_owner_dead': 'rejected',
    'branch_reinitialize': 'accepted', 'loop_borrow_end': 'accepted',
    'loop_repeated_move': 'rejected', 'returned_reference': 'accepted',
    'returned_mutable_reference': 'accepted', 'return_through_cleanup': 'accepted',
    'return_cleanup_conflict': 'rejected', 'callee_local_escape': 'rejected',
    'temporary_call': 'accepted', 'temporary_return': 'rejected',
    'temporary_mutable': 'rejected', 'normal_cleanup': 'accepted',
    'failure_cleanup': 'accepted', 'failure_cleanup_wrong_order': 'rejected',
    'allocation_reuse': 'accepted', 'freed_reference': 'rejected',
    'unknown_call': 'unsupported', 'recursive_call': 'unsupported', 'raw_pointer': 'unsupported',
}

# Fixed, public finite alphabet. All products are grammar-well-formed but can
# violate name/lifetime/permission rules; those invalid cases are intentional.
LINEAR_ALPHABET = (
    borrow(), borrow(mode='mut'), borrow('s', 'r'), end(), read('o'), read(),
    {'op': 'write', 'target': 'o', 'value': 2}, {'op': 'drop', 'target': 'o'},
    {'op': 'move', 'source': 'o', 'name': 'm'}, {'op': 'init', 'target': 'o.value', 'value': 3},
)


def enumerate_programs(max_length=3):
    """All suffixes of length 0..max_length after one allocation.

    Exact count is sum(10**n for n in range(max_length+1)). No stochastic
    filtering or validity pruning silently removes part of this grammar.
    """
    if not isinstance(max_length, int) or max_length < 0:
        raise ValueError('max_length must be nonnegative')
    for length in range(max_length + 1):
        for suffix in itertools.product(LINEAR_ALPHABET, repeat=length):
            yield program([alloc(), *copy.deepcopy(suffix)])


def seeded_programs(seed, count):
    """Seeded combinations of hand-derived structured programs and mutations.

    This is sampled structured testing, not exhaustive coverage of the core.
    """
    if count < 0:
        raise ValueError('count must be nonnegative')
    rng = random.Random(seed)
    examples = list(core_examples().values())
    for _ in range(count):
        result = copy.deepcopy(rng.choice(examples))
        choice = rng.randrange(5)
        if choice == 0:
            result['body'] = [{'op': 'if', 'condition': 'choice', 'then': result['body'], 'else': []}]
        elif choice == 1:
            result['body'] = [{'op': 'loop', 'condition': 'count', 'iterations': rng.randrange(4),
                               'body': result['body']}]
        elif choice == 2 and result['body']:
            del result['body'][rng.randrange(len(result['body']))]
        elif choice == 3:
            result['body'].insert(rng.randrange(len(result['body']) + 1), copy.deepcopy(rng.choice(LINEAR_ALPHABET)))
        yield result


def alpha_rename(program_value, suffix='_renamed'):
    """Rename local roots/formals/function names, preserving field projections."""
    # Detach repeated dict/list identities even for a caller-created Python DAG.
    # Renaming each syntax occurrence must never rename a shared node twice.
    result = json.loads(json.dumps(program_value))
    def target(text):
        if not isinstance(text, str):
            return text
        root, dot, path = text.partition('.')
        return root + suffix + (dot + path if dot else '')
    def body(nodes):
        for node in nodes:
            for key in ('name', 'target', 'source', 'function'):
                if key in node:
                    node[key] = target(node[key])
            if 'args' in node:
                node['args'] = [target(a) for a in node['args']]
            for key in ('body', 'then', 'else', 'cleanup'):
                if key in node:
                    body(node[key])
    body(result['body'])
    functions = {}
    for name, fn in result.get('functions', {}).items():
        for param in fn.get('params', []):
            param['name'] += suffix
        body(fn.get('body', []))
        functions[name + suffix] = fn
    if functions:
        result['functions'] = functions
    return result


# Separate finite structured grammar: two record owners, two disjoint fields,
# and every pair of bounded branch bodies / every bounded loop body. This is
# exhaustive within this exact alphabet, not every schema-1 program.
STRUCTURED_ALPHABET = (
    borrow('r', 'o.x', 'shared'),
    borrow('r', 'o.x', 'mut'),
    borrow('r', 'p.x', 'mut'),
    borrow('s', 'o.y', 'mut'),
    {'op': 'write', 'target': 'o.x', 'value': 3},
    read('r'), end('r'), {'op': 'drop', 'target': 'p'},
)


def structured_count(max_body_length=1):
    if not isinstance(max_body_length, int) or not 0 <= max_body_length <= 2:
        raise ValueError('max_body_length must be 0..2 for the experimental structured grammar')
    bodies = sum(len(STRUCTURED_ALPHABET) ** n for n in range(max_body_length + 1))
    return bodies * bodies + bodies


def enumerate_structured(max_body_length=1):
    """Exactly S*S conditionals and S loops, S=sum(8**i, i=0..bound).

    Bound 0 produces 2 programs, bound 1 produces 90, bound 2 produces 5,402.
    Each program starts with two initialized records and reads both owners
    after the control construct. Concrete schedules come from execute inputs;
    static checking explores unknown branches and loop fixed points.
    """
    structured_count(max_body_length)  # enforce public resource bound
    bodies = [list(nodes) for length in range(max_body_length + 1)
              for nodes in itertools.product(STRUCTURED_ALPHABET, repeat=length)]
    prefix = [{'op': 'alloc', 'name': 'o', 'fields': {'x': 1, 'y': 2}},
              {'op': 'alloc', 'name': 'p', 'fields': {'x': 3, 'y': 4}}]
    suffix = [read('o.x'), read('p.x')]
    for then, otherwise in itertools.product(bodies, repeat=2):
        yield program(copy.deepcopy(prefix + [{'op': 'if', 'condition': 'choice',
                       'then': then, 'else': otherwise}] + suffix))
    for body in bodies:
        yield program(copy.deepcopy(prefix + [{'op': 'loop', 'condition': 'count',
                                               'body': body}] + suffix))


def compiler_core_examples():
    """Deterministic end-to-end witnesses, independent of compiler outcomes.

    Each has a source-model program and structural adapter options. A caller
    must independently check/execute the model and require both MIR stages and
    native execution; mere membership does not establish coverage.
    """
    examples=core_examples()
    witnesses={}
    coverage={
        'shared_read':['shared-borrow'],
        'mutable_write':['mutable-borrow'],
        'disjoint_fields':['record','named-field','disjoint-field-borrow'],
        'reborrow_restore':['reborrow','parent-restoration'],
        'move_then_read':['move'],
        'whole_move_reinitialize':['whole-reinitialize'],
        'partial_move_reinitialize':['partial-move','field-reinitialize'],
        'branch_reinitialize':['conditional'],
        'loop_borrow_end':['loop'],
        'returned_reference':['direct-call','reference-result'],
        'returned_mutable_reference':['mutable-reference-result'],
        'return_through_cleanup':['return-reborrow','return-cleanup'],
        'temporary_call':['temporary-read-argument','temporary-cleanup'],
        'normal_cleanup':['normal-cleanup','owned-allocation','free'],
        'failure_cleanup':['defined-failure','failure-cleanup','owned-allocation','free'],
        'allocation_reuse':['allocation-generation','free','reallocate'],
    }
    # This end-to-end witness follows the published finite input count, so the
    # four inputs exercise both zero-trip and repeated execution. The independent
    # named-model example retains its separate explicit three-iteration schedule.
    examples['loop_borrow_end']['body'][1].pop('iterations')
    for name,features in coverage.items():
        witnesses[name]={'program':examples[name], 'features':features,
            'heap_owners':['o'] if name in ('normal_cleanup','failure_cleanup','allocation_reuse') else []}
    return witnesses
