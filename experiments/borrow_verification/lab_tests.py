"""Self-tests for lab plumbing and evidence accounting. Run explicitly."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

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


if __name__ == '__main__':
    unittest.main()
