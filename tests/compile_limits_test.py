#!/usr/bin/env python3
"""Per-request compile limits through every configuration surface.

One CompileLimits value configures a compilation: `--<limit> <n>` overrides
the root package's gti.toml `[limits]` table, which overrides the defaults.
Dependency manifests are validated but never applied. The same resolution
reaches project builds, their build reports and gti_lsp.
"""

import json
import pathlib
import subprocess
import sys
import tempfile

from lsp_project_facts_test import diagnostics_for, initialize, open_document
from lsp_smoke_test import LspSession

LIMIT_KEYS = (
    "constexpr-steps",
    "constexpr-depth",
    "macro-depth",
    "macro-expansions",
    "constexpr-for-elements",
    "constexpr-for-copies",
)

# A user-defined accumulator: the limit is a language rule, not a library one.
SPIN = (
    "struct Tally {\n"
    "  uint64_t count;\n"
    "  constexpr Tally(uint64_t c) : count(c) {}\n"
    "};\n"
    "constexpr Tally spin(uint64_t n) {\n"
    "  mut uint64_t i = 0;\n"
    "  while (i < n) { i = i + 1; }\n"
    "  return Tally(i);\n"
    "}\n"
)


def program(iterations):
    return (SPIN + f"static_assert(spin(uint64_t({iterations})).count == "
            f"uint64_t({iterations}));\nint main() {{ return 0; }}\n")


def run(command, cwd, status=0):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    assert result.returncode == status, (
        command, result.returncode, result.stdout[-2000:], result.stderr[-4000:])
    return result


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def manifest(name, limits=None, extra=""):
    text = (f'manifest-version = 1\n[package]\nname = "{name}"\n'
            f'version = "1.0.0"\n[targets.{name}]\nkind = "executable"\n'
            f'root = "src/main.gti"\n{extra}')
    if limits is not None:
        text += "[limits]\n" + limits
    return text


def exhausted(stderr, limit, function="spin"):
    return ("GTI-S2057" in stderr and
            f"used all {limit} steps of its constant-evaluation "
            f"execution-step limit while executing '{function}'" in stderr and
            f"The limit is {limit}; raise it with '--constexpr-steps <n>' or "
            "with 'constexpr-steps = <n>' in the [limits] table of the root "
            "package's gti.toml." in stderr)


def test_direct(gti, root):
    usage = run([gti, "--help"], root).stdout
    for key in LIMIT_KEYS:
        assert f"--{key} <n>" in usage, (key, usage)
    source = root / "direct.gti"
    write(source, program(5000))
    emit = [gti, str(source), "--emit-mir", "-o", str(root / "direct.mir")]
    run(emit, root)
    for spelling in (["--constexpr-steps", "4096"], ["--constexpr-steps=4096"]):
        result = run([*emit, *spelling], root, 65)
        assert exhausted(result.stderr, 4096), result.stderr
    for value, message in (
            ("0", "must be an integer from 1 to 4294967295; got 0."),
            ("4294967296", "must be an integer from 1 to 4294967295"),
            ("12x", "must be a positive decimal integer; got '12x'."),
            ("-5", "must be a positive decimal integer")):
        result = run([*emit, "--constexpr-steps", value], root, 64)
        assert f"gti: invalid --constexpr-steps: 'constexpr-steps' {message}" \
            in result.stderr, result.stderr
    result = run([*emit, "--constexpr-steps", "5", "--constexpr-steps=6"],
                 root, 64)
    assert "--constexpr-steps may be specified only once" in result.stderr
    result = run([*emit, "--constexpr-steps"], root, 64)
    assert "missing count after --constexpr-steps" in result.stderr


def test_project(gti, root):
    app = root / "app"
    write(app / "src/main.gti", program(5000))

    # Defaults admit the request; the root's [limits] lowers it, and the
    # command line overrides the manifest in both directions.
    write(app / "gti.toml", manifest("app"))
    run([gti, "check"], app)
    write(app / "gti.toml", manifest("app", "constexpr-steps = 4096\n"))
    result = run([gti, "check"], app, 65)
    assert exhausted(result.stderr, 4096), result.stderr
    run([gti, "check", "--constexpr-steps", "200000"], app)
    write(app / "gti.toml", manifest("app", "constexpr-steps = 200000\n"))
    result = run([gti, "check", "--constexpr-steps=4096"], app, 65)
    assert exhausted(result.stderr, 4096), result.stderr

    # Raising: a request beyond the 1048576-step default needs the setting.
    write(app / "src/main.gti", program(200000))
    write(app / "gti.toml", manifest("app"))
    result = run([gti, "check"], app, 65)
    assert exhausted(result.stderr, 1048576), result.stderr
    write(app / "gti.toml", manifest("app", "constexpr-steps = 4000000\n"))
    run([gti, "check"], app)

    # Every limit key is accepted; invalid tables and values are attributed
    # manifest diagnostics.
    every = "".join(f"{key} = 64\n" for key in LIMIT_KEYS[1:])
    write(app / "gti.toml",
          manifest("app", "constexpr-steps = 4000000\n" + every))
    run([gti, "check"], app)
    for limits, code, message in (
            ("constexpr-steps = 0\n", "GTI-B1005",
             "Invalid limit: 'constexpr-steps' must be an integer from 1 to "
             "4294967295; got 0."),
            ("constexpr-steps = 4294967296\n", "GTI-B1005",
             "must be an integer from 1 to 4294967295"),
            ('constexpr-steps = "many"\n', "GTI-B1004",
             "Limit 'constexpr-steps' must be an integer."),
            ("constexpr-steps = 1.5\n", "GTI-B1004",
             "Limit 'constexpr-steps' must be an integer."),
            ("constexpr-step = 10\n", "GTI-B1001",
             "Unknown limits field 'constexpr-step'.")):
        write(app / "gti.toml", manifest("app", limits))
        result = run([gti, "check"], app, 65)
        assert code in result.stderr and message in result.stderr, (
            limits, result.stderr)
    assert "Did you mean 'constexpr-steps'?" in result.stderr, result.stderr
    write(app / "gti.toml", "limits = 5\n" + manifest("app"))
    result = run([gti, "check"], app, 65)
    assert "GTI-B1004" in result.stderr and \
        "Manifest field 'limits' must be a table." in result.stderr


