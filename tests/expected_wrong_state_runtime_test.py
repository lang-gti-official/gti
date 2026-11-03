#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: expected_wrong_state_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    cases = (
        (
            "value-on-error",
            """\
expected<int32_t, int32_t> make_result() {
  return unexpected(int32_t(7));
}

int main() {
  mut expected<int32_t, int32_t> result = make_result();
  return result.value();
}
""",
            "value_on_error",
        ),
        (
            "error-on-value",
            """\
expected<int32_t, int32_t> make_result() {
  return int32_t(7);
}

int main() {
  mut expected<int32_t, int32_t> result = make_result();
  return result.error();
}
""",
            "error_on_value",
        ),
    )

    with tempfile.TemporaryDirectory(prefix="gti-expected-wrong-state-") as temporary:
        root = pathlib.Path(temporary)
        for label, source_text, detail in cases:
            source = root / f"{label}.gti"
            source.write_text(source_text, encoding="utf8")
            for mode, flags, native in native_modes():
                executable = root / f"{label}-{mode}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        *flags,
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(
                        f"build {label} {mode}", built
                    )

                executed = run([str(executable)])
                expected_stderr = (
                    f"{source.name}:7: runtime error[GTI-R0009]: "
                    f"invalid_expected_access in {detail}\n"
                )
                if (
                    executed.returncode != 70
                    or executed.stdout != ""
                    or executed.stderr != expected_stderr
                ):
                    return fail(
                        f"run {label} {mode}", executed
                    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
