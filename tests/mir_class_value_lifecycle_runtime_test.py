#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mir_class_value_lifecycle_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
mut int32_t default_constructions = 0;

class Result {
public:
  int32_t value = 0;

  Result() {
    default_constructions += 1;
  }

  Result(int32_t initial) : value(initial) {}
};

Result make_result(int32_t value) {
  return Result(value + 0);
}

Result make_checked_result(int32_t value) {
  mut int32_t checked = value + 1;
  return Result(checked);
}

int main() {
  Result result = make_result(7);
  Result checked = make_checked_result(8);
  std::println(default_constructions);
  if (result.value == 7 && checked.value == 9) {
    return default_constructions;
  }
  return 1;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-mir-class-value-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "class-value-lifecycle.gti"
        source.write_text(source_text, encoding="utf8")

        for optimization in ("O0", "O3"):
            generated = root / f"class-value-lifecycle-{optimization}.cpp"
            emitted = run(
                [
                    str(compiler),
                    str(source),
                    f"-{optimization}",
                    "--emit-cpp",
                    "-o",
                    str(generated),
                ]
            )
            if emitted.returncode != 0:
                return fail(emitted)
            generated_text = generated.read_text(encoding="utf8")
            required_fragments = (
                "std::construct_at(__gti_mir_out_result",
                ".construction_address()",
                ".mark_constructed()",
            )
            if any(fragment not in generated_text for fragment in required_fragments):
                sys.stderr.write(
                    "class-valued failure publication did not use the "
                    "verified-MIR uninitialized result protocol\n"
                )
                return 1

            executable = root / f"class-value-lifecycle-{optimization}"
            built = run(
                [str(compiler), str(source), f"-{optimization}", "-o", str(executable)]
            )
            if built.returncode != 0:
                return fail(built)

            executed = run([str(executable)])
            if executed.returncode != 0 or executed.stdout != "0\n":
                sys.stderr.write(
                    "class-valued MIR publication introduced an observable "
                    "default construction: "
                    f"exit={executed.returncode} stdout={executed.stdout!r} "
                    f"stderr={executed.stderr!r}\n"
                )
                return 1

        class_parameter_source = root / "class-parameter-fail-closed.gti"
        class_parameter_source.write_text(
            """\
class RelayValue {
public:
  RelayValue(int32_t initial) : value(initial) {}
  RelayValue(RelayValue&& other) = default;

  int32_t value = 0;
};

RelayValue relay(RelayValue value, int32_t checked) {
  mut int32_t observed = checked + 1;
  return std::move(value);
}

int main() {
  RelayValue source{7};
  RelayValue result = relay(std::move(source), 0);
  return result.value - 7;
}
""",
            encoding="utf8",
        )
        class_parameter_executable = root / "class-parameter-fail-closed"
        class_parameter_build = run(
            [
                str(compiler),
                str(class_parameter_source),
                "-o",
                str(class_parameter_executable),
            ]
        )
        if class_parameter_build.returncode != 0:
            return fail(class_parameter_build)
        class_parameter_run = run([str(class_parameter_executable)])
        if class_parameter_run.returncode != 0:
            return fail(class_parameter_run)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
