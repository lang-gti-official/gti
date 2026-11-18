#!/usr/bin/env python3
"""Keep the tree-consistency CTest label honest and the tree's own references whole.

`tree-consistency` labels the registered tests that check the repository
against itself without a built compiler: imports between test scripts, lists
of harnesses and fixtures, allowlists of source files, registration and
ownership maps, manifests and workflow contracts. CI's gate runs the label
from a configure-only tree on every pull request and merge-queue candidate
(.github/workflows/ci.yml, job automation-tests), so a deleted or added file
that one of them cross-references fails before integration, not in
post-merge full validation.

Membership is a rule over the CTest inventory, not a judgement per test: an
entry belongs when it runs under the Python interpreter or CMake and names no
path inside the build tree other than the build directory itself. Such an
entry needs nothing built. tests/TestThroughput.cmake assigns the label from
its `gti_tree_consistency_tests` list; `membership_errors` requires that list
to be exactly the entries the rule selects, less EXEMPT below, whose entries
say why they still need a build. A labelled test that starts needing a binary
fails in the gate, which builds nothing, and `run_errors` rejects a labelled
test that was skipped or not run, so it cannot pass by skipping.

Generic reference checks run with it (`repository_errors`): every module
a Python file under tests/ or scripts/ imports exists, and every bare
`<name>_test.py` (or `.lua`) string in a collection literal there (the
expectation tables meta-tests keep, such as harness lists) names a file in
tests/. In a Git checkout it also rejects any file under the gti-test roots
(tests/ui, tests/lsp, tests/project, tests/slow, tests/unit) that Git
ignores, because `git add` would skip it without a word.

Usage:
  tree_consistency.py check --build <dir> [--ctest <ctest>]
  tree_consistency.py run --build <dir> --junit <ctest-junit.xml> [--ctest <ctest>]
  tree_consistency.py --self-test

tests/ownership_map_test.py calls `check` (it is itself labelled) and the
self-test, so `ctest -L '^tree-consistency$'` runs both; the gate runs `run`
after CTest.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
LABEL = "tree-consistency"
LIST = "gti_tree_consistency_tests"
# Interpreter-only entries the rule selects that still need a build. Each says
# what it needs; an entry here must stay registered somewhere.
EXEMPT = {
    "test_throughput_registration": "reads gti-corpus's built executable from the inventory",
    "llvm_cache_installed_toolchain": "installs and runs the built toolchain from the build directory",
    "macos_installed_runtime": "installs and runs the built toolchain from the build directory",
    "scoped_concurrency_installed": "installs and runs the built toolchain from the build directory",
}
REGISTRATION_FILES = ("CMakeLists.txt", "tests/CMakeLists.txt", "tests/*.cmake")
ADD_TEST = re.compile(r"\badd_test\(\s*NAME\s+([A-Za-z0-9_.-]+)\s+([^)]*)\)")
# Modules resolve against the importing file's directory and the directories
# the repository's scripts put on sys.path.
IMPORT_ROOTS = ("tests", "scripts", "tests/runner", ".codex/skills/verify-change/scripts")
SCANNED = ("tests", "scripts")
HARNESS_NAME = re.compile(r"[A-Za-z0-9_]+_test\.(?:py|lua)\Z")
# Executables a configure-only tree must not contain: their presence would let
# a labelled test pass on a binary the gate never builds.
BUILT_PRODUCTS = ("gti", "gti_lsp", "gti-test")
# Directories whose files gti-test discovers as tests; none may be ignored.
TEST_ROOTS = ("tests/ui", "tests/lsp", "tests/project", "tests/slow", "tests/unit")
# Editor and OS droppings the root .gitignore rightly ignores anywhere.
JUNK = (r"\.DS_Store", r".*\.sw[op]", r".*~", r"__pycache__", r".*\.py[cod]")


def registrations(root: Path) -> dict[str, str]:
    """Every literal add_test(NAME ...) in the tree's CMake files, with its arguments."""
    found = {}
    for pattern in REGISTRATION_FILES:
        for path in sorted(root.glob(pattern)):
            for name, arguments in ADD_TEST.findall(path.read_text(encoding="utf-8")):
                found.setdefault(name, arguments)
    return found


