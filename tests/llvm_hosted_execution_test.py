#!/usr/bin/env python3
"""Uninstrumented hosted programs through the real compiler/driver boundary."""

import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute
from llvm_evidence_manifest import build_manifest


HOSTED_CASES = {
    "arguments": ("llvm_hosted_arguments.gti", None),
    "scalar-return": ("llvm_hosted_scalar_return.gti", None),
    "scalar-return-failure": ("llvm_hosted_scalar_return_failure.gti",
                              "scalar return failure"),
    "union-first-field": ("llvm_hosted_union_first_field.gti", None),
    "union-first-field-aggregate": ("llvm_hosted_union_first_field_aggregate.gti", None),
    "union-first-field-failure": ("llvm_hosted_union_first_field_failure.gti",
                                 "union first-field initialization failure"),
    "generated-default": ("llvm_hosted_generated_default.gti", None),
    "generated-default-contexts": ("llvm_hosted_generated_default_contexts.gti", None),
    "generated-default-failure": ("llvm_hosted_generated_default_failure.gti",
                                  "generated default argument failure"),
    "success": ("llvm_hosted_success.gti", None),
    "startup-failure": ("llvm_hosted_startup_failure.gti",
                        "hosted startup failure"),
    "entry-failure": ("llvm_hosted_entry_failure.gti",
                      "hosted entry failure"),
    "source-location": ("stdlib_surface_source_location.gti", None),
    "unsafe-reference-load": ("llvm_hosted_unsafe_reference_load.gti", None),
    "unsafe-reference-return": ("llvm_hosted_unsafe_reference_return.gti", None),
    "class-owner-reference": ("llvm_hosted_class_owner_reference.gti", None),
    "class-pointee-reference": ("llvm_hosted_class_pointee_reference.gti", None),
    "reference-input": ("llvm_hosted_unsafe_reference_input.gti", None),
    "reference-input-failure": ("llvm_hosted_reference_input_failure.gti",
                                "borrowed argument failure"),
    "reference-return-readonly": ("llvm_hosted_unsafe_reference_invoke_readonly.gti", None),
    "reference-return-mutable": ("llvm_hosted_unsafe_reference_invoke_mutable.gti", None),
    "reference-return-failure": ("llvm_hosted_reference_return_failure.gti",
                                 "reference return failure"),
    "vector": ("stdlib_surface_vector.gti", None),
    "array": ("stdlib_surface_array.gti", None),
    "unsafe-text-return": ("llvm_hosted_unsafe_text_return.gti", None),
    "unsafe-cstring-return": ("llvm_hosted_unsafe_cstring_return.gti", None),
    "class-owner-text-return": ("llvm_hosted_class_owner_text_return.gti", None),
    "text-return-failure": ("llvm_hosted_text_return_failure.gti",
                            "counted text failure"),
}

HOSTED_GROUPS = {
    "arguments": ("arguments",),
    "startup": ("success", "startup-failure", "entry-failure",
                "unsafe-reference-load", "unsafe-reference-return",
                "reference-input", "reference-input-failure",
                "class-owner-reference", "class-pointee-reference"),
    "source-location": ("source-location",),
    "scalar-return": ("scalar-return", "scalar-return-failure"),
    "union-construction": ("union-first-field", "union-first-field-aggregate",
                           "union-first-field-failure"),
    "generated-construction": ("generated-default", "generated-default-contexts",
                               "generated-default-failure"),
    "reference-input": ("reference-input", "reference-input-failure"),
    "reference-return": ("reference-return-readonly", "reference-return-mutable",
                         "reference-return-failure"),
    "vector": ("vector",),
    "array": ("array",),
    "text-return": ("unsafe-text-return", "unsafe-cstring-return",
                    "class-owner-text-return", "text-return-failure"),
}


def evidence_cases(root: Path) -> dict[str, str]:
    fixtures = root / "tests/fixtures"
    return {
        name: (fixtures / filename).read_text(encoding="utf-8")
        for name, (filename, _) in HOSTED_CASES.items()
    }


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return HOSTED_GROUPS.copy()


