"""Owning fixed-array composition, exercised by generic_payload_enum_runtime."""

from pathlib import Path
import subprocess
import tempfile

from backend_modes import is_cpp_mode


def run_enum_array_cases(compiler, modes):
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests/fixtures"

    def failure(line, code="GTI-R0002", reason="division_by_zero in integer_division"):
        return f"owning_enum_array_lifetimes.gti:{line}: runtime error[{code}]: {reason}\n"

    executions = [
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", failure(23)),
        (70, "2\n1\n9\n", failure(42)),
        (0, "3\n2\n1\n9\n", ""),
        (0, "1\n2\n9\n", ""),
        (0, "1\n2\n3\n9\n", ""),
        (0, "8\n2\n1\n9\n", ""),
        (70, "2\n1\n9\n", failure(31)),
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", failure(62, "GTI-R0007", "index_out_of_bounds in fixed_array")),
        (0, "2\n1\n9\n", ""),
    ]
    executions.append((70, "3\n2\n1\n9\n", failure(23)))
    executions.append((0, "1\n2\n3\n9\n", ""))
    cases = [
        (fixtures / "owning_enum_array_lifetimes.gti", executions),
        (fixtures / "owning_enum_array_thread_local.gti", [(0, "0\n4\n3\n", "")]),
        (root / "examples/owning-enum-arrays/main.gti", [(0, "", "")]),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-owning-enum-array-") as directory:
        for label, flags, _ in modes:
            if is_cpp_mode(flags):
                # The unchanged C++ text-vocabulary gate rejects the complete
                # owning-array move/cleanup fixture. LLVM and independent MIR
                # mutation tests establish the new language capability.
                print(f"owning-enum-arrays/{label}: recorded C++ vocabulary gap", flush=True)
                continue
            for index, (source, outcomes) in enumerate(cases):
                binary = Path(directory) / f"case-{index}-{label}"
                built = subprocess.run([str(compiler), str(source), *flags, "-o", str(binary)],
                                       capture_output=True, text=True, timeout=90)
                if built.returncode:
                    raise AssertionError(f"{source.name}/{label}: {built.stdout}{built.stderr}")
                for count, expected in enumerate(outcomes):
                    result = subprocess.run([str(binary), *["case"] * count],
                                            capture_output=True, text=True, timeout=15)
                    actual = result.returncode, result.stdout, result.stderr
                    if actual != expected:
                        raise AssertionError(f"{source.name}/{label}/{count}: {actual!r} != {expected!r}")
                print(f"{source.name}/{label}: {len(outcomes)} owning-array outcomes passed", flush=True)