def listed(root: Path) -> list[str]:
    """The names tests/TestThroughput.cmake labels, comments removed."""
    text = (root / "tests/TestThroughput.cmake").read_text(encoding="utf-8")
    match = re.search(r"\bset\(" + LIST + r"\b(.*?)\)", text, re.S)
    if not match:
        raise ValueError(f"tests/TestThroughput.cmake must set({LIST} ...)")
    body = "\n".join(line.split("#", 1)[0] for line in match.group(1).splitlines())
    return body.split()


def harness_files(root: Path) -> dict[str, list[str]]:
    """Map each literal registration to the tests/ scripts its command names."""
    return {name: re.findall(r"/tests/([A-Za-z0-9_]+_test\.(?:py|lua))\b", arguments)
            for name, arguments in registrations(root).items()}


def inventory(build: Path, ctest: str = "ctest") -> list[dict]:
    # `ctest --show-only` writes into Testing/; query a copy of the generated
    # registration files, never the tree a running CTest owns.
    with tempfile.TemporaryDirectory(prefix="gti-tree-consistency-") as temporary:
        copy = Path(temporary)
        for path in build.rglob("CTestTestfile.cmake"):
            target = copy / path.relative_to(build)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
        result = subprocess.run([ctest, "--test-dir", str(copy), "--show-only=json-v1"],
                                capture_output=True, text=True, timeout=120, check=True)
    tests = json.loads(result.stdout)["tests"]
    # Paths CTest resolved against the copy name the real tree again.
    text = json.dumps(tests).replace(json.dumps(str(copy))[1:-1], json.dumps(str(build))[1:-1])
    return json.loads(text)


def properties(test: dict) -> dict:
    return {item["name"]: item["value"] for item in test.get("properties", [])}


def labels(test: dict) -> list[str]:
    return properties(test).get("LABELS", [])


def interpreter(program: str) -> bool:
    name = Path(program).name.lower()
    return re.fullmatch(r"python(?:3(?:\.[0-9]+)?)?(?:\.exe)?|cmake(?:\.exe)?", name) is not None


def build_paths(test: dict, build: Path) -> list[str]:
    """Paths inside the build tree a test's command names (including `cmake -E
    env` assignments), other than the build directory itself. The ENVIRONMENT
    property is left out: a release build adds the runtime library to every
    test's environment, and no harness here takes its binary that way."""
    values = list((test.get("command") or [])[1:])
    base = os.path.normpath(str(build))
    found = []
    for value in values:
        for part in re.split(r"[=;]", str(value)):
            path = os.path.normpath(part) if part else ""
            if path.startswith(base + os.sep):
                found.append(part)
    return found


def selected(test: dict, build: Path) -> bool:
    """The membership rule: an interpreter command that names no build product."""
    command = test.get("command") or []
    return bool(command) and interpreter(command[0]) and not build_paths(test, build)


def membership_errors(tests: list[dict], build: Path, root: Path) -> list[str]:
    errors = []
    known = registrations(root)
    members = listed(root)
    duplicates = sorted({name for name in members if members.count(name) > 1})
    if duplicates:
        errors.append(f"{LIST} repeats {duplicates}")
    for name in members:
        if name not in known:
            errors.append(f"{name}: in {LIST} but no CMake file registers it; remove it")
        if name in EXEMPT:
            errors.append(f"{name}: both labelled and exempt")
    for name in EXEMPT:
        if name not in known:
            errors.append(f"{name}: exempt in scripts/tree_consistency.py but no longer registered; remove it")
    for test in tests:
        name = test["name"]
        labelled = LABEL in labels(test)
        if labelled != (name in members):
            errors.append(f"{name}: the {LABEL} label must come only from {LIST}")
        if selected(test, build):
            if not labelled and name not in EXEMPT:
                errors.append(
                    f"{name}: runs an interpreter and names no build product, so it can run from a "
                    f"configure-only tree; add it to {LIST} in tests/TestThroughput.cmake, or to "
                    "EXEMPT in scripts/tree_consistency.py saying what it needs built")
        elif labelled:
            command = test.get("command") or []
            if not command:
                reason = "has no runnable command (an unbuilt executable?)"
            elif not interpreter(command[0]):
                reason = f"runs {Path(command[0]).name}, not an interpreter"
            else:
                reason = f"names build-tree paths {build_paths(test, build)[:3]}"
            errors.append(f"{name}: labelled {LABEL} but {reason}; it needs a build, so drop it from {LIST}")
    return errors


