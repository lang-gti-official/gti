"""Native test providers preserve link order, mode and failure evidence."""

import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from native_fixture_build import build_fixture
import container_iterator_movement_test as movement
from iterator_sanitizer_support import (gcc_sanitizer_arguments,
                                        native_driver_is_gcc,
                                        partition_main,
                                        selected_native_driver)
import lambda_pack_invocation_runtime_test as matrix
from sanitizer_support import SANITIZED_BUILD_TIMEOUT, scaled_timeout
import shared_accessor_range_runtime_test as accessor
import storage_lane_iterator_runtime_test as storage


class NativeFixtureBuildTests(unittest.TestCase):
    def test_ordinary_direct_build_is_unchanged(self):
        result = subprocess.CompletedProcess([], 0, "built", "")
        with patch("native_fixture_build.subprocess.run", return_value=result) as run:
            self.assertIs(build_fixture(
                ["gti", "main.gti", "--keep-cpp", "-O3"], "program",
                native_arguments=["-fno-elide-constructors"], text=True), result)
        run.assert_called_once_with(
            ["gti", "main.gti", "--keep-cpp", "-O3", "-o", "program",
             "--", "-fno-elide-constructors"], text=True)

    def test_provider_precedes_archive_and_preserves_driver_and_flags(self):
        environment = {"GTI_CXX": "/native/clang++",
                       "GTI_RUNTIME_LIBRARY": "/runtime/libgti_runtime.a",
                       "GTI_RUNTIME_INCLUDE": "/runtime/include"}
        emitted = subprocess.CompletedProcess([], 0, "emit\n", "warning\n")
        compiled = subprocess.CompletedProcess([], 9, "native\n", "link failure\n")
        with patch("native_fixture_build.subprocess.run",
                   side_effect=[emitted, compiled]) as run:
            result = build_fixture(
                ["gti", "fixture.gti", "--std", "c++20", "-O1"],
                Path("output/program"), native_sources=["provider one.cpp", "two.o"],
                native_arguments=["-fsanitize=address,undefined", "-ffast-math"],
                standard="c++20", optimization="O1", env=environment,
                capture_output=True, text=True, timeout=120)
        self.assertEqual(run.call_args_list[0].args[0],
                         ["gti", "fixture.gti", "--std", "c++20", "-O1",
                          "--emit-cpp", "-o", "output/program.gti.cpp"])
        native = run.call_args_list[1].args[0]
        self.assertEqual(native[0], "/native/clang++")
        self.assertEqual(native[1:3], ["-std=c++20", "-O1"])
        self.assertEqual([argument for argument in native if argument.startswith("-I")],
                         ["-I/runtime/include"])
        self.assertLess(native.index("provider one.cpp"), native.index("two.o"))
        self.assertLess(native.index("two.o"), native.index("/runtime/libgti_runtime.a"))
        self.assertLess(native.index("-ffast-math"), native.index("-fno-fast-math"))
        self.assertIn("-D__gti_strict_ieee754=1", native)
        self.assertEqual(run.call_args_list[1].kwargs, run.call_args_list[0].kwargs)
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (9, "emit\nnative\n", "warning\nlink failure\n"))

    def test_frontend_failure_never_invokes_native_compiler(self):
        failed = subprocess.CompletedProcess([], 65, "", "source error")
        with patch("native_fixture_build.subprocess.run", return_value=failed) as run:
            self.assertIs(build_fixture(["gti", "invalid.gti"], "program",
                                        native_sources=["provider.cpp"]), failed)
        self.assertEqual(run.call_count, 1)

    def test_native_suffix_is_in_same_translation_unit_and_keeps_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "probe"
            generated = output.with_suffix(".gti.cpp")
            calls = []

            def process(command, **options):
                calls.append(command)
                if "--emit-cpp" in command:
                    generated.write_text("// generated helper\n")
                    return subprocess.CompletedProcess(command, 0, "emit", "")
                self.assertEqual(generated.read_text(),
                                 "// generated helper\n\n// executing probe\n")
                self.assertIn(str(generated), command)
                return subprocess.CompletedProcess(command, 4, "", "probe failed")

            with patch("native_fixture_build.subprocess.run", side_effect=process):
                result = build_fixture(["gti", "fixture.gti"], output,
                                       native_suffix="// executing probe\n",
                                       text=True, capture_output=True)
            self.assertEqual(len(calls), 2)
            self.assertEqual((result.returncode, result.stdout, result.stderr),
                             (4, "emit", "probe failed"))


