#!/usr/bin/env python3
"""Every shared-harness client is scheduled/accounted, without nested log damage."""

import ast
import copy
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def inventory(build):
    # `ctest --show-only` writes LastTest.log. Query a copy of only the generated
    # registration files, never the build tree running the outer CTest process.
    with tempfile.TemporaryDirectory(prefix="gti-test-inventory-") as temporary:
        root = pathlib.Path(temporary)
        for path in build.rglob("CTestTestfile.cmake"):
            target = root / path.relative_to(build)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        result = subprocess.run(["ctest", "--test-dir", str(root), "--show-only=json-v1"],
                                capture_output=True, text=True, timeout=60, check=True)
        tests = json.loads(result.stdout)["tests"]
        for test in tests:
            for prop in test["properties"]:
                if prop["name"] == "WORKING_DIRECTORY":
                    directory = pathlib.Path(prop["value"])
                    # CTest derives an unspecified cwd from its registration
                    # directory. Restore that default to the real build tree;
                    # explicit source/build/fixture directories stay unchanged.
                    if directory.is_relative_to(root):
                        prop["value"] = str(build / directory.relative_to(root))
        return tests


def verify(tests, repository):
    wrappers = set()
    for path in (repository / "tests").glob("*.py"):
        nodes = ast.walk(ast.parse(path.read_text()))
        if any(isinstance(node, ast.ImportFrom) and
               node.module in {"case_jobs", "lambda_pack_invocation_runtime_test"}
               for node in nodes):
            wrappers.add(path.name)
    found = set()
    names = set()
    feedback = set()
    for test in tests:
        if test["name"] in names:
            raise AssertionError("duplicate CTest identity")
        names.add(test["name"])
        properties = {item["name"]: item["value"] for item in test["properties"]}
        labels = properties.get("LABELS", [])
        if "coverage-full" not in labels or properties.get("DISABLED"):
            raise AssertionError(f"{test['name']}: omitted from full coverage")
        timeout = properties.get("TIMEOUT")
        if not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
            raise AssertionError(f"{test['name']}: no explicit timeout bound")
        if "feedback-fast" in labels:
            feedback.add(test["name"])
        files = {pathlib.Path(arg).name for arg in test.get("command", [])}
        selected = wrappers & files
        if selected:
            found.update(selected)
            jobs = [value.split("=", 1)[1] for value in properties.get("ENVIRONMENT", [])
                    if value.startswith("GTI_TEST_CASE_JOBS=")]
            if len(jobs) != 1 or not jobs[0].isdigit() or int(jobs[0]) < 1:
                raise AssertionError(f"{test['name']}: missing/unbounded workers")
            if properties.get("PROCESSORS", 1) != int(jobs[0]):
                raise AssertionError(f"{test['name']}: worker/scheduler allocation mismatch")
            if "bounded-case-workers" not in labels:
                raise AssertionError(f"{test['name']}: missing worker label")
            if properties.get("COST") != max(1, properties.get("TIMEOUT", 1)):
                raise AssertionError(f"{test['name']}: missing compound-suite ordering hint")
    if found != wrappers:
        raise AssertionError(f"unregistered shared matrix clients: {wrappers - found}")
    if not feedback or feedback == names:
        raise AssertionError("fast feedback must be a nonempty partial selection")
    return found


def harness_sources(repository):
    """Map each tests/ harness and each possible referrer to its text."""
    harnesses = sorted({*repository.glob("tests/*_test.py"), *repository.glob("tests/*_test.lua")})
    referrers = {path for pattern in ("tests/*.py", "tests/*.lua", "tests/*.cpp", "tests/*.cmake",
                                      "tests/CMakeLists.txt", "fuzz/CMakeLists.txt", "scripts/*.py",
                                      ".gitea/workflows/*.yml")
                 for path in repository.glob(pattern)}
    return ({path.name for path in harnesses},
            {path.relative_to(repository).as_posix(): path.read_text(errors="replace")
             for path in referrers})


def verify_harness_inventory(tests, harnesses, referrers, inventory):
    # A harness nobody runs is invisible coverage. Every tracked harness is a
    # registered command, conditionally registered or used by another file, or
    # is deliberately standalone and says why; stale entries are rejected too.
    registered = {pathlib.Path(arg).name for test in tests for arg in test.get("command", [])}
    def referenced(name):
        stem = name.rsplit(".", 1)[0]
        pattern = re.compile(r"\b(?:import|from)\s+" + re.escape(stem) + r"\b")
        return any(other != "tests/" + name and (name in text or pattern.search(text))
                   for other, text in referrers.items())
    categories = inventory.get("categories", {})
    listed = {}
    for row in inventory.get("harnesses", []):
        name = row.get("harness")
        if name in listed or row.get("category") not in categories or not str(row.get("purpose", "")).strip():
            raise AssertionError(f"{name}: invalid standalone harness entry")
        listed[name] = row
    for name in listed:
        if name not in harnesses:
            raise AssertionError(f"{name}: standalone entry names no harness")
        if name in registered or referenced(name):
            raise AssertionError(f"{name}: standalone entry is registered or used; remove it")
    unlisted = sorted(name for name in harnesses
                      if name not in registered and not referenced(name) and name not in listed)
    if unlisted:
        raise AssertionError(f"unregistered test harnesses: {unlisted}")
    return len(listed)


