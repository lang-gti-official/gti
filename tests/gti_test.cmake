# gti-test: the directory-walking test runner. One `.gti` file under
# tests/ui is one test; adding a test touches no registration. See
# tests/ui/README.md and docs/plans/test-suite-redesign.md.
add_executable(gti-test
  "${PROJECT_SOURCE_DIR}/tests/runner/main.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/execute.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/snapshot.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/lsp.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/project.cpp"
  "${PROJECT_SOURCE_DIR}/tests/runner/test_file.cpp")
target_link_libraries(gti-test PRIVATE gti_driver)
# Workers end with _exit; under source coverage they write their profile
# first, so in-process cases count (docs/architecture/verification.md).
if(GTI_ENABLE_SOURCE_COVERAGE)
  target_compile_definitions(gti-test PRIVATE GTI_SOURCE_COVERAGE)
endif()
target_include_directories(gti-test SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
# --isolate runs the sibling `gti`.
add_dependencies(gti-test gti)
# LSP tests drive the sibling `gti_lsp`.
if(TARGET gti_lsp)
  add_dependencies(gti-test gti_lsp)
endif()

# CI sets this so every gti-test entry below also writes a per-case JUnit
# report, <directory>/<entry>.xml; the workflows upload it. Empty: none.
set(GTI_TEST_REPORT_DIRECTORY "" CACHE PATH
  "Directory for the gti-test entries' per-case JUnit reports (CI)")
function(gti_test_report name out)
  if(GTI_TEST_REPORT_DIRECTORY)
    set(${out} --junit "${GTI_TEST_REPORT_DIRECTORY}/${name}.xml" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# Per-test limits are CPU seconds measured on a fast development machine;
# hosted runners are about twice as slow, and sanitizer or other slow trees
# scale every bound by GTI_TEST_TIMEOUT_SCALE.
# (TestThroughput.cmake defines the cache entry after this file and also
# multiplies each entry's TIMEOUT below by it.)
set(gti_test_timeout_scale "${GTI_TEST_TIMEOUT_SCALE}")
if(NOT gti_test_timeout_scale MATCHES "^[1-9][0-9]*$")
  set(gti_test_timeout_scale 1)
endif()
math(EXPR gti_test_time_scale "2 * ${gti_test_timeout_scale}")

# The per-commit gti-test entries, all named gti_test_ui*, so `ctest -R
# '^gti_test_ui'` runs every tests/ui, tests/project and tests/lsp case and
# CI's gate selects them with one pattern. One entry for the whole tree
# outgrew a 600 s bound on 4-vCPU hosted runners (about 2.5x slower than a
# development machine), so tests/ui is sharded; each entry stays near a
# minute locally and its bound only catches hangs.
# - gti_test_ui: the runner's self-test, `--tidy` over every root, and the
#   tests/project and tests/lsp cases. Validation blocks cite this name.
# - gti_test_ui_<i>: shard i of tests/ui (`gti-test --shard i/N`, by a stable
#   hash of the path). Raise gti_test_ui_shards when a shard nears two
#   minutes locally. At 6,800 files four shards took 118-137 s each locally
#   and hosted macOS runs about 3x slower, near the 600 s bound; eight keep
#   each shard near a minute locally.
set(gti_test_ui_shards 8)
gti_test_report(gti_test_ui gti_test_ui_report)
add_test(NAME gti_test_ui
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/runner/check.py"
    $<TARGET_FILE:gti-test> --jobs 4 --without-ui ${gti_test_ui_report})
set_tests_properties(gti_test_ui PROPERTIES PROCESSORS 4 TIMEOUT 600
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale}")
foreach(gti_test_ui_shard RANGE 1 ${gti_test_ui_shards})
  set(gti_test_ui_name gti_test_ui_${gti_test_ui_shard})
  gti_test_report(${gti_test_ui_name} gti_test_ui_shard_report)
  add_test(NAME ${gti_test_ui_name}
    COMMAND gti-test "${PROJECT_SOURCE_DIR}/tests/ui"
      --shard ${gti_test_ui_shard}/${gti_test_ui_shards} --jobs 4
      ${gti_test_ui_shard_report})
  set_tests_properties(${gti_test_ui_name} PROPERTIES PROCESSORS 4 TIMEOUT 600
    ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale};GTI_TEST_LAST_RUN=${CMAKE_CURRENT_BINARY_DIR}/${gti_test_ui_name}.last-run")
  # The seeded-bug measure's gate tier (suite-measures.yml) selects by label.
  set_property(TEST ${gti_test_ui_name} APPEND PROPERTY LABELS "gti-test-ui")
endforeach()

# Every single-file example (`examples/*.gti`) must analyse to verified MIR
# with no diagnostics: gti-test treats a directive-less file as check-pass.
# examples/gti-test.defaults holds the few per-example analysis options;
# imports of sibling directories (`import "demo/html.gti"`) are copied beside
# the case. Subdirectories hold project-style examples and those modules;
# project tests own them. Running and census belong to the slow tier's corpus
# tool.
file(GLOB gti_examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/*.gti")
gti_test_report(gti_test_examples gti_test_examples_report)
add_test(NAME gti_test_examples
  COMMAND gti-test --root "${PROJECT_SOURCE_DIR}/examples" --jobs 4
    ${gti_test_examples_report} ${gti_examples})
# Examples are whole programs, several above the 2 s check budget; the scaled
# limit here only catches hangs.
math(EXPR gti_examples_time_scale "8 * ${gti_test_timeout_scale}")
set_tests_properties(gti_test_examples PROPERTIES PROCESSORS 4
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_examples_time_scale};GTI_TEST_LAST_RUN=${CMAKE_CURRENT_BINARY_DIR}/gti-test-examples.last-run")

# The daily slow tier's gti-test entries (TestThroughput.cmake labels them
# `slow`, so per-commit full validation leaves them to slow.yml):
# - gti_test_slow: every `//@ slow` test under tests/slow, at its 120 s limit.
# - gti_test_isolate: tests/ui with every case in a fresh `gti` process through
#   the real CLI. Per-commit runs share one analysed stdlib per worker, a path
#   users never take; this proves both paths agree.
gti_test_report(gti_test_slow gti_test_slow_report)
add_test(NAME gti_test_slow
  COMMAND gti-test "${PROJECT_SOURCE_DIR}/tests/slow" --jobs 4
    ${gti_test_slow_report})
set_tests_properties(gti_test_slow PROPERTIES PROCESSORS 4 TIMEOUT 1800
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale};GTI_TEST_LAST_RUN=${CMAKE_CURRENT_BINARY_DIR}/gti-test-slow.last-run")
# - gti_unit_tests_slow: the RFC 0003 retained-storage unit cases over about
#   2 s each (forged and structural MIR checks over std-importing programs);
#   gti_unit_tests skips them unless GTI_UNIT_SLOW=1.
add_test(NAME gti_unit_tests_slow
  COMMAND gti_unit_tests
    --test ownerless_failure_snapshot_retention
    --test ownerless_generic_array_element_return_proof
    --test ownerless_inline_array_call_supplier_set
    --test ownerless_multiple_text_owners
    --test ownerless_owned_storage_borrowed_read
    --test ownerless_owned_storage_multiple_owners
    --test ownerless_owned_storage_multiple_records
    --test ownerless_owned_storage_read_wrapper
    --test ownerless_owned_storage_receiver_read
    --test ownerless_owned_storage_receiver_write
    --test ownerless_owned_storage_reference_argument
    --test ownerless_owned_storage_reference_receiver
    --test ownerless_replacement_dependency_verification
    --test ownerless_retaining_call_conditional_cleanup_proof
    --test ownerless_retaining_explicit_cleanup
    --test ownerless_retaining_local_cleanup
    --test ownerless_retaining_local_construction
    --test ownerless_retaining_named_callable_escape
    --test ownerless_retaining_store_contract_separation
    --test ownerless_retaining_store_effects_on_lowered_program
    --test ownerless_returned_text_retention
    --test replaced_record_and_pack_proofs
)
set_tests_properties(gti_unit_tests_slow PROPERTIES TIMEOUT 1800
  ENVIRONMENT "GTI_UNIT_SLOW=1;GTI_TEST_TIME_SCALE=${gti_test_time_scale}")
gti_test_report(gti_test_isolate gti_test_isolate_report)
add_test(NAME gti_test_isolate
  COMMAND gti-test "${PROJECT_SOURCE_DIR}/tests/ui" --isolate --jobs 4
    ${gti_test_isolate_report})
set_tests_properties(gti_test_isolate PROPERTIES PROCESSORS 4 TIMEOUT 3600
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale};GTI_TEST_LAST_RUN=${CMAKE_CURRENT_BINARY_DIR}/gti-test-isolate.last-run")

# gti-corpus: one walk of the example corpus checking the whole-program
# analysis invariants (body-analysis order, trusted-body parity, instance
# reuse) per entry, with a JSON report. Slow tier only.
add_executable(gti-corpus "${PROJECT_SOURCE_DIR}/tests/corpus/gti_corpus.cpp")
target_link_libraries(gti-corpus PRIVATE gti_compiler)
add_test(NAME gti_corpus
  COMMAND gti-corpus --root "${PROJECT_SOURCE_DIR}" --jobs 4
          --json "${CMAKE_CURRENT_BINARY_DIR}/gti-corpus.json")
set_tests_properties(gti_corpus PROPERTIES TIMEOUT 7200 PROCESSORS 4)

# gti_unit_tests: every tests/unit/*.cpp linked into one executable of C++
# unit tests of library code (runtime, driver, LLVM passes, formatter). A file
# declares GTI_UNIT_TEST cases (tests/unit/unit_test.h); adding one needs no
# CMake edit. `gti_unit_tests --list`, `--test NAME`.
file(GLOB gti_unit_sources CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/tests/unit/*.cpp")
add_executable(gti_unit_tests ${gti_unit_sources})
target_include_directories(gti_unit_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/unit" "${PROJECT_SOURCE_DIR}/runtime/src")
target_include_directories(gti_unit_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_compile_features(gti_unit_tests PRIVATE cxx_std_23)
target_compile_definitions(gti_unit_tests PRIVATE GTI_UNIT_TESTS_RUNNER)
target_link_libraries(gti_unit_tests PRIVATE
  gti_driver gti_llvm_backend gti_runtime Threads::Threads
  ${CMAKE_DL_LIBS})
# The embedding context checks are assertions.
set_source_files_properties(
  "${PROJECT_SOURCE_DIR}/tests/unit/runtime_embedding_tests.cpp"
  PROPERTIES COMPILE_OPTIONS "-UNDEBUG")
add_test(NAME gti_unit_tests COMMAND gti_unit_tests)
set_tests_properties(gti_unit_tests PROPERTIES TIMEOUT 300
  ENVIRONMENT "GTI_TEST_TIME_SCALE=${gti_test_time_scale}")
