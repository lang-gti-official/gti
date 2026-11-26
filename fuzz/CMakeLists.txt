# Opt-in libFuzzer harnesses (Q-FUZZ-01B,
# docs/plans/compiler-bug-finding-and-triage.md). LLVM-first: they build in the
# default configuration and never need GTI_ENABLE_CPP_BACKEND; the pipeline
# harness ends at the LLVM backend.
#
# Only reached when GTI_BUILD_FUZZERS=ON, which the root CMakeLists gates on a
# Clang toolchain and pairs with tree-wide
# `-fsanitize=fuzzer-no-link,address,undefined` instrumentation so libFuzzer
# receives coverage feedback from the compiler library itself, not just the
# harness translation unit. The harness executables alone add the fuzzer
# driver at link.
#
# These targets have no install() rule on purpose: they must remain absent
# from normal installation, packages, and release artifacts.

foreach(harness "parser" "formatter" "pipeline" "source_loader")
  add_executable("gti_${harness}_fuzzer" "${harness}_fuzzer.cpp")
  target_link_options("gti_${harness}_fuzzer" PRIVATE "-fsanitize=fuzzer")
  set_target_properties("gti_${harness}_fuzzer"
                        PROPERTIES EXCLUDE_FROM_ALL FALSE)
endforeach()

target_link_libraries("gti_parser_fuzzer" PRIVATE gti_compiler)
target_link_libraries("gti_formatter_fuzzer" PRIVATE gti_compiler)
# gti_llvm_backend links gti_compiler publicly. The pipeline harness also
# re-parses and verifies the backend's textual module, so it links the LLVM
# components the backend's own closure already carries (one static copy).
llvm_map_components_to_libnames(gti_fuzz_llvm_libraries asmparser core support)
target_include_directories("gti_pipeline_fuzzer" SYSTEM PRIVATE
                           ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries("gti_pipeline_fuzzer" PRIVATE gti_llvm_backend
                      ${gti_fuzz_llvm_libraries})
target_link_libraries("gti_source_loader_fuzzer" PRIVATE gti_compiler)

# Fixed-corpus smoke: every harness executes its seed corpus under ASan/UBSan
# (-runs=0), and an injected failure must produce a replayable crash
# artifact. Bounded and deterministic; coverage exploration is an explicit
# local activity, never a merge gate.
if(BUILD_TESTING)
  add_test(
    NAME fuzzer_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/fuzzer_smoke_test.py"
            "$<TARGET_FILE:gti_parser_fuzzer>"
            "$<TARGET_FILE:gti_formatter_fuzzer>"
            "$<TARGET_FILE:gti_pipeline_fuzzer>"
            "$<TARGET_FILE:gti_source_loader_fuzzer>"
            "${CMAKE_CURRENT_SOURCE_DIR}/corpus"
  )
  set_tests_properties(fuzzer_smoke PROPERTIES TIMEOUT 600
                      LABELS "coverage-full;end-to-end")
endif()
