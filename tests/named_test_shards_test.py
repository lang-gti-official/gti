#!/usr/bin/env python3
"""Exercise named-case execution, time bounds and fail-closed coverage."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('shards', ROOT / 'scripts/named_test_shards.py')
shards = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shards)


class NamedShards(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / 'fake'
        self.fake()

    def fake(self, names=('a', 'b', 'c', 'd', 'e'), fail='', delay=0):
        self.binary.write_text('#!/usr/bin/env python3\nimport sys, time\n'
            f'names = {list(names)!r}\n'
            'if sys.argv[1:] == ["--list"]:\n'
            ' print("\\n".join(names)); sys.exit(0)\n'
            'assert sys.argv[1] == "--test" and sys.argv[2] in names\n'
            f'time.sleep({delay})\n'
            f'sys.exit(1 if sys.argv[2] == {fail!r} else 0)\n')
        self.binary.chmod(0o755)

    def reports(self):
        paths = [self.root / f'lifecycle-{i}.json' for i in (1, 2)]
        for i, path in enumerate(paths, 1):
            self.assertEqual(shards.run(self.binary, i, 2, 'head', path, 10), 0)
        return paths

    def test_complete_partition_executes_every_case(self):
        paths = self.reports()
        shards.verify(paths, 'head', 2)
        self.assertEqual(json.loads(paths[0].read_text())['selected'], ['a', 'c', 'e'])

    def test_missing_duplicate_stale_and_corrupt_evidence(self):
        paths = self.reports()
        for bad in (paths[:1], [paths[0], paths[0]]):
            with self.assertRaises(ValueError):
                shards.verify(bad, 'head', 2)
        original = json.loads(paths[0].read_text())
        for key, value in [('source', 'old'), ('count', 3), ('inventory', ['a']),
                           ('selected', ['a']), ('results', [])]:
            changed = copy.deepcopy(original)
            changed[key] = value
            paths[0].write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                shards.verify(paths, 'head', 2)
        for code in (1, 'timeout', False):
            changed = copy.deepcopy(original)
            changed['results'][0]['code'] = code
            paths[0].write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                shards.verify(paths, 'head', 2)

    def test_failure_does_not_silently_drop_remaining_cases(self):
        self.fake(fail='a')
        path = self.root / 'failed.json'
        self.assertEqual(shards.run(self.binary, 1, 2, 'head', path, 10), 1)
        results = json.loads(path.read_text())['results']
        self.assertEqual([r['name'] for r in results], ['a', 'c', 'e'])
        self.assertEqual([r['code'] for r in results], [1, 0, 0])

    def test_timeout_is_reported_as_failure(self):
        self.fake(delay=10)
        path = self.root / 'timeout.json'
        self.assertEqual(shards.run(self.binary, 1, 2, 'head', path, 0.05), 1)
        self.assertEqual(json.loads(path.read_text())['results'][0]['code'], 'timeout')

    def test_invalid_inventory_and_empty_shards(self):
        for names in ([], ['a', 'a'], [' a'], ['']):
            self.fake(names=names)
            with self.assertRaises(ValueError):
                shards.inventory(self.binary)
        for index, count in [(0, 2), (3, 2), (1, 0), (1, 6)]:
            with self.assertRaises(ValueError):
                shards.selection(['a', 'b', 'c', 'd', 'e'], index, count)

    def test_ci_keeps_native_aggregate_and_requires_both_sanitizer_reports(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertIn("-E '^(compiler_pipeline|cpp_mir_body_emitter_lifecycle)$'", workflow)
        self.assertIn('--executable build-sanitized/gti_cpp_mir_body_emitter_lifecycle_tests', workflow)
        self.assertIn('--count 2 --timeout 5400', workflow)
        self.assertIn('named_test_shards.py verify', workflow)
        self.assertIn('pattern: sanitizers-libraries-*', workflow)
        self.assertIn('NAME cpp_mir_body_emitter_lifecycle\n', (ROOT / 'tests/CMakeLists.txt').read_text())


if __name__ == '__main__':
    unittest.main()
