#!/usr/bin/env python3
"""The required automated example gates and their inventory/result checks.

Source-library CI and LSP CI use these to reject a missing, disabled or
skipped example gate rather than silently passing without it.
"""

from pathlib import Path
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
