#!/usr/bin/env python3
"""TLS callbacks retain incoming primary failure across source calls."""

import argparse
import json
import os
from pathlib import Path
import tempfile

from backend_execution import Endpoint, execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("gti", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    root, gti = args.root.resolve(), args.gti.resolve()
    fixture = root / "tests/fixtures/llvm_tls_cleanup_context.gti"
    template = fixture.read_text()
    lines = template.splitlines()

    def site(marker):
        return next(i for i, line in enumerate(lines, 1) if marker in line)

    secondary = site("int secondary =")
    primary = site("int primary =")
    outer_secondary = site("int outer_secondary =")
    endpoints = [Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", f"O{level}") for level in range(4))]
    cases = (("success", 1, 1, False, 0, None, None),
             ("tls-first", 0, 1, False, 70, secondary, None),
             ("body-primary", 0, 1, True, 70, secondary, primary),
             ("tls-primary", 0, 0, False, 70, outer_secondary, secondary))
    temporary = None
    if args.evidence_dir:
        scratch = args.evidence_dir.resolve()
        scratch.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="gti-llvm-tls-context-")
        scratch = Path(temporary.name)
    try:
        for name, divisor, outer, fail_body, code, failure, inherited in cases:
            source = scratch / f"{name}.gti"
            source.write_text(
                template.replace("divisor = 1; // CASE_DIVISOR",
                                 f"divisor = {divisor}; // CASE_DIVISOR")
                .replace("outer_divisor = 1; // CASE_OUTER_DIVISOR",
                         f"outer_divisor = {outer}; // CASE_OUTER_DIVISOR")
                .replace("forbid_local = false; // CASE_FORBID_LOCAL",
                         f"forbid_local = {str(fail_body).lower()}; // CASE_FORBID_LOCAL")
                .replace("forbid_outer_local = false; // CASE_FORBID_OUTER",
                         f"forbid_outer_local = {str(outer == 0).lower()}; // CASE_FORBID_OUTER")
                .replace("10 / 1; // CASE_PRIMARY",
                         "10 / divisor; // CASE_PRIMARY" if fail_body
                         else "10 / 1; // CASE_PRIMARY"))
            stderr = ""
            if inherited is not None:
                stderr = (f"{source.name}:{failure}: runtime error[GTI-R0014]: "
                          "failure_during_cleanup; primary [GTI-R0002] at "
                          f"{source.name}:{inherited}; secondary [GTI-R0002]\n")
            elif failure is not None:
                stderr = (f"{source.name}:{failure}: runtime error[GTI-R0002]: "
                          "division_by_zero in integer_division\n")
            for endpoint in endpoints:
                directory = scratch / f"{name}-{endpoint.backend}-{endpoint.optimization}"
                result = execute(gti, root, directory, source, endpoint,
                                 {**os.environ, "NO_COLOR": "1"})
                (directory / "result.json").write_text(json.dumps(result.__dict__, indent=2))
                if result.error or result.observable != (code, "", stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                print(f"{name} {endpoint}: passed", flush=True)
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
