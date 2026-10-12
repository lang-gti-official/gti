#!/usr/bin/env python3
"""Run the authoritative automated example gates, rejecting missing/skipped tests."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

# The corpus owns all numbered top-level programs; the remaining established
# harnesses own nested projects and local HTTP/native prerequisites.
TESTS = {"mir_cutover_corpus_oracle", "practical_examples",
         "project_cli_workflow", "scoped_configuration_defines", "native_enum_bindgen",
         "native_typedef_bindgen", "process_resource_policy"}
NESTED = {"allocator-support", "libc-region-provider", "native-callback-slots",
          "native-enum-bindings", "objective-cpp-bridge", "scoped-configuration",
          "toy-3d-engine", "transit-planner"}


def validate_sources(paths):
    directories = {Path(path).parts[1] for path in paths if len(Path(path).parts) > 2 and path.endswith(".gti")}
    if directories != NESTED:
        raise ValueError(f"Review nested example coverage: added={sorted(directories - NESTED)}, "
                         f"removed={sorted(NESTED - directories)}")


def required_tests(platform):
    return TESTS | ({"objective_cpp_bridge"} if platform == "darwin" else set())


def validate_inventory(document, required):
    found = {test["name"]: test for test in document["tests"]}
    if required - found.keys():
        raise ValueError(f"Missing required example gates: {sorted(required - found.keys())}")
    for name in required:
        if any(prop["name"] == "DISABLED" and prop["value"] for prop in found[name].get("properties", [])):
            raise ValueError(f"Required example gate is disabled: {name}")


def validate_results(path, required):
    cases = ET.parse(path).getroot().findall(".//testcase")
    if {case.attrib.get("name") for case in cases} != required or len(cases) != len(required):
        raise ValueError("Example result inventory does not match the required gates")
    if any(case.find("skipped") is not None or case.find("failure") is not None or
           case.find("error") is not None or case.attrib.get("status") == "notrun" for case in cases):
        raise ValueError("A required example gate failed or was skipped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build")
    parser.add_argument("--focused", action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = Path(args.build).resolve()
    cache = dict(re.findall(r"^([^/#][^:]*):[^=]+=(.*)$", (build / "CMakeCache.txt").read_text(), re.M))
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != root or cache.get("CMAKE_BUILD_TYPE") not in {
            "Release", "RelWithDebInfo", "MinSizeRel"}:
        raise ValueError("Example gate requires this candidate's optimized compiler build")
    required = required_tests(sys.platform) | set(args.focused)
    sources = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "examples"],
                             capture_output=True, text=True, check=True)
    validate_sources(sources.stdout.split("\0"))
    inventory = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                               capture_output=True, text=True, check=True)
    validate_inventory(json.loads(inventory.stdout), required)
    print("Preview runs numbered corpus plus established automated project/native/HTTP gates.", flush=True)
    print("Interactive toy-3d-engine requires its native dependencies and a graphics session; "
          "GUI execution is not certified by this automated preview gate.", flush=True)
    if sys.platform != "darwin":
        print("Objective-C++ example is macOS-only; this host does not certify its execution.", flush=True)
    with tempfile.TemporaryDirectory(prefix="gti-preview-examples-") as temporary:
        results = Path(temporary) / "results.xml"
        subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
                        "--output-junit", str(results), "-R", "^(" + "|".join(re.escape(name) for name in sorted(required)) + ")$"], check=True)
        validate_results(results, required)


if __name__ == "__main__":
    main()
