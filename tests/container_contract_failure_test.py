#!/usr/bin/env python3
"""Checked container contracts preserve precise outcomes and owned cleanup."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    repository = Path(__file__).resolve().parent.parent
    compiler = Path(sys.argv[1]).resolve()
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    prefix = """#include <std/memory>
#include <std/string>
#include <std/unordered_map>
#include <std/cmath>
class Guard {
  std::unique_ptr<int> token;
public:
  Guard() : token(std::make_unique<int>(7)) {}
  ~Guard() { if (this.token != nullptr) std::println("cleaned"); }
};
"""
    cases = {
        "argument-check": ("mut std::unordered_map<int, int> values{}; values.max_load_factor(0.0);", 16,
                           "invalid_container_argument", "container_argument"),
        "map-missing": ("std::map<int, int> values{}; int key = 1; [[discard]] values.at(key);",
                        15, "key_not_found", "key_lookup"),
        "map-arrow-overflow": ("mut std::map<int, int> values{}; mut auto result = values.insert(1, 2147483647); result.first->second() += 1;",
                               1, "integer_overflow", "addition"),
        "unordered-missing": ("std::unordered_map<int, int> values{}; int key = 1; [[discard]] values.at(key);",
                              15, "key_not_found", "key_lookup"),
        "unordered-end-dereference": ("std::unordered_map<int, int> values{}; auto end = values.begin(); int value = (*end).second;",
                                      16, "invalid_container_argument", "container_argument"),
        "unordered-end-increment": ("std::unordered_map<int, int> values{}; mut auto end = values.begin(); ++end;",
                                    16, "invalid_container_argument", "container_argument"),
        "success": ("mut std::unordered_map<int, int> values{}; values.max_load_factor(0.5); [[discard]] values.insert(1, 2); int key = 1; [[discard]] values.at(key);",
                    0, "", ""),
    }
    for label, value in (("negative", "-0.5"), ("nan", "std::sqrt(-1.0)"),
                         ("infinity", "std::exp(1000.0)")):
        cases[f"argument-{label}"] = (
            f"mut std::unordered_map<int, int> values{{}}; values.max_load_factor({value});",
            16, "invalid_container_argument", "container_argument")
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    selected = [sys.argv[index + 1] for index, value in enumerate(sys.argv) if value == "--mode"]
    if selected:
        modes = [mode for mode in modes if mode[0] in selected]
    with tempfile.TemporaryDirectory(prefix="gti-container-contract-") as temporary:
        root = Path(temporary)
        for expression in ("gti_internal::require_key(1)",
                           "gti_internal::require_container_argument()",
                           "gti_internal::require_key(true, false)"):
            source = root / "invalid.gti"
            source.write_text(f"int main() {{ {expression}; return 0; }}")
            result = subprocess.run([str(compiler), str(source), "--emit-mir", "-o", str(root / "invalid.mir")],
                                    capture_output=True, text=True, timeout=90, env=environment)
            assert result.returncode == 65 and "GTI-S" in result.stderr and "GTI-B0001" not in result.stderr, result
        for name, (statement, code, category, detail) in cases.items():
            source = root / f"{name}.gti"
            includes = "#include <std/map>\n" if name.startswith("map-") else ""
            source.write_text(includes + prefix + f"int main() {{ Guard guard{{}}; {statement} return 0; }}\n")
            for label, flags, native in modes:
                executable = root / f"{name}-{label}"
                command = [str(compiler), str(source), *flags, "-o", str(executable)]
                if native:
                    command += ["--", *native]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
                assert result.returncode == 0, (name, label, result.stdout, result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30, env=environment)
                assert result.returncode == (70 if code else 0) and result.stdout == "cleaned\n", (name, label, result)
                if code:
                    assert re.fullmatch(rf".+:\d+: runtime error\[GTI-R{code:04d}\]: {category} in {detail}\n", result.stderr), result.stderr
                else:
                    assert result.stderr == "", result.stderr
                print(f"container contract {name}: {label} passed", flush=True)


if __name__ == "__main__":
    main()
