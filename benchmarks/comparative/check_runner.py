#!/usr/bin/env python3
"""Manual harness checks; intentionally not registered with CTest/CI."""
import importlib.util, json, tempfile, unittest
from pathlib import Path
spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)

class Checks(unittest.TestCase):

    def test_comparisons(self):
        self.assertEqual(r.compare([100] * 11, [100] * 11)['verdict'], 'competitive')
        self.assertEqual(r.compare([200] * 11, [100] * 11)['verdict'], 'slower')
        self.assertEqual(r.compare([100, 140] * 6, [100] * 12)['verdict'], 'inconclusive')

    def test_denominator_retains_failures(self):
        s = r.summary([{'status': 'competitive'}, {'status': 'slower'}, {'status': 'compile_failed'}, {'status': 'noisy'}])
        self.assertEqual(s['competitive_percent_of_comparable'], 50)
        self.assertEqual(s['competitive_percent_of_entire_corpus'], 25)

    def program(self, body):
        path = Path(self.temp.name) / 'program'
        path.write_text('#!/usr/bin/env python3\n' + body)
        path.chmod(493)
        return path

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temp.cleanup()

    def test_sample(self):
        got = r.sample(self.program('print("GTI-COMPARE-1 37 200")'), 1, 17, 2)
        self.assertEqual(got['digest'], 37)
        self.assertEqual(got['ns'], 200)

    def test_invalid_output_rejected(self):
        for body in ['print("GTI-COMPARE-1 37 0")', 'print("junk")', 'print("GTI-COMPARE-1 37 200\\nextra")', 'raise SystemExit(2)']:
            with self.assertRaises(ValueError):
                r.sample(self.program(body), 1, 17, 2)

    def test_calibration_respects_slowest(self):
        self.assertEqual(r.calibration_rounds(1, 1000, 1000000000, 100000000, 30), 15)
        self.assertEqual(r.calibration_rounds(100, 1000000, 20000000000, 100000000, 30), 100)

    def test_failure_classification(self):
        try:
            r.sample(self.program('raise SystemExit(2)'), 1, 17, 2)
        except r.SampleError as e:
            self.assertEqual(e.status, 'correctness_failed')
        else:
            self.fail('expected rejected process')

    def test_timeout(self):
        with self.assertRaises(ValueError):
            r.sample(self.program('import time;time.sleep(5)'), 1, 17, 0.05)
if __name__ == '__main__':
    unittest.main()
