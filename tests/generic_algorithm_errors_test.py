#!/usr/bin/env python3
"""Generic algorithm type/loan rejections and checked failure cleanup."""
import os
import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))
    headers = """#include <std/algorithm>
#include <std/vector>
#include <std/list>
#include <std/deque>
#include <std/array>
#include <std/set>
#include <std/string>
"""
    negatives = {
        "overlapping-transform": 'int main() { mut std::vector<int> x = {1}; std::transform(x, x, [](int& n) -> int { return n + 1; }); return 0; }',
        "readonly-transform": 'int main() { std::vector<int> x = {1}; std::vector<int> y = {0}; std::transform(x, y, [](int& n) -> int { return n; }); return 0; }',
        "wrong-transform-result": 'int main() { std::vector<int> x = {1}; mut std::vector<int> y = {0}; std::transform(x, y, [](int& n) -> double { return double(n); }); return 0; }',
        "wrong-element": 'int main() { std::list<int> x = {1}; double key = 1.0; return std::contains(x, key) ? 1 : 0; }',
        "bad-predicate": 'int main() { mut std::vector<int> x = {1}; int value = 2; std::replace_if(x, [](int& n) -> int { return n; }, value); return 0; }',
        "readonly-output": 'int main() { std::vector<int> x = {1}; std::vector<int> y = {0}; std::copy_n(x.begin(), std::size_t(1), y.begin()); return 0; }',
        "copy-owner": 'int main() { std::vector<std::string> x = {std::string("a")}; mut std::vector<std::string> y = {std::string("b")}; std::copy_n(x.begin(), std::size_t(1), y.begin_mut()); return 0; }',
        "overlapping-copy": 'int main() { mut std::vector<int> x = {1}; std::copy_n(x.begin(), std::size_t(1), x.begin_mut()); return 0; }',
        "immutable-keys": 'int main() { mut std::set<int> x = {1}; int old = 1; int replacement = 2; std::replace(x, old, replacement); return 0; }',
        "bad-comparison": 'struct Item { int x = 0; }; int main() { std::vector<Item> x = {Item()}; return std::equal(x.begin(), x.end(), x.begin(), x.end()) ? 1 : 0; }',
    }
    for container in ("list", "deque", "array"):
        init = f'mut std::{container}<int> x = {{1, 2}};' if container != "array" else 'mut std::array<int, 2> x{};'
        mutation = 'x.clear();' if container != "array" else 'x.fill(3);'
        negatives[f"{container}-retained"] = f'int main() {{ {init} auto found = std::min_element(x.begin(), x.end()); {mutation} return *found; }}'
        negatives[f"{container}-range-alias"] = f'int main() {{ {init} for (mut auto& element : x) {{ {mutation} element = 2; }} return 0; }}'
        cursor = f'std::detail::{container}_mut_iterator<int>' if container != "array" else 'std::detail::array_mut_iterator<int, 2>'
        negatives[f"{container}-cursor-escape"] = f'{cursor} dangling() {{ {init} return x.begin_mut(); }} int main() {{ return 0; }}'
        negatives[f"{container}-cursor-alias"] = f'int main() {{ {init} mut auto cursor = x.begin_mut(); {mutation} return *cursor; }}'
    with tempfile.TemporaryDirectory(prefix="gti-algorithm-errors-") as directory:
        root = pathlib.Path(directory)
        for name, source in negatives.items():
            path = root / f"{name}.gti"
            path.write_text(headers + source + "\n")
            checked = subprocess.run([str(compiler), str(path), "--emit-mir", "-o", str(root / "invalid.mir")],
                                     env=environment, capture_output=True, text=True, timeout=scaled(60))
            assert checked.returncode == 65 and "error[GTI-S" in checked.stderr, (name, checked)
            assert "GTI-B0001" not in checked.stderr, (name, checked.stderr)
            if name == "copy-owner":
                assert "error[GTI-S2003]" in checked.stderr and "Cannot copy assign" in checked.stderr, checked.stderr
        for level in ("O0", "O3"):
            binary = root / level
            build = subprocess.run([str(compiler), str(repository / "tests/fixtures/algorithm_failure.gti"),
                                    f"-{level}", "-o", str(binary)], env=environment, capture_output=True, text=True, timeout=scaled(120))
            assert build.returncode == 0, build
            for count in range(4):
                result = subprocess.run([str(binary), *(["fail"] * count)], env=environment,
                                        capture_output=True, text=True, timeout=scaled(30))
                assert result.returncode == 70 and result.stdout == "99\n", (count, result)
                assert "runtime error[GTI-R" in result.stderr, result
    print(f"generic algorithms: {len(negatives)} semantic rejections and eight failure cleanups passed")


if __name__ == "__main__":
    main()
