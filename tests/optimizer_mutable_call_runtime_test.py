#!/usr/bin/env python3
"""Mutation through staged calls must invalidate stale arithmetic proofs."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


PRELUDE = """#include <std/string>
class Guard {
public:
  Guard() {}
  ~Guard() { std::println("cleaned"); }
};
void setmax(mut int& value) { value = 2147483647; }
void relay(mut int& value) { setmax(value); }
mut int shared = 0;
void setglobal() { shared = 2147483647; }
class Writer {
  mut int& value;
public:
  Writer(mut int& source) : value(source) {}
  void setmax() mut { mut int& target = this.value; target = 2147483647; }
};
"""

CASES = {
    "direct": "mut int total = 0; setmax(total);\n  return total + 1;",
    "forwarded": "mut int total = 0; relay(total);\n  return total + 1;",
    "projected": "mut int values[2] = {0, 0}; setmax(values[1]);\n"
                 "  return values[1] + 1;",
    "carrier": "mut int total = 0;\n"
               "  { mut Writer writer{total}; writer.setmax(); }\n"
               "  return total + 1;",
    "carrier_reference": "mut int total = 0;\n"
                         "  { mut Writer writer{total}; mut Writer& alias = writer;\n"
                         "    alias.setmax(); }\n  return total + 1;",
    "local_reference": "mut int total = 0;\n"
                       "  { mut int& alias = total; setmax(alias); }\n"
                       "  return total + 1;",
    "global": "shared = 0; setglobal();\n  return shared + 1;",
}


def main():
    compiler = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-mutable-call-proof-") as directory:
        root = Path(directory)
        for name, body in CASES.items():
            text = PRELUDE + "int main() {\n  Guard guard{};\n  " + body + "\n}\n"
            source = root / f"{name}.gti"
            source.write_text(text)
            failure_line = next(index for index, line in enumerate(text.splitlines(), 1)
                                if "return " in line)
            for standard in ("c++20", "c++23"):
                for level in ("O0", "O2", "O3"):
                    label = f"{name}-{standard}-{level}"
                    executable = root / label
                    built = subprocess.run(
                        [str(compiler), str(source), "--std", standard,
                         f"-{level}", "-o", str(executable)],
                        capture_output=True, text=True, timeout=scaled(120), check=False)
                    assert built.returncode == 0, (label, built.stdout, built.stderr)
                    result = subprocess.run([str(executable)], capture_output=True,
                                            text=True, timeout=scaled(30), check=False)
                    assert (result.returncode, result.stdout) == (70, "cleaned\n"), (
                        label, result.returncode, result.stdout, result.stderr)
                    expected = (rf"{re.escape(source.name)}:{failure_line}: runtime "
                                r"error\[GTI-R0001\]: integer_overflow in addition\n")
                    assert re.fullmatch(expected, result.stderr), (label, result.stderr)
            print(f"mutable-call arithmetic proofs: {name} passed (6 modes)", flush=True)


if __name__ == "__main__":
    main()
