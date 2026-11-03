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
TESTS = {"owner_chain_runtime", "compiled_type_forwarding", "compiled_namespaces", "compiled_dependency_project", "compiled_value_types", "compiled_borrow_library", "compiled_primitive_library", "compiled_libraries_project", "compiled_library_project", "compiled_scalar_library", "mir_cutover_corpus_oracle", "practical_examples",
         "project_cli_workflow", "scoped_configuration_defines", "native_enum_bindgen",
         "native_typedef_bindgen", "native_union_bindgen", "native_variable_bindgen",
         "process_resource_policy", "llvm_embedding_project",
         "annotated_test_suites", "out_of_class_runtime", "module_system_runtime", "module_discovery_runtime", "typed_differential_smoke",
         "llvm_scalar_execution", "llvm_place_execution", "llvm_class_execution",
         "llvm_payload_execution", "llvm_placement_execution", "llvm_dispatch_execution",
         "llvm_native_c_indirect_execution", "llvm_native_c_aggregate_execution",
         "llvm_native_c_storage_execution"}
NESTED = {"ownership", "compiled-type-forwarding", "compiled-namespaces", "compiled-dependencies", "compiled-value-types", "compiled-borrowing", "compiled-primitive-library", "compiled-libraries", "compiled-scalar-library", "allocator-support", "annotated-tests", "infallible-embedding",
          "libc-region-provider", "llvm", "module-only", "module-system", "module-discovery", "native-callback-slots",
          "native-enum-bindings", "native-union-bindings", "native-variable-bindings",
          "objective-cpp-bridge", "scoped-configuration",
          "toy-3d-engine", "transit-planner", "window-factory"}


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


def load_manifest(path):
    manifest = json.loads(path.read_text())
    if manifest.get("schema") != 1 or not isinstance(manifest.get("features"), list):
        raise ValueError("Invalid preview manifest")
    return manifest


def feature_tests(feature):
    names = feature.get("tests")
    if not isinstance(names, list) or not names or any(
            not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name) for name in names):
        raise ValueError("Invalid focused preview checks")
    return list(names)


def manifest_tests(path):
    """Every committed feature's focused checks (maintainer replay)."""
    focused = []
    for feature in load_manifest(path)["features"]:
        focused.extend(feature_tests(feature))
    return focused


def pending_features(manifest, root):
    """The features this candidate admits: those whose merge is not already in the published preview.

    Preview promises that each admitted feature compiled and passed its own
    focused checks once; it does not re-prove every earlier feature on every
    admission. With no published preview (`previous` is null) every feature
    is pending.
    """
    previous = manifest.get("previous")
    pending = []
    for feature in manifest["features"]:
        merge = feature.get("merge")
        if not isinstance(merge, str) or not re.fullmatch(r"[0-9a-f]{40}", merge):
            raise ValueError("Invalid preview feature merge identity")
        if previous is None or subprocess.run(
                ["git", "-C", str(root), "merge-base", "--is-ancestor", merge, previous],
                capture_output=True).returncode != 0:
            pending.append(feature)
    return pending


def pending_tests(path, root):
    focused = []
    for feature in pending_features(load_manifest(path), root):
        focused.extend(feature_tests(feature))
    return focused


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build")
    parser.add_argument("--focused", action="append", default=[])
    parser.add_argument("--manifest", action="store_true", help="include every committed preview feature's focused checks")
    parser.add_argument("--pending", action="store_true",
                        help="include the focused checks of the features this candidate admits (not yet in the published preview)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = Path(args.build).resolve()
    cache = dict(re.findall(r"^([^/#\r\n][^:\r\n]*):[^=\r\n]+=([^\r\n]*)$",
                            (build / "CMakeCache.txt").read_text(), re.M))
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != root or cache.get("CMAKE_BUILD_TYPE") not in {
            "Release", "RelWithDebInfo", "MinSizeRel"}:
        raise ValueError("Example gate requires this candidate's optimized compiler build")
    focused = list(args.focused)
    if args.manifest:
        focused.extend(manifest_tests(root / ".gti-preview.json"))
    if args.pending:
        focused.extend(pending_tests(root / ".gti-preview.json", root))
    if "preview_admission" in focused:
        raise ValueError("Preview admission cannot recursively select itself")
    required = required_tests(sys.platform) | set(focused)
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
