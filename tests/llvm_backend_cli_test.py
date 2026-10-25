#!/usr/bin/env python3

import pathlib
import re
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
    help_text = run([sys.argv[1], "--help"], 0).stdout
    if "partial language support" not in help_text or "GTI-B0003" not in help_text:
        raise AssertionError("CLI help lost the LLVM admission limitation")

    with tempfile.TemporaryDirectory(prefix="gti-llvm-backend-cli-") as value:
        root = pathlib.Path(value)
        source = root / "main.gti"
        output = root / "main"
        source.write_text("int main() { return 0; }\n", encoding="utf-8")

        run([sys.argv[1], str(source), "--backend", "llvm", "-o", str(output)], 0)
        run([str(output)], 0)
        output.unlink()
        source.write_text("class Owner { public: virtual int read() { return 0; } }; int main() { Owner value{}; Owner copied = value; return copied.read(); }\n", encoding="utf-8")
        transfer_mir = root / "unsupported.mir"
        run([sys.argv[1], str(source), "-O0", "--emit-mir", "-o", str(transfer_mir)], 0)
        if not re.search(r"^\s+i\d+ load\b", transfer_mir.read_text(encoding="utf-8"), re.MULTILINE):
            raise AssertionError("the transfer control lost its verified class-copy load")
        unsupported = run(
            [sys.argv[1], str(source), "-O0", "--backend", "llvm", "-o", str(output)],
            65,
        )
        if "error[GTI-B0003]" not in unsupported.stderr or (
            "executable LLVM lowering is not implemented yet"
            not in unsupported.stderr
        ) or "O-LLVM-10" not in unsupported.stderr or "Shape.PolymorphicTransfer" not in unsupported.stderr or str(source) + ":" not in unsupported.stderr:
            raise AssertionError(
                "LLVM selection did not preserve the structured limitation:\n"
                + unsupported.stderr
            )
        if output.exists() or list(root.glob("*.o")) or list(root.glob("*.obj")):
            raise AssertionError("an unsupported LLVM build published native output")

        output.write_bytes(b"existing artifact")
        run([sys.argv[1], str(source), "-O0", "--backend", "llvm", "-o", str(output)], 65)
        if output.read_bytes() != b"existing artifact" or list(root.glob("*.o")) or list(root.glob("*.obj")):
            raise AssertionError("unsupported LLVM admission replaced an existing artifact")

        # O0 deliberately retains this unused construction. Required-body
        # assertions for read -> helper live in llvm_admission_tests; the CLI
        # preserves the structured rejection and artifact boundary here.
        source.write_text("""
int helper() { auto call = []() -> int { return 9; }; return call(); }
class Value { public: virtual int read() { return helper(); } };
int main() { Value value{}; return 0; }
""", encoding="utf-8")
        mir = root / "required.mir"
        run([sys.argv[1], str(source), "-O0", "--emit-mir", "-o", str(mir)], 0)
        if not re.search(r"^\s+i\d+ construct\b", mir.read_text(encoding="utf-8"), re.MULTILINE):
            raise AssertionError("the no-call dependency control lost its verified construction")
        rejected = run([sys.argv[1], str(source), "-O0", "--backend", "llvm", "-o", str(output)], 65)
        if "error[GTI-B0003]" not in rejected.stderr:
            raise AssertionError("the no-call dependency control lost its structured limitation")
        if output.read_bytes() != b"existing artifact" or list(root.glob("*.o")):
            raise AssertionError("rejected dispatch dependencies published partial native output")

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
