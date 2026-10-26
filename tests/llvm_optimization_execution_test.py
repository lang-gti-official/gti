#!/usr/bin/env python3
"""Native O0-O3 execution at contraction, IEEE and checked-failure boundaries."""

import argparse
import json
from pathlib import Path
import re
import subprocess

SUCCESS = '''double strict(double a, double b, double c) { return a * b + c; }
double divide(double a, double b) { return a / b; }
int main() {
  if (strict(1.000000007450580596923828125d, 0.999999992549419403076171875d, -1.0d) != 0.0d) { return 1; }
  double positive = divide(1.0d, 0.0d);
  double negative = divide(1.0d, -0.0d);
  double nan = divide(0.0d, 0.0d);
  if (positive < 1000000.0 || negative > -1000000.0 || nan == nan || nan < 0.0) { return 2; }
  mut int values[3] = {3, 4, 5};
  mut int& selected = values[1]; selected += 7;
  return values[1] == 11 ? 0 : 3;
}
'''
FAILURE = '''int checked(int value) { return value + 1; }
int main() { return checked(2147483647); }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gti", required=True, type=Path)
    parser.add_argument("--gti-build-cache", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    assert re.search(r"^CMAKE_BUILD_TYPE:STRING=(Release|RelWithDebInfo|MinSizeRel)$",
                     args.gti_build_cache.read_text(), re.M)
    assert args.gti.resolve().parent == args.gti_build_cache.resolve().parent
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    output.mkdir(parents=True)
    evidence = []
    for name, text in (("success", SUCCESS), ("failure", FAILURE)):
        source = output / f"{name}.gti"
        source.write_text(text)
        for level in range(4):
            binary = output / f"{name}-O{level}"
            command = [str(args.gti), str(source), "--backend", "llvm", f"-O{level}", "-o", str(binary)]
            built = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=180)
            (output / f"{name}-O{level}-build.log").write_text(built.stdout + built.stderr)
            assert built.returncode == 0
            result = subprocess.run([str(binary)], cwd=root, text=True, capture_output=True, timeout=15)
            (output / f"{name}-O{level}-run.log").write_text(result.stdout + result.stderr)
            assert result.returncode == (0 if name == "success" else 70) and not result.stdout
            assert result.stderr == ("" if name == "success" else
                "failure.gti:1: runtime error[GTI-R0001]: integer_overflow in addition\n")
            evidence.append({"case": name, "level": level, "command": command, "exit": result.returncode})
    (output / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print("PASS: native O0-O3 contraction, IEEE, place mutation and checked failure execution")


if __name__ == "__main__":
    main()
