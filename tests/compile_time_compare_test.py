#!/usr/bin/env python3
"""Threshold-free integration test for clean compilation attribution."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    script, compiler, native = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='gti-compile-measure-test-') as temporary:
        output = Path(temporary) / 'evidence'
        command = [sys.executable, script, '--gti', compiler,
                   '--baseline-gti', compiler, '--cxx', native,
                   '--output', str(output), '--workload', 'vector-checked-loop',
                   '--optimization', '2', '--runs', '1', '--warmup', '0']
        subprocess.run(command, check=True, timeout=scaled(180))
        report = json.loads((output / 'report.json').read_text())
        assert report['schema'] == 1
        assert report['runs'] == 1 and report['warmup'] == 0
        row, = report['rows']
        assert row['baseline_emission_identical'] is True
        assert len(set(row['runtime_outputs'].values())) == 1
        expected = {'gti-full', 'gti-emit', 'gti-native',
                    'baseline-gti-full', 'baseline-gti-emit', 'baseline-gti-native',
                    'cpp-idiomatic', 'cpp-semantic'}
        assert set(row['samples']) == expected
        assert set(row['order'][0]) == expected
        for name, samples in row['samples'].items():
            assert len(samples) == 1 and samples[0]['duration_ns'] > 0
            assert row['warmups'][name] == []
            assert samples[0]['argv']
        assert all(value['sha256'] for value in row['sources'].values())
        original = (output / 'report.json').read_bytes()
        refused = subprocess.run(command, capture_output=True, timeout=scaled(20))
        assert refused.returncode != 0
        assert b'output must be a new directory' in refused.stderr
        assert (output / 'report.json').read_bytes() == original
        invalid = subprocess.run(command + ['--runs', '0'], capture_output=True,
                                 timeout=scaled(20))
        assert invalid.returncode != 0
        assert b'runs must be positive' in invalid.stderr
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