def test_dependency(gti, root):
    # A dependency's [limits] is validated with its manifest but never
    # applied: only the root (selected) package configures the compilation.
    dependency = root / "dep"
    write(dependency / "lib/spin.gti",
          "export module spin;\nnamespace dep {\n" +
          SPIN.replace("constexpr Tally spin", "export constexpr Tally spin")
          .replace("struct Tally", "export struct Tally") + "}\n")
    write(dependency / "gti.toml",
          'manifest-version = 1\n[package]\nname = "dep"\nversion = "1.0.0"\n'
          'source-root = "lib"\n[modules]\nspin = "lib/spin.gti"\n'
          "[limits]\nconstexpr-steps = 1\n")
    app = root / "consumer"
    write(app / "src/main.gti",
          "import dep.spin;\nstatic_assert(dep::spin(uint64_t(5000)).count "
          "== uint64_t(5000));\nint main() { return 0; }\n")
    dependencies = '[dependencies]\ndep = { path = "../dep" }\n'
    write(app / "gti.toml", manifest("consumer", extra=dependencies))
    run([gti, "check"], app)
    write(app / "gti.toml", manifest("consumer", "constexpr-steps = 4096\n",
                                     extra=dependencies))
    result = run([gti, "check"], app, 65)
    assert exhausted(result.stderr, 4096, "dep::spin"), result.stderr
    write(dependency / "gti.toml",
          'manifest-version = 1\n[package]\nname = "dep"\nversion = "1.0.0"\n'
          'source-root = "lib"\n[modules]\nspin = "lib/spin.gti"\n'
          "[limits]\nconstexpr-steps = 4294967295\n")
    result = run([gti, "check"], app, 65)
    assert exhausted(result.stderr, 4096, "dep::spin"), result.stderr


def test_build_report(gti, root):
    app = root / "reported"
    write(app / "src/main.gti", program(100))
    write(app / "gti.toml", manifest("reported", "constexpr-steps = 300000\n"))
    report = root / "reported.json"
    run([gti, "build", "--build-report", str(report)], app)
    recorded = json.loads(report.read_text(encoding="utf-8"))
    assert recorded["compile_limits"]["constexpr-steps"] == 300000, recorded
    assert set(recorded["compile_limits"]) == set(LIMIT_KEYS), recorded
    run([gti, "verify", "--build-report", str(report)], app)
    # The limits are build identity: a changed limit is a recorded difference.
    result = run([gti, "verify", "--build-report", str(report),
                  "--constexpr-steps", "300001"], app, 70)
    assert "build policy: compile limits" in result.stderr and \
        "recorded: constexpr-steps=300000;" in result.stderr and \
        "rebuilt:  constexpr-steps=300001;" in result.stderr, result.stderr


def test_lsp(lsp, root):
    app = root / "editor"
    path = app / "src/main.gti"
    source = program(5000)
    write(path, source)
    write(app / "gti.toml", manifest("editor", "constexpr-steps = 4096\n"))
    other = root / "editor-default"
    other_path = other / "src/main.gti"
    write(other_path, source)
    write(other / "gti.toml", manifest("editor-default"))
    session = LspSession(lsp)
    try:
        initialize(session)
        uri = open_document(session, path, source)
        diagnostics = diagnostics_for(session, uri, version=1)
        assert any(item.get("code") == "GTI-S2057" and
                   "used all 4096 steps" in item["message"]
                   for item in diagnostics), diagnostics
        other_uri = open_document(session, other_path, source)
        assert diagnostics_for(session, other_uri, version=1) == []
    finally:
        session.close()


def main():
    gti = pathlib.Path(sys.argv[1]).resolve()
    lsp = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-compile-limits-") as directory:
        root = pathlib.Path(directory).resolve()
        test_direct(str(gti), root)
        test_project(str(gti), root)
        test_dependency(str(gti), root)
        test_build_report(str(gti), root)
        test_lsp(str(lsp), root)
    print("compile limits tests passed")


if __name__ == "__main__":
    main()
