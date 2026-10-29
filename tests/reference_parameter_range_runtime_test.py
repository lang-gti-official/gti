#!/usr/bin/env python3

import pathlib
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


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: reference_parameter_range_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
import std.vector;

class DispatchBase {
public:
  DispatchBase() {}
  virtual int32_t value() {
    return 1;
  }
};

class DispatchDerived : public DispatchBase {
public:
  DispatchDerived() {}
  int32_t value() override {
    return 3;
  }
};

interface Shape {
  int32_t area();
};

class Square : public Shape {
public:
  Square() {}
  int32_t area() override {
    return 5;
  }
  int32_t plain() {
    return 2;
  }
};

mut Square global_square = Square();

class Holder {
public:
  Holder() {}
  int32_t read() {
    return this.item.area();
  }

private:
  Square item = Square();
};

class GenericSquare<T> : public Shape {
public:
  GenericSquare() {}
  int32_t area() override {
    return 6;
  }
};

interface Measure {
  int32_t measure();
};

class StableMeasure : public Measure {
public:
  StableMeasure() {}
  int32_t measure() override {
    return 8;
  }
};

class CheckedMeasure : public StableMeasure {
public:
  CheckedMeasure() : StableMeasure() {}
  int32_t measure() override {
    return this.value + 1;
  }

private:
  int32_t value = 8;
};

Square make_square() {
  return Square();
}

int32_t read_base(DispatchBase& value) {
  return value.value();
}

int32_t read_bound(mut DispatchDerived& value) {
  mut DispatchBase& bound = value;
  return bound.value();
}

int32_t read_shape(Shape& value) {
  return value.area();
}

int32_t read_global() {
  return global_square.area();
}

int32_t read_concrete(StableMeasure& value) {
  return value.measure();
}

int32_t sum(std::vector<int32_t>& values) {
  mut int32_t total = 0;
  for (int32_t value : values) {
    total += value;
  }
  return total;
}

int32_t nested_sum(std::vector<int32_t>& values) {
  mut int32_t pair_total = 0;
  for (int32_t left : values) {
    for (int32_t right : values) {
      pair_total += left + right;
    }
  }

  mut int32_t triple_count = 0;
  for (int32_t first : values) {
    for (int32_t second : values) {
      for (int32_t third : values) { triple_count += 1; }
    }
  }
  return pair_total - 28 + triple_count - 8;
}

int32_t main() {
  mut std::vector<int32_t> values = std::vector<int32_t>();
  values.push_back(3);
  values.push_back(4);
  mut DispatchDerived derived = DispatchDerived();
  mut Square square = Square();
  mut Holder holder = Holder();
  mut GenericSquare<int32_t> generic = GenericSquare<int32_t>();
  mut Square returned = make_square();
  mut StableMeasure stable = StableMeasure();
  mut CheckedMeasure checked = CheckedMeasure();
  return sum(values) - 7 + nested_sum(values) + read_bound(derived) - 3 +
         read_base(derived) - 3 + read_shape(square) - 5 +
         square.area() - 5 + read_global() - 5 + holder.read() - 5 +
         generic.area() - 6 + returned.area() - 5 + derived.value() - 3 +
         square.plain() - 2 + read_concrete(stable) - 8 +
         read_concrete(checked) - 9;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-reference-range-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "reference-range.gti"
        source.write_text(source_text, encoding="utf8")
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"reference-range-{standard}-{optimization}"
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
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
