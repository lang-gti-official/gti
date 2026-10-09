#!/usr/bin/env python3
"""Source-defined terminated prefixes preserve strings and their checked views."""

import pathlib
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix
from allocation_abort_runtime_test import ABORT_STATUS

if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    overflow = fixtures / "source_prefix_string_overflow.gti"
    # The hidden NUL slot is part of the requested extent. Its size overflow
    # remains a cleanup-preserving programming error, not exhaustion.
    standard = pathlib.Path(__file__).parent.parent / "stdlib/std/string.gti"
    overflow_line = next(index for index, text in enumerate(standard.read_text().splitlines(), 1)
                         if "replacement{requested + size_t(1)}" in text)
    failure = (f"<std/string>:{overflow_line}: runtime error[GTI-R0001]: "
               "integer_overflow in addition\n")
    memory = pathlib.Path(__file__).parent.parent / "stdlib/std/memory.gti"
    provider_line = next(index for index, text in enumerate(memory.read_text().splitlines(), 1)
                         if "require_allocation(false)" in text)
    provider_failure = (f"<std/memory>:{provider_line}: runtime error[GTI-R0011]: "
                        "allocation_failure in private_storage\n")
    run_runtime_matrix(
        names=("source_prefix_string", "source_prefix_string_overflow",
               "source_prefix_string_provider_failure"),
        cases={"source_prefix_string": [((), (0, "", ""))],
               "source_prefix_string_overflow": [((), (70, "55\n", failure))],
               "source_prefix_string_provider_failure":
                   [((), (ABORT_STATUS, "", provider_failure))]},
        native_inputs_by_name={"source_prefix_string":
                               [str(fixtures / "source_text_address_bridge.cpp")],
                               "source_prefix_string_provider_failure":
                               [str(fixtures / "stdlib_adoption_allocation_counts.cpp")]})
    compiler = pathlib.Path(sys.argv[1]).resolve()
    rejected = {
        "source_prefix_string_cstring_mutation_rejected": ("text.push_back('a');", "GTI-S2017"),
        "source_prefix_string_span_mutation_rejected": ("text.reserve(uint64_t(20));", "GTI-S2017"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-source-string-negatives-") as temporary:
        for name, (operation, code) in rejected.items():
            source = fixtures / f"{name}.gti"
            line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                        if text.strip() == operation)
            pattern = re.escape(f"{source.name}:{line}:") + rf"\d+: error\[{code}\]"
            for audit in ("off", "warn"):
                artifact = pathlib.Path(temporary) / f"{name}-{audit}.mir"
                result = subprocess.run(
                    [str(compiler), str(source), "--emit-mir", "-o", str(artifact),
                     f"--raw-pointer-audit={audit}", "--progress", "never"],
                    capture_output=True, text=True, timeout=60)
                assert result.returncode == 65, (name, audit, result)
                assert re.search(pattern, result.stderr), (name, audit, result.stderr)
                assert "GTI-B0001" not in result.stderr, (name, audit, result.stderr)
                assert not artifact.exists(), (name, audit, artifact)
                print(f"PASS {name}/{audit}", flush=True)
