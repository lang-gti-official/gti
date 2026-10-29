#!/usr/bin/env python3
"""Optional mutation results preserve effects, borrows and required failures."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def execute(command, environment):
    return subprocess.run(list(map(str, command)), env=environment,
                          capture_output=True, text=True, timeout=180)


def main():
    compiler = Path(sys.argv[1]).resolve()
    repository = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"),
                       GTI_RUNTIME_INCLUDE=str(repository / "runtime/include"),
                       ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [("c++20", "O0", []), ("c++23", "O3", []),
             ("c++23", "O1", ["-fsanitize=address,undefined",
                               "-fno-omit-frame-pointer"])]
    sources = [(repository / "tests/fixtures/stdlib_discardable_containers.gti", ""),
               (repository / "tests/fixtures/stdlib_discardable_misc.gti", "1\n2\n"),
               (repository / "examples/115-discardable-results.gti", "")]
    with tempfile.TemporaryDirectory(prefix="gti-stdlib-discardable-") as directory:
        temporary = Path(directory)
        for standard, optimization, native in modes:
            for source, expected_output in sources:
                output = temporary / f"{source.stem}-{standard}-{optimization}"
                command = [compiler, source, "--std", standard,
                           f"-{optimization}", "-o", output]
                if source.name == "stdlib_discardable_misc.gti":
                    command += ["--execution-profile", "concurrent"]
                if native:
                    command += ["--", *native]
                compiled = execute(command, environment)
                assert compiled.returncode == 0, f"{command}\n{compiled.stdout}\n{compiled.stderr}"
                result = execute([output], environment)
                assert (result.returncode, result.stdout, result.stderr) == (0, expected_output, ""), \
                    (source.name, standard, optimization, result.returncode, result.stdout, result.stderr)
                print(f"{source.name}: {standard}-{optimization} passed", flush=True)

        # Instantiate the JSON branches whose redundant discards were removed.
        # Native conversion algorithms are unchanged; this guards the selected
        # optional/map declarations and their discard policies through MIR.
        source = repository / "tests/fixtures/stdlib_discardable_json.gti"
        result = execute([compiler, source, "--emit-cpp", "-o", temporary / "json.cpp"], environment)
        assert result.returncode == 0, f"JSON discard policy\n{result.stdout}\n{result.stderr}"
        print("JSON optional/map decode: verified emission passed", flush=True)

        # A mutation annotation must not make queries, allocation errors,
        # conditional atomic updates, or ownership-transfer results optional.
        negative = {
            "query": ("vector", "mut std::vector<int> values{}; values.size();"),
            "access": ("optional", "std::optional<int> slot{1}; slot.value();"),
            "allocation": ("memory", "std::allocator allocator{}; allocator.try_storage<int>(std::size_t(1));"),
            "exchange": ("utility", "mut int value = 1; std::exchange(value, 2);"),
            "conditional_update": ("atomic", "std::atomic<int> value{1}; value.compare_exchange_strong(1, 2);"),
            "resource_failure": ("resource_vector", "std::process_resource resource{}; "
                                 "auto binding = unsafe { std::bind_resource(resource) }; "
                                 "mut std::resource_vector<int, std::process_resource> values{binding}; "
                                 "values.try_reserve(std::size_t(1));"),
        }
        for name, (include, body) in negative.items():
            source = temporary / f"required-{name}.gti"
            imports = f"import std.{include};\n"
            if name == "resource_failure":
                imports += "import std.resource;\n"
            source.write_text(f"{imports}int main() {{ {body} return 0; }}\n")
            command = [compiler, source, "--emit-cpp", "-o", temporary / f"{name}.cpp"]
            if name == "conditional_update":
                command += ["--execution-profile", "concurrent"]
            result = execute(command, environment)
            errors = re.findall(r"error\[(GTI-[A-Z]\d+)\]", result.stderr)
            assert result.returncode != 0 and errors == ["GTI-S2009"], \
                f"{name}: {result.returncode}\n{result.stdout}\n{result.stderr}"
            print(f"required result {name}: rejected", flush=True)


if __name__ == "__main__":
    main()
