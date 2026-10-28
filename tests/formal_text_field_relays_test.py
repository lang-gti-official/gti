#!/usr/bin/env python3
"""A by-value text relay of a formal's text field is a text field result.

`relay(label.text)` through a reference formal, a chained relay and the
receiver's own field run on both backends over ownerless and dependent text.
"""

import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute

ENDPOINTS = (Endpoint('cpp', 'O0', 'c++20'), Endpoint('cpp', 'O2', 'c++20'),
             Endpoint('llvm', 'O0'), Endpoint('llvm', 'O2'))


def main() -> None:
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    env = dict(os.environ)
    env.setdefault('GTI_STDLIB_PATH', str(root / 'stdlib'))
    source = root / 'tests/fixtures/formal_text_field_relays.gti'
    with tempfile.TemporaryDirectory(prefix='gti-formal-text-relays-') as directory:
        for endpoint in ENDPOINTS:
            case = Path(directory) / f'{endpoint.backend}-{endpoint.optimization}'
            result = execute(gti, root, case, source, endpoint, env)
            if result.error or result.observable != (0, '', ''):
                raise AssertionError(f'{endpoint}: {result}')
        print('formal text field relays passed', flush=True)


if __name__ == '__main__':
    main()
