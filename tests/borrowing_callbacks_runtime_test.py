#!/usr/bin/env python3
"""Checked reference capture environments preserve identity without allocations."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    failure = pathlib.Path(__file__).parent / "fixtures" / "checked_callable_erasure_failure.gti"
    line = next(index for index, text in enumerate(failure.read_text().splitlines(), 1)
                if 'std::panic("callback failed")' in text)
    main(names=("borrowing_callbacks", "checked_callable_pointer", "checked_callable_transport",
                "checked_callable_erasure", "checked_callable_erasure_failure",
                "checked_callable_result_origin"),
         cases={"checked_callable_erasure_failure": [((), (70, "1\n2\n",
             f'{failure.name}:{line}: runtime error[GTI-R0017]: panic: "callback failed"\n'))]})
