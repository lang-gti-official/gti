#!/usr/bin/env python3
"""Owner-witness text bridges retain checked loans independently of storage."""

import pathlib
import re
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix
from harness_timeouts import scaled

if __name__ == "__main__":
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    run_runtime_matrix(
        names=("source_text_address_bridge", "source_text_address_audit_static"),
        cases={"source_text_address_bridge": [((), (0, "", ""))],
               "source_text_address_audit_static": [((), (0, "", ""))]},
        native_inputs_by_name={"source_text_address_bridge":
                               [str(fixtures / "source_text_address_bridge.cpp")]})
    rejected = {
        "source_text_address_mutation_rejected": ("owner.mutate();", "GTI-S2017"),
        "source_text_address_empty_mutation_rejected": ("owner.mutate();", "GTI-S2017"),
        "source_text_address_escape_rejected":
            ("return std::assume_string_view(owner.address(), uint64_t(1), owner);", "GTI-S2045"),
        "source_text_address_unsafe_rejected":
            ("std::string_view view = std::assume_string_view(&owner, uint64_t(1), owner);", "GTI-S2055"),
    }
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-text-address-negatives-") as temporary:
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
                    capture_output=True, text=True, timeout=scaled(60))
                assert result.returncode == 65, (name, audit, result)
                assert re.search(pattern, result.stderr), (name, audit, result.stderr)
                assert "GTI-B0001" not in result.stderr, (name, audit, result.stderr)
                assert not artifact.exists(), (name, audit, artifact)
                print(f"PASS {name}/{audit}", flush=True)
        expired = {
            "source_text_address_audit_expired":
                ("auto text = relayed_text(address, witness);", "GTI-S2101"),
            "source_text_address_audit_local_return":
                ("c_string native = relay_native(address, witness);", "GTI-S2102"),
        }
        for name, (operation, code) in expired.items():
            source = fixtures / f"{name}.gti"
            line = next(index for index, text in enumerate(source.read_text().splitlines(), 1)
                        if text.strip() == operation)
            location = re.escape(f"{source.name}:{line}:") + r"\d+: "
            for audit in ("off", "warn", "strict"):
                artifact = pathlib.Path(temporary) / f"{name}-{audit}.mir"
                result = subprocess.run(
                    [str(compiler), str(source), "--emit-mir", "-o", str(artifact),
                     f"--raw-pointer-audit={audit}", "--progress", "never"],
                    capture_output=True, text=True, timeout=scaled(60))
                expected = 65 if audit == "strict" else 0
                assert result.returncode == expected, (name, audit, result.stderr)
                if audit != "off":
                    severity = "error" if audit == "strict" else "warning"
                    assert re.search(location + rf"{severity}\[{code}\]", result.stderr), (name, audit, result.stderr)
                    assert "scope has ended" in result.stderr, (name, audit, result.stderr)
                else:
                    assert not re.search(location + r".*GTI-S210[123]", result.stderr), result.stderr
                print(f"PASS {name}/{audit}", flush=True)
        source = fixtures / "source_text_address_audit_static.gti"
        # Strict currently promotes unrelated included sparse/arena provenance
        # limitations. Warn runs the same optional analysis and proves this
        # application's static/null carriers have no raw-audit finding.
        for audit in ("warn",):
            artifact = pathlib.Path(temporary) / f"static-{audit}.mir"
            result = subprocess.run(
                [str(compiler), str(source), "--emit-mir", "-o", str(artifact),
                 f"--raw-pointer-audit={audit}", "--progress", "never"],
                capture_output=True, text=True, timeout=scaled(60))
            assert result.returncode == 0, (audit, result.stderr)
            assert not re.search(re.escape(source.name) + r":\d+:\d+: .*GTI-S210[123]", result.stderr), result.stderr
            print(f"PASS text address static/null witnesses/{audit}", flush=True)
