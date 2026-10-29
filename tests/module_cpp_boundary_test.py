#!/usr/bin/env python3
"""Classify C++ module spelling collisions without replacing valid artifacts."""

import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(command, directory, label):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    (directory / f"{label}.log").write_text(result.stdout + result.stderr)
    return result


def main():
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    root = pathlib.Path(tempfile.mkdtemp(prefix="gti-module-cpp-boundary-"))
    cases = {
        "type-alias": (
            "using Number=int; export int left(){Number value=20;return value;}",
            "using Number=bool; export int right(){Number value=true;return value?22:0;}",
            False),
        "namespace-alias": (
            "namespace A {int value(){return 20;}} namespace Common=A; export int left(){return Common::value();}",
            "namespace B {int value(){return 22;}} namespace Common=B; export int right(){return Common::value();}",
            False),
        "global": (
            "int value=20; export int left(){return value;}",
            "int value=22; export int right(){return value;}", False),
        "class-alias": (
            "struct Number {int value;}; export int left(){Number value{.value=20};return value.value;}",
            "using Number=int; export int right(){return Number(22);}", False),
        "namespace-type-alias": (
            "namespace Common {int value(){return 20;}} export int left(){return Common::value();}",
            "using Common=int; export int right(){return Common(22);}", False),
        "namespace-namespace-alias": (
            "namespace Common {int value(){return 20;}} export int left(){return Common::value();}",
            "namespace B {int value(){return 22;}} namespace Common=B; export int right(){return Common::value();}",
            False),
        "namespace-global": (
            "namespace Common {int value(){return 20;}} export int left(){return Common::value();}",
            "int Common=22; export int right(){return Common;}", False),
        "namespace-type": (
            "namespace Common {int value(){return 20;}} export int left(){return Common::value();}",
            "struct Common{int value;}; export int right(){Common value{.value=22};return value.value;}",
            False),
        "equal-alias": (
            "using Number=int; export int left(){return Number(20);}",
            "using Number=int; export int right(){return Number(22);}", True),
        "equal-namespace-alias": (
            "namespace A {int value(){return 20;}} namespace Common=A; export int left(){return Common::value();}",
            "namespace A {int value(){return 22;}} namespace Common=A; export int right(){return Common::value();}",
            True),
        "source-local-storage": (
            "static int value=20; export int left(){return value;}",
            "static int value=22; export int right(){return value;}", True),
        "functions": (
            "int helper(){return 20;} export int left(){return helper();}",
            "int helper(){return 22;} export int right(){return helper();}", True),
    }
    try:
        for name, (left, right, cpp_supported) in cases.items():
            directory = root / name
            directory.mkdir()
            (directory / "left.gti").write_text("export module left;" + left)
            (directory / "right.gti").write_text("export module right;" + right)
            source = directory / "main.gti"
            source.write_text('import "left.gti"; import "right.gti"; '
                              'int main(){return left()+right()-42;}')
            for backend in ("llvm", "cpp"):
                binary = directory / (backend + (".exe" if sys.platform == "win32" else ""))
                previous = None
                if backend == "cpp" and not cpp_supported:
                    seed = directory / "seed.gti"
                    seed.write_text("int main(){return 0;}")
                    built = run([compiler, str(seed), "--backend", "cpp", "-O2",
                                 "-o", str(binary)], directory, "seed")
                    assert built.returncode == 0, built.stderr
                    previous = binary.read_bytes()
                built = run([compiler, str(source), "--backend", backend, "-O2",
                             "-o", str(binary)], directory, backend)
                if previous is not None:
                    assert built.returncode == 65, built.stderr
                    assert "GTI-B0003" in built.stderr, built.stderr
                    assert "same qualified name" in built.stderr, built.stderr
                    assert "GTI-B0001" not in built.stderr, built.stderr
                    assert binary.read_bytes() == previous, "rejection replaced valid executable"
                else:
                    assert built.returncode == 0, built.stderr
                executed = run([str(binary)], directory, backend + "-execution")
                assert executed.returncode == 0, executed.stderr
        print("module C++ representation boundary checks passed")
    except BaseException:
        print(f"Module boundary evidence retained at {root}", file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
