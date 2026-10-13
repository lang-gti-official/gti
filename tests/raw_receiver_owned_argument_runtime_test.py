#!/usr/bin/env python3
"""Raw pointee receivers retain ordinary ordered owning argument cleanup."""

from pathlib import Path
from lambda_pack_invocation_runtime_test import main as runtime_matrix


if __name__ == "__main__":
    name = "raw_receiver_owned_argument"
    source = Path(__file__).parent / "fixtures" / f"{name}.gti"
    lines = source.read_text().splitlines()
    def line_for(fragment):
        return next(i for i, line in enumerate(lines, 1) if fragment == line)
    def failure(line, context="integer_division", code="GTI-R0002", kind="division_by_zero"):
        return f"{source.name}:{line}: runtime error[{code}]: {kind} in {context}\n"
    cases = {name: [
        ((), (0, "receiver\nargument\nbody\n2\n9\n", "")),
        (("read",), (0, "receiver\nread\n2\n9\n", "")),
        (("wrong", "value"), (70, "receiver\n3\n9\n",
            failure(line_for("          std::move(result).value(),"), "value_on_error", "GTI-R0009", "invalid_expected_access"))),
        (("receiver", "failure", "now"), (70, "receiver\n2\n9\n",
            failure(line_for("  int checked = 1 / divisor;")))),
        (("argument", "failure", "after", "move"), (70, "receiver\nargument\n2\n9\n",
            failure(line_for("  return 1 / divisor;")))),
        (("body", "failure", "after", "all", "inputs"), (70, "receiver\nargument\nbody\n2\n9\n",
            failure(line_for("    int checked = 1 / divisor;")))),
        (("error", "payload", "moves", "into", "raw", "call"), (0, "receiver\nbody\n3\n2\n9\n", "")),
    ]}
    runtime_matrix(names=(name,), cases=cases)
