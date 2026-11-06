"""Opt-in self-tests. Run directly; intentionally not registered with CI/CTest."""
import copy
import json
import unittest
from generation import (EXPECTED, alpha_rename, core_examples, enumerate_programs,
                        enumerate_structured, structured_count, seeded_programs)
from model import check, execute
from mir_adapter import verify_mir_dump


class ModelTests(unittest.TestCase):
    def test_hand_derived_static_cases(self):
        self.assertEqual(set(EXPECTED), set(core_examples()))
        for name, program in core_examples().items():
            with self.subTest(name=name):
                result = check(program)
                self.assertEqual(EXPECTED[name], result.status, result.to_dict())

    def test_hand_derived_concrete_cases(self):
        for name, program in core_examples().items():
            with self.subTest(name=name):
                result = execute(program, inputs={'choice': True, 'count': 2})
                self.assertEqual(EXPECTED[name], result.status, result.to_dict())

    def test_values(self):
        expected = {'mutable_write': [8], 'disjoint_fields': [2, 3],
                    'reborrow_restore': [4, 4], 'move_then_read': [1],
                    'partial_move_reinitialize': [9, 7], 'temporary_call': [42],
                    'loop_borrow_end': [1, 1, 1, 1], 'allocation_reuse': [1, 7]}
        for name, values in expected.items():
            with self.subTest(name=name):
                self.assertEqual(values, execute(core_examples()[name]).observations)

    def test_static_finds_unexecuted_branch(self):
        p = core_examples()['branch_owner_dead']
        self.assertEqual('rejected', check(p).status)
        self.assertEqual('accepted', execute(p, inputs={'choice': False}).status)

    def test_loop_fixpoint_not_just_supplied_iteration_count(self):
        p = core_examples()['loop_repeated_move']
        p['body'][1]['iterations'] = 0
        self.assertEqual('accepted', execute(p).status)
        self.assertEqual('rejected', check(p).status)

    def test_failure_cleanup_and_unreachable_tail(self):
        p = core_examples()['failure_cleanup']
        result = execute(p)
        self.assertEqual('accepted', result.status)
        self.assertEqual('defined-failure', result.violation)
        self.assertEqual(['end r', 'drop o'], result.trace[-2:])

    def test_model_mutations_are_detected_by_expected_outcomes(self):
        # Kill each intentionally disabled rule with a minimal, specifically
        # designated case. Merely observing some unrelated error is not a kill.
        pairs = {
            'overlap': ('overlapping_mutable', 'borrow-conflict'),
            'owner_liveness': ('borrowed_drop', 'owner-borrowed'),
            'escape': ('callee_local_escape', 'escaping-local'),
            'init': ('partial_move_read', 'uninitialized'),
            'cleanup': ('child_before_parent', 'live-child'),
        }
        for mutation, (name, violation) in pairs.items():
            p = copy.deepcopy(core_examples()[name])
            if mutation == 'owner_liveness':
                p['body'] = p['body'][:-1]  # isolate free's own precondition
            for evaluator in (check, execute):
                with self.subTest(mutation=mutation, evaluator=evaluator.__name__):
                    baseline = evaluator(p)
                    self.assertEqual('rejected', baseline.status)
                    self.assertTrue(baseline.violation.startswith(violation), baseline)
                    mutant = evaluator(p, mutation=mutation)
                    self.assertEqual('accepted', mutant.status, mutant.to_dict())

    def test_epoch_reuse_detects_stale_reference_even_if_free_check_is_broken(self):
        result = execute(core_examples()['freed_reference'], mutation='owner_liveness')
        self.assertEqual('rejected', result.status)
        self.assertTrue(result.violation.startswith('stale-generation'), result)

    def test_budget_is_not_a_pass(self):
        p = core_examples()['shared_read']
        self.assertEqual('timeout', check(p, max_steps=1).status)
        self.assertEqual('timeout', execute(p, max_steps=1).status)
        self.assertEqual('timeout', check(core_examples()['branch_reinitialize'], max_states=0).status)

    def test_unsupported_is_not_a_pass(self):
        self.assertEqual('unsupported', check({'schema': 999, 'body': []}).status)
        self.assertEqual('unsupported', execute({'schema': 1, 'body': [{'op': 'ffi'}]}).status)
        self.assertEqual('unsupported', check({'schema': 1, 'body': []}, mutation='unknown').status)

    def test_exhaustive_linear_grammar(self):
        count = 0
        for p in enumerate_programs(3):
            count += 1
            static, runtime = check(p), execute(p)
            self.assertEqual(static.status, runtime.status, json.dumps(p))
            renamed = alpha_rename(p)
            self.assertEqual(static.status, check(renamed).status, json.dumps(p))
            rerun = execute(renamed)
            self.assertEqual((runtime.status, runtime.observations),
                             (rerun.status, rerun.observations), json.dumps(p))
        self.assertEqual(1111, count)

    def test_seed_replay_and_alpha_invariance(self):
        left = list(seeded_programs(12345, 150))
        self.assertEqual(left, list(seeded_programs(12345, 150)))
        for p in left:
            renamed = alpha_rename(p)
            self.assertEqual(check(p).status, check(renamed).status, json.dumps(p))
            for inputs in ({'choice': False, 'count': 0}, {'choice': True, 'count': 2}):
                a, b = execute(p, inputs=inputs), execute(renamed, inputs=inputs)
                self.assertEqual((a.status, a.observations), (b.status, b.observations), json.dumps(p))
                if check(p).status == 'accepted':
                    self.assertNotEqual('rejected', a.status, (p, a.to_dict()))

    def test_structured_exhaustive_grammar(self):
        for bound, count in ((0, 2), (1, 90), (2, 5402)):
            cases = list(enumerate_structured(bound))
            self.assertEqual(count, structured_count(bound))
            self.assertEqual(count, len(cases))
            self.assertEqual(cases, list(enumerate_structured(bound)))
            for p in cases:
                static = check(p)
                renamed = alpha_rename(p)
                self.assertEqual(static.status, check(renamed).status, json.dumps(p))
                for inputs in ({'choice': False, 'count': 0}, {'choice': True, 'count': 1},
                               {'choice': False, 'count': 2}):
                    concrete = execute(p, inputs=inputs)
                    rerun = execute(renamed, inputs=inputs)
                    self.assertEqual((concrete.status, concrete.observations),
                                     (rerun.status, rerun.observations), json.dumps(p))
                    if static.status == 'accepted':
                        self.assertEqual('accepted', concrete.status, (p, concrete.to_dict()))
                self.assertNotIn(static.status, ('timeout', 'unsupported'), p)
        for bad in (-1, 3):
            with self.assertRaises(ValueError):
                list(enumerate_structured(bad))

    def test_inputs_not_mutated(self):
        for p in core_examples().values():
            before = copy.deepcopy(p)
            check(p)
            execute(p)
            self.assertEqual(before, p)