def resolvable(module: str, source: Path, root: Path) -> bool:
    top = module.split(".", 1)[0]
    if top in sys.stdlib_module_names or top == "__future__":
        return True
    for directory in (source.parent, *(root / name for name in IMPORT_ROOTS)):
        if (directory / f"{top}.py").is_file() or (directory / top / "__init__.py").is_file() or \
                (directory / top).is_dir():
            return True
    return False


def guarded_imports(tree: ast.AST) -> set[int]:
    """Imports inside `try: ... except ImportError` are optional by design."""
    guarded = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Try):
            continue
        caught = set()
        for handler in node.handlers:
            kinds = handler.type.elts if isinstance(handler.type, ast.Tuple) else [handler.type]
            caught.update(kind.id for kind in kinds if isinstance(kind, ast.Name))
        if caught & {"ImportError", "ModuleNotFoundError", "Exception"}:
            for statement in node.body:
                guarded.update(id(child) for child in ast.walk(statement))
    return guarded


def python_sources(root: Path) -> list[Path]:
    return sorted(path for directory in SCANNED for path in (root / directory).rglob("*.py")
                  if "__pycache__" not in path.parts)


def repository_errors(root: Path) -> list[str]:
    """Imports of modules that do not exist, and bare harness names in
    expectation tables that name no file under tests/."""
    errors = []
    for source in python_sources(root):
        relative = source.relative_to(root).as_posix()
        try:
            tree = ast.parse(source.read_text(encoding="utf-8"), filename=relative)
        except SyntaxError as error:
            errors.append(f"{relative}: does not parse: {error.msg} (line {error.lineno})")
            continue
        guarded = guarded_imports(tree)
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom) and not node.level and node.module:
                modules = [node.module]
            else:
                modules = []
            for module in modules:
                if id(node) not in guarded and not resolvable(module, source, root):
                    errors.append(f"{relative}:{node.lineno}: imports {module}, which is not in the tree")
            if isinstance(node, ast.Dict):
                elements = [key for key in node.keys if key is not None]
            elif isinstance(node, (ast.Set, ast.List, ast.Tuple)):
                elements = node.elts
            else:
                continue
            for element in elements:
                if (isinstance(element, ast.Constant) and isinstance(element.value, str) and
                        HARNESS_NAME.match(element.value) and
                        not (root / "tests" / element.value).is_file() and
                        not (source.parent / element.value).is_file()):
                    errors.append(f"{relative}:{element.lineno}: names {element.value}, which is not in tests/")
    return errors + ignored_test_errors(root)


def ignored_test_errors(root: Path) -> list[str]:
    """Files under the test roots that Git ignores: `git add` skips them
    silently, so a test written there never reaches a commit. A fresh CI
    checkout has none; this guards the local checkout the tree-consistency run
    is required in before a hand-off. Outside a Git checkout it checks nothing."""
    try:
        top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, timeout=60)
        if top.returncode or Path(top.stdout.strip()).resolve() != root.resolve():
            return []
        listed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--others", "--ignored", "--exclude-standard",
             "--directory", "--", *TEST_ROOTS],
            capture_output=True, text=True, timeout=60)
        tracked = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--ignored", "--exclude-standard",
             "--", *TEST_ROOTS],
            capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return []
    if listed.returncode or tracked.returncode:
        return []
    errors = []
    for path in sorted(set(filter(None, listed.stdout.split("\0") + tracked.stdout.split("\0")))):
        if any(re.fullmatch(pattern, part) for part in path.rstrip("/").split("/") for pattern in JUNK):
            continue
        rule = subprocess.run(["git", "-C", str(root), "check-ignore", "-v", "--no-index", path.rstrip("/")],
                              capture_output=True, text=True, timeout=60).stdout.split("\t", 1)[0]
        errors.append(f"{path}: ignored by git ({rule or 'an ignore rule'}), so `git add` skips it; "
                      "narrow that pattern rather than force-adding")
    return errors


