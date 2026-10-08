#!/usr/bin/env python3
"""Range-for over temporaries whose lexical cleanup is passive or active."""

import pathlib
import subprocess
import sys
import tempfile

SOURCE = """\
class Cursor {
  mut int32_t at;
public:
  Cursor(int32_t at) : at(at) {}
  int32_t& operator*() { return this.at; }
  void operator++() mut { this.at += 1; }
  bool operator!=(Cursor& end) { return this.at < end.at; }
};

// No destructor and only scalar fields: passive cleanup.
class Span {
  int32_t last;
public:
  Span(int32_t last) : last(last) {}
  Cursor begin() { return Cursor(0); }
  Cursor end() { return Cursor(this.last); }
};

// A source destructor makes the same range's cleanup active.
class CountedSpan {
  int32_t last;
public:
  CountedSpan(int32_t last) : last(last) {}
  ~CountedSpan() {}
  Cursor begin() { return Cursor(0); }
  Cursor end() { return Cursor(this.last); }
};

int32_t first_over(int32_t limit) {
  for (int32_t value : Span(10)) {
    if (value > limit) { return value; }
  }
  return -1;
}

int main() {
  mut int32_t total = 0;
  for (int32_t value : Span(4)) {
    if (value == 1) { continue; }
    if (value == 3) { break; }
    total += value;
  }
  for (int32_t value : CountedSpan(3)) { total += value; }
  for (int32_t value : Span(0)) { return 10 + value; }
  return total - 5 + first_over(6) - 7;
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: passive_range_temporary_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-passive-range-") as directory:
        root = pathlib.Path(directory)
        source = root / "main.gti"
        source.write_text(SOURCE, encoding="utf-8")
        for standard in ("c++20", "c++23"):
            for optimization in ("-O0", "-O3"):
                executable = root / f"main-{standard}{optimization}"
                built = subprocess.run(
                    [str(compiler), str(source), "--std", standard,
                     optimization, "-o", str(executable)],
                    text=True, capture_output=True, check=False)
                if built.returncode != 0:
                    sys.stderr.write(
                        f"build {standard} {optimization} failed:\n"
                        f"{built.stdout}{built.stderr}")
                    return 1
                ran = subprocess.run([str(executable)], text=True,
                                     capture_output=True, check=False)
                if ran.returncode != 0:
                    sys.stderr.write(
                        f"run {standard} {optimization} exited "
                        f"{ran.returncode}: {ran.stdout}{ran.stderr}")
                    return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