class AdditionalModelTests(unittest.TestCase):
    def test_alpha_rename_handles_repeated_python_nodes(self):
        read = {'op': 'read', 'target': 'o'}
        body = [{'op': 'alloc', 'name': 'o', 'fields': {'value': 1}}, read, read]
        p = {'schema': 1, 'body': body}
        renamed = alpha_rename(p)
        self.assertEqual('o_renamed', renamed['body'][1]['target'])
        self.assertEqual('o_renamed', renamed['body'][2]['target'])
        self.assertIsNot(renamed['body'][1], renamed['body'][2])
        self.assertEqual('accepted', check(renamed).status)
        self.assertEqual([1, 1], execute(renamed).observations)
        self.assertEqual('o', read['target'])

    def test_generated_programs_have_tree_identity(self):
        for p in list(enumerate_programs(2)) + list(enumerate_structured(1)):
            seen = set()
            def visit(node):
                if isinstance(node, (list, dict)):
                    self.assertNotIn(id(node), seen)
                    seen.add(id(node))
                    for item in (node.values() if isinstance(node, dict) else node):
                        visit(item)
            visit(p)

    def test_result_reference_cannot_be_rebound(self):
        p = core_examples()['returned_reference']
        p['body'] = p['body'][:2] + [copy.deepcopy(p['body'][1])]
        for evaluator in (check, execute):
            result = evaluator(p)
            self.assertEqual('rejected', result.status)
            self.assertTrue(result.violation.startswith('reference-rebinding'))

    def test_user_names_do_not_collide_with_call_temporaries(self):
        p = core_examples()['temporary_call']
        p['body'].insert(0, {'op': 'alloc', 'name': 'temporary_0', 'fields': {'value': 3}})
        for evaluator in (check, execute):
            self.assertEqual('accepted', evaluator(p).status)

    def test_unknown_effect_fields_rejected(self):
        p = core_examples()['shared_read']
        p['body'][0]['unknown_destructor'] = 'writes_owner'
        self.assertEqual('unsupported', check(p).status)
        self.assertEqual('unsupported', execute(p).status)

    def test_unconditional_loop_does_not_invent_exit(self):
        p = {'schema': 1, 'body': [{'op': 'loop', 'condition': True, 'iterations': 0, 'body': []},
                                  {'op': 'read', 'target': 'missing'}]}
        self.assertEqual('accepted', check(p).status)
        # An empty body still consumes loop iterations; no silently invented exit.
        self.assertEqual('timeout', execute(p, max_steps=10).status)


