#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(arguments: list[str], expected: int) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(arguments, text=True, capture_output=True, check=False,
                            timeout=30)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}\n"
            f"command: {arguments}\nstdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: llvm_backend_cli_test.py /path/to/gti")

    run([sys.argv[1], "--version"], 0)

    with tempfile.TemporaryDirectory(prefix="gti-llvm-backend-cli-") as value:
        root = pathlib.Path(value)
        source = root / "main.gti"
        output = root / "main"
        source.write_text("int main() { return 0; }\n", encoding="utf-8")

        run([sys.argv[1], str(source), "--backend", "llvm", "-o", str(output)], 0)
        run([str(output)], 0)
        output.unlink()
        source.write_text("class Owner { public: Owner() {} ~Owner() {} }; int main() { Owner value{}; return 0; }\n", encoding="utf-8")
        unsupported = run(
            [sys.argv[1], str(source), "--backend", "llvm", "-o", str(output)],
            65,
        )
        if "error[GTI-B0003]" not in unsupported.stderr or (
            "executable LLVM lowering is not implemented yet"
            not in unsupported.stderr
        ) or "O-LLVM-" not in unsupported.stderr or "MirInstructionKind.Construct" not in unsupported.stderr:
            raise AssertionError(
                "LLVM selection did not preserve the structured limitation:\n"
                + unsupported.stderr
            )
        if output.exists():
            raise AssertionError("an unsupported LLVM build published an executable")

        output.write_bytes(b"existing artifact")
        run([sys.argv[1], str(source), "--backend", "llvm", "-o", str(output)], 65)
        if output.read_bytes() != b"existing artifact":
            raise AssertionError("unsupported LLVM admission replaced an existing artifact")

        run([sys.argv[1], str(source), "--backend", "unknown"], 64)
        run(
            [sys.argv[1], str(source), "--backend", "llvm", "--emit-cpp"],
            64,
        )
        run(
            [sys.argv[1], str(source), "--backend", "llvm", "--std", "c++23"],
            64,
        )
        run(
            [sys.argv[1], str(source), "--backend", "llvm", "--keep-cpp"],
            64,
        )

    print("LLVM backend CLI selection passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
