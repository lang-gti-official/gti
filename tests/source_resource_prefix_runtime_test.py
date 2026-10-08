#!/usr/bin/env python3
"""Single-region source prefix ownership preserves resource/failure contracts."""

import pathlib
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/source_resource_prefix_cleanup.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / this.divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    run_runtime_matrix(
        names=("source_resource_prefix_policy", "source_resource_prefix_cleanup"),
        cases={
            "source_resource_prefix_policy": [((), (0, "", ""))],
            "source_resource_prefix_cleanup": [
                ((), (0, "2\n1\n900\n901\n", "")),
                (("fail",), (70, "2\n1\n900\n901\n", failure)),
                (("fail", "release"), (70, "2\n1\n900\n901\n", failure)),
                (("success", "release"), (0, "2\n1\n900\n901\n", "")),
            ],
        })

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = fixture.parent
    rejected = {
        "source_resource_prefix_element_loan_rejected":
            ("values.push_back(8);", "GTI-S2017"),
        "source_resource_prefix_empty_provider_rejected":
            ("provider.reset();", "GTI-S2017"),
        "source_resource_prefix_reassignment_rejected":
            ("left = std::move(right);", "GTI-S2003"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-resource-prefix-negatives-") as temporary:
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
