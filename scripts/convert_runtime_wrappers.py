#!/usr/bin/env python3
"""Convert shared-harness runtime wrappers into `gti-test` files.

Temporary migration tool for phase 3 of docs/plans/test-suite-redesign.md;
it is deleted when the last wrapper has been converted.

Each wrapper in the family is a thin script over
`tests/lambda_pack_invocation_runtime_test.py:main`. This tool runs the
wrapper with that harness replaced by a recorder, so computed expectations
(line numbers read from a fixture, say) are captured exactly as the old
harness would have checked them. For every fixture it writes
`tests/ui/<area>/<name>.gti` with the run directives, then deletes the wrapper,
its CTest registration and its entries in the registries that name it.

A fixture is moved when nothing else in the repository names it, and copied
otherwise. A wrapper the directives cannot express faithfully is reported and
left untouched; nothing is converted half-way.

    scripts/convert_runtime_wrappers.py --list            # classify all wrappers
    scripts/convert_runtime_wrappers.py WRAPPER... [--dry-run]

WRAPPER is a script name such as `expected_class_return_runtime_test.py`.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import runpy
import subprocess
import sys
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
FIXTURES = TESTS / "fixtures"
HARNESS = "lambda_pack_invocation_runtime_test"
# Files that only register or police the wrappers; their entries go with them.
REGISTRIES = ("tests/CMakeLists.txt", "tests/TestThroughput.cmake",
              "tests/ci_test_durations.json", "tests/ci_darwin_tests.json",
              "tests/ownership.toml", "tests/stdlib_surface_manifest.json")
# Documents a conversion may not edit (owned by other work); stale mentions
# there are reported instead.
UNTOUCHABLE: tuple[str, ...] = ()
SUPPORTED_ARGUMENTS = {"names", "cases", "llvm_modes", "stdin_by_name", "fixture_paths"}

# Area of a wrapper, by the first matching pattern on its CTest name. The
# layout follows tests/ui/README.md: language/, ownership/, generics/,
# stdlib/<module>/.
AREAS = (
    (r"^(expected|owned_expected|owned_unexpected|nested_guarded_expected|virtual_expected)", "language/expected"),
    (r"^(consuming|owning_function|owning_expression|retained_owner)", "ownership/consuming"),
    (r"^(construct|constructor|generated_base|generated_constructor|object_construction|"
     r"private_constructor|mixed_constructor|moved_passive_constructor|pack_prefix_constructor|"
     r"declaration_owned_defaults|owned_argument_member|plain_call_member|failure_free_passive|"
     r"passive_class|array_field_constructor|base_rollback)", "language/construction"),
    (r"^(borrow|borrowed|temporary_owner_borrow|scalar_temporary_borrow|nested_borrow|"
     r"llvm_borrowed|shared_accessor|text_reference|view_field|recursive_text|"
     r"dependent_reference|reference_)", "ownership/borrow"),
    (r"^(conditional|class_conditional|owning_conditional|short_circuit|comma_class)", "language/conditionals"),
    (r"^(lambda|grouped_lambda|generic_callable|fresh_callable|constructed_callback|"
     r"borrowing_callbacks|callable_return|passive_function_item)", "language/callables"),
    (r"^contextual_borrowed", "ownership/borrow"),
    (r"^(out_of_class_cleanup|source_storage_drain)", "ownership/cleanup"),
    (r"^(object_relocation|structural_relocation|raw_owner_witness|raw_pointer_conversion|"
     r"owned_pointee|allocation_default|raw_owned_field|raw_receiver_owned)", "memory"),
    (r"^(assignment_result|class_store_destinations|payload_enum_reference_assignment|"
     r"implicit_receiver_owned_assignment)", "language/assignment"),
    (r"^(moved_local_field_read|owning_temporary_field_read|virtual_temporary_receiver|"
     r"owning_expected_field)", "language/temporaries"),
    (r"^operator_(place|relay)", "language/operators"),
    (r"^(accessor_text_carrier|passive_text_view_class|deferred_bound_field)", "language/views"),
    (r"^nested_nominal_result", "language/classes"),
    (r"^(abstract_generic|enum_selector|semantic_call_regressions|contextual_numeric_literals)", "generics"),
    (r"^coverage_composition", "regressions"),
    (r"^generic_algorithm_breadth", "stdlib/algorithm"),
    (r"^restoring_", "stdlib/utility"),
    (r"^string_(operations|ordering)", "stdlib/string"),
    (r"^extensible_formatting", "stdlib/format"),
    (r"^composable_io", "stdlib/io"),
)


def area_of(test_name: str) -> str | None:
    for pattern, area in AREAS:
        if re.search(pattern, test_name):
            return area
    return None


# -- Capture ------------------------------------------------------------------

def capture(wrapper: pathlib.Path) -> tuple[list[dict], str | None]:
    """The keyword arguments of every harness call the wrapper makes."""
    calls: list[dict] = []

    def record(*positional, **keywords):
        if positional:
            keywords = dict(keywords, names=positional[0])
        calls.append(keywords)

    stub = types.ModuleType(HARNESS)
    stub.main = record
    saved_module = sys.modules.get(HARNESS)
    saved_argv = sys.argv
    saved_path = list(sys.path)
    sys.modules[HARNESS] = stub
    # A wrapper that does more than call the harness reaches for this compiler.
    sys.argv = [str(wrapper), str(ROOT / "nonexistent-gti-for-conversion")]
    sys.path.insert(0, str(TESTS))
    error = None
    try:
        runpy.run_path(str(wrapper), run_name="__main__")
    except SystemExit as exit_:
        if exit_.code not in (None, 0):
            error = f"exited with {exit_.code}"
    except Exception as exception:  # noqa: BLE001 - reported, not hidden
        error = f"{type(exception).__name__}: {exception}"
    finally:
        sys.argv = saved_argv
        sys.path[:] = saved_path
        if saved_module is None:
            sys.modules.pop(HARNESS, None)
        else:
            sys.modules[HARNESS] = saved_module
    return calls, error


# -- Classification -----------------------------------------------------------

def ctest_names() -> dict[str, str]:
    """Wrapper script name -> CTest name, from tests/CMakeLists.txt."""
    text = (TESTS / "CMakeLists.txt").read_text()
    names = {}
    for match in re.finditer(r"add_test\(", text):
        end = statement_end(text, match.start())
        statement = text[match.start():end]
        name = re.search(r"NAME\s+\"?([A-Za-z0-9_.-]+)", statement)
        script = re.search(r"tests/([A-Za-z0-9_]+_test\.py)", statement)
        if name and script and "$" not in name.group(1):
            names[script.group(1)] = name.group(1)
    return names


def statement_end(text: str, start: int) -> int:
    depth = 0
    for index in range(text.index("(", start), len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    raise ValueError("unbalanced CMake statement")


def wrappers() -> list[pathlib.Path]:
    return sorted(path for path in TESTS.glob("*_test.py")
                  if path.name != f"{HARNESS}.py"
                  and re.search(rf"^from {HARNESS} import main", path.read_text(), re.M))


class Plan:
    """What converting one wrapper would do, or why it cannot be converted."""

    def __init__(self, wrapper: pathlib.Path, test_name: str | None):
        self.wrapper = wrapper
        self.test_name = test_name
        self.reasons: list[str] = []
        self.files: dict[pathlib.Path, str] = {}   # new path -> contents
        self.moves: list[tuple[pathlib.Path, pathlib.Path]] = []  # fixture -> test
        self.copies: list[tuple[pathlib.Path, pathlib.Path]] = []
        self.reductions: list[str] = []
        self.fixtures: list[str] = []
        self.modes: dict[str, str] = {}
        self.signatures: dict[str, str] = {}  # fixture -> its observations
        self.notes: list[str] = []

    def drop(self, fixture: str):
        """Leave out a fixture another wrapper in the batch converts identically."""
        target = next(target for _, target in self.moves + self.copies
                      if target.stem == fixture)
        self.files = {path: text for path, text in self.files.items()
                      if path.parent != target.parent
                      or not (path == target or path.name.startswith(f"{fixture}."))}
        self.moves = [move for move in self.moves if move[1] != target]
        self.copies = [copy for copy in self.copies if copy[1] != target]
        self.fixtures.remove(fixture)
        self.modes.pop(fixture, None)


def docstring(wrapper: pathlib.Path) -> str:
    import ast
    text = ast.get_docstring(ast.parse(wrapper.read_text())) or ""
    return " ".join(text.split("\n\n")[0].split())


def importers(wrapper: pathlib.Path) -> list[str]:
    """Other Python files that import the wrapper as a module."""
    pattern = rf"^(from|import) {re.escape(wrapper.stem)}\b"
    return sorted({hit.split(":", 1)[0] for hit in git_grep(pattern)
                   if hit.split(":", 1)[0] != f"tests/{wrapper.name}"})


def plan(wrapper: pathlib.Path, test_names: dict[str, str], area: str | None,
         selected: set[str] = frozenset()) -> Plan:
    result = Plan(wrapper, test_names.get(wrapper.name))
    if result.test_name is None:
        result.reasons.append("no add_test names this script directly")
        return result
    users = [user for user in importers(wrapper)
             if pathlib.PurePath(user).name not in selected]
    if users:
        result.reasons.append("imported by " + ", ".join(users) + "; convert them together")
    calls, error = capture(wrapper)
    if error:
        result.reasons.append(f"does more than call the harness ({error})")
    if len(calls) != 1:
        result.reasons.append(f"calls the harness {len(calls)} times")
    if result.reasons:
        return result
    call = calls[0]
    unsupported = sorted(set(call) - SUPPORTED_ARGUMENTS)
    if unsupported:
        result.reasons.append("harness arguments " + ", ".join(unsupported))
    area = area or area_of(result.test_name)
    if area is None:
        result.reasons.append("no area rule matches; pass --area")
    if result.reasons:
        return result
    description = docstring(wrapper)
    if not description:
        result.reasons.append("wrapper has no docstring to describe its tests")
        return result
    names = list(call.get("names") or ())
    cases = call.get("cases") or {}
    stdin_by_name = call.get("stdin_by_name") or {}
    fixture_paths = {name: pathlib.Path(path) for name, path in
                     (call.get("fixture_paths") or {}).items()}
    for name in names:
        source = fixture_paths.get(name, FIXTURES / f"{name}.gti")
        if not source.is_file() or not source.resolve().is_relative_to(TESTS):
            result.reasons.append(f"{name}: fixture {source} is not a file under tests/")
            continue
        runs = cases.get(name, [((), (0, "", ""))])
        stdin = stdin_by_name.get(name)
        converted = convert(name, source, runs, stdin, description, result)
        if converted is None:
            continue
        target = TESTS / "ui" / area / f"{name}.gti"
        text, stdout_files = converted
        if target.exists():
            if not stdout_files and same_test(target.read_text(), text):
                # Already seeded with the same program and observations (a
                # stdlib surface run case): nothing to add.
                result.notes.append(f"{name}: {target.relative_to(ROOT)} already checks it")
                continue
            result.reasons.append(f"{name}: {target.relative_to(ROOT)} already exists")
            continue
        result.files[target] = text
        for suffix, content in stdout_files.items():
            result.files[target.with_name(f"{name}{suffix}.stdout")] = content
        (result.copies if shared(source, name, wrapper) else result.moves).append((source, target))
        result.fixtures.append(name)
        result.signatures[name] = repr((runs, stdin))
    return result


def same_test(existing: str, generated: str) -> bool:
    """Whether two test files differ only in their leading description."""
    def essence(text: str) -> list[str]:
        lines = text.rstrip("\n").split("\n")
        if lines and lines[0].startswith("// ") and not lines[0].startswith("// ---"):
            lines = lines[1:]
        return lines
    return essence(existing) == essence(generated)


# -- Conversion of one fixture --------------------------------------------------

STD_LINE = re.compile(r"(<std/[^>]+>):\d+(?=:)")


def stderr_expectations(name: str, stderr: str, plan_: Plan) -> list[str]:
    """`stderr-contains` lines for an exact stderr expectation."""
    needles = []
    for line in stderr.splitlines():
        if not line.strip():
            continue
        if STD_LINE.search(line):
            # The old wrapper read this standard-library line number from the
            # library source at run time; keep the location and the message
            # without the number.
            pieces = STD_LINE.split(line)
            for index, piece in enumerate(pieces):
                if index % 2 == 1:
                    needles.append(piece + ":")
                elif piece.strip():
                    needles.append(piece.strip())
            plan_.reductions.append(f"{name}: standard-library line number dropped from '{line}'")
        else:
            needles.append(line.strip())
    return needles


def revision_names(runs) -> list[str]:
    """Revision names: the arguments when short and distinct, else run1..N."""
    names = []
    for arguments, _ in runs:
        name = "-".join(arguments) if arguments else "default"
        names.append(re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_-"))
    if (len(set(names)) == len(names) and all(names)
            and all(len(name) <= 24 for name in names)):
        return names
    return [f"run{index}" for index in range(1, len(runs) + 1)]


def stdout_directives(prefix: str, stdout: str) -> list[str] | None:
    """Inline `stdout:` lines, or None when the text needs a .stdout file."""
    if not stdout:
        return []
    lines = stdout[:-1].split("\n") if stdout.endswith("\n") else None
    if (lines is None or len(lines) > 8 or "\t" in stdout or "\r" in stdout
            or any(line != line.rstrip() for line in lines)):
        return None
    return [f"//@{prefix} stdout: {line}" for line in lines]


# Anything that can make a run observe something beyond "it compiled": output,
# checks that fail at run time, cleanup, arithmetic traps, indexing, memory.
RUNTIME_SIGNALS = re.compile(
    r"\b(print\w*|write\w*|std::out|std::err|panic|assert\w*|abort|exit|expect\w*|"
    r"requires|ensures|contract|unreachable|unsafe|new|delete|alloc\w*|thread\w*|"
    r"atomic\w*|mutex|unique\w*|shared\w*|vector|string|map|optional|span)\b"
    r"|[\[/%~*&]")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def main_body(text: str) -> str | None:
    match = re.search(r"\bint32?_?t?\s+main\s*\([^)]*\)\s*\{", text)
    if not match:
        return None
    depth, index = 1, match.end()
    while depth and index < len(text):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
    return text[match.end():index - 1]


def shows_nothing(text: str) -> bool:
    """Conservatively: the program cannot observe anything at run time.

    `main` returns the constant 0 on every path and the source has no output,
    no runtime checks, no cleanup, no arithmetic that can trap, no indexing and
    no library types. When unsure, the test keeps running.
    """
    code = strip_comments(text)
    body = main_body(code)
    if body is None:
        return False
    returns = re.findall(r"\breturn\b([^;]*);", body)
    if not returns or any(value.strip() != "0" for value in returns):
        return False
    return not re.search(r"^\s*import\b", code, re.M) and not RUNTIME_SIGNALS.search(code)


def convert(name, source, runs, stdin, description, plan_):
    text = source.read_text()
    if not runs:
        plan_.reasons.append(f"{name}: no execution observations")
        return None
    directives: list[str] = []
    stdout_files: dict[str, str] = {}
    for arguments, (code, _stdout, stderr) in runs:
        if code < 0 or code > 255:
            plan_.reasons.append(f"{name}: expects termination by signal {-code}")
            return None
        if any(not argument or any(c.isspace() for c in argument) for argument in arguments):
            plan_.reasons.append(f"{name}: an argument contains whitespace or is empty")
            return None
    if stdin is not None and stdin and not stdin.endswith("\n"):
        plan_.reasons.append(f"{name}: stdin without a final newline")
        return None
    if (len(runs) == 1 and not runs[0][0] and tuple(runs[0][1]) == (0, "", "")
            and not stdin and shows_nothing(text)):
        plan_.modes[name] = "check-pass"
        lines = text.split("\n")
        if lines and lines[0].startswith("// ") and not lines[0].startswith("// ---"):
            return "\n".join([lines[0], "//@ check-pass", *lines[1:]]), {}
        return "\n".join([f"// {description}", "//@ check-pass", text]), {}
    codes = {code for _, (code, _, _) in runs}
    file_mode = "run-pass" if 0 in codes else "run-fail"
    directives.append(f"//@ {file_mode}")
    if stdin:
        directives += [f"//@ stdin: {line}" for line in stdin[:-1].split("\n")]
    revisions = len(runs) > 1
    labels = revision_names(runs) if revisions else [""]
    if revisions:
        directives.append("//@ revisions: " + " ".join(labels))
    mentions_self = False
    for label, (arguments, (code, stdout, stderr)) in zip(labels, runs):
        prefix = f"[{label}]" if revisions else ""
        if revisions and (code == 0) != (file_mode == "run-pass"):
            directives.append(f"//@{prefix} {'run-pass' if code == 0 else 'run-fail'}")
        if arguments:
            directives.append(f"//@{prefix} args: {' '.join(arguments)}")
        if code != 0:
            directives.append(f"//@{prefix} exit-code: {code}")
        inline = stdout_directives(prefix, stdout)
        if inline is None:
            stdout_files[f".{label}" if revisions else ""] = stdout
        else:
            directives += inline
        for needle in stderr_expectations(name, stderr, plan_):
            directives.append(f"//@{prefix} stderr-contains: {needle}")
        if f"{source.name}" in stdout or f"{source.name}" in stderr:
            mentions_self = True
        if not stderr and code == 0:
            pass
    plan_.modes[name] = file_mode if not revisions else f"{file_mode} ({len(runs)} revisions)"
    lines = text.split("\n")
    if mentions_self:
        # Expectations name the fixture's file and lines: keep both exactly by
        # giving the source its own section, as the old harness compiled it.
        header = [f"// {description}", *directives, f"// --- {source.name}"]
        return "\n".join(header) + "\n" + text, stdout_files
    if lines and lines[0].startswith("// ") and not lines[0].startswith("// ---"):
        return "\n".join([lines[0], *directives, *lines[1:]]), stdout_files
    return "\n".join([f"// {description}", *directives, text]), stdout_files


# -- Sharing ------------------------------------------------------------------

def git_grep(pattern: str, *flags: str) -> list[str]:
    output = subprocess.run(["git", "grep", "-n", *(flags or ("-E",)), pattern], cwd=ROOT,
                            capture_output=True, text=True).stdout
    return [line for line in output.splitlines() if line]


def shared(source: pathlib.Path, name: str, wrapper: pathlib.Path) -> bool:
    """Whether a file other than the wrapper and the registries uses the fixture."""
    stem = re.escape(source.stem)
    hits = git_grep(rf"[\"'/]{stem}(\.gti)?[\"']|{stem}\.gti")
    for hit in hits:
        path, _, line = hit.split(":", 2)
        if path == "tests/stdlib_surface_manifest.json" and '"source"' in line:
            return True  # stdlib_surface compiles and runs it
        if (path == f"tests/{wrapper.name}" or path in REGISTRIES
                or path.startswith("tests/ui/")):
            continue
        if path.endswith(".md"):
            continue  # documentation paths are rewritten
        return True
    return False


# -- Registry edits -------------------------------------------------------------

def remove_cmake(text: str, test: str, script: str) -> tuple[str, int]:
    removed = 0
    # add_test(...) naming the script, then set_tests_properties of the test.
    for match in list(re.finditer(r"[ \t]*add_test\(", text))[::-1]:
        end = statement_end(text, match.start())
        if f"tests/{script}" in text[match.start():end]:
            text = text[:match.start()] + text[end:].lstrip(" \t").removeprefix("\n")
            removed += 1
    pattern = re.compile(r"[ \t]*set_tests_properties\(\s*\"?([A-Za-z0-9_.-]+)\"?\s")
    for match in list(pattern.finditer(text))[::-1]:
        if match.group(1) == test:
            end = statement_end(text, match.start())
            statement = text[match.start():end]
            if re.search(r"set_tests_properties\(\s*\"?" + re.escape(test) + r"\"?\s+PROPERTIES", statement):
                text = text[:match.start()] + text[end:].lstrip(" \t").removeprefix("\n")
                removed += 1
    return text, removed


def remove_word_lists(text: str, test: str) -> tuple[str, int]:
    """Drop `test` (and a following timeout number) from CMake word lists."""
    removed = 0
    lines = text.split("\n")
    output = []
    token = re.escape(test) + r"(?:[ \t]+\d+)?(?![\w.}-])"
    leading = re.compile(r"^([ \t]*)" + token + r"[ \t]*")
    inner = re.compile(r"[ \t]+" + token)
    for line in lines:
        if line.lstrip().startswith("#") or "add_test" in line:
            output.append(line)
            continue
        new = leading.sub(lambda m: m.group(1), line)
        new = inner.sub("", new) if new == line else new
        if new == line:
            output.append(line)
            continue
        removed += 1
        new = re.sub(r"\(\s+\)", "()", new.rstrip())
        if new.strip() == "":
            continue
        output.append(new)
    return "\n".join(output), removed


def edit_registries(test: str, script: str, moves, report, dry_run: bool):
    durations = 0.0
    path = ROOT / "tests/CMakeLists.txt"
    text, count = remove_cmake(path.read_text(), test, script)
    if count == 0:
        text, count = remove_foreach_item(text, test)
    if count == 0:
        report.append(f"warning: {test}: no CMake registration removed")
    text2, extra = remove_word_lists(text, test)
    if extra:
        report.append(f"{test}: removed {extra} further CMake list mention(s)")
    write(path, text2, dry_run)

    path = ROOT / "tests/TestThroughput.cmake"
    text, count = remove_word_lists(path.read_text(), test)
    write(path, text, dry_run)

    path = ROOT / "tests/ci_test_durations.json"
    durations = float(json.loads(path.read_text())["seconds"].get(test, 0.0))
    write(path, remove_json_entry(path.read_text(), test), dry_run)

    path = ROOT / "tests/ci_darwin_tests.json"
    write(path, remove_json_entry(path.read_text(), test), dry_run)

    path = ROOT / "tests/ownership.toml"
    text = path.read_text()
    new = remove_toml_owner(text, test)
    write(path, new, dry_run)

    path = ROOT / "tests/stdlib_surface_manifest.json"
    text = path.read_text()
    text = re.sub(r'\n\s*"tests/' + re.escape(script) + r'",?', "", text)
    text = fix_json_commas(text)
    for source, target in moves:
        text = text.replace(f'"{source.relative_to(ROOT)}"', f'"{target.relative_to(ROOT)}"')
    write(path, text, dry_run)
    return durations


def remove_json_entry(text: str, test: str) -> str:
    """Drop a `"test",` list item or `"test": value,` member line."""
    pattern = re.compile(r'^\s*"' + re.escape(test) + r'"\s*(:\s*[^,\n]+)?,?\s*$\n', re.M)
    new = pattern.sub("", text)
    if new != text:
        new = fix_json_commas(new)
        json.loads(new)
    return new


def remove_foreach_item(text: str, test: str) -> tuple[str, int]:
    for match in re.finditer(r"foreach\(\s*\w+\s+IN\s+ITEMS", text):
        end = statement_end(text, match.start())
        statement = text[match.start():end]
        new = re.sub(r"(?<![\w-])" + re.escape(test) + r"(?![\w-])\s*", "", statement)
        if new != statement:
            new = re.sub(r"\s+\)$", ")", new)
            return text[:match.start()] + new + text[end:], 1
    return text, 0


def remove_toml_owner(text: str, test: str) -> str:
    """Remove the test from owners/consumers; gti_test_ui takes its place."""
    lines = text.split("\n")
    output = []
    for index, line in enumerate(lines):
        if line.strip() in (f'"{test}",', f'"{test}"'):
            # Find the enclosing list and make sure gti_test_ui is in it.
            start = index
            while start > 0 and not re.match(r"\s*(owners|consumers)\s*=\s*\[", lines[start]):
                start -= 1
            end = index
            while end < len(lines) and lines[end].strip() != "]":
                end += 1
            block = lines[start:end + 1]
            if not any(item.strip().startswith('"gti_test_ui"') for item in block) and \
                    not any(item.strip().startswith('"gti_test_ui"') for item in output[-(index - start):]):
                output.append('  "gti_test_ui",')
            continue
        if f'"{test}"' in line:
            line = re.sub(r'\s*"' + re.escape(test) + r'",?', "", line)
        output.append(line)
    return fix_toml_duplicates("\n".join(output))


def fix_toml_duplicates(text: str) -> str:
    """Keep one gti_test_ui per list."""
    output, seen = [], False
    for line in text.split("\n"):
        if re.match(r"\s*(owners|consumers)\s*=\s*\[", line):
            seen = False
        if line.strip() == '"gti_test_ui",':
            if seen:
                continue
            seen = True
        output.append(line)
    return "\n".join(output)


def fix_json_commas(text: str) -> str:
    return re.sub(r",(\s*[\]}])", r"\1", text)


def write(path: pathlib.Path, text: str, dry_run: bool):
    if not dry_run and path.read_text() != text:
        path.write_text(text)


def rewrite_documents(moves, dry_run: bool, report: list[str]):
    for source, target in moves:
        old = str(source.relative_to(ROOT))
        for hit in git_grep(re.escape(old)):
            path = hit.split(":", 1)[0]
            if path in REGISTRIES:
                continue
            if path in UNTOUCHABLE:
                report.append(f"stale: {path} names {old}")
                continue
            file = ROOT / path
            write(file, file.read_text().replace(old, str(target.relative_to(ROOT))), dry_run)


def mention_replacement(entry: dict) -> str:
    """What a document names instead of a deleted CTest entry: its one test
    file, or the directory holding its several."""
    files = entry["moved"] + entry["copied"]
    if len(files) == 1:
        return files[0]
    directories = sorted({str(pathlib.PurePath(path).parent) for path in files})
    return directories[0] + "/" if len(directories) == 1 else "tests/ui/"


def rewrite_mentions(entries: list[dict], dry_run: bool) -> list[str]:
    """Point Markdown mentions of deleted CTest entries at their test files.

    Release notes (CHANGELOG.md, changes/) record history and stay as written.
    """
    changed = set()
    for entry in entries:
        if entry.get("reasons") or not (entry["moved"] or entry["copied"]):
            continue
        test = entry["test"]
        replacement = mention_replacement(entry)
        pattern = re.compile(r"(?<![\w/.-])" + re.escape(test) + r"(?![\w.-])")
        for hit in git_grep(test, "-w", "-F", "-l"):
            path = hit.split(":", 1)[0]
            if not path.endswith(".md") or path == "CHANGELOG.md" or path.startswith("changes/"):
                continue
            file = ROOT / path
            text = file.read_text()
            new = pattern.sub(replacement, text)
            if new != text:
                write(file, new, dry_run)
                changed.add(path)
    return sorted(changed)


def prune_copies(dry_run: bool) -> int:
    """Remove a copied fixture once its last other user has gone."""
    moves = []
    for test in sorted((TESTS / "ui").rglob("*.gti")):
        source = FIXTURES / test.name
        if not source.is_file():
            continue
        body = source.read_text()
        if body.startswith("// "):
            body = body.split("\n", 1)[1] if "\n" in body else ""
        if body not in test.read_text():
            continue
        if shared(source, source.stem, TESTS / "none_test.py"):
            continue
        print(f"prune: {source.relative_to(ROOT)} (now only {test.relative_to(ROOT)})")
        moves.append((source, test))
        if not dry_run:
            subprocess.run(["git", "rm", "-q", str(source.relative_to(ROOT))], cwd=ROOT, check=True)
    report: list[str] = []
    path = ROOT / "tests/stdlib_surface_manifest.json"
    text = path.read_text()
    for source, target in moves:
        text = text.replace(f'"{source.relative_to(ROOT)}"', f'"{target.relative_to(ROOT)}"')
    write(path, text, dry_run)
    rewrite_documents(moves, dry_run, report)
    for line in report:
        print("  " + line)
    return 0


# -- Driver -------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wrappers", nargs="*")
    parser.add_argument("--list", action="store_true", help="classify every wrapper")
    parser.add_argument("--area", help="area for every given wrapper, overriding the rules")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--json", type=pathlib.Path, help="write a conversion record here")
    parser.add_argument("--rewrite-mentions", type=pathlib.Path, metavar="RECORD",
                        help="point documents naming the converted entries of a --json record "
                             "at their test files")
    parser.add_argument("--prune-copies", action="store_true",
                        help="delete tests/fixtures originals of converted tests that nothing uses now")
    arguments = parser.parse_args()
    if arguments.prune_copies:
        return prune_copies(arguments.dry_run)
    if arguments.rewrite_mentions:
        entries = json.loads(arguments.rewrite_mentions.read_text())
        for path in rewrite_mentions(entries, arguments.dry_run):
            print(f"rewrote: {path}")
        return 0
    names = ctest_names()
    selected = wrappers() if arguments.list or not arguments.wrappers else \
        [TESTS / name for name in arguments.wrappers]
    record = []
    status = 0
    batch = {path.name for path in selected}
    plans = []
    claimed: dict[str, tuple[str, str]] = {}
    for wrapper in selected:
        if not wrapper.is_file():
            print(f"missing: {wrapper.name}")
            status = 1
            continue
        result = plan(wrapper, names, arguments.area, batch)
        for fixture in list(result.fixtures):
            if fixture not in claimed:
                continue
            owner, signature = claimed[fixture]
            if signature == result.signatures[fixture]:
                # Both wrappers make the same observations: one test file.
                result.drop(fixture)
                print(f"shared: {fixture} is converted with {owner}")
            else:
                result.reasons.append(f"{fixture} is also run by {owner}")
        if not result.reasons:
            claimed.update({fixture: (wrapper.name, result.signatures[fixture])
                            for fixture in result.fixtures})
        plans.append(result)
    blocked = {result.wrapper.name for result in plans if result.reasons}
    for result in plans:
        # A wrapper whose importer stays cannot go.
        users = [user for user in importers(result.wrapper)
                 if pathlib.PurePath(user).name in blocked]
        if users and not result.reasons:
            result.reasons.append("imported by unconverted " + ", ".join(users))
    for result in plans:
        wrapper = result.wrapper
        entry = {"wrapper": wrapper.name, "test": result.test_name,
                 "reasons": result.reasons, "fixtures": result.fixtures,
                 "modes": result.modes, "reductions": result.reductions,
                 "moved": [str(t.relative_to(ROOT)) for _, t in result.moves],
                 "copied": [str(t.relative_to(ROOT)) for _, t in result.copies]}
        record.append(entry)
        if result.reasons:
            print(f"irregular: {wrapper.name}: " + "; ".join(result.reasons))
            continue
        for note in result.notes:
            print(f"note: {note}")
        print(f"{'would convert' if arguments.dry_run or arguments.list else 'convert'}: "
              f"{wrapper.name} -> {len(result.fixtures)} file(s), "
              f"{len(result.moves)} moved, {len(result.copies)} copied")
        if arguments.list:
            continue
        report: list[str] = []
        if not arguments.dry_run:
            for path, text in result.files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            for source, _ in result.moves:
                subprocess.run(["git", "rm", "-q", str(source.relative_to(ROOT))], cwd=ROOT, check=True)
            subprocess.run(["git", "rm", "-q", f"tests/{wrapper.name}"], cwd=ROOT, check=True)
        entry["seconds"] = edit_registries(result.test_name, wrapper.name, result.moves,
                                           report, arguments.dry_run)
        rewrite_documents(result.moves, arguments.dry_run, report)
        leftovers = [hit for hit in git_grep(result.test_name, "-w", "-F")
                     if not hit.startswith(f"tests/{wrapper.name}:")]
        entry["leftovers"] = leftovers
        for line in report + [f"mention: {hit[:160]}" for hit in leftovers]:
            print("  " + line)
    if arguments.json:
        arguments.json.write_text(json.dumps(record, indent=1) + "\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
