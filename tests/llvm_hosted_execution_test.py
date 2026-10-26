#!/usr/bin/env python3
"""Uninstrumented hosted programs through the real compiler/driver boundary."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


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


def arguments(gti, root, scratch, endpoints, env):
    source = root / "tests/fixtures/llvm_hosted_arguments.gti"
    for index, endpoint in enumerate(endpoints):
        case = scratch / f"arguments-{index}"
        result = execute(gti, root, case, source, endpoint, env)
        if result.error or result.observable != (0, "", ""):
            raise AssertionError(f"hosted arguments {endpoint}: {result}")
        ran = subprocess.run([str(case / "program"), "", "a,b\nc", "\u00e9"],
                             cwd=root, env=env, text=True, capture_output=True,
                             check=False, timeout=15)
        if (ran.returncode, ran.stdout, ran.stderr) != (0, "", ""):
            raise AssertionError(f"owned argument bytes {endpoint}: {ran}")
    print("hosted arguments: empty, multiline and UTF-8 inputs passed all endpoints",
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


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selection = sys.argv[3] if len(sys.argv) > 3 else "all"
    if selection not in {"all", "startup", "status", "source-location", "source-literals", "arguments", "reference-input", "vector", "array", "text-return"}:
        raise ValueError(f"unknown hosted case: {selection}")
    cases = (
        ("llvm_hosted_success.gti", None),
        ("llvm_hosted_startup_failure.gti", "hosted startup failure"),
        ("llvm_hosted_entry_failure.gti", "hosted entry failure"),
        ("stdlib_surface_source_location.gti", None),
        ("llvm_hosted_unsafe_reference_load.gti", None),
        ("llvm_hosted_unsafe_reference_return.gti", None),
        ("llvm_hosted_unsafe_reference_input.gti", None),
        ("llvm_hosted_reference_input_failure.gti", "borrowed argument failure"),
        ("llvm_hosted_class_owner_reference.gti", None),
        ("llvm_hosted_class_pointee_reference.gti", None),
        ("stdlib_surface_vector.gti", None),
        ("stdlib_surface_array.gti", None),
        ("llvm_hosted_unsafe_text_return.gti", None),
        ("llvm_hosted_unsafe_cstring_return.gti", None),
        ("llvm_hosted_text_return_failure.gti", "counted text failure"),
    )
    endpoints = (
        Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
        Endpoint("llvm", "O0"), Endpoint("llvm", "O3"),
    )
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-hosted-cli-") as temporary:
        scratch = Path(temporary)
        if selection in {"all", "source-literals"}:
            source_literals(gti, root, scratch, endpoints, env)
        for filename, failure in cases:
            if selection in {"status", "source-literals", "arguments"}:
                continue
            if selection == "reference-input" and filename not in {
                    "llvm_hosted_unsafe_reference_input.gti",
                    "llvm_hosted_reference_input_failure.gti"}:
                continue
            if selection == "vector" and filename != "stdlib_surface_vector.gti":
                continue
            if selection == "array" and filename != "stdlib_surface_array.gti":
                continue
            if selection == "text-return" and filename not in {
                    "llvm_hosted_unsafe_text_return.gti",
                    "llvm_hosted_unsafe_cstring_return.gti",
                    "llvm_hosted_text_return_failure.gti"}:
                continue
            if selection == "startup" and filename in {
                    "stdlib_surface_source_location.gti", "stdlib_surface_vector.gti",
                    "stdlib_surface_array.gti", "llvm_hosted_unsafe_text_return.gti",
                    "llvm_hosted_unsafe_cstring_return.gti",
                    "llvm_hosted_text_return_failure.gti"}:
                continue
            if selection == "source-location" and filename != "stdlib_surface_source_location.gti":
                continue
            observations = []
            for index, endpoint in enumerate(endpoints):
                result = execute(gti, root, scratch / filename / str(index),
                                 root / "tests/fixtures" / filename, endpoint, env)
                if result.error:
                    raise AssertionError(f"{filename} {endpoint}: {result}")
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
            print(f"{filename}: uninstrumented C++20/23 O3 and LLVM O0/O3 passed",
                  flush=True)
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
        if selection in {"all", "arguments"}:
            arguments(gti, root, scratch, endpoints, env)


if __name__ == "__main__":
    main()
