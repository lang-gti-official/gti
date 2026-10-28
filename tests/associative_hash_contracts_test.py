#!/usr/bin/env python3
"""Source-defined hash boundaries and exact read-callable key contracts."""

import os
import pathlib
import subprocess
import sys
import tempfile

from lambda_pack_invocation_runtime_test import main as run_runtime_matrix


def reject_bad_contracts(compiler: pathlib.Path) -> None:
    cases = {
        "mutable-hash": "public: std::size_t operator()(int32_t& key) mut { return 0; }",
        "consuming-hash": "public: std::size_t operator()(int32_t& key) && { return 0; }",
        "wrong-result": "public: int32_t operator()(int32_t& key) { return 0; }",
        "wrong-key": "public: std::size_t operator()(int64_t& key) { return 0; }",
        "private-hash": "std::size_t operator()(int32_t& key) { return 0; }",
        "missing-hash": "",
    }
    prefix = """#include <std/functional>
std::size_t invoke<Hash>(Hash hash, int32_t& key)
  requires std::read_callable<Hash, int32_t, std::size_t> { return hash(key); }
"""
    with tempfile.TemporaryDirectory(prefix="gti-hash-negative-") as temporary:
        root = pathlib.Path(temporary)
        sources = {name: prefix + "class Bad { " + body + " };\n"
                   "int main() { int32_t key = 1; return int(invoke(Bad(), key)); }\n"
                   for name, body in cases.items()}
        sources["unspecialized"] = """#include <std/functional>
class Key { public: Key() = default; };
std::size_t invoke<Hash>(Hash hash, Key& key)
  requires std::read_callable<Hash, Key, std::size_t> { return hash(key); }
int main() {
  Key key{};
  std::hash<Key> unavailable{};
  return int(invoke(unavailable, key));
}
"""
        sources["mutable-equality"] = """#include <std/functional>
class Bad { public: bool operator()(int32_t& a, int32_t& b) mut { return true; }  public: Bad() = default; };
bool invoke<Equal>(Equal equal, int32_t& a, int32_t& b)
  requires std::read_binary_callable<Equal, int32_t, int32_t, bool> {
  return equal(a, b);
}
int main() { int32_t key = 1; if (invoke(Bad(), key, key)) { return 0; } return 1; }
"""
        for name, source in sources.items():
            path = root / f"{name}.gti"
            path.write_text(source)
            result = subprocess.run(
                [str(compiler), str(path), "--emit-mir", "-o", str(root / f"{name}.mir")],
                capture_output=True, text=True, timeout=60, check=False,
            )
            assert result.returncode == 65 and "error[GTI-S2029]" in result.stderr, (
                name, result.returncode, result.stdout, result.stderr)
            assert "GTI-B0001" not in result.stderr, (name, result.stderr)
    print("hash contracts: eight invalid callable shapes rejected before backend", flush=True)


if __name__ == "__main__":
    repository = pathlib.Path(__file__).resolve().parent.parent
    os.environ["GTI_STDLIB_PATH"] = str(repository / "stdlib")
    reject_bad_contracts(pathlib.Path(sys.argv[1]).resolve())
    run_runtime_matrix(names=("associative_hash_contracts",))
