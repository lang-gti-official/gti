#!/usr/bin/env python3
"""Native projected replacement, synchronized storage and intrinsic contracts."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from sanitizer_support import scaled_timeout


def run(command, environment):
    return subprocess.run(command, env=environment, capture_output=True,
                          text=True, timeout=scaled_timeout(120), check=False)


def require(process, label):
    assert process.returncode == 0, (
        f"{label}: {process.returncode}\n{process.stdout}\n{process.stderr}")


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-backend-contracts-") as temporary:
        root = Path(temporary)
        library = root / "stdlib"
        shutil.copytree(repository / "stdlib", library)
        prelude = library / "prelude.gti"
        declaration = "  void require_key(bool found);"
        text = prelude.read_text()
        assert text.count(declaration) == 1
        prelude.write_text(text.replace(declaration,
                                       "  [[track_caller]] void require_key(bool found);"))
        capabilities = library / "std/detail/memory_capabilities.gti"
        text = capabilities.read_text()
        for spelling in ("export uint64_t object_size<T>();",
                         "export uint64_t object_alignment<T>();",
                         "export void object_construct<T, Args...>(T* address, Args... args);"):
            assert text.count(spelling) == 1
            text = text.replace(spelling, spelling.replace("export ", "export [[track_caller]] ", 1))
        capabilities.write_text(text)
        mapping = library / "std/map.gti"
        text = mapping.read_text()
        assert text.count("  V& at(K& key)") == 1
        assert text.count("  mut V& at(K& key)") == 1
        mapping.write_text(text.replace("  V& at(K& key)",
                                        "  [[track_caller]] V& at(K& key)")
                          .replace("  mut V& at(K& key)",
                                   "  [[track_caller]] mut V& at(K& key)"))
        environment = dict(os.environ, GTI_STDLIB_PATH=str(library),
                           ASAN_OPTIONS="halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        success = root / "intrinsic-success.gti"
        success.write_text("import std.map;\nint main() { mut std::map<int, int> m{}; "
                           "int key = 3; m.insert(key, 7); return m.at(key) == 7 ? 0 : 1; }\n")
        failure = root / "intrinsic-failure.gti"
        failure.write_text("import std.map;\nint main() { mut std::map<int, int> m{};\n"
                           "  int key = 3;\n  return m.at(key);\n}\n")
        ordinary = root / "ordinary-contract.gti"
        ordinary.write_text("namespace user { [[track_caller]] int require_key(bool value) "
                            "{ return value ? 9 : 7; } }\n"
                            "int main() { return user::require_key(false) == 7 ? 0 : 1; }\n")
        live_guard = root / "live-guard.gti"
        live_guard.write_text(
            "import std.vector; import std.mutex;\n"
            "int main() { mut std::vector<std::mutex<int>> v{}; "
            "v.push_back(std::mutex<int>(3)); "
            "auto guard = v[std::size_t(0)].lock(); "
            "v.push_back(std::mutex<int>(4)); return guard.get(); }\n")
        cases = [(repository / "tests/fixtures/projected_move_assignment.gti", []),
                 (repository / "tests/fixtures/projected_move_user.gti", []),
                 (repository / "tests/fixtures/mutex_vector_storage.gti",
                  ["--execution-profile", "concurrent"]),
                 (success, []), (ordinary, [])]
        modes = [("llvm", "O0", []), ("llvm", "O3", []),
                 ("cpp", "O0", ["--std", "c++20"]),
                 ("cpp", "O3", ["--std", "c++23"])]
        for backend, optimization, extra in modes:
            label = f"{backend}-{optimization}"
            for source, flags in cases:
                executable = root / f"{source.stem}-{label}"
                require(run([str(compiler), str(source), "--backend", backend,
                             f"-{optimization}", *extra, *flags, "-o", str(executable)],
                            environment), f"{source.name} {label} build")
                require(run([str(executable)], environment),
                        f"{source.name} {label} execution")
            source = repository / "tests/fixtures/projected_move_failure.gti"
            executable = root / f"projected-failure-{label}"
            require(run([str(compiler), str(source), "--backend", backend,
                         f"-{optimization}", *extra, "-o", str(executable)],
                        environment), f"projected failure {label} build")
            result = run([str(executable)], environment)
            assert (result.returncode, result.stdout) == (70, "2\n1\n"), result
            assert "GTI-R0007" in result.stderr and "index_out_of_bounds" in result.stderr, result.stderr
            executable = root / f"failure-{label}"
            require(run([str(compiler), str(failure), "--backend", backend,
                         f"-{optimization}", *extra, "-o", str(executable)], environment),
                    f"tracked intrinsic {label} build")
            result = run([str(executable)], environment)
            assert result.returncode == 70, (label, result.returncode, result.stderr)
            assert ("intrinsic-failure.gti:4: runtime error[GTI-R0015]: "
                    "key_not_found in key_lookup\n") in result.stderr, result.stderr
            assert "map.gti:" not in result.stderr, result.stderr
            rejected = run([str(compiler), str(live_guard), "--backend", backend,
                            "--execution-profile", "concurrent", "-O3",
                            "-o", str(root / "invalid")], environment)
            assert rejected.returncode != 0, rejected.stderr
            assert ("error[GTI-S2017]: Mutable method cannot use move-only storage "
                    "while a reference borrowed from it may still be live.") in rejected.stderr, rejected.stderr
            assert "GTI-B000" not in rejected.stderr, rejected.stderr
            print(f"{label}: projected moves, mutex growth/cleanup, intrinsic attribution pass")
        for source, flags in cases[:2]:
            executable = root / f"{source.stem}-sanitized"
            require(run([str(compiler), str(source), "--backend", "cpp", "-O1",
                         *flags, "-o", str(executable), "--",
                         "-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
                        environment), f"{source.name} sanitizer build")
            require(run([str(executable)], environment),
                    f"{source.name} sanitizer execution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
