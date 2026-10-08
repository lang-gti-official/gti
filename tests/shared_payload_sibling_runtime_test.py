#!/usr/bin/env python3
"""Nested read-only reborrows beside live read-only payload siblings."""

import pathlib
import subprocess
import sys
import tempfile

SOURCE = """\
enum Event<T> { item(T value) };
class Inner {
public:
  Event<int32_t> data;
  Inner(Event<int32_t> data) : data(data) {}
};
enum Pair<T> { items(T first, T second), other(T first, T second) };

enum class Tag { small(int32_t value), big(int64_t value) };
class Cell {
public:
  Tag tag;
  int32_t weight;
  Cell(Tag tag, int32_t weight) : tag(tag), weight(weight) {}
};
enum class Trio { all(Cell a, Cell b, Cell c), none };

int32_t generic_pair() {
  Inner left = Inner(Event<int32_t>::item(1));
  Inner right = Inner(Event<int32_t>::item(2));
  Pair<Inner> pair = Pair<Inner>::items(left, right);
  switch (pair) {
    case Pair<Inner>::items(auto& first, auto& second):
      switch (first.data) {
        case Event<int32_t>::item(auto& value):
          if (value != 1) { return 1; }
          break;
      }
      switch (second.data) {
        case Event<int32_t>::item(value): return value - 2;
      }
    case Pair<Inner>::other: return 3;
  }
}

int32_t three_siblings() {
  Cell x = Cell(Tag::small(1), 2);
  Cell y = Cell(Tag::big(int64_t(3)), 4);
  Cell z = Cell(Tag::small(5), 6);
  Trio trio = Trio::all(x, y, z);
  switch (trio) {
    case Trio::all(auto& a, auto& b, auto& c):
      switch (a.tag) {
        case Tag::small(auto& v): if (v != 1) { return 1; } break;
        case Tag::big(auto& v): return 2;
      }
      switch (b.tag) {
        case Tag::small(auto& v): return 3;
        case Tag::big(auto& v): return int32_t(v) + a.weight + c.weight - 11;
      }
    case Trio::none: return 4;
  }
}

int main() { return generic_pair() + three_siblings(); }
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: shared_payload_sibling_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-payload-siblings-") as directory:
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