def verify_cold_ordering():
    # Exercise the public COST/PROCESSORS scheduler contract on a cold tree,
    # rather than assuming that JSON properties imply the desired start order.
    with tempfile.TemporaryDirectory(prefix="gti-test-cost-") as temporary:
        root = pathlib.Path(temporary)
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Cost NONE)\n'
            'enable_testing()\n'
            'add_test(NAME small_first COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'add_test(NAME small_second COMMAND "${CMAKE_COMMAND}" -E true)\n'
            f'set_tests_properties(small_second PROPERTIES WORKING_DIRECTORY "{root.as_posix()}")\n'
            'add_test(NAME compound COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'set_tests_properties(compound PROPERTIES PROCESSORS 2 COST 100)\n'
            'add_test(NAME largest COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'set_tests_properties(largest PROPERTIES PROCESSORS 2 COST 200)\n')
        build = root / "build"
        subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja"],
                       capture_output=True, text=True, timeout=30, check=True)
        first, second = inventory(build), inventory(build)
        if first != second:
            raise AssertionError("scratch discovery changes the registered inventory")
        for test in first:
            properties = {prop["name"]: prop["value"] for prop in test["properties"]}
            expected = root if test["name"] == "small_second" else build
            if properties["WORKING_DIRECTORY"] != str(expected):
                raise AssertionError("inventory lost the default or explicit working directory")
        result = subprocess.run(["ctest", "--test-dir", str(build), "--parallel", "2"],
                                capture_output=True, text=True, timeout=30, check=True)
        starts = re.findall(r"Start\s+\d+:\s+(\S+)", result.stdout)
        if starts[:2] != ["largest", "compound"] or set(starts) != {
                "small_first", "small_second", "compound", "largest"}:
            raise AssertionError(f"cold compound-suite ordering lost: {starts}")


def main():
    repository, build = map(lambda value: pathlib.Path(value).resolve(), sys.argv[1:])
    tests = inventory(build)
    clients = verify(tests, repository)
    for target_name, mutation in ((name, kind)
                                 for name in ("stdlib_surface", "generic_payload_enum_runtime")
                                 for kind in ("allocation", "full", "disabled", "cost", "timeout")):
        changed = copy.deepcopy(tests)
        target = next(test for test in changed if test["name"] == target_name)
        properties = target["properties"]
        if mutation == "allocation":
            properties.append({"name": "PROCESSORS", "value": 999})
        elif mutation == "full":
            for prop in properties:
                if prop["name"] == "LABELS":
                    prop["value"].remove("coverage-full")
        elif mutation == "disabled":
            properties.append({"name": "DISABLED", "value": True})
        elif mutation == "timeout":
            properties[:] = [prop for prop in properties if prop["name"] != "TIMEOUT"]
        else:
            properties.append({"name": "COST", "value": 0})
        try:
            verify(changed, repository)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"accepted forged {mutation} registration")
    harnesses, referrers = harness_sources(repository)
    standalone_inventory = json.loads((repository / "tests/standalone_harnesses.json").read_text())
    standalone = verify_harness_inventory(tests, harnesses, referrers, standalone_inventory)
    forged_inventories = []
    dropped = copy.deepcopy(standalone_inventory)
    dropped["harnesses"].pop()
    forged_inventories.append(("dropped", tests, dropped))
    phantom = copy.deepcopy(standalone_inventory)
    phantom["harnesses"].append({"harness": "phantom_harness_test.py", "category": "automation",
                                 "purpose": "Names no file."})
    forged_inventories.append(("phantom", tests, phantom))
    stale = copy.deepcopy(tests)
    stale[0].setdefault("command", []).append(standalone_inventory["harnesses"][0]["harness"])
    forged_inventories.append(("registered", stale, standalone_inventory))
    for kind, forged_tests, forged in forged_inventories:
        try:
            verify_harness_inventory(forged_tests, harnesses, referrers, forged)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"accepted forged {kind} standalone harness inventory")
    verify_cold_ordering()
    print(f"Full census: {len(tests)} gates; {len(clients)} parallel clients accounted; "
          f"{standalone} standalone harnesses inventoried; "
          "forged allocation, omission, disabling, ordering and unbounded registration rejected")


if __name__ == "__main__":
    main()
