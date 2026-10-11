#!/usr/bin/env python3
"""Every harness wait and CTest timeout scales by one factor, GTI_TEST_TIMEOUT_SCALE."""

import ast
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from unittest import mock

import harness_timeouts
from harness_timeouts import scaled

TESTS = pathlib.Path(__file__).resolve().parent
VARIABLE = harness_timeouts.VARIABLE


def check_scale_contract():
    assert harness_timeouts.timeout_scale({}) == 1
    assert harness_timeouts.timeout_scale({VARIABLE: ""}) == 1
    assert harness_timeouts.timeout_scale({VARIABLE: "4"}) == 4
    for invalid in ("0", "-1", "1.5", "two", " 2", "2 "):
        try:
            harness_timeouts.timeout_scale({VARIABLE: invalid})
        except ValueError as error:
            assert VARIABLE in str(error) and repr(invalid) in str(error)
        else:
            raise AssertionError(f"accepted {invalid!r} as a timeout scale")
    with mock.patch.dict(os.environ, {VARIABLE: ""}):
        del os.environ[VARIABLE]
        assert scaled(120) == 120 and type(scaled(120)) is int
        assert scaled(0.5) == 0.5
    with mock.patch.dict(os.environ, {VARIABLE: "3"}):
        assert scaled(120) == 360 and type(scaled(120)) is int
        assert scaled(0.5) == 1.5
    with mock.patch.dict(os.environ, {VARIABLE: "zero"}):
        try:
            scaled(1)
        except ValueError:
            pass
        else:
            raise AssertionError("scaled() accepted an invalid scale")


def literal_timeouts(source):
    """Lines where a harness fixes a timeout instead of scaling it."""
    tree = ast.parse(source)
    found = []

    def numeric(node):
        return isinstance(node, ast.Constant) and type(node.value) in (int, float)

    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            found.extend(keyword.value.lineno for keyword in node.keywords
                         if keyword.arg == "timeout" and numeric(keyword.value))
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            arguments = node.args
            positional = arguments.posonlyargs + arguments.args
            defaults = zip(positional[len(positional) - len(arguments.defaults):],
                           arguments.defaults)
            keyword_only = zip(arguments.kwonlyargs, arguments.kw_defaults)
            found.extend(default.lineno for argument, default in (*defaults, *keyword_only)
                         if default is not None and argument.arg == "timeout"
                         and numeric(default))
    return sorted(found)


def imports_scaled(source):
    return any(isinstance(node, ast.ImportFrom) and node.module == "harness_timeouts"
               and any(alias.name == "scaled" for alias in node.names)
               for node in ast.parse(source).body)


def calls_scaled(source):
    return any(isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
               and node.func.id == "scaled" for node in ast.walk(ast.parse(source)))


def check_census():
    # The checker itself must see through the shapes a harness uses.
    assert literal_timeouts("import subprocess\nsubprocess.run(x, timeout=120)\n") == [2]
    assert literal_timeouts("def run(command, timeout=30):\n    pass\n") == [1]
    assert literal_timeouts("def run(command, *, timeout=0.5):\n    pass\n") == [1]
    assert literal_timeouts("f = lambda timeout=5: timeout\n") == [1]
    assert literal_timeouts("x.communicate(timeout=scaled(20))\n") == []
    assert literal_timeouts("def run(command, timeout):\n    x(timeout=timeout)\n") == []
    assert literal_timeouts("x(timeout=max(300.0, 40.0 * small))\n") == []
    assert imports_scaled("from harness_timeouts import scaled\n")
    assert not imports_scaled("import harness_timeouts\n")
    assert calls_scaled("x(timeout=scaled(20))\n")
    assert not calls_scaled('source = "int scaled(int value) { return value; }"\n')
    offenders = []
    checked = 0
    for path in sorted(TESTS.glob("*.py")):
        if path.name == harness_timeouts.__name__ + ".py":
            continue
        source = path.read_text()
        checked += 1
        offenders.extend(f"{path.name}:{line}: literal timeout" for line in literal_timeouts(source))
        if calls_scaled(source) and not imports_scaled(source):
            offenders.append(f"{path.name}: uses scaled() without importing harness_timeouts.scaled")
    if offenders:
        raise AssertionError("harness timeouts bypass GTI_TEST_TIMEOUT_SCALE:\n" + "\n".join(offenders))
    assert checked > 100, checked


def inventory(build):
    result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                            capture_output=True, text=True, timeout=scaled(60), check=True)
    return {test["name"]: {item["name"]: item["value"] for item in test["properties"]}
            for test in json.loads(result.stdout)["tests"]}


def check_cmake_scaling():
    with tempfile.TemporaryDirectory(prefix="gti-timeout-scale-") as temporary:
        root = pathlib.Path(temporary)
        module = (TESTS / "TestTimeouts.cmake").as_posix()
        interpreter = pathlib.Path(sys.executable).as_posix()
        (root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(Scale NONE)\n'
            'enable_testing()\n'
            f'include("{module}")\n'
            'add_test(NAME bounded COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'set_tests_properties(bounded PROPERTIES TIMEOUT 300 ENVIRONMENT "GTI_TEST_CASE_JOBS=2")\n'
            'add_test(NAME unbounded COMMAND "${CMAKE_COMMAND}" -E true)\n'
            f'add_test(NAME exported COMMAND "{interpreter}" -c '
            '"import os, sys; sys.exit(0 if os.environ.get(\'GTI_TEST_TIMEOUT_SCALE\') == \'3\' else 1)")\n'
            'gti_scale_test_timeouts()\n')

        def configure(scale):
            build = root / f"build-{scale}"
            return build, subprocess.run(
                ["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                 f"-DGTI_TEST_TIMEOUT_SCALE={scale}"],
                capture_output=True, text=True, timeout=scaled(60))

        build, result = configure(1)
        assert result.returncode == 0, result.stderr
        assert "Test timeouts scaled" not in result.stdout
        tests = inventory(build)
        assert tests["bounded"]["TIMEOUT"] == 300, tests["bounded"]
        assert tests["bounded"]["ENVIRONMENT"] == ["GTI_TEST_CASE_JOBS=2", "GTI_TEST_TIMEOUT_SCALE=1"]
        assert "TIMEOUT" not in tests["unbounded"], tests["unbounded"]
        assert tests["unbounded"]["ENVIRONMENT"] == ["GTI_TEST_TIMEOUT_SCALE=1"]

        build, result = configure(3)
        assert result.returncode == 0, result.stderr
        assert "Test timeouts scaled by 3 for 3 tests" in result.stdout, result.stdout
        tests = inventory(build)
        assert tests["bounded"]["TIMEOUT"] == 900, tests["bounded"]
        assert tests["bounded"]["ENVIRONMENT"] == ["GTI_TEST_CASE_JOBS=2", "GTI_TEST_TIMEOUT_SCALE=3"]
        assert tests["unbounded"]["TIMEOUT"] == 4500, tests["unbounded"]
        run = subprocess.run(["ctest", "--test-dir", str(build), "-R", "^exported$"],
                             capture_output=True, text=True, timeout=scaled(60))
        assert run.returncode == 0, run.stdout + run.stderr

        for invalid in ("0", "two"):
            build, result = configure(invalid)
            assert result.returncode != 0 and "GTI_TEST_TIMEOUT_SCALE" in result.stderr, invalid


def main():
    check_scale_contract()
    check_census()
    check_cmake_scaling()
    print("harness timeouts scale by GTI_TEST_TIMEOUT_SCALE")


if __name__ == "__main__":
    main()
