#!/usr/bin/env python3
"""Run the domain-value operator capability family end to end."""

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


SOURCE = """\
#include <std/numeric>

class vec3 {
public:
  vec3(float x, float y, float z) : x(x), y(y), z(z) {}
  float x;
  float y;
  float z;

  vec3 operator+(vec3& other) {
    return vec3(this.x + other.x, this.y + other.y, this.z + other.z);
  }
  vec3 operator-(vec3& other) {
    return vec3(this.x - other.x, this.y - other.y, this.z - other.z);
  }
  vec3 operator-() { return vec3(-this.x, -this.y, -this.z); }
  vec3 operator*(float scale) {
    return vec3(this.x * scale, this.y * scale, this.z * scale);
  }
};

// A second domain type over a different scalar proves the family is not
// specialised to one shape.
class span64 {
public:
  span64(int64_t lo, int64_t hi) : lo(lo), hi(hi) {}
  int64_t lo;
  int64_t hi;

  span64 operator+(span64& o) { return span64(this.lo + o.lo, this.hi + o.hi); }
  span64 operator-(span64& o) { return span64(this.lo - o.lo, this.hi - o.hi); }
  span64 operator-() { return span64(-this.lo, -this.hi); }
  span64 operator*(int64_t s) { return span64(this.lo * s, this.hi * s); }
};

// One generic body, two instantiations over different domain types
// and different scalars.
T midpoint<T, Scalar>(T& from, T& to, Scalar half)
  requires std::vector_space<T, Scalar> {
  return std::lerp(from, to, half);
}

int main() {
  vec3 origin = vec3(0.0, 0.0, 0.0);
  vec3 target = vec3(10.0, 20.0, 30.0);

  vec3 half = std::lerp(origin, target, 0.5);
  if (half.x != 5.0 || half.y != 10.0 || half.z != 15.0) {
    return 1;
  }

  // Endpoints are exactly the inputs' arithmetic, not a promised identity.
  vec3 start = std::lerp(origin, target, 0.0);
  vec3 finish = std::lerp(origin, target, 1.0);
  if (start.x != 0.0 || finish.z != 30.0) {
    return 2;
  }

  // Extrapolation is ordinary arithmetic, not clamped.
  vec3 beyond = std::lerp(origin, target, 2.0);
  if (beyond.x != 20.0) {
    return 3;
  }

  vec3 opposite = std::negated(target);
  if (opposite.x != -10.0 || opposite.y != -20.0 || opposite.z != -30.0) {
    return 4;
  }

  // A different domain type and scalar through the same generic body.
  span64 low = span64(int64_t(0), int64_t(10));
  span64 high = span64(int64_t(100), int64_t(110));
  span64 stepped = midpoint(low, high, int64_t(1));
  if (stepped.lo != int64_t(100) || stepped.hi != int64_t(110)) {
    return 5;
  }

  span64 flipped = std::negated(high);
  if (flipped.lo != int64_t(-100)) {
    return 6;
  }

  // Built-in floating point uses the exact overloads, not the member family.
  float quarter = std::lerp(0.0, 8.0, 0.25);
  if (quarter != 2.0) {
    return 7;
  }

  double wide = std::lerp(0.0d, 100.0d, 0.75d);
  if (wide != 75.0d) {
    return 8;
  }
  return 0;
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: domain_operator_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-domain-operator-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "domain-operator.gti"
        source.write_text(SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"domain-operator-{standard}-{optimization}"
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
                    return fail(built)

                executed = run([str(executable)])
                if executed.returncode != 0:
                    sys.stderr.write(
                        "domain-value operator behavior failed for "
                        f"{standard} {optimization}: exit={executed.returncode} "
                        f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                    )
                    return 1

    print("domain-value operator capability runtime behavior verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