def tiny_dump():
    # Compact schema-conformant fixture, not compiler-generated validity evidence.
    def instruction(i, op, **fields):
        defaults = {'tombstone': 0, 'unsafe-operation': 0, 'raw-memory': 0,
                    'object-allocation': 0, 'result': '-', 'destination': '-',
                    'loan': '-', 'operands': '[]', 'parameters': '[]',
                    'lifecycle': '[]', 'intrinsic': 'none', 'function': '-',
                    'constructor': '-', 'receiver': '-', 'lambda': '-', 'element-cleanup': '-'}
        defaults.update(fields)
        return '    i' + str(i) + ' ' + op + ' ' + ' '.join(str(k)+'='+str(v) for k,v in defaults.items())
    operand = lambda k, v=0, p=0, l=0: f'operand({k};value={v};place={p};loan={l};literal=-;type=type(4;))'
    term = lambda k, rest: f'    term {k} {rest} target-arguments=[] else-arguments=[] success-lifecycle=[]'
    lines = ['mir-v120 valid=1', 'program-initialization units=1 steps=0',
             'function @1 entry=1:0 parameters=[]', 'body entry=bb1',
             '  p1 root=0 binding=1 type=type(4;) projections=[] access=1 initially-available=0',
             '  p2 root=0 binding=2 type=type(21;access=0;) projections=[] access=0 initially-available=0',
             '  p3 root=0 binding=2 type=type(4;) projections=[projection(2;field=0;index=0;constant=-;selection=0)] access=0 initially-available=0',
             '  loan1 kind=0 source=p1 parent=loan0 access=0 additional-dependencies=[] conditions=[] two-phase=0 escapes=0',
             '  bb1 parameters=[]', instruction(1, 'compute', result=1, operation='literal', literal='u64:9'),
             instruction(2, 'initialize', destination=1, operands='['+operand(0,v=1)+']'),
             instruction(3, 'borrow', loan=1, operands='['+operand(5,p=1)+']'),
             instruction(4, 'initialize', destination=2, operands='['+operand(7,p=1,l=1)+']'),
             instruction(5, 'load', result=2, operands='['+operand(3,p=3)+']'),
             instruction(6, 'compute', result=3, operation='add', operands='['+operand(0,v=2)+','+operand(0,v=1)+']'),
             term(4, 'invoke=i6 target=bb2 else=bb3'),
             '  bb2 parameters=[]', instruction(7, 'end-borrow', loan=1),
             term(5, 'value='+operand(0,v=3)),
             '  bb3 parameters=[]', instruction(8, 'end-borrow', loan=1),
             term(6, 'value=-'), 'end-body']
    return '\n'.join(lines)


