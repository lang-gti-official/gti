#!/usr/bin/env python3
"""Hash-table ownership, collisions, iteration and invalidation checks."""

import os
import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def reject_invalid_sources(compiler: pathlib.Path) -> None:
    prefix = "#include <std/unordered_map>\n#include <std/functional>\n"
    cases = {
        "immutable-key": """int main() {
  mut std::unordered_map<int, int> values{};
  [[discard]] values.insert(1, 2);
  for (mut auto& entry : values) { entry.first = 9; }
  return 0;
}""",
        "live-iterator": """int main() {
  mut std::unordered_map<int, int> values{};
  [[discard]] values.insert(1, 2);
  auto first = values.begin();
  values.clear();
  return (*first).second;
}""",
        "live-value": """int main() {
  mut std::unordered_map<int, int> values{};
  [[discard]] values.insert(1, 2);
  int key = 1;
  int& value = values.at(key);
  [[discard]] values.erase(key);
  return value;
}""",
        "copy-container": """int main() {
  std::unordered_map<int, int> values{};
  auto copy = values;
  return int(copy.size());
}""",
        "live-insertion-result": """int main() {
  mut std::unordered_map<int, int> values{};
  auto result = values.insert(1, 2);
  values.rehash(std::size_t(100));
  return result.first->second;
}""",
        "copy-insertion-result": """int main() {
  mut std::unordered_map<int, int> values{};
  auto result = values.insert(1, 2);
  auto copied = result;
  return copied.first->second;
}""",
        "immutable-result-key": """int main() {
  mut std::unordered_map<int, int> values{};
  mut auto result = values.insert(1, 2);
  result.first->first = 3;
  return 0;
}""",
        "mutable-hasher": """class Hash {
public: std::size_t operator()(int& key) mut { return 0; }
};
int main() { std::unordered_map<int, int, Hash> values{}; return 0; }
""",
        "missing-hasher": """class Key {};
class Equal { public: bool operator()(Key& left, Key& right) { return true; } };
int main() { std::unordered_map<Key, int, std::hash<Key>, Equal> values{}; return 0; }
""",
    }
    with tempfile.TemporaryDirectory(prefix="gti-unordered-negative-") as temporary:
        root = pathlib.Path(temporary)
        for name, body in cases.items():
            path = root / f"{name}.gti"
            path.write_text(prefix + body)
            result = subprocess.run(
                [str(compiler), str(path), "--emit-mir", "-o", str(root / f"{name}.mir")],
                capture_output=True, text=True, timeout=60, check=False,
            )
            assert result.returncode == 65 and "error[GTI-S" in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
    print(f"unordered_map: {len(cases)} invalid ownership/callable cases rejected", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reject_invalid_sources(pathlib.Path(sys.argv[1]).resolve())
    run_runtime_matrix(names=("unordered_map_core", "unordered_map_iteration",
                              "unordered_map_ownership", "unordered_map_collisions",
                              "unordered_map_load_factor", "unordered_map_insert_result"))
