#!/usr/bin/env python3
"""A reference bound to a field of a call's returned reference retains it.

Through a method, an overloaded `operator->` or `operator*`, a smart pointer
held locally or by reference, a user-defined pointer-like class, the hidden
range of a range-for and a recursive n-ary fold, on both backends. A mutable
range through a mutable smart-pointer reference parameter runs natively; the
C++ backend's text vocabulary lacks it (recorded C++ limitation). An
`operator->` step is selected from its use: a mutable owner's two read-only
bindings coexist, a counting user-defined pointer proves each selection, and
a mutation through `->` still conflicts with a live read-only reference.
"""

import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute

BOTH = (Endpoint('cpp', 'O0', 'c++20'), Endpoint('cpp', 'O2', 'c++20'),
        Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))
NATIVE = (Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))
CPP = (Endpoint('cpp', 'O0', 'c++20'),)
SUCCESS = (0, '', '')


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = dict(os.environ)
    env.setdefault('GTI_STDLIB_PATH', str(root / 'stdlib'))
    fixtures = root / 'tests/fixtures'
    with tempfile.TemporaryDirectory(prefix='gti-projected-refs-') as directory:
        work = Path(directory)

        def run(name: str, endpoint: Endpoint):
            case = work / f'{name}-{endpoint.backend}-{endpoint.optimization}'
            return execute(gti, root, case, fixtures / f'{name}.gti', endpoint, env)

        for endpoint in BOTH:
            result = run('projected_reference_bindings', endpoint)
            if result.error or result.observable != SUCCESS:
                raise AssertionError(f'bindings {endpoint}: {result}')
        for endpoint in NATIVE:
            result = run('projected_reference_mutation', endpoint)
            if result.error or result.observable != SUCCESS:
                raise AssertionError(f'mutation {endpoint}: {result}')
        # C++ limitation: its text vocabulary lacks the retained parented
        # call-result chain, a bug-fix-only backend gap (AGENTS.md).
        for endpoint in CPP:
            result = run('projected_reference_mutation', endpoint)
            if result.compile_returncode != 65 or 'GTI-B0001' not in result.stderr:
                raise AssertionError(f'mutation C++ limitation changed: {result}')
        for name in ('projected_reference_mut_owner', 'arrow_step_selection'):
            for endpoint in BOTH:
                result = run(name, endpoint)
                if result.error or result.observable != SUCCESS:
                    raise AssertionError(f'{name} {endpoint}: {result}')
        for endpoint in (BOTH[0], NATIVE[0]):
            result = run('arrow_step_mutation_conflict', endpoint)
            if result.compile_returncode == 0 or 'GTI-S2017' not in result.stderr:
                raise AssertionError(f'arrow mutation conflict changed: {result}')
        print('projected reference bindings passed', flush=True)


if __name__ == '__main__':
    main()
