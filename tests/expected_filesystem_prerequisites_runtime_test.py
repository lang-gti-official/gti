#!/usr/bin/env python3
"""General expected-value control flow used by filesystem wrappers."""

from lambda_pack_invocation_runtime_test import main

if __name__ == "__main__":
    main(names=("expected_passive_text_parameter", "expected_argument_loop",
                "expected_passive_failure", "expected_argument_failure",
                "expected_temporary_observer",
                "expected_temporary_observer_failure_free"),
         cases={"expected_passive_failure": [((), (70, "caller cleaned\n",
                 "expected_passive_failure.gti:12: runtime error[GTI-R0002]: "
                 "division_by_zero in integer_division\n"))],
                "expected_argument_failure": [
                    (arguments, (70, "item cleaned\n" * 3 + "caller cleaned\n",
                     "expected_argument_failure.gti:17: runtime error[GTI-R0002]: "
                     "division_by_zero in integer_division\n"))
                    for arguments in ((), ("member",))]})
