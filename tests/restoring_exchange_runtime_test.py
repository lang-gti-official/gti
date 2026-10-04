#!/usr/bin/env python3
"""Restoring transfer preserves ownership across construction and cleanup failure."""

import pathlib

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    source = pathlib.Path(__file__).parent / "fixtures/restoring_exchange.gti"
    lines = source.read_text().splitlines()

    def line_for(text):
        return next(index + 1 for index, line in enumerate(lines) if text in line)

    construction = line_for('std::panic("replacement construction")')
    cleanup = line_for('std::panic("resource cleanup")')
    primary = line_for('std::panic("after exchange")')
    main(names=("restoring_exchange",), cases={"restoring_exchange": [
        ((), (0, "2\n1\n3\n", "")),
        (("construction",), (70, "2\n1\n",
            f"restoring_exchange.gti:{construction}: runtime error[GTI-R0017]: "
            'panic: "replacement construction"\n')),
        (("cleanup", "double"), (70, "1\n2\n5\n",
            f"restoring_exchange.gti:{cleanup}: runtime error[GTI-R0014]: "
            f"failure_during_cleanup; primary [GTI-R0017] at restoring_exchange.gti:{primary} "
            'message="after exchange"; secondary [GTI-R0017] message="resource cleanup"\n')),
    ]})
