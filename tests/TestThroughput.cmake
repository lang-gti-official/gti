# One suite occupies exactly its bounded case-worker allocation in CTest.
# This is test orchestration only; every case, mode and assertion is retained.
# The harnesses continue to own their individual compile/execution deadlines.
set(GTI_TEST_CASE_JOBS "2" CACHE STRING "Bounded workers per parallel test suite")
if(NOT GTI_TEST_CASE_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_CASE_JOBS must be a positive integer")
endif()
# The largest compound suites bound the suite's critical path: with two
# workers each took eight to ten minutes alone on a tree that finishes in
# twenty. They reserve more workers; the scheduler charges them accordingly.
set(GTI_TEST_HEAVY_CASE_JOBS "4" CACHE STRING
  "Bounded workers for the largest parallel test suites")
if(NOT GTI_TEST_HEAVY_CASE_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_HEAVY_CASE_JOBS must be a positive integer")
endif()
set(gti_heavy_case_suites
  stdlib_surface returned_field_address_runtime
  complete_containers)

# Every gate has a bound, so a hung test fails at a known point instead of at
# CTest's implicit 1500 seconds. A test without its own TIMEOUT gets the
# default. Single-process gates measured near or past it get explicit
# headroom: the next standard bound at or above 1.5x their slowest contended
# full-suite duration (remote runs, October 2026). Instrumented or slow hosts
# scale every bound instead of relying on `ctest --timeout`, which reaches only
# tests that declare none.
set(GTI_TEST_DEFAULT_TIMEOUT "600" CACHE STRING
  "Timeout in seconds for a test that declares none")
if(NOT GTI_TEST_DEFAULT_TIMEOUT MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_DEFAULT_TIMEOUT must be a positive integer")
endif()
set(GTI_TEST_TIMEOUT_SCALE "1" CACHE STRING
  "Integer multiplier applied to every test timeout, such as for sanitizer builds")
if(NOT GTI_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_TIMEOUT_SCALE must be a positive integer")
endif()
set(gti_long_gate_timeouts
  failure_metadata 2400
  operator_relay_pipeline 2400
  compiler_pipeline 1800
  reflection_semantics 1800
  view_fields 900)
list(LENGTH gti_long_gate_timeouts gti_long_timeout_count)
math(EXPR gti_long_timeout_last "${gti_long_timeout_count} - 1")
foreach(index RANGE 0 ${gti_long_timeout_last} 2)
  list(GET gti_long_gate_timeouts ${index} gti_long_gate)
  math(EXPR gti_timeout_index "${index} + 1")
  list(GET gti_long_gate_timeouts ${gti_timeout_index} gti_long_gate_timeout)
  if(NOT TEST "${gti_long_gate}")
    message(FATAL_ERROR "Long-timeout gate is not registered: ${gti_long_gate}")
  endif()
  get_test_property("${gti_long_gate}" TIMEOUT gti_declared_timeout)
  if(gti_declared_timeout)
    message(FATAL_ERROR "Long-timeout gate already declares a TIMEOUT: ${gti_long_gate}")
  endif()
  set_tests_properties("${gti_long_gate}" PROPERTIES TIMEOUT "${gti_long_gate_timeout}")
endforeach()
get_property(gti_bounded_tests DIRECTORY PROPERTY TESTS)
foreach(suite IN LISTS gti_bounded_tests)
  get_test_property("${suite}" TIMEOUT gti_declared_timeout)
  if(NOT gti_declared_timeout)
    set(gti_declared_timeout "${GTI_TEST_DEFAULT_TIMEOUT}")
  endif()
  math(EXPR gti_scaled_timeout "${gti_declared_timeout} * ${GTI_TEST_TIMEOUT_SCALE}")
  set_tests_properties("${suite}" PROPERTIES TIMEOUT "${gti_scaled_timeout}")
  # Harnesses scale their own per-compile and per-run bounds to match
  # (tests/sanitizer_support.py scaled_timeout).
  set_property(TEST "${suite}" APPEND PROPERTY ENVIRONMENT
    "GTI_TEST_TIMEOUT_SCALE=${GTI_TEST_TIMEOUT_SCALE}")
endforeach()
# Runtime harnesses run on LLVM only (tests/backend_modes.py); the frozen C++
# backend's tests were removed. Their optimisation levels come from the
# caller's GTI_TEST_LLVM_LEVELS (default O0), which the daily slow tier sets
# through scripts/validation_checks.py; CTest passes it through, so no test
# pins it here.

if(Python3_Interpreter_FOUND)
  set(gti_parallel_case_suites
    callable_return_type_runtime
    consuming_range_runtime
    ordered_search_runtime
    associative_constructor_runtime associative_hash_contracts associative_map
    owner_chain_runtime borrowing_composition
    complete_containers composable_io_runtime
    constructor_null_native_runtime
    contiguous_iterator_completion container_iterator_movement
    paired_span_iterator_completion cursor_carrier_publication
    range_endpoint_parking_runtime user_dual_range_lifetime
    user_owning_reference_replacement
    dormant_capability_runtime
    expected_payload_pair_arguments_runtime
    generic_enum_instances_runtime generic_payload_enum_runtime hosted_constructor_identity_runtime
    hosted_exit_status_runtime
    returned_field_address_runtime
    http_url_operators
    lambda_pack_invocation_runtime
    nested_borrow_fields
    programmable_allocators_runtime
    shared_accessor_range_runtime
    scoped_parallel_threads
    source_prefix_owner_runtime source_resource_prefix_runtime
    source_text_address_runtime source_prefix_string_runtime
    many_fallible_scope_guards_runtime
    optimizer_raw_context_runtime optimizer_object_context_runtime
    cleanup_context_scope_runtime
    unordered_map
    vector_empty_constructor_runtime
    stdlib_modules
    stdlib_surface
  )
  foreach(suite IN LISTS gti_parallel_case_suites)
    if(NOT TEST "${suite}")
      message(FATAL_ERROR "Parallel case suite is not registered: ${suite}")
    endif()
    set(gti_suite_jobs "${GTI_TEST_CASE_JOBS}")
    if(suite IN_LIST gti_heavy_case_suites)
      set(gti_suite_jobs "${GTI_TEST_HEAVY_CASE_JOBS}")
    endif()
    set_property(TEST "${suite}" APPEND PROPERTY ENVIRONMENT
      "GTI_TEST_CASE_JOBS=${gti_suite_jobs}")
    set_tests_properties("${suite}" PROPERTIES PROCESSORS "${gti_suite_jobs}")
    set_property(TEST "${suite}" APPEND PROPERTY LABELS "bounded-case-workers")
    # Fresh CTest trees give unknown tests zero cost. Without a hint, one-slot
    # tests can keep filling the budget while compound suites wait for enough
    # slots, pushing the largest matrices into the tail. Use the unchanged
    # suite timeout as a conservative ordering hint, not a measured duration.
    get_test_property("${suite}" TIMEOUT gti_case_timeout)
    if(gti_case_timeout GREATER 1)
      set_tests_properties("${suite}" PROPERTIES COST "${gti_case_timeout}")
    else()
      set_tests_properties("${suite}" PROPERTIES COST 1)
    endif()
  endforeach()
  foreach(suite IN LISTS gti_heavy_case_suites)
    if(NOT suite IN_LIST gti_parallel_case_suites)
      message(FATAL_ERROR "Heavy case suite is not a parallel case suite: ${suite}")
    endif()
  endforeach()
  # Every registered test gets its observed duration as a scheduling hint.
  # CTest runs higher COST first; a fresh tree knows no durations, so without
  # this the largest tests start in declaration order and finish the shard
  # alone (a 25-minute test starting at minute 60 is the measured 85-89-minute
  # shard wall). tests/ci_test_durations.json is refreshed from CI JUnit; a
  # test missing there keeps its default, and the explicit hints below still
  # win so a stale table cannot demote a known long gate. Scheduling only: no
  # case, assertion or timeout changes.
  file(READ "${PROJECT_SOURCE_DIR}/tests/ci_test_durations.json" gti_durations_json)
  string(JSON gti_durations_seconds GET "${gti_durations_json}" seconds)
  string(JSON gti_durations_count LENGTH "${gti_durations_seconds}")
  if(gti_durations_count GREATER 0)
    math(EXPR gti_durations_last "${gti_durations_count} - 1")
    foreach(index RANGE 0 ${gti_durations_last})
      string(JSON gti_duration_name MEMBER "${gti_durations_seconds}" ${index})
      if(NOT TEST "${gti_duration_name}")
        continue()
      endif()
      string(JSON gti_duration_value GET "${gti_durations_seconds}" "${gti_duration_name}")
      # The table stores seconds with a fraction; COST is an integer hint.
      string(REGEX REPLACE "\\..*$" "" gti_duration_cost "${gti_duration_value}")
      if(NOT gti_duration_cost MATCHES "^[0-9]+$")
        message(FATAL_ERROR "ci_test_durations.json: non-numeric duration for ${gti_duration_name}")
      endif()
      get_test_property("${gti_duration_name}" COST gti_existing_cost)
      if(gti_duration_cost GREATER 1 AND (NOT gti_existing_cost OR gti_existing_cost LESS gti_duration_cost))
        set_tests_properties("${gti_duration_name}" PROPERTIES COST "${gti_duration_cost}")
      endif()
    endforeach()
  endif()
  # Long single-process gates keep an explicit hint that outranks the table.
  # Values are measured seconds from a full run, used only for scheduling
  # order; no case, assertion or timeout changes.
  set(gti_long_gate_costs
    failure_metadata 520
    allocator_raw_audit_independence 290
    operator_relay_pipeline 285
    filesystem_operations_runtime 220
    language_coverage_corpus 170
    aggregate_projection_runtime 140
    panic_runtime 140
    container_contract_failure 115
    cli_workflow 95
    mir_cutover_corpus_oracle 85
    simd_runtime 85)
  list(LENGTH gti_long_gate_costs gti_long_gate_count)
  math(EXPR gti_long_gate_last "${gti_long_gate_count} - 1")
  foreach(index RANGE 0 ${gti_long_gate_last} 2)
    list(GET gti_long_gate_costs ${index} gti_long_gate)
    math(EXPR gti_cost_index "${index} + 1")
    list(GET gti_long_gate_costs ${gti_cost_index} gti_long_gate_cost)
    if(NOT TEST "${gti_long_gate}")
      message(FATAL_ERROR "Long gate is not registered: ${gti_long_gate}")
    endif()
    if(NOT gti_long_gate IN_LIST gti_parallel_case_suites)
      get_test_property("${gti_long_gate}" COST gti_existing_cost)
      if(NOT gti_existing_cost OR gti_existing_cost LESS gti_long_gate_cost)
        set_tests_properties("${gti_long_gate}" PROPERTIES COST "${gti_long_gate_cost}")
      endif()
    endif()
  endforeach()
  add_test(NAME test_case_jobs
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/case_jobs_test.py")
  add_test(NAME test_throughput_registration
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_throughput_registration_test.py"
      "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
  # Registered after the bounding pass above, so bound them here.
  math(EXPR gti_scaled_timeout "300 * ${GTI_TEST_TIMEOUT_SCALE}")
  set_tests_properties(test_case_jobs test_throughput_registration
    PROPERTIES TIMEOUT "${gti_scaled_timeout}")
  set_property(TEST test_case_jobs test_throughput_registration APPEND PROPERTY
    ENVIRONMENT "GTI_TEST_TIMEOUT_SCALE=${GTI_TEST_TIMEOUT_SCALE}")
endif()

# Full is a census of every registered gate, never a hand-maintained subset.
# Fast is deliberately partial feedback, not completion/release evidence.
get_property(gti_throughput_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${gti_throughput_tests} APPEND PROPERTY LABELS "coverage-full")
set(gti_fast_feedback_tests
  compiler_library_boundary driver_library_boundary
  project_model executable_entry_semantics include_suggestions
  raw_pointer_pipeline raw_borrow_contract raw_access_contract
  mir_full_expression mir_program_initialization mir_hosted_startup
  optimizer_foundation optimizer_analyses optimizer_conformance
  layout_query_pipeline native_record_pipeline bindgen_pipeline
  source_text_pipeline source_unit_services optional_compiler_contract
  storage_adoption_contract resource_protocol_contract ephemeral_carrier_mir
  parser_conditional_recovery
)
foreach(suite IN LISTS gti_fast_feedback_tests)
  if(TEST "${suite}")
    set_property(TEST "${suite}" APPEND PROPERTY LABELS "feedback-fast")
  endif()
endforeach()
foreach(suite IN LISTS gti_throughput_tests)
  if(suite MATCHES "(_lsp$|^lsp_)")
    set_property(TEST "${suite}" APPEND PROPERTY LABELS "lsp")
  endif()
  if(suite MATCHES "(runtime|end_to_end|corpus|oracle|workflow|^stdlib_surface$)")
    set_property(TEST "${suite}" APPEND PROPERTY LABELS "end-to-end")
  endif()
endforeach()

# The daily slow tier: tests too slow, load-sensitive or corpus-wide for every
# commit. Each pattern below labels its matches `slow`; per-commit full
# validation excludes the label and .github/workflows/slow.yml runs it daily
# on the nightly commit, whose verdict the release train requires before a
# beta cut or stable promotion (docs/plans/test-suite-redesign.md, phase 0).
# Labels change only the tier; no case, assertion or timeout changes. A
# pattern that matches no registered test is an error, so the list cannot rot.
set(gti_slow_test_patterns
  # Whole-corpus walks over examples/ and the census baselines.
  "^gti_corpus$"
  # The corpus binaries' own fixtures (40-50 s of std-importing analyses
  # each) until they move to tests/ui.
  "^body_analysis_order_fixtures$"
  "^instance_reuse_fixtures$"
  "^trusted_body_analysis_fixtures$"
  "^mir_cutover_corpus_oracle$"
  "^language_coverage_corpus$"
  "^lowered_program_corpus_census$"
  # gti-test's tests/slow cases and its --isolate run (tests/gti_test.cmake).
  "^gti_test_(slow|isolate)$"
  # Raw-pointer-audit independence, including the >20-minute std import.
  "^allocator_raw_audit_independence(_.+)?$"
  # Whole-program MIR mutation loops.
  "^failure_metadata$"
  "^operator_relay_pipeline$"
  "^view_fields$"
  "^optimizer_prefix_relocation$"
  # LLVM execution matrices over 100 seconds; the cheaper ones stay per commit
  # as each family's LLVM execution smoke.
  "^llvm_(atomic|callable|class|dispatch|hosted|memory|native_c_aggregate|payload|place|placement|scalar|thread)_execution$"
  # The LLVM execution-evidence audit over the whole registered inventory.
  "^llvm_coverage_evidence$"
  # A CPU-ratio measurement that fails under runner load.
  "^single_body_compile_scaling$")
foreach(pattern IN LISTS gti_slow_test_patterns)
  set(gti_slow_matched FALSE)
  foreach(suite IN LISTS gti_throughput_tests)
    if(suite MATCHES "${pattern}")
      set_property(TEST "${suite}" APPEND PROPERTY LABELS "slow")
      set(gti_slow_matched TRUE)
    endif()
  endforeach()
  if(NOT gti_slow_matched)
    message(FATAL_ERROR "Slow-tier pattern matches no registered test: ${pattern}")
  endif()
endforeach()
