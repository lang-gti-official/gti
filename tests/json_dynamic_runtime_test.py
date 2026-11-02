#!/usr/bin/env python3
"""Owned dynamic JSON: native round trips, cleanup, rejection and checked access."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def run(command, env, cwd, *, code=0):
    result = subprocess.run(list(map(str, command)), env=env, cwd=cwd,
                            capture_output=True, text=True, timeout=180)
    if result.returncode != code:
        raise AssertionError(f"{command}: exit {result.returncode}, wanted {code}\n"
                             f"{result.stdout}{result.stderr}")
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parents[1]
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"))
    with tempfile.TemporaryDirectory(prefix="gti-dynamic-json-") as temporary:
        work = Path(temporary)
        fixture = root / "tests/fixtures/json_dynamic.gti"
        modes = [("llvm-O0", ["--backend", "llvm", "-O0"], []),
                 ("llvm-O3", ["--backend", "llvm", "-O3"], []),
                 ("cpp20-O0", ["--std", "c++20", "-O0"], []),
                 ("cpp23-O3", ["--std", "c++23", "-O3"], []),
                 ("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
        for name, flags, native in modes:
            binary = work / name
            run([compiler, fixture, *flags, "-o", binary, *native], env, work)
            result = run([binary], env, work)
            assert not result.stdout and not result.stderr, result
            print(f"PASS {name}", flush=True)

        bad_access = work / "bad-access.gti"
        bad_access.write_text('''import std.json;
import std.string;
import std.vector;
int main(int argc, std::vector<std::string> argv) {
  if (argv[std::size_t(1)] == "kind") {
    auto value = std::json::value();
    return value.as_bool() ? 1 : 2;
  }
  if (argv[std::size_t(1)] == "key") {
    auto value = std::json::value::object();
    std::string key{"missing"};
    return value.at(key).is_null() ? 1 : 2;
  }
  auto value = std::json::value::array();
  return value.at(std::size_t(0)).is_null() ? 1 : 2;
}
''')
        binary = work / "bad-access"
        run([compiler, bad_access, "--backend", "llvm", "-O3", "-o", binary], env, work)
        for mode in ("kind", "key", "index"):
            result = run([binary, mode], env, work, code=70)
            assert "GTI-R" in result.stderr, result
        print("PASS checked kind/key/index failures", flush=True)

        borrowed = work / "borrowed.gti"
        borrowed.write_text('''import std.json;
int main() {
  mut auto values = std::json::value::array();
  values.push_back(std::json::value(1));
  auto& first = values.at(std::size_t(0));
  values.push_back(std::json::value(2));
  return int(first.as_int64());
}
''')
        result = subprocess.run([str(compiler), str(borrowed), "--emit-mir", "-o", str(work / "bad.mir")],
                                env=env, cwd=work, capture_output=True, text=True, timeout=90)
        assert result.returncode != 0 and "borrow" in result.stderr.lower(), result
        print("PASS live child borrow prevents structural mutation", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