class MirAdapterTests(unittest.TestCase):
    def test_supported_scalar_dump(self):
        result = verify_mir_dump(tiny_dump())
        self.assertEqual('accepted', result.status, result.to_dict())
        self.assertEqual([18], result.observations)

    def test_mutable_loan_initialization_uses_loan_authority(self):
        text = tiny_dump().replace('parent=loan0 access=0', 'parent=loan0 access=1')
        text = text.replace('operand(5;value=0;place=1;', 'operand(6;value=0;place=1;')
        result = verify_mir_dump(text)
        self.assertEqual('accepted', result.status, result.to_dict())

    def mutable_dump(self):
        text = tiny_dump().replace('parent=loan0 access=0', 'parent=loan0 access=1')
        text = text.replace('operand(5;value=0;place=1;', 'operand(6;value=0;place=1;')
        return text.replace('type(21;access=0;)', 'type(21;access=1;)')

    def test_reference_permission_comes_from_type_and_executed_loan(self):
        text = self.mutable_dump()
        initializer = next(l for l in text.splitlines() if l.startswith('    i2 '))
        assignment = initializer.replace('i2 initialize ', 'i9 assign ').replace('destination=1 ', 'destination=3 ')
        text = text.replace('    i5 load ', assignment + '\n    i5 load ')
        result = verify_mir_dump(text)
        self.assertEqual('accepted', result.status, result.to_dict())
        # A read-only projection is writable through a mutable reference; a
        # forged writable projection cannot upgrade a shared reference.
        shared = text.replace('type(21;access=1;)', 'type(21;access=0;)')
        shared = shared.replace('selection=0)] access=0', 'selection=0)] access=1')
        result = verify_mir_dump(shared)
        self.assertEqual('rejected', result.status)
        self.assertIn('read-only write', result.violation)
        forged = tiny_dump().replace('type(21;access=0;)', 'type(21;access=1;)')
        result = verify_mir_dump(forged)
        self.assertEqual('rejected', result.status)
        self.assertIn('mutable reference from shared loan', result.violation)

    def test_loop_loan_reactivation_requires_reference_reinitialization(self):
        text = self.mutable_dump()
        lines = text.splitlines()
        end = next(l for l in lines if l.startswith('    i7 ')).replace('i7 ', 'i9 ')
        borrow = next(l for l in lines if l.startswith('    i3 ')).replace('i3 ', 'i10 ')
        initialize = next(l for l in lines if l.startswith('    i4 ')).replace('i4 ', 'i11 ')
        # Static loan/binding IDs recur in loops, but each activation is new.
        good = text.replace('    i5 load ', end+'\n'+borrow+'\n'+initialize+'\n    i5 load ')
        result = verify_mir_dump(good)
        self.assertEqual('accepted', result.status, result.to_dict())
        stale = text.replace('    i5 load ', end+'\n'+borrow+'\n    i5 load ')
        result = verify_mir_dump(stale)
        self.assertEqual('rejected', result.status)
        self.assertIn('ended reference used', result.violation)
        active = text.replace('    i5 load ', initialize+'\n    i5 load ')
        result = verify_mir_dump(active)
        self.assertEqual('rejected', result.status)
        self.assertIn('double initialization', result.violation)

    def test_missing_scalar_initialization_detected(self):
        text = '\n'.join(l for l in tiny_dump().splitlines() if not l.startswith('    i2 '))
        result = verify_mir_dump(text)
        self.assertEqual('rejected', result.status)
        self.assertIn('uninitialized scalar read', result.violation)

    def test_valid_flag_is_not_trusted(self):
        # A harmless validity flag change cannot affect our independently checked result.
        self.assertEqual('accepted', verify_mir_dump(tiny_dump().replace('valid=1', 'valid=0')).status)
        broken = '\n'.join(line for line in tiny_dump().splitlines() if not line.startswith('    i3 '))
        result = verify_mir_dump(broken)
        self.assertEqual('rejected', result.status)
        self.assertIn('inactive loan operand', result.violation)

    def test_missing_failure_cleanup_detected(self):
        broken = '\n'.join(line for line in tiny_dump().splitlines() if not line.startswith('    i8 '))
        result = verify_mir_dump(broken)
        self.assertEqual('rejected', result.status)
        self.assertIn('live loans at exit', result.violation)

    def test_early_endpoint_detected(self):
        lines = tiny_dump().splitlines()
        index = next(i for i,l in enumerate(lines) if l.startswith('    i5 '))
        ending = next(l for l in lines if l.startswith('    i7 '))
        lines.insert(index, ending.replace('i7 ', 'i9 '))
        result = verify_mir_dump('\n'.join(lines))
        self.assertEqual('rejected', result.status)
        self.assertIn('ended reference used', result.violation)

    def test_forged_loan_source_detected(self):
        result = verify_mir_dump(tiny_dump().replace('source=p1 parent=loan0', 'source=p2 parent=loan0'))
        self.assertEqual('rejected', result.status)
        self.assertIn('metadata disagrees', result.violation)

    def test_unsupported_never_passes(self):
        for broken in (tiny_dump().replace('mir-v120', 'mir-v121'),
                       tiny_dump().replace('i5 load ', 'i5 call '),
                       tiny_dump().replace('steps=0', 'steps=1')):
            self.assertEqual('unsupported', verify_mir_dump(broken).status)
        self.assertEqual('timeout', verify_mir_dump(tiny_dump(), max_steps=1).status)


if __name__ == '__main__':
    unittest.main()
