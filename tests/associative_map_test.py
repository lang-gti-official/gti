#!/usr/bin/env python3
"""Ordered dictionary ownership, ordering, mutation and AVL differential tests."""

import os
import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def reject_invalid_sources(compiler: pathlib.Path) -> None:
    cases = {
        "immutable-key": ("""int main() {
  mut std::map<int, int> values{};
  [[discard]] values.insert(1, 2);
  for (mut auto& entry : values) { entry.first = 9; }
  return 0;
}""", "GTI-S2000"),
        "live-iterator": ("""int main() {
  mut std::map<int, int> values{};
  [[discard]] values.insert(1, 2);
  auto first = values.begin();
  values.clear();
  return (*first).second;
}""", "GTI-S2017"),
        "live-value": ("""int main() {
  mut std::map<int, int> values{};
  [[discard]] values.insert(1, 2);
  int key = 1;
  int& value = values.at(key);
  [[discard]] values.erase(key);
  return value;
}""", "GTI-S2017"),
        "copy-container": ("""int main() {
  std::map<int, int> values{};
  auto copied = values;
  return int(copied.size());
}""", "GTI-S2003"),
        "mutable-comparator": ("""class Compare {
public: bool operator()(int& a, int& b) mut { return a < b; }
public:
  Compare() = default;
};
int main() { std::map<int, int, Compare> values{}; return 0; }
""", "GTI-S2029"),
        "wrong-comparator-result": ("""class Compare {
public: int operator()(int& a, int& b) { return 0; }
public:
  Compare() = default;
};
int main() { std::map<int, int, Compare> values{}; return 0; }
""", "GTI-S2029"),
        "copy-exclusive-iterator": ("""int main() {
  mut std::map<int, int> values{};
  auto first = values.begin_mut();
  auto copied = first;
  return (*copied).second;
}""", "GTI-S2003"),
        "overlap-exclusive-iterator": ("""int main() {
  mut std::map<int, int> values{};
  auto first = values.begin_mut();
  [[discard]] values.size();
  return (*first).second;
}""", "GTI-S2017"),
        "nested-result-invalidation": ("""int main() {
  mut std::map<int, int> values{};
  auto result = values.insert(1, 2);
  values.clear();
  return result.first->second;
}""", "GTI-S2017"),
        "copy-nested-exclusive-result": ("""int main() {
  mut std::map<int, int> values{};
  auto result = values.insert(1, 2);
  auto copied = result;
  return copied.first->second;
}""", "GTI-S2003"),
        "nested-result-key-immutable": ("""int main() {
  mut std::map<int, int> values{};
  mut auto result = values.insert(1, 2);
  result.first->first = 3;
  return 0;
}""", "GTI-S2000"),
    }
    with tempfile.TemporaryDirectory(prefix="gti-map-negative-") as temporary:
        root = pathlib.Path(temporary)
        for name, (body, code) in cases.items():
            path = root / f"{name}.gti"
            path.write_text("#include <std/map>\n" + body)
            result = subprocess.run(
                [str(compiler), str(path), "--emit-mir", "-o", str(root / f"{name}.mir")],
                capture_output=True, text=True, timeout=60, check=False,
            )
            assert result.returncode == 65 and f"error[{code}]" in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
    print(f"map: {len(cases)} invalid ownership/callable cases rejected", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reject_invalid_sources(pathlib.Path(sys.argv[1]).resolve())
    run_runtime_matrix(names=("associative_map", "associative_map_balancing",
                              "associative_map_iteration", "associative_map_ownership",
                              "associative_map_insert_result"))
