#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def verify_emitted_order(source: str) -> bool:
    calls = []
    for name in ("a", "b", "sum"):
        match = re.search(
            rf"::__gti_program::__gti_fn_[0-9]+_{name}(?:__gti_mir_failure)?\(",
            source,
        )
        if match is None:
            return False
        calls.append(match.start())
    return calls[0] < calls[1] < calls[2]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: evaluation_order_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
int a() {
  std::println("a");
  return 1;
}

int b() {
  std::println("b");
  return 2;
}

int sum(int left, int right) {
  return left + right;
}

int main() {
  return sum(a(), b()) - 3;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-evaluation-order-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "evaluation-order.gti"
        source.write_text(source_text, encoding="utf8")
        for standard in ("c++20", "c++23"):
            generated = root / f"evaluation-order-{standard}.cpp"
            emitted = run(
                [
                    str(compiler),
                    str(source),
                    "--std",
                    standard,
                    "--emit-cpp",
                    "-o",
                    str(generated),
                ]
            )
            if emitted.returncode != 0:
                return fail(f"emit {standard}", emitted)
            if not verify_emitted_order(generated.read_text(encoding="utf8")):
                sys.stderr.write(
                    f"emit {standard}: effectful arguments were not emitted "
                    "as ordered calls before invocation\n"
                )
                return 1

            for optimization in ("O0", "O3"):
                executable = root / f"evaluation-order-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {standard} {optimization}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != "a\nb\n"
                    or executed.stderr != ""
                ):
                    return fail(f"run {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
