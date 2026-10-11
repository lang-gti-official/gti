#!/usr/bin/env python3
"""Exact dispatch preserves evaluation, first failure, and caller cleanup."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from harness_timeouts import scaled

def invoke(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=scaled(120))

def main():
    compiler, source, cxx = sys.argv[1:4]
    proof = sys.argv[4] if len(sys.argv) > 4 else 'exact-receiver'
    with tempfile.TemporaryDirectory(prefix='gti-exact-receiver-') as temporary:
        root = Path(temporary)
        reference = {}
        for standard in ('c++20', 'c++23'):
            for level in ('0', '2', '3'):
                for failing in (False, True):
                    name = f'{standard}-{level}-{failing}'
                    executable, report = root/name, root/(name+'.json')
                    cmd = [compiler, source, '-O'+level, '--std', standard, '--cxx', cxx,
                           '--keep-cpp', '--optimization-report', str(report), '-o', str(executable)]
                    if failing: cmd += ['-D', 'FAIL_RUNTIME']
                    built = invoke(cmd)
                    assert built.returncode == 0, (cmd, built.stdout, built.stderr)
                    result = invoke([str(executable)])
                    observed = (result.returncode, result.stdout, result.stderr)
                    if failing not in reference: reference[failing] = observed
                    assert observed == reference[failing], (name, observed, reference[failing])
                    if failing:
                        assert result.returncode != 0 and result.stdout == 'argument\nbias\ncleanup\n', observed
                        assert 'GTI-R0001' in result.stderr, observed
                    else:
                        assert result.returncode == 0 and result.stdout == 'argument\nbias\n42\ncleanup\n', observed
                    data = json.loads(report.read_text())
                    proofs = [r for r in data['transformations'] if r['kind']=='devirtualize-call']
                    assert len(proofs) == (0 if level=='0' else 1), (name, proofs)
                    assert all(r['proof'] == proof for r in proofs), (name, proofs)
        print('12 exact-dispatch C++20/C++23 O0/O2/O3 success/failure executions passed')
    return 0

if __name__ == '__main__': raise SystemExit(main())