class IteratorSanitizerToolingTests(unittest.TestCase):
    fixtures = Path(__file__).parent / "fixtures"

    def check_exact_partition(self, fixture, partition, names):
        original = (self.fixtures / f"{fixture}.gti").read_bytes()
        prefix, opening, body = original.partition(b"int main() {\n")
        closing = b"  return 0;\n}\n"
        parts = partition(original)
        self.assertEqual(tuple(parts), names)
        regions = []
        for contents in parts.values():
            self.assertTrue(contents.startswith(prefix + opening))
            self.assertTrue(contents.endswith(closing))
            regions.append(contents[len(prefix + opening):-len(closing)])
        self.assertEqual(prefix + opening + b"".join(regions) + closing, original)
        # Verify exact assertion multiplicities as well as reconstruction.
        # Common generic declarations intentionally recur in each partition.
        expected = re.findall(rb"return [0-9]+;", body[:-len(closing)])
        actual = [value for region in regions for value in
                  re.findall(rb"return [0-9]+;", region)]
        self.assertEqual(actual, expected)
        return regions

    def test_movement_exact_regions_retain_common_relays_and_assertions(self):
        self.check_exact_partition("container_iterator_movement", movement.sanitizer_partitions,
                                   ("vector-array", "deque-list", "map-set", "unordered"))
        parts = movement.sanitizer_partitions(
            (self.fixtures / "container_iterator_movement.gti").read_bytes())
        for contents in parts.values():
            self.assertIn(b"class OwnerPair {", contents)
            self.assertIn(b"void exclusive_random<I>", contents)
            self.assertIn(b"void exclusive_bidirectional<I>", contents)

    def test_sparse_exact_regions_retain_all_windows_and_complete_states(self):
        regions = self.check_exact_partition("sparse_mask_iterator_allocations", storage.sanitizer_partitions,
                                            ("pool", "resource-storage", "simd-mask"))
        for region in regions:
            self.assertEqual(region.count(b"adoption_count_begin();"), 1)
            self.assertEqual(region.count(b"adoption_count_end();"), 1)
            self.assertEqual(region.count(b"round < 64"), 1)
            self.assertIn(b"adoption_allocations() != uint64_t(0)", region)
        self.assertIn(b"checksum != 7680", regions[0])
        self.assertIn(b"pool.contains(index) != live", regions[0])
        self.assertIn(b"checksum != 7680", regions[1])
        self.assertIn(b"slots.initialized(index) != live", regions[1])
        self.assertIn(b"checksum != 2048", regions[2])
        self.assertIn(b"mask[index] !=", regions[2])

    def test_partition_rejects_missing_duplicate_reordered_or_changed_cuts(self):
        source = b"// common\nint main() {\n  first();\n  second();\n  third();\n  return 0;\n}\n"
        names = ("first", "second", "third")
        cuts = (b"  second();", b"  third();")
        malformed = (source.replace(b"second();", b"renamed();"),
                     source.replace(b"second();", b"second();\n  second();"),
                     source.replace(b"second();", b"temporary();").replace(
                         b"third();", b"second();").replace(b"temporary();", b"third();"),
                     source.replace(b"  return 0;", b"  return 9;"),
                     source + b"// unaccounted suffix\n",
                     source + b"int main() {\n")
        for changed in malformed:
            with self.assertRaises(AssertionError):
                partition_main(changed, names, cuts)
        with self.assertRaises(AssertionError):
            partition_main(source, ("one", "one", "three"), cuts)
        with self.assertRaises(AssertionError):
            partition_main(source, ("one", "two"), cuts)
        with self.assertRaises(AssertionError):
            partition_main(source, names, (b"  first();", b"  second();"))
        added = source.replace(b"  third();", b"  check_new_state();\n  third();")
        parts = partition_main(added, names, cuts)
        self.assertEqual(sum(value.count(b"check_new_state();") for value in parts.values()), 1)

    def test_movement_preserves_original_modes_budgets_and_non_sanitized_source(self):
        built = []
        plans = []

        def process(command, **options):
            binary = Path(command[command.index("-o") + 1])
            binary.with_suffix(".gti.cpp").write_text("// GTI verified-MIR body:\n")
            built.append((command, options))
            return subprocess.CompletedProcess(command, 0, "", "")

        def run_plan(plan, build, jobs, observe):
            plans.append([(name, case[:4], case[4].read_bytes(), case[5])
                          for name, case in plan] if len(plans) == 0 else list(plan))
            if len(plans) == 1:
                for _, case in plan:
                    build(case)

        with patch.object(sys, "argv", ["movement", "fake-gti", "--jobs", "1"]), \
                patch.object(movement, "run_cases", run_plan), \
                patch.object(movement.subprocess, "run", process):
            movement.main()
        self.assertEqual(len(plans), 2)
        self.assertEqual(len(plans[0]), 27)  # original 24 cases; one positive becomes four
        self.assertTrue(plans[1], "failure/cleanup cases must still be scheduled")
        for _, (fixture, label, flags, native), contents, region in plans[0]:
            if not region:
                self.assertEqual(contents, (self.fixtures / f"{fixture}.gti").read_bytes())
            else:
                self.assertEqual((fixture, label), ("container_iterator_movement", "sanitized"))
            self.assertIn((label, flags, native), movement.MODES)
        for _, options in built:
            self.assertEqual(options["timeout"], 180)
            self.assertEqual(options["env"]["ASAN_OPTIONS"], "halt_on_error=1")
            self.assertEqual(options["env"]["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")

    def test_sparse_provider_order_original_modes_and_budget_remain(self):
        records = []

        def build(command, output, **options):
            records.append((command, Path(command[1]).read_bytes(), options))
            output.with_suffix(".gti.cpp").write_text("// GTI verified-MIR body:\n")
            return subprocess.CompletedProcess(command, 0, "", "")

        with tempfile.TemporaryDirectory(prefix="gti-iterator-partition-unit-") as directory, \
                patch.object(storage, "build_fixture", build), \
                patch.object(storage, "run", return_value=subprocess.CompletedProcess([], 0, "", "")), \
                contextlib.redirect_stdout(io.StringIO()):
            storage.prove_allocation_free_families(Path("fake-gti"), self.fixtures, Path(directory))
        self.assertEqual(len(records), 14)  # original twelve; sparse sanitizer becomes three
        for command, contents, options in records:
            name = Path(command[1]).stem
            self.assertEqual(options["timeout"], 180)
            self.assertEqual(options["native_sources"],
                             [self.fixtures / "stdlib_adoption_allocation_counts.cpp"])
            if name in ("sparse_mask_iterator_allocations-pool",
                        "sparse_mask_iterator_allocations-resource-storage",
                        "sparse_mask_iterator_allocations-simd-mask"):
                self.assertEqual((options["standard"], options["optimization"]), ("c++23", "O1"))
                self.assertEqual(options["native_arguments"],
                                 ("-fsanitize=address,undefined", "-fno-omit-frame-pointer"))
            else:
                self.assertEqual(contents, (self.fixtures / f"{name}.gti").read_bytes())

    def test_gcc_selection_uses_native_driver_macros_not_compatible_name_or_host(self):
        environment = {"GTI_CXX": "/native path/c++", "CXX": "/other/g++"}
        self.assertEqual(selected_native_driver(environment), "/native path/c++")
        self.assertEqual(selected_native_driver({"GTI_CXX": "", "CXX": "clang++"}), "clang++")
        self.assertEqual(selected_native_driver({}), "c++")
        gnu = "#define __GNUC__ 15\n#define __GNUG__ 15\n"
        with patch("iterator_sanitizer_support.subprocess.run",
                   return_value=subprocess.CompletedProcess([], 0, gnu, "")) as probe:
            self.assertEqual(gcc_sanitizer_arguments(environment),
                             ("-fno-inline-functions-called-once", "-fno-inline"))
        probe.assert_called_once_with(["/native path/c++", "-dM", "-E", "-x", "c++", "-"],
                                      input="", env=environment, capture_output=True, text=True, timeout=10)
        for macros in ("", "#define __GNUC__ 15\n", *(gnu + f"#define {other} 1\n" for other in
                ("__clang__", "__INTEL_COMPILER", "__INTEL_LLVM_COMPILER", "__NVCC__", "__CUDACC__"))):
            with patch("iterator_sanitizer_support.subprocess.run",
                       return_value=subprocess.CompletedProcess([], 0, macros, "")):
                self.assertEqual(gcc_sanitizer_arguments(environment), ())
        with patch("iterator_sanitizer_support.subprocess.run",
                   return_value=subprocess.CompletedProcess([], 1, "", "unknown driver")):
            with self.assertRaises(AssertionError):
                gcc_sanitizer_arguments(environment)

    def test_generic_hook_preserves_native_modes_input_order_and_budget(self):
        commands = []
        callbacks = []

        def process(command, **options):
            if "-o" in command:
                commands.append((command, options))
            return subprocess.CompletedProcess(command, 0, "", "")

        def extra(name, label, environment):
            callbacks.append((name, label))
            self.assertEqual(environment["ASAN_OPTIONS"], "halt_on_error=1")
            self.assertEqual(environment["UBSAN_OPTIONS"], "halt_on_error=1:print_stacktrace=1")
            self.assertEqual(accessor.native_arguments_by_case(name, label, environment), ())
            if name == "with_provider" and label == "sanitized":
                return ("-fno-inline-functions-called-once", "-fno-inline")
            return ()

        with patch.object(sys, "argv", ["matrix", "fake-gti", "--jobs", "1"]), \
                patch.object(matrix.subprocess, "run", process), \
                contextlib.redirect_stdout(io.StringIO()):
            matrix.main(names=("with_provider", "other"),
                        native_inputs_by_name={"with_provider": ("provider.cpp",)},
                        native_arguments_by_case=extra)
        self.assertEqual(len(commands), 16)
        self.assertEqual(len(callbacks), 16)
        for command, options in commands:
            provided = Path(command[1]).stem == "with_provider"
            sanitized = "-fsanitize=address,undefined" in command
            self.assertEqual(options["timeout"], SANITIZED_BUILD_TIMEOUT if sanitized else scaled_timeout(120))
            self.assertEqual("-fno-inline-functions-called-once" in command, provided and sanitized)
            self.assertEqual("-fno-inline" in command, provided and sanitized)
            if provided and sanitized:
                self.assertLess(command.index("-fno-omit-frame-pointer"), command.index("provider.cpp"))
                self.assertLess(command.index("provider.cpp"), command.index("-fno-inline-functions-called-once"))
                self.assertLess(command.index("-fno-inline-functions-called-once"), command.index("-fno-inline"))

    def test_selected_gcc_sanitizer_flag_retains_real_asan_and_ubsan_instrumentation(self):
        environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        if not native_driver_is_gcc(environment):
            self.skipTest("the extra sanitizer flag applies only to the selected GCC native driver")
        with tempfile.TemporaryDirectory(prefix="gti-iterator-sanitizer-control-") as directory:
            binary = Path(directory) / "control"
            command = [selected_native_driver(environment), "-std=c++23", "-O1",
                       str(self.fixtures / "iterator_sanitizer_control.cpp"),
                       str(self.fixtures / "iterator_sanitizer_control_allocation.cpp"),
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-fno-fast-math", "-ffp-contract=off", "-D__gti_strict_ieee754=1",
                       *gcc_sanitizer_arguments(environment), "-o", str(binary)]
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, (command, built.stdout, built.stderr))
            symbols = subprocess.run(["nm", str(binary)], env=environment,
                                     capture_output=True, text=True, timeout=30)
            self.assertEqual(symbols.returncode, 0, symbols.stderr)
            self.assertIn("__asan_init", symbols.stdout)
            self.assertIn("__ubsan_handle_add_overflow", symbols.stdout)
            for argument, diagnostic in (("a", "AddressSanitizer: heap-buffer-overflow"),
                                         ("u", "runtime error: signed integer overflow")):
                failed = subprocess.run([str(binary), argument], env=environment,
                                        capture_output=True, text=True, timeout=30)
                self.assertNotEqual(failed.returncode, 0, failed)
                self.assertEqual(failed.stdout, "", failed)
                self.assertIn(diagnostic, failed.stderr, failed)


if __name__ == "__main__":
    unittest.main()