def run_errors(path: Path, tests: list[dict], build: Path) -> list[str]:
    """The gate's CTest run executed every labelled test from a configure-only
    tree, and none was skipped or left unrun."""
    errors = [f"{build / name} exists: the tree-consistency run needs a configure-only tree"
              for name in BUILT_PRODUCTS if (build / name).exists()]
    expected = {test["name"] for test in tests if LABEL in labels(test)}
    if not expected:
        errors.append(f"no registered test carries {LABEL}")
    cases = {}
    for case in ET.parse(path).getroot().iter("testcase"):
        cases[case.get("name")] = case
    if set(cases) != expected:
        errors.append(f"the run does not match the label: missing {sorted(expected - set(cases))}, "
                      f"extra {sorted(set(cases) - expected)}")
    for name, case in sorted(cases.items()):
        if case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            errors.append(f"{name}: skipped or not run; a {LABEL} test must run and pass without a build")
        elif case.find("failure") is not None or case.find("error") is not None:
            errors.append(f"{name}: failed")
    return errors


def self_test() -> None:
    """Forged trees and inventories that each rule must reject."""
    with tempfile.TemporaryDirectory(prefix="gti-tree-consistency-self-") as temporary:
        root = Path(temporary) / "repo"
        build = Path(temporary) / "build"
        (root / "tests").mkdir(parents=True)
        (root / "scripts").mkdir()
        build.mkdir()
        python = "/usr/bin/python3"

        def write(relative, text):
            (root / relative).write_text(text, encoding="utf-8")

        def entry(name, *arguments, label=True, program=python):
            return {"name": name, "command": [program, *arguments],
                    "properties": [{"name": "LABELS", "value": [LABEL] if label else []}]}

        write("tests/TestThroughput.cmake", f"set({LIST}\n  tidy # a comment\n  maps\n)\n")
        write("tests/CMakeLists.txt",
              "add_test(NAME tidy COMMAND python \"${S}/tests/tidy_test.py\")\n"
              "add_test(NAME maps COMMAND python \"${S}/tests/maps_test.py\" \"${B}\")\n"
              "add_test(NAME runtime COMMAND python \"${S}/tests/runtime_test.py\" $<TARGET_FILE:gti>)\n"
              "add_test(NAME unit COMMAND unit_tests)\n"
              "add_test(NAME test_throughput_registration COMMAND python x)\n"
              "add_test(NAME llvm_cache_installed_toolchain COMMAND python x)\n"
              "add_test(NAME macos_installed_runtime COMMAND python x)\n"
              "add_test(NAME scoped_concurrency_installed COMMAND python x)\n")
        assert listed(root) == ["tidy", "maps"]
        assert harness_files(root)["maps"] == "maps_test.py".split()
        good = [entry("tidy", str(root / "tests/tidy_test.py")),
                entry("maps", str(root / "tests/maps_test.py"), str(build)),
                entry("runtime", str(root / "tests/runtime_test.py"), str(build / "gti"), label=False),
                {"name": "unit", "command": None, "properties": []},
                entry("test_throughput_registration", "x", str(build), label=False)]
        assert membership_errors(good, build, root) == [], membership_errors(good, build, root)

        def rejects(tests, fragment):
            errors = membership_errors(tests, build, root)
            assert any(fragment in error for error in errors), (fragment, errors)

        # A new interpreter-only entry is neither labelled nor exempt.
        rejects([*good, entry("fresh", "y", label=False)], "add it to " + LIST)
        # A labelled entry that names a build product, or has no command.
        forged = [dict(test) for test in good]
        forged[0] = entry("tidy", str(root / "tests/tidy_test.py"), str(build / "gti"))
        rejects(forged, "needs a build")
        forged[0] = {"name": "tidy", "command": None, "properties": [{"name": "LABELS", "value": [LABEL]}]}
        rejects(forged, "no runnable command")
        forged[0] = entry("tidy", "x", program=str(build / "tidy_tests"))
        rejects(forged, "needs a build")
        # A label applied outside the list, and a listed name no CMake registers.
        rejects([*good[:2], entry("runtime", "z", str(build))], "only from " + LIST)
        write("tests/TestThroughput.cmake", f"set({LIST}\n  tidy\n  maps\n  deleted\n)\n")
        rejects(good, "deleted: in " + LIST)
        write("tests/TestThroughput.cmake", f"set({LIST}\n  tidy\n  maps\n)\n")

        # Generic reference checks: a deleted module and a deleted harness name.
        write("tests/tidy_test.py", "import os\nimport maps_test\nfrom helper import x\n"
                                    "try:\n    import optional_thing\nexcept ImportError:\n    pass\n"
                                    "HARNESSES = {'maps_test.py': 1}\nPATHS = ['tests/synthetic_test.py']\n")
        write("tests/maps_test.py", "")
        write("scripts/helper.py", "")
        assert repository_errors(root) == [], repository_errors(root)
        (root / "scripts/helper.py").unlink()
        write("tests/tidy_test.py", "import deleted_wrapper_test\nfrom helper import x\n"
                                    "HARNESSES = {'deleted_test.py': {'a'}}\n")
        errors = repository_errors(root)
        assert len(errors) == 3 and "deleted_wrapper_test" in errors[0] and "helper" in errors[1] \
            and "deleted_test.py" in errors[2], errors

        # A test directory an over-broad ignore pattern swallows (as `Testing/`
        # once swallowed tests/ui/stdlib/testing/ on a case-insensitive disk),
        # and a tracked test the pattern matches; OS droppings are fine.
        checkout = Path(temporary) / "checkout"
        (checkout / "tests/ui/testing").mkdir(parents=True)
        (checkout / "tests/lsp").mkdir(parents=True)
        (checkout / ".gitignore").write_text("testing/\n*.out\n.DS_Store\n", encoding="utf-8")
        (checkout / "tests/ui/testing/check.gti").write_text("", encoding="utf-8")
        (checkout / "tests/ui/kept.gti").write_text("", encoding="utf-8")
        (checkout / "tests/ui/.DS_Store").write_text("", encoding="utf-8")
        (checkout / "tests/lsp/hover.out").write_text("", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(checkout)], check=True, timeout=60)
        subprocess.run(["git", "-C", str(checkout), "add", "-f", "tests/lsp/hover.out"], check=True, timeout=60)
        errors = ignored_test_errors(checkout)
        assert len(errors) == 2 and errors[0].startswith("tests/lsp/hover.out: ignored by git (.gitignore:2:") \
            and errors[1].startswith("tests/ui/testing/: ignored by git (.gitignore:1:"), errors
        assert ignored_test_errors(root) == []

        # The gate's run: every labelled test ran and passed, none skipped.
        report = Path(temporary) / "report.xml"

        def junit(*cases):
            report.write_text("<testsuite>" + "".join(cases) + "</testsuite>", encoding="utf-8")
            return run_errors(report, good, build)

        passed = '<testcase name="tidy" status="run"/>', '<testcase name="maps" status="run"/>'
        assert junit(*passed) == []
        assert any("skipped" in error for error in
                   junit(passed[0], '<testcase name="maps" status="run"><skipped/></testcase>'))
        assert any("not run" in error for error in junit(passed[0], '<testcase name="maps" status="notrun"/>'))
        assert any("missing ['maps']" in error for error in junit(passed[0]))
        (build / "gti").write_text("")
        assert any("configure-only" in error for error in junit(*passed))
    print("tree consistency: self-test rejected unlabelled, mislabelled, stale, broken-import, "
          "missing-harness, ignored-test, skipped and built-tree forgeries")


def report(title: str, errors: list[str]) -> int:
    if errors:
        print(f"{title}: {len(errors)} error(s):\n  " + "\n  ".join(errors), file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--root", default=str(ROOT))
    commands = parser.add_subparsers(dest="command")
    for name in ("check", "run"):
        command = commands.add_parser(name)
        command.add_argument("--build", required=True)
        command.add_argument("--ctest", default="ctest")
        if name == "run":
            command.add_argument("--junit", required=True)
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    if not args.command:
        parser.error("name a command or --self-test")
    root, build = Path(args.root).resolve(), Path(args.build).resolve()
    tests = inventory(build, args.ctest)
    if args.command == "run":
        status = report("tree-consistency run", run_errors(Path(args.junit), tests, build))
        if not status:
            print(f"tree consistency: {sum(LABEL in labels(test) for test in tests)} labelled tests "
                  "ran and passed from a configure-only tree")
        return status
    errors = membership_errors(tests, build, root) + repository_errors(root)
    status = report("tree consistency", errors)
    if not status:
        print(f"tree consistency: {len(listed(root))} labelled tests; membership, imports and "
              "harness tables pass")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