def evidence_manifest(root: Path) -> dict:
    origins = {
        name: f"tests/fixtures/{filename}"
        for name, (filename, _) in HOSTED_CASES.items()
    }
    return build_manifest(
        "llvm_hosted_execution", evidence_cases(root), evidence_profiles(root),
        origins)


def list_evidence(root: Path) -> None:
    print(json.dumps(evidence_manifest(root), sort_keys=True))


def source_literals(gti, root, scratch, endpoints, env):
    library = scratch / "stdlib"
    shutil.copytree(root / "stdlib", library)
    declarations = library / "std/source_location.gti"
    with declarations.open("a", encoding="utf-8") as stream:
        stream.write("""
namespace llvm_source_metadata {
  bool file(std::string_view expected, std::size_t bytes,
            std::string_view value = gti_internal::source_file_name()) {
    return value == expected && value.size() == bytes;
  }
  bool function(std::string_view expected,
                std::string_view value = gti_internal::source_function_name()) {
    return value == expected;
  }
  uint32_t line(uint32_t value = gti_internal::source_line()) { return value; }
  uint32_t column(uint32_t value = gti_internal::source_column()) { return value; }
}
""")
    source = scratch / "metadata-\u00e9.gti"
    filename = json.dumps(str(source.resolve()), ensure_ascii=False)
    source.write_text(
        "#include <std/source_location>\n"
        "int main() {\n"
        f"  if (!llvm_source_metadata::file({filename}, std::size_t({len(str(source.resolve()).encode('utf-8'))}))) {{ return 1; }}\n"
        '  if (!llvm_source_metadata::function("main")) { return 2; }\n'
        "  if (llvm_source_metadata::line() != 5 || llvm_source_metadata::column() != 0) { return 3; }\n"
        "  return 0;\n}\n", encoding="utf-8")
    configured = {**env, "GTI_STDLIB_PATH": str(library)}
    for index, endpoint in enumerate(endpoints):
        result = execute(gti, root, scratch / f"source-literals-{index}",
                         source, endpoint, configured)
        if result.error or result.observable != (0, "", ""):
            raise AssertionError(f"source literals {endpoint}: {result}")
    print("source literals: UTF-8 byte extent and caller metadata passed all endpoints",
          flush=True)


def text_source_rejections(gti, root, scratch, endpoints, env):
    cases = (
        ("llvm_hosted_unsafe_text_escape.gti", "GTI-S2045", 5),
        ("llvm_hosted_unsafe_text_owner_write.gti", "GTI-S2017", 15),
        ("llvm_hosted_unsafe_cstring_escape.gti", "GTI-S2045", 5),
        ("llvm_hosted_unsafe_cstring_owner_write.gti", "GTI-S2017", 12),
    )
    for filename, code, line in cases:
        source = root / "tests/fixtures" / filename
        for index, endpoint in enumerate(endpoints):
            directory = scratch / f"text-rejection-{filename}-{index}"
            directory.mkdir()
            output = directory / "program"
            marker = b"preserved hosted text rejection\n"
            output.write_bytes(marker)
            command = [str(gti), str(source), "--backend", endpoint.backend,
                       f"-{endpoint.optimization}", "-o", str(output)]
            if endpoint.standard:
                command += ["--std", endpoint.standard]
            built = subprocess.run(command, cwd=root, env=env, text=True,
                                   capture_output=True, check=False, timeout=30)
            if (built.returncode != 65 or code not in built.stderr
                    or f"{filename}:{line}:" not in built.stderr
                    or "GTI-B0001" in built.stderr or "GTI-B0003" in built.stderr
                    or output.read_bytes() != marker):
                raise AssertionError(f"text lifetime rejection {endpoint}: {built}")
        print(f"{filename}: {code} and output preservation passed all endpoints",
              flush=True)



