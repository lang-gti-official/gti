#!/usr/bin/env python3
"""Bound native secondary-record storage without changing cleanup execution."""

from pathlib import Path
import sys

from lambda_pack_invocation_runtime_test import main


if __name__ == "__main__":
    fixture = Path(__file__).parent / "fixtures/many_fallible_scope_guards.gti"
    lines = fixture.read_text().splitlines()
    primary = next(index for index, line in enumerate(lines, 1)
                   if 'std::panic("first cleanup failure")' in line)
    secondary = next(index for index, line in enumerate(lines, 1)
                     if 'std::panic("second cleanup failure")' in line)
    completed = "".join(f"{index}\n" for index in range(48, 0, -1))
    ordinary = (f"{fixture.name}:{primary}: runtime error[GTI-R0017]: "
                'panic: "first cleanup failure"\n')
    emergency = (f"{fixture.name}:{secondary}: runtime error[GTI-R0014]: "
                 "failure_during_cleanup; primary [GTI-R0017] at "
                 f"{fixture.name}:{primary} message=\"first cleanup failure\"; "
                 'secondary [GTI-R0017] message="second cleanup failure"\n')
    main(names=("many_fallible_scope_guards",), cases={
        "many_fallible_scope_guards": [
            ((), (0, completed, "")),
            (("primary",), (70, completed, ordinary)),
            (("primary", "secondary"), (70, "48\n47\n46\n", emergency)),
        ]
    }, native_inputs_by_name={
        "many_fallible_scope_guards":
            ["-Werror=uninitialized", "-Werror=sometimes-uninitialized"]
            if sys.platform == "darwin" else []
    })
