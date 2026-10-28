#!/usr/bin/env python3
"""A type alias of a class constructs as the class's own name does.

A bare function name argument infers a function item through the alias, for a
user-defined class and for std::function_ptr, in return and initializer
positions, on both backends.
"""

import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute

ENDPOINTS = (Endpoint('cpp', 'O0', 'c++20'), Endpoint('cpp', 'O2', 'c++20'),
             Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))

# 7 + 7 + 5, then 6 + 9.
EXPECTED = (0, '19\n15\n', '')


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = dict(os.environ)
    env.setdefault('GTI_STDLIB_PATH', str(root / 'stdlib'))
    source = root / 'tests/fixtures/alias_function_item_construction.gti'
    with tempfile.TemporaryDirectory(prefix='gti-alias-construction-') as directory:
        work = Path(directory)
        for endpoint in ENDPOINTS:
            case = work / f'{endpoint.backend}-{endpoint.optimization}'
            result = execute(gti, root, case, source, endpoint, env)
            if result.error or result.observable != EXPECTED:
                raise AssertionError(f'{endpoint}: {result}')
            print(f'alias construction: {endpoint.backend} {endpoint.optimization} passed',
                  flush=True)


if __name__ == '__main__':
    main()
