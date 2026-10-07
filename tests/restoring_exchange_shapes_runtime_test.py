#!/usr/bin/env python3
"""Restoring exchange transports every eligible owning result shape."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    source = pathlib.Path(__file__).parent / "fixtures/restoring_array_failure.gti"
    lines = source.read_text().splitlines()

    def line_for(text):
        return next(index + 1 for index, line in enumerate(lines) if text in line)

    def panic(message):
        line = line_for(f'std::panic("{message}")')
        return f'restoring_array_failure.gti:{line}: runtime error[GTI-R0017]: panic: "{message}"\n'

    cleanup = line_for('std::panic("array resource cleanup")')
    primary = line_for('std::panic("array return cleanup")')
    result_source = source.with_name("restoring_array_results.gti")
    result_panic_line = next(
        index + 1 for index, line in enumerate(result_source.read_text().splitlines())
        if 'std::panic("later array argument")' in line
    )
    main(names=("restoring_exchange_shapes", "restoring_array_failure", "restoring_array_results"), cases={
        "restoring_array_results": [
            ((), (0, "2\n1\n4\n3\n6\n5\n", "")),
            (("later-failure",), (70, "2\n1\n4\n3\n",
                f'restoring_array_results.gti:{result_panic_line}: runtime error[GTI-R0017]: panic: "later array argument"\n')),
        ],
        "restoring_array_failure": [
            ((), (0, "2\n1\n4\n3\n", "")),
            (("construction",), (70, "3\n2\n1\n", panic("array replacement construction"))),
            (("after", "exchange"), (70, "2\n1\n4\n3\n", panic("after array exchange"))),
            (("return", "cleanup", "failure"), (70, "2\n1\n4\n3\n", panic("array return cleanup"))),
            (("return", "cleanup", "double", "failure"), (70, "2\n5\n",
                f"restoring_array_failure.gti:{cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0017] at restoring_array_failure.gti:{primary} "
                'message="array return cleanup"; secondary [GTI-R0017] message="array resource cleanup"\n')),
        ],
    })
