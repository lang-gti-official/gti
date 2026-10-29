#!/usr/bin/env python3
"""One-region ownership preserves exact provider descriptors and failures."""

import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix
from allocation_abort_runtime_test import ABORT_STATUS


if __name__ == "__main__":
    fixture = pathlib.Path(__file__).parent / "fixtures/source_resource_prefix_cleanup.gti"
    line = next(index for index, text in enumerate(fixture.read_text().splitlines(), 1)
                if "1 / this.divisor" in text)
    failure = (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    policy = fixture.parent / "source_resource_prefix_policy.gti"
    memory = fixture.parents[2] / "stdlib/std/memory.gti"
    allocation_line = next(index for index, text in enumerate(memory.read_text().splitlines(), 1)
                           if "require_allocation(false)" in text)
    exhaustion = (f"<std/memory>:{allocation_line}: runtime error[GTI-R0011]: "
                  "allocation_failure in private_storage\n")
    policy_lines = policy.read_text().splitlines()
    def rejected_release(marker, message):
        sites = [index for index, text in enumerate(policy_lines, 1)
                 if text.endswith("// " + marker)]
        assert len(sites) == 1, marker
        return (70, "", f'<std/source_resource_prefix_policy>:{sites[0]}: runtime error[GTI-R0017]: '
                f'panic: "{message}"\n')
    policy_cases = [((), (0, "", ""))]
    for argument, marker, message in (
            ("wrong-provider", "wrong-provider", "wrong resource provider"),
            ("address", "descriptor-mismatch", "mismatched resource descriptor"),
            ("extent", "descriptor-mismatch", "mismatched resource descriptor"),
            ("alignment", "descriptor-mismatch", "mismatched resource descriptor"),
            ("identity", "wrong-region", "wrong resource region identity"),
            ("duplicate", "duplicate-release", "duplicate resource release")):
        policy_cases.append(((argument,), rejected_release(marker, message)))
    # This library-internal contract also calls prefix_slots::try_allocate
    # directly: public reserve intentionally skips rejected repeat allocation.
    # Stage the test as a trusted stdlib unit without changing the production
    # library or granting private-name access to application source.
    previous_stdlib = os.environ.get("GTI_STDLIB_PATH")
    library = pathlib.Path(previous_stdlib) if previous_stdlib else policy.parents[2] / "stdlib"
    with tempfile.TemporaryDirectory(prefix="gti-prefix-policy-stdlib-") as temporary:
        staged = pathlib.Path(temporary) / "stdlib"
        shutil.copytree(library, staged)
        policy_source = policy.read_text()
        assert policy_source.count("int main(") == 1
        (staged / "std" / policy.name).write_text(
            policy_source.replace("int main(", "int prefix_policy_main(", 1))
        entry = pathlib.Path(temporary) / "prefix_policy_test.gti"
        entry.write_text("import std.source_resource_prefix_policy;\n"
                         "import std.string;\nimport std.vector;\n"
                         "int main(int argc, std::vector<std::string> argv) {\n"
                         "  return prefix_policy_main(argc, std::move(argv));\n}\n")
        os.environ["GTI_STDLIB_PATH"] = str(staged)
        try:
            run_runtime_matrix(
                names=("source_resource_prefix_policy", "source_resource_prefix_cleanup",
                       "source_resource_prefix_exhaustion"),
                fixture_paths={"source_resource_prefix_policy": entry},
                cases={
                    "source_resource_prefix_policy": policy_cases,
                    "source_resource_prefix_cleanup": [
                        ((), (0, "2\n1\n900\n901\n", "")),
                        (("fail",), (70, "2\n1\n900\n901\n", failure)),
                        (("fail", "release"), (70, "2\n1\n900\n901\n", failure)),
                        (("success", "release"), (0, "2\n1\n900\n901\n", "")),
                    ],
                    "source_resource_prefix_exhaustion": [
                        ((), (ABORT_STATUS, "11\n", exhaustion)),
                        (("push",), (ABORT_STATUS, "8\n11\n", exhaustion)),
                        (("emplace", "item"), (ABORT_STATUS, "11\n", exhaustion)),
                        (("try", "push", "item"), (0, "8\n11\n88\n91\n", "")),
                    ],
                }, native_inputs_by_name={"source_resource_prefix_exhaustion":
                    [str(fixture.parent / "unique_scalar_allocation_fault.cpp")]})
        finally:
            if previous_stdlib is None:
                os.environ.pop("GTI_STDLIB_PATH", None)
            else:
                os.environ["GTI_STDLIB_PATH"] = previous_stdlib

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
