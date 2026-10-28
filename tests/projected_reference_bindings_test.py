#!/usr/bin/env python3
"""A reference bound to a field of a call's returned reference retains it.

Through a method, an overloaded `operator->` or `operator*`, a smart pointer
held locally or by reference, a user-defined pointer-like class, the hidden
range of a range-for and a recursive n-ary fold, on both backends. A mutable
range through a mutable smart-pointer reference parameter runs natively; the
C++ backend's text vocabulary lacks it (recorded C++ limitation). A mutable
owner's double read-only binding is a pinned known-failing control.
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
CONTROL_MESSAGE = ('failed to construct valid MIR. read of a place conflicts '
                   'with active mutable loan')


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
        for endpoint in (BOTH[0], NATIVE[0]):
            result = run('projected_reference_mut_owner_control', endpoint)
            if (result.compile_returncode != 65 or 'GTI-B0001' not in result.stderr
                    or CONTROL_MESSAGE not in result.stderr):
                raise AssertionError(f'known-failing control changed: {result}')
        print('projected reference bindings passed', flush=True)


if __name__ == '__main__':
    main()
