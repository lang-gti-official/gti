#!/usr/bin/env python3
"""Checked shared/exclusive movement across sequence and associative cursors."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from contiguous_iterator_completion_test import MODES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in MODES])
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = options.compiler.resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    fixtures = ("container_iterator_movement", "container_reverse_probe",
                "iterator_coordinate_boundaries")
    modes = [mode for mode in MODES if not options.mode or mode[0] in options.mode]
    with tempfile.TemporaryDirectory(prefix="gti-container-movement-") as temporary:
        output = Path(temporary)

        def build(case):
            fixture, label, flags, native = case
            binary = output / f"{fixture}-{label}"
            command = [str(compiler), str(root / "tests/fixtures" / f"{fixture}.gti"),
                       *flags, "--progress=never", "--keep-cpp", "-o", str(binary)]
            if native:
                command += ["--", *native]
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, timeout=180)
            assert result.returncode == 0, (fixture, label, result.stdout, result.stderr)
            assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            return binary

        def observe(binary):
            result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                    text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result

        cases = [(f"{fixture}-{label}", (fixture, label, flags, native))
                 for fixture in fixtures for label, flags, native in modes]
        run_cases(cases, build, options.jobs, observe)

        # The inclusive cursor coordinate can span the full unsigned domain;
        # the signed difference still has to be representable. Reject before
        # conversion or state mutation, including INT64_MIN subtraction.
        invalid = {
            "positive-distance-overflow":
                "std::detail::checked_iterator_difference(std::size_t(9223372036854775808), std::size_t(0))",
            "negative-distance-overflow":
                "std::detail::checked_iterator_difference(std::size_t(0), std::size_t(9223372036854775809))",
            "minimum-offset-before-begin":
                "std::detail::checked_iterator_position(std::size_t(0), std::size_t(0), std::size_t(3), std::ptrdiff_t(-9223372036854775807) - std::ptrdiff_t(1))",
            "minimum-subtraction-after-end":
                "std::detail::checked_iterator_position(std::size_t(0), std::size_t(0), std::size_t(3), std::ptrdiff_t(-9223372036854775807) - std::ptrdiff_t(1), true)",
        }

        # An unrelated source owner publishes its coordinate only after the
        # checked RHS returns. Failure cleanup observes the original state,
        # between two surrounding drops, including malformed starting states.
        step_states = {
            "step-forward-at-end": (0, 0, 3, 0, 3),
            "step-forward-before-begin": (0, 0, 4, 5, 9),
            "step-forward-after-end": (0, 0, 10, 5, 9),
            "step-forward-at-maximum": (0, 0, 18446744073709551615, 0, 18446744073709551615),
            "step-backward-at-begin": (0, 1, 5, 5, 9),
            "step-backward-before-begin": (0, 1, 4, 5, 9),
            "step-backward-after-end": (0, 1, 10, 5, 9),
            "step-backward-at-zero": (0, 1, 0, 0, 9),
            "step-forward-reversed-boundaries": (0, 0, 5, 9, 3),
            "step-backward-reversed-boundaries": (0, 1, 5, 9, 3),
            "step-reverse-increment-at-begin": (1, 0, 5, 5, 9),
            "step-reverse-decrement-at-end": (1, 1, 9, 5, 9),
        }
        state_source = """#include <std/string>
#include <std/detail/iterator>
class StepDrop {
  int id;
public:
  StepDrop(int id) : id(id) {}
  ~StepDrop() { std::println(this.id); }
};
class StepState<uint64_t Reverse, uint64_t Decrement> {
  mut std::size_t position;
  std::size_t begin;
  std::size_t end;
public:
  StepState(std::size_t position, std::size_t begin, std::size_t end)
      : position(position), begin(begin), end(end) {}
  void step() mut {
    this.position = std::detail::checked_iterator_step<Reverse, Decrement>(
        this.position, this.begin, this.end);
  }
  ~StepState() { std::println(this.position); }
};
"""

        def build_failure(case):
            name, expression, label, flags, native = case
            fixture = output / f"{name}-{label}.gti"
            expected_stdout = ""
            if name in step_states:
                reverse, decrement, position, begin, end = step_states[name]
                fixture.write_text(state_source + "int main() {\n"
                    "  StepDrop outer{10};\n"
                    f"  mut StepState<{reverse}, {decrement}> state{{std::size_t({position}), std::size_t({begin}), std::size_t({end})}};\n"
                    "  StepDrop inner{20};\n  state.step();\n  return 99;\n}\n")
                expected_stdout = f"20\n{position}\n10\n"
            else:
                fixture.write_text("#include <std/detail/iterator>\nint main() {\n"
                                   f"auto result = {expression};\nreturn 0;\n}}\n")
            binary = output / f"{name}-{label}"
            command = [str(compiler), str(fixture), *flags,
                       "--progress=never", "-o", str(binary)]
            if native:
                command += ["--", *native]
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, timeout=180)
            assert result.returncode == 0, (name, label, result.stdout, result.stderr)
            return binary, expected_stdout

        def observe_failure(observation):
            binary, expected_stdout = observation
            result = subprocess.run([str(binary)], env=environment, capture_output=True,
                                    text=True, timeout=30)
            assert result.returncode == 70 and result.stdout == expected_stdout, result
            assert "runtime error[GTI-R0016]" in result.stderr, result
            assert "AddressSanitizer" not in result.stderr and "UndefinedBehaviorSanitizer" not in result.stderr, result

        failures = [(f"{name}-{label}", (name, expression, label, flags, native))
                    for name, expression in {**invalid, **dict.fromkeys(step_states, "")}.items()
                    for label, flags, native in modes]
        run_cases(failures, build_failure, options.jobs, observe_failure)


if __name__ == "__main__":
    main()