def scalar_return_rejections(gti, root, scratch, endpoints, env):
    cases = (
        ("float-narrow", "float source(double value) { return value; }", "double(1.25)"),
        ("integer-narrow", "int32_t source(int64_t value) { return value; }", "int64_t(7)"),
        ("signed-unsigned", "uint64_t source(int32_t value) { return value; }", "-1"),
        ("unsigned-signed", "int64_t source(uint64_t value) { return value; }", "uint64_t(7)"),
        ("bool-number", "int32_t source(bool value) { return value; }", "true"),
        ("char-number", "int32_t source(char value) { return value; }", "'Q'"),
        ("enum-number", "enum class Flag { one }; int32_t source(Flag value) { return value; }", "Flag::one"),
        ("typed-constant-narrow", "int8_t source() { return int64_t(7); }", ""),
        ("constexpr-local-narrow", "int8_t source() { constexpr int64_t value = 7; return value; }", ""),
    )
    for name, declaration, argument in cases:
        source = scratch / f"scalar-return-{name}.gti"
        source.write_text(declaration + f"\nint main() {{ [[discard]] source({argument}); return 0; }}\n",
                          encoding="utf-8")
        for index, endpoint in enumerate(endpoints):
            directory = scratch / f"scalar-reject-{name}-{index}"
            directory.mkdir()
            output = directory / "program"
            marker = b"preserved scalar return rejection\n"
            output.write_bytes(marker)
            command = [str(gti), str(source), "--backend", endpoint.backend,
                       f"-{endpoint.optimization}", "-o", str(output)]
            if endpoint.standard:
                command += ["--std", endpoint.standard]
            result = subprocess.run(command, cwd=root, env=env, text=True,
                                    capture_output=True, check=False, timeout=30)
            if (result.returncode != 65 or "GTI-S2003" not in result.stderr
                    or "GTI-B" in result.stderr or source.name not in result.stderr
                    or output.read_bytes() != marker):
                raise AssertionError(f"scalar source rejection {name} {endpoint}: {result}")
        print(f"scalar return {name}: GTI-S2003 and output preservation passed all endpoints",
              flush=True)
    checked = scratch / "scalar-return-checked-cast.gti"
    checked.write_text("int8_t source(int64_t value) { return int8_t(value); }\n"
                       "int main() { [[discard]] source(int64_t(128)); return 0; }\n",
                       encoding="utf-8")
    observations = []
    for index, endpoint in enumerate(endpoints):
        result = execute(gti, root, scratch / f"scalar-checked-cast-{index}",
                         checked, endpoint, env)
        if (result.error or result.returncode != 70 or result.stdout
                or "GTI-R0006" not in result.stderr or checked.name not in result.stderr):
            raise AssertionError(f"checked scalar return cast {endpoint}: {result}")
        observations.append(result.observable)
    if any(value != observations[0] for value in observations):
        raise AssertionError(f"checked scalar return cast attribution drift: {observations}")
    print("scalar return checked cast: original failure/site preserved at all endpoints", flush=True)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        list_evidence(Path(sys.argv[2]).resolve())
        return
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else "all"
    if selection not in {*HOSTED_GROUPS, "all", "status", "source-literals"}:
        raise ValueError(f"unknown hosted case: {selection}")
    selected_names = (tuple(HOSTED_CASES) if selection == "all"
                      else HOSTED_GROUPS.get(selection, ()))
    endpoints = (
        Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
        *(Endpoint("llvm", f"O{level}") for level in
          (range(4) if selection in {"all", "reference-return", "text-return", "generated-construction", "union-construction", "scalar-return", "arguments"}
           else (0, 3))),
    )
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-hosted-cli-") as temporary:
        scratch = Path(temporary)
        if selection in {"all", "source-literals"}:
            source_literals(gti, root, scratch, endpoints, env)
        selected_cases = 0
        for name in selected_names:
            filename, failure = HOSTED_CASES[name]
            selected_cases += 1
            observations = []
            comparator_gaps = []
            for index, endpoint in enumerate(endpoints):
                result = execute(gti, root, scratch / filename / str(index),
                                 root / "tests/fixtures" / filename, endpoint, env)
                if (result.error and endpoint.backend == "cpp"
                        and filename == "llvm_hosted_union_first_field_aggregate.gti"):
                    # Preserve real comparator evidence for this existing gap.
                    diagnostic = re.search(r"backend diagnostics saved at (.+/backend\.log)",
                                           result.stderr)
                    detail = (Path(diagnostic.group(1)).read_text(encoding="utf-8")
                              if diagnostic else "")
                    if (result.compile_returncode != 65 or "GTI-B0001" not in result.stderr
                            or "complete C++ text vocabulary" not in detail
                            or not any(name in detail for name in ("make_nested_union", "__gti_entry"))):
                        raise AssertionError(f"unexpected aggregate comparator failure {endpoint}: {result}")
                    comparator_gaps.append(endpoint.standard)
                    print(f"{filename}: retained {endpoint.standard} vocabulary rejection at {diagnostic.group(1)}",
                          flush=True)
                    continue
                if result.error:
                    raise AssertionError(f"{filename} {endpoint}: {result}")
                if name == "arguments":
                    ran = subprocess.run(
                        [str(scratch / filename / str(index) / "program"),
                         "", "a,b\nc", "\u00e9"], cwd=root, env=env,
                        text=True, capture_output=True, check=False, timeout=15)
                    if (ran.returncode, ran.stdout, ran.stderr) != (0, "", ""):
                        raise AssertionError(f"owned argument bytes {endpoint}: {ran}")
                expected_code = 70 if failure else 0
                if (result.returncode != expected_code or result.stdout
                        or (not failure and result.stderr)
                        or (failure and ("GTI-R0017" not in result.stderr
                                         or failure not in result.stderr
                                         or filename not in result.stderr))):
                    raise AssertionError(f"{filename} {endpoint}: {result}")
                observations.append(result.observable)
            if any(value != observations[0] for value in observations):
                raise AssertionError(f"hosted observable drift: {filename}: {observations}")
            levels = "O0-O3" if selection in {"all", "reference-return", "text-return", "generated-construction", "union-construction", "scalar-return", "arguments"} else "O0/O3"
            scope = (f"LLVM {levels}; recorded C++ aggregate vocabulary gap {comparator_gaps}"
                     if comparator_gaps else f"C++20/23 O3 and LLVM {levels}")
            print(f"{filename}: uninstrumented {scope} passed", flush=True)
        if selection == "scalar-return" and selected_cases != 2:
            raise AssertionError(f"scalar return requires both fixtures, ran {selected_cases}")
        if selection in {"all", "scalar-return"}:
            scalar_return_rejections(gti, root, scratch, endpoints, env)
        if selection == "union-construction" and selected_cases != 3:
            raise AssertionError(f"union construction requires all three fixtures, ran {selected_cases}")
        for status in (-2147483648, -1, 0, 1, 70, 255, 256, 512, 2147483647):
            if selection not in {"all", "status"}:
                continue
            expression = "-2147483647 - 1" if status == -2147483648 else str(status)
            source = scratch / f"exit-{status}.gti"
            source.write_text(
                "int ordinary() { return 256; }\n"
                "int main() { if (ordinary() != 256) { return 93; } return "
                + expression + "; }\n", encoding="utf-8")
            expected = status if 0 <= status <= 255 else 1
            for index, endpoint in enumerate(endpoints):
                result = execute(gti, root, scratch / f"status-{status}-{index}",
                                 source, endpoint, env)
                if result.error or result.observable != (expected, "", ""):
                    raise AssertionError(f"portable exit {status} {endpoint}: {result}")
            print(f"portable exit {status}: preserved GTI function result and status {expected}",
                  flush=True)
        if selection in {"all", "text-return"}:
            text_source_rejections(gti, root, scratch, endpoints, env)


if __name__ == "__main__":
    main()
