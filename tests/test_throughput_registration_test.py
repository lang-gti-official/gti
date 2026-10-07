#!/usr/bin/env python3
"""Every shared-harness client is scheduled/accounted, without nested log damage."""

import ast
import copy
import json
import pathlib
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
        return json.loads(result.stdout)["tests"]


def verify(tests, repository):
    wrappers = {"lambda_pack_invocation_runtime_test.py", "stdlib_surface_test.py"}
    for path in (repository / "tests").glob("*.py"):
        nodes = ast.walk(ast.parse(path.read_text()))
        if any(isinstance(node, ast.ImportFrom) and
               node.module == "lambda_pack_invocation_runtime_test" for node in nodes):
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
    if found != wrappers:
        raise AssertionError(f"unregistered shared matrix clients: {wrappers - found}")
    if not feedback or feedback == names:
        raise AssertionError("fast feedback must be a nonempty partial selection")
    return found


def main():
    repository, build = map(lambda value: pathlib.Path(value).resolve(), sys.argv[1:])
    tests = inventory(build)
    clients = verify(tests, repository)
    for mutation in ("allocation", "full", "disabled"):
        changed = copy.deepcopy(tests)
        target = next(test for test in changed if test["name"] == "stdlib_surface")
        properties = target["properties"]
        if mutation == "allocation":
            properties.append({"name": "PROCESSORS", "value": 999})
        elif mutation == "full":
            for prop in properties:
                if prop["name"] == "LABELS":
                    prop["value"].remove("coverage-full")
        else:
            properties.append({"name": "DISABLED", "value": True})
        try:
            verify(changed, repository)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"accepted forged {mutation} registration")
    print(f"Full census: {len(tests)} gates; {len(clients)} parallel clients accounted; "
          "forged allocation, omission and disabling rejected")


if __name__ == "__main__":
    main()
