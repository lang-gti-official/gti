"""Self-tests for lab plumbing and evidence accounting. Run explicitly."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from corpus import cases
from generation import core_examples
from model import check
from runner import ROOT, classify_probe, command, shrink, mutation_gate
from source_bridge import Unsupported, render


class LabTests(unittest.TestCase):
    def test_rule_inventory_pairs_and_paths(self):
        data = json.loads((Path(__file__).parent / 'inventory.json').read_text())
        by_name = {c.name: c for c in cases()}
        self.assertEqual(len(by_name), len(cases()))
        for rule in data['rules']:
            with self.subTest(rule=rule['id']):
                self.assertTrue((ROOT / rule['owner']).is_file())
                self.assertTrue((ROOT / rule['contract'].split('#')[0]).is_file())
                self.assertTrue(by_name[rule['positive']].accepted)
                if rule['negative']:
                    self.assertFalse(by_name[rule['negative']].accepted)
                else:
                    self.assertTrue(rule['negative_reason'])
                for case in rule.get('interactions', []):
                    self.assertIn(case, by_name)
        self.assertEqual({r['id'] for r in data['rules']}, {c.rule for c in cases()})

    def test_no_ci_or_ctest_registration(self):
        candidates = list((ROOT / '.github').rglob('*.yml')) + list((ROOT / '.github').rglob('*.yaml'))
        candidates += list(ROOT.glob('CMakeLists.txt')) + list((ROOT / 'tests').rglob('CMakeLists.txt'))
        candidates += list((ROOT / 'cmake').rglob('*.cmake'))
        for path in candidates:
            self.assertNotIn('experiments/borrow_verification', path.read_text(), str(path))

    def test_classification_does_not_hide_ice_or_phase_disagreement(self):
        raw = {'syntax_valid': True, 'semantic_valid': True, 'full_semantic_valid': True,
               'accepted': True, 'diagnostics': []}
        self.assertEqual('accepted', classify_probe(raw))
        raw['diagnostics'] = [{'code': 'GTI-B0001'}]
        self.assertEqual('internal-error', classify_probe(raw))
        raw['diagnostics'] = []
        raw['full_semantic_valid'] = False
        self.assertEqual('phase-disagreement', classify_probe(raw))

    def test_reducer_preserves_the_actual_failure(self):
        p = core_examples()['use_after_move']
        violation = check(p).violation
        reduced, count = shrink(p, lambda q: check(q).violation == violation, budget=12)
        self.assertEqual(violation, check(reduced).violation)
        self.assertLessEqual(count, 12)

    def test_source_anchors_last_use(self):
        p = core_examples()['shared_read']
        source = render(p)
        self.assertGreater(source.index('lab_keep(labv_r_'), source.index('std::println(labv_r_'))
        p['body'] = p['body'][:-1]
        self.assertIn('lab_keep(labv_r_', render(p))

    def test_source_rejects_unfaithful_mappings(self):
        examples = core_examples()
        for name in ('child_before_parent', 'failure_cleanup', 'allocation_reuse', 'return_through_cleanup'):
            with self.subTest(name=name), self.assertRaises(Unsupported):
                render(examples[name])

    def test_source_keeps_immutability_and_loop_schedule(self):
        p = {'schema': 1, 'body': [{'op': 'alloc', 'name': 'o', 'fields': {'value': 1}, 'mutable': False}]}
        self.assertNotIn('mut int labv_o', render(p))
        p = {'schema': 1, 'body': [{'op': 'loop', 'condition': '*', 'body': []}]}
        self.assertIn(' < 0;', render(p, {'count': 4}))

    def test_source_records_have_explicit_constructor(self):
        source = render(core_examples()['disjoint_fields'])
        self.assertIn('LabRecord0(int arg0, int arg1)', source)
        self.assertIn('LabRecord0(2, 3)', source)

    def test_command_timeout_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            result = command([sys.executable, '-c', 'import time; time.sleep(10)'], Path(directory), 'timeout', 0.05)
            self.assertEqual('timeout', result['status'])
            self.assertIsNone(result['exit_code'])
            self.assertTrue(Path(result['stderr']).exists())

    def test_surviving_or_shape_only_mutation_never_passes_gate(self):
        records = [{'name': 'shared_aliases', 'probe': {'baseline_verified': True, 'mutations': [
            {'mutation': 'missing_endpoint', 'status': 'survived'},
            {'mutation': 'wrong_origin', 'status': 'rejected', 'errors': ['unrelated structural inconsistency']},
        ]}}]
        gates = {(x['case'], x['mutation']): x['status'] for x in mutation_gate(records)}
        self.assertEqual('survived', gates['shared_aliases', 'missing_endpoint'])
        self.assertEqual('shape-only-or-unrelated-rejection', gates['shared_aliases', 'wrong_origin'])
        self.assertEqual('missing-witness', gates['shared_aliases', 'missing_initialization'])

    def test_initialization_gate_requires_the_actual_availability_diagnostic(self):
        records = [{'name': 'shared_aliases', 'probe': {'baseline_verified': True, 'mutations': [
            {'mutation': 'missing_initialization', 'status': 'rejected',
             'errors': ['place access requires a definitely initialized scalar place']},
        ]}}]
        gate = next(g for g in mutation_gate(records) if g['mutation'] == 'missing_initialization')
        self.assertEqual('intended-kill', gate['status'])

    def test_operation_accounting_includes_calls_branches_and_cleanup(self):
        from campaign import operations
        p = core_examples()['failure_cleanup']
        self.assertEqual(['alloc', 'borrow', 'drop', 'end', 'fail', 'read', 'scope', 'write'], operations(p))
        self.assertIn('return', operations(core_examples()['returned_reference']))

    def test_seeded_disagreement_categories_are_distinct(self):
        from campaign import compiler_comparison
        args = argparse.Namespace(no_native=True)
        examples = core_examples()
        # Harness fault injection, not a claim of a real compiler defect.
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            with patch('campaign.analyze', return_value={'classification': 'accepted'}):
                result = compiler_comparison(examples['overlapping_mutable'], args, out)
                self.assertEqual('accepted-model-violation', result['classification'])
            with patch('campaign.analyze', return_value={'classification': 'rejected'}):
                result = compiler_comparison(examples['shared_read'], args, out)
                self.assertEqual('conservative-rejection-or-model-error', result['classification'])
            for classification in ('internal-error', 'lowering-rejection', 'timeout'):
                with patch('campaign.analyze', return_value={'classification': classification}):
                    self.assertEqual(classification, compiler_comparison(examples['shared_read'], args, out)['classification'])
            with patch('campaign.analyze', return_value={'classification': 'syntax-error'}):
                self.assertEqual('source-adapter-error', compiler_comparison(examples['shared_read'], args, out)['classification'])

    def test_compiler_reduction_preserves_each_required_failure_category(self):
        from campaign import compiler_comparison, reduce_compiler_finding, violation_rule
        examples = core_examples()
        args = argparse.Namespace(no_native=True)
        witnesses = (
            ('overlapping_mutable', 'accepted', 'accepted-model-violation'),
            ('shared_read', 'rejected', 'conservative-rejection-or-model-error'),
            ('shared_read', 'lowering-rejection', 'lowering-rejection'),
            ('shared_read', 'internal-error', 'internal-error'),
        )
        for name, injected, expected in witnesses:
            with self.subTest(classification=expected), tempfile.TemporaryDirectory() as directory:
                original = examples[name]
                baseline = check(original)
                with patch('campaign.analyze', return_value={'classification': injected}), patch('campaign.native') as native:
                    record = compiler_comparison(original, args, Path(directory))
                    self.assertEqual(expected, record['classification'])
                    reduced, attempts = reduce_compiler_finding(original, record, args, Path(directory), 30)
                    self.assertEqual(expected, compiler_comparison(reduced, args, Path(directory))['classification'])
                    self.assertEqual((baseline.status, violation_rule(baseline)),
                                     (check(reduced).status, violation_rule(check(reduced))))
                    self.assertLess(len(reduced['body']), len(original['body']))
                    self.assertLessEqual(attempts, 30)
                    native.assert_not_called()

    def test_lowering_reduction_does_not_switch_to_another_compiler_error(self):
        from campaign import compiler_comparison, reduce_compiler_finding
        original = core_examples()['shared_read']
        args = argparse.Namespace(no_native=True)
        def injected(source, *args):
            return {'classification': 'lowering-rejection' if 'std::println(labv_r_' in source else 'internal-error'}
        with tempfile.TemporaryDirectory() as directory, patch('campaign.analyze', side_effect=injected):
            record = compiler_comparison(original, args, Path(directory))
            reduced, attempts = reduce_compiler_finding(original, record, args, Path(directory), 30)
            self.assertEqual('lowering-rejection', compiler_comparison(reduced, args, Path(directory))['classification'])
            self.assertTrue(any(n['op'] == 'read' for n in reduced['body']))
            self.assertLess(len(reduced['body']), len(original['body']))

    def test_noneligible_reduction_does_not_report_false_replay_evidence(self):
        from campaign import reduce_compiler_finding
        args = argparse.Namespace(no_native=True)
        with tempfile.TemporaryDirectory() as directory, patch('campaign.compiler_comparison') as compare:
            for classification in ('timeout', 'unsupported-source', 'native-disagreement-or-adapter-error'):
                self.assertIsNone(reduce_compiler_finding(core_examples()['shared_read'],
                    {'classification': classification}, args, Path(directory), 30))
            compare.assert_not_called()

    def test_model_fault_reduction_uses_the_campaign_predicate(self):
        from campaign import model_comparison, reduce_model_finding, violation_rule
        original = core_examples()['overlapping_mutable']
        # A real intentional reference-analysis fault; the concrete executor
        # remains unmodified and discovers the incompatible access independently.
        with patch('campaign.check', side_effect=lambda p: check(p, mutation='overlap')):
            static, concrete, failure = model_comparison(original)
            self.assertEqual('model-accepted-invalid-execution', failure)
            rules = {violation_rule(r) for r in concrete if r.status == 'rejected'}
            reduced, attempts = reduce_model_finding(original, failure, budget=30)
            _, observed, reduced_failure = model_comparison(reduced)
            self.assertEqual(failure, reduced_failure)
            self.assertEqual(rules, {violation_rule(r) for r in observed if r.status == 'rejected'})
            self.assertLess(len(reduced['body']), len(original['body']))
            self.assertLessEqual(attempts, 30)

    def test_each_designated_semantic_mutation_witness_is_mandatory(self):
        from compiler_mutation import REQUIRED_WITNESSES, mandatory_witnesses_killed
        complete = [{'name': name, 'killed_by_semantic_stage_expectation': True} for name in REQUIRED_WITNESSES]
        complete.append({'name': 'call_overlap', 'killed_by_semantic_stage_expectation': False})
        self.assertTrue(mandatory_witnesses_killed(complete))
        for name in REQUIRED_WITNESSES:
            with self.subTest(missing=name):
                self.assertFalse(mandatory_witnesses_killed([c for c in complete if c['name'] != name]))
            with self.subTest(failed=name):
                self.assertFalse(mandatory_witnesses_killed([
                    {**c, 'killed_by_semantic_stage_expectation': False} if c['name'] == name else c for c in complete]))

    def test_core_loop_uses_zero_and_repeated_input_counts(self):
        from generation import compiler_core_examples
        from model import execute
        from source_bridge import render_core
        witness = compiler_core_examples()['loop_borrow_end']
        for count, observations in ((0, [1]), (2, [1, 1, 1])):
            with self.subTest(count=count):
                result = execute(witness['program'], inputs={'count': count})
                self.assertEqual('accepted', result.status)
                self.assertEqual(observations, result.observations)
                source = render_core(witness['program'], observations, {'count': count})
                self.assertIn(' < ' + str(count) + ';', source)


if __name__ == '__main__':
    unittest.main()
