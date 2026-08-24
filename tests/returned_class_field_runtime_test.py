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
        raise SystemExit("usage: returned_class_field_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source_text = """\
struct Packet {
  int32_t value;
  Packet(int32_t initial) : value(initial) {}
};

Packet make_packet(int32_t value) {
  return Packet(value);
}

int32_t read_packet_value() {
  return make_packet(5).value;
}

struct Factory {
  Packet make(int32_t value) {
    return Packet(value);
  }
};

class Cell<std::default_initializable T> {
public:
  Cell() {}
  mut T value = T();
};

int32_t main() {
  int32_t returned = read_packet_value();
  int32_t direct = make_packet(7).value;
  Factory factory{};
  int32_t method = factory.make(11).value;
  int32_t larger = int32_t(1) + make_packet(12).value;
  mut Cell<Cell<int32_t>> nested = Cell<Cell<int32_t>>();
  return (returned - 5) + (direct - 7) + (method - 11) + (larger - 13) +
         nested.value.value;
}
"""

    with tempfile.TemporaryDirectory(prefix="gti-returned-class-field-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "returned-class-field.gti"
        source.write_text(source_text, encoding="utf8")
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"returned-field-{standard}-{optimization}"
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
