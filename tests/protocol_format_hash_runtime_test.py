#!/usr/bin/env python3
"""Source protocol clients, explicit legacy hash state, and constraint failures."""
import json
import pathlib
import subprocess
import sys
import tempfile

from backend_modes import cpp_modes

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER = pathlib.Path(sys.argv[1]).resolve()


def invoke(args, *, success=True, expected=None):
    result = subprocess.run(args, text=True, capture_output=True, timeout=180)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != success or (expected and expected not in output):
        raise AssertionError(f"command: {args}\n{output}")
    return result.stdout


with tempfile.TemporaryDirectory(prefix="gti-protocol-format-hash-") as temporary:
    directory = pathlib.Path(temporary)
    for fixture, output in (("tests/fixtures/protocol_format_hash.gti", "(8, 9)\n"),
                            ("examples/189-protocol-format-hash.gti", "(1, true)\nbox(7)\n")):
        for optimization in ("-O0", "-O3"):
            executable = directory / (pathlib.Path(fixture).stem + optimization)
            invoke([str(COMPILER), str(ROOT / fixture), "--backend", "llvm", optimization,
                    "-o", str(executable)])
            actual = invoke([str(executable)])
            if actual != output:
                raise AssertionError(f"unexpected output: {actual!r}")
    legacy_fixture = ROOT / "tests/fixtures/legacy_hash_policy_state.gti"
    # The C++ backend joins only when GTI_TEST_BACKENDS selects it.
    for backend in (("llvm", "cpp") if cpp_modes() else ("llvm",)):
        for optimization in ("-O0", "-O3"):
            executable = directory / ("legacy-hash-state-" + backend + optimization)
            invoke([str(COMPILER), str(legacy_fixture), "--backend", backend, optimization,
                    "-o", str(executable)])
            assert invoke([str(executable)]) == ""
    legacy_prefix = legacy_fixture.read_text().split("int main() {", 1)[0]
    migration_hint = "std::unordered_map<K, V, std::hash<K>>"
    for label, statement in {
        "legacy-default-map": "std::unordered_map<SeededKey, int> values{};",
        "legacy-default-set": "std::unordered_set<SeededKey> values{};",
        "legacy-default-call": "SeededKey key{7}; std::default_hash<SeededKey> hash{}; hash(key);",
        "legacy-pair-map": "std::unordered_map<Pair, int> values{};",
        "legacy-pair-set": "std::unordered_set<Pair> values{};",
    }.items():
        source = directory / (label + ".gti")
        source.write_text(legacy_prefix + "int main() { " + statement + " return 0; }\n")
        rejected = subprocess.run([str(COMPILER), str(source), "--emit-mir", "-o",
                                   str(directory / (label + ".mir"))],
                                  text=True, capture_output=True, timeout=90)
        assert rejected.returncode == 65 and "GTI-S2029" in rejected.stderr, rejected
        assert (migration_hint in rejected.stderr and
                "std::unordered_set<K, std::hash<K>>" in rejected.stderr), rejected.stderr
        if label.startswith("legacy-pair"):
            assert "explicit composite Hash policy" in rejected.stderr, rejected.stderr
    assertion_fixture = ROOT / "tests/fixtures/protocol_testing_assertions.gti"
    assertion_line = next(index for index, line in
                          enumerate(assertion_fixture.read_text().splitlines(), 1)
                          if '"protocol-only assertion"' in line)
    for optimization in ("-O0", "-O3"):
        executable = directory / ("protocol-testing" + optimization)
        invoke([str(COMPILER), str(assertion_fixture), "--backend", "llvm", optimization,
                "-o", str(executable)])
        failed = subprocess.run([str(executable)], text=True, capture_output=True, timeout=30)
        assert failed.returncode == 70 and not failed.stdout, failed
        assert (f"{assertion_fixture.name}:{assertion_line}:" in failed.stderr
                and "GTI-R0017" in failed.stderr), failed.stderr
        message = json.loads(failed.stderr.split("panic: ", 1)[1].strip())
        assert message == "protocol-only assertion\n  actual: 3\nexpected: 4", message
    failures = {
        "format": ("import std.format; import std.utility; struct Opaque { Opaque() = default; }; "
                   "int main() { auto p = std::make_pair(Opaque(), 1); "
                   "std::println(\"{}\", p); return 0; }", "std::formattable"),
        "hash": ("import std.functional; import std.utility; struct Opaque { Opaque() = default; }; "
                 "int main() { auto p = std::make_pair(Opaque(), 1); "
                 "std::default_hash<std::pair<Opaque, int>> h{}; h(p); return 0; }", "std::hashing"),
        "map_key": ("import std.unordered_map; import std.utility; struct Opaque { Opaque() = default; "
                    "bool operator==(Opaque& other) { return true; } "
                    "bool operator!=(Opaque& other) { return false; } }; "
                    "int main() { std::unordered_map<std::pair<Opaque, int>, int> values{}; "
                    "return 0; }", "std::read_callable"),
        "specialization": ("import std.format; import std.utility; class std::formatter<std::pair<T, U>> {}; "
                           "int main() { return 0; }", "GTI-S2078"),
    }
    for name, (source, diagnostic) in failures.items():
        path = directory / (name + ".gti")
        path.write_text(source)
        invoke([str(COMPILER), str(path), "--emit-mir", "-o", str(directory / (name + ".mir"))],
               success=False, expected=diagnostic)
print("protocol formatting/hashing: fixtures/example/assertions LLVM O0/O3, seeded legacy policies LLVM/C++ O0/O3, 9 rejections passed")
