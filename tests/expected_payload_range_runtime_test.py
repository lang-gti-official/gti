#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


SUCCESS_SOURCE = r"""
#include <std/string>

expected<std::string, int32_t> make_text() {
  return std::string("ab");
}

interface CounterIteratorContract {
  int32_t& operator*();
  void operator++() mut;
};

class CounterIterator : public CounterIteratorContract {
  mut int32_t current;
public:
  CounterIterator(int32_t value) : current(value) {}
  int32_t& operator*() override { return this.current; }
  void operator++() mut override { this.current += 1; }
  bool operator!=(CounterIterator& other) {
    return this.current != other.current;
  }
};

class CounterRange {
  int32_t first;
  int32_t last;
public:
  CounterRange(int32_t first, int32_t last) : first(first), last(last) {}
  CounterIterator begin() { return CounterIterator(this.first); }
  CounterIterator end() { return CounterIterator(this.last); }
};

expected<CounterRange, int32_t> make_range(int32_t first, int32_t last) {
  return CounterRange(first, last);
}

int count_parameter(expected<std::string, int32_t> text) {
  mut int32_t count = 0;
  for (auto& outer : text.value()) {
    for (auto& inner : text.value()) {
      count += 1;
      if (count == 1) { continue; }
    }
    if (count == 4) { break; }
  }
  return count;
}

int return_from_loop(expected<std::string, int32_t> text) {
  for (auto& byte : text.value()) { return 2; }
  return 1;
}

int nominal_ranges() {
  mut auto values = make_range(1, 4);
  mut int32_t total = 0;
  for (auto& value : values.value()) { total += value; }
  mut auto empty = make_range(0, 0);
  for (auto& value : empty.value()) { return 1; }
  return total - 6;
}

int main() {
  mut auto first = make_text();
  mut int32_t count = 0;
  for (auto& byte : first.value()) { count += 1; }
  mut auto second = make_text();
  mut auto third = make_text();
  return count + count_parameter(std::move(second)) +
         return_from_loop(std::move(third)) + nominal_ranges() - 8;
}
"""


FAILURE_SOURCE = r"""
#include <std/string>

expected<std::string, int32_t> make_error() {
  return unexpected(int32_t(7));
}

int main() {
  mut auto text = make_error();
  for (auto& byte : text.value()) { return 1; }
  return 2;
}
"""


CSTDIO_SOURCE = r"""
#include <std/cstdio>

int main() {
  mut auto chunk = std::read_stdin_some(std::size_t(8));
  if (!chunk) { return 1; }
  mut int32_t count = 0;
  for (auto& byte : chunk.value()) { count += 1; }
  return count - 1;
}
"""


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
        raise SystemExit("usage: expected_payload_range_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-expected-payload-range-") as temp:
        root = pathlib.Path(temp)
        success_source = root / "expected-payload-range-success.gti"
        failure_source = root / "expected-payload-range-failure.gti"
        cstdio_source = root / "expected-payload-range-cstdio.gti"
        success_source.write_text(SUCCESS_SOURCE, encoding="utf8")
        failure_source.write_text(FAILURE_SOURCE, encoding="utf8")
        cstdio_source.write_text(CSTDIO_SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                suffix = f"{standard}-{optimization}"
                success = root / f"success-{suffix}"
                built = run(
                    [
                        str(compiler),
                        str(success_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(success),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build success {suffix}", built)
                executed = run([str(success)])
                if executed.returncode != 0:
                    return fail(f"run success {suffix}", executed)

                failure = root / f"failure-{suffix}"
                built = run(
                    [
                        str(compiler),
                        str(failure_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(failure),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build failure {suffix}", built)
                executed = run([str(failure)])
                if (
                    executed.returncode != 70
                    or executed.stdout != ""
                    or "GTI-R0009" not in executed.stderr
                    or "value_on_error" not in executed.stderr
                ):
                    return fail(f"run failure {suffix}", executed)

                cstdio = root / f"cstdio-{suffix}"
                built = run(
                    [
                        str(compiler),
                        str(cstdio_source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(cstdio),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build cstdio {suffix}", built)
                executed = subprocess.run(
                    [str(cstdio)],
                    text=True,
                    input="Q",
                    capture_output=True,
                    check=False,
                )
                if executed.returncode != 0:
                    return fail(f"run cstdio {suffix}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
