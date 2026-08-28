# Preserve the build-tree location of test executables and corpus tools while
# moving their target ownership out of the root project file.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

add_executable("gti_c_abi_smoke" "${PROJECT_SOURCE_DIR}/tests/c_abi_smoke.c")
target_include_directories(
  "gti_c_abi_smoke"
  PRIVATE "${PROJECT_SOURCE_DIR}/runtime/include"
)
target_compile_features("gti_c_abi_smoke" PRIVATE c_std_11)
add_test(NAME c_abi_header_boundary COMMAND gti_c_abi_smoke)

add_executable("gti_runtime_failure_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_failure_tests.cpp")
target_include_directories(
  "gti_runtime_failure_tests"
  PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src"
)
target_compile_features("gti_runtime_failure_tests" PRIVATE cxx_std_23)
target_link_libraries("gti_runtime_failure_tests" PRIVATE gti_runtime)
add_test(NAME runtime_failure_boundary COMMAND gti_runtime_failure_tests)

add_executable("gti_runtime_thread_tests" "${PROJECT_SOURCE_DIR}/tests/runtime_thread_tests.cpp")
target_include_directories(
  "gti_runtime_thread_tests"
  PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src"
)
target_compile_features("gti_runtime_thread_tests" PRIVATE cxx_std_23)
target_link_libraries("gti_runtime_thread_tests" PRIVATE gti_runtime)
add_test(NAME managed_thread_runtime COMMAND gti_runtime_thread_tests)

add_executable("gti_runtime_failure_header_smoke"
               "${PROJECT_SOURCE_DIR}/tests/runtime_failure_header_smoke.cpp")
target_compile_features("gti_runtime_failure_header_smoke" PRIVATE cxx_std_20)
target_link_libraries("gti_runtime_failure_header_smoke"
                      PRIVATE gti_compiler gti_runtime)
add_test(NAME runtime_failure_header_coexistence
         COMMAND gti_runtime_failure_header_smoke)

add_executable("gti_runtime_failure_helper"
               "${PROJECT_SOURCE_DIR}/tests/runtime_failure_helper.cpp")
target_link_libraries("gti_runtime_failure_helper"
                      PRIVATE gti_runtime Threads::Threads)

add_executable("gti_compiler_library_smoke"
               "${PROJECT_SOURCE_DIR}/tests/compiler_library_smoke.cpp")
target_link_libraries("gti_compiler_library_smoke" PRIVATE gti_compiler)
add_test(NAME compiler_library_boundary COMMAND gti_compiler_library_smoke)

add_executable("gti_documentation_index_tests"
               "${PROJECT_SOURCE_DIR}/tests/documentation_index_tests.cpp")
target_link_libraries("gti_documentation_index_tests" PRIVATE gti_compiler)
add_test(NAME documentation_index COMMAND gti_documentation_index_tests)

if(TARGET gti_lsp_state)
  add_executable("gti_frontend_cache_tests"
                 "${PROJECT_SOURCE_DIR}/tests/frontend_cache_tests.cpp")
  target_link_libraries("gti_frontend_cache_tests" PRIVATE gti_lsp_state)
  add_test(NAME frontend_cache_invalidation COMMAND gti_frontend_cache_tests)
endif()

add_executable("gti_cpp_backend_library_smoke"
               "${PROJECT_SOURCE_DIR}/tests/cpp_backend_library_smoke.cpp")
target_link_libraries("gti_cpp_backend_library_smoke"
                      PRIVATE gti_cpp_backend)
add_test(NAME cpp_backend_library_boundary
         COMMAND gti_cpp_backend_library_smoke)

add_executable("gti_driver_library_smoke" "${PROJECT_SOURCE_DIR}/tests/driver_library_smoke.cpp")
target_link_libraries("gti_driver_library_smoke" PRIVATE gti_driver)
add_test(NAME driver_library_boundary COMMAND gti_driver_library_smoke)

add_executable("gti_driver_tests" "${PROJECT_SOURCE_DIR}/tests/driver_tests.cpp")
target_link_libraries("gti_driver_tests" PRIVATE gti_driver)
add_test(NAME driver_pipeline COMMAND gti_driver_tests)

add_executable("gti_project_tests" "${PROJECT_SOURCE_DIR}/tests/project_tests.cpp")
target_link_libraries("gti_project_tests" PRIVATE gti_driver)
add_test(NAME project_model COMMAND gti_project_tests)

set(GTI_COMPILER_TEST_SUITES
    frontend
    declarations
    ownership
    constexpr
    diagnostics
    e2e)
add_library(
  gti_compiler_test_pch OBJECT
  "${PROJECT_SOURCE_DIR}/tests/compiler_test_pch.cpp"
)
target_link_libraries(gti_compiler_test_pch PRIVATE gti_cpp_backend)
target_precompile_headers(
  gti_compiler_test_pch
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/compiler_test_fixture.h"
)
set(GTI_COMPILER_TEST_TARGETS)
set(GTI_COMPILER_TEST_AGGREGATE_ARGUMENTS)
foreach(suite IN LISTS GTI_COMPILER_TEST_SUITES)
  set(target "gti_compiler_${suite}_tests")
  add_executable("${target}" "${PROJECT_SOURCE_DIR}/tests/compiler_${suite}_tests.cpp")
  target_link_libraries("${target}" PRIVATE gti_cpp_backend)
  target_precompile_headers("${target}" REUSE_FROM gti_compiler_test_pch)
  list(APPEND GTI_COMPILER_TEST_TARGETS "${target}")
  list(APPEND GTI_COMPILER_TEST_AGGREGATE_ARGUMENTS
       "${suite}=$<TARGET_FILE:${target}>")
endforeach()
# Preserve the familiar build target without retaining the monolithic test
# executable. Each owned suite remains independently buildable and runnable.
add_custom_target(gti_tests DEPENDS ${GTI_COMPILER_TEST_TARGETS})
if(Python3_Interpreter_FOUND)
  add_test(
    NAME compiler_pipeline
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compiler_test_aggregate.py"
            "${PROJECT_SOURCE_DIR}/tests/compiler_test_manifest.json"
            ${GTI_COMPILER_TEST_AGGREGATE_ARGUMENTS}
  )
  add_test(
    NAME partitioned_test_protocol
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/partitioned_test_protocol_test.py"
            "${PROJECT_SOURCE_DIR}/tests/compiler_test_aggregate.py"
            "${PROJECT_SOURCE_DIR}/tests/named_test_census.py"
  )
else()
  message(FATAL_ERROR
          "Python 3 is required to enforce the compiler-test census")
endif()

add_executable("gti_block_comment_tests" "${PROJECT_SOURCE_DIR}/tests/block_comment_tests.cpp")
target_link_libraries("gti_block_comment_tests" PRIVATE gti_compiler)
add_test(NAME block_comment_pipeline COMMAND gti_block_comment_tests)

add_executable("gti_source_unit_services_tests"
               "${PROJECT_SOURCE_DIR}/tests/source_unit_services_tests.cpp")
target_link_libraries("gti_source_unit_services_tests" PRIVATE gti_compiler)
add_test(NAME source_unit_services COMMAND gti_source_unit_services_tests)

add_test(
  NAME semantic_header_boundary
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/semantic_header_boundary_test.py"
          "${PROJECT_SOURCE_DIR}"
)

add_executable(
  gti_semantic_header_smoke
  ${PROJECT_SOURCE_DIR}/tests/semantic_core_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/semantic_model_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/semantic_analyzer_header_smoke.cpp
)
target_link_libraries(gti_semantic_header_smoke PRIVATE gti_compiler)
add_test(NAME semantic_header_self_containment
         COMMAND gti_semantic_header_smoke)

add_test(
  NAME mir_header_boundary
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/mir_header_boundary_test.py"
          "${PROJECT_SOURCE_DIR}"
)

add_executable(
  gti_mir_header_smoke
  ${PROJECT_SOURCE_DIR}/tests/mir_model_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/mir_effects_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/mir_editor_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/mir_lowering_header_smoke.cpp
  ${PROJECT_SOURCE_DIR}/tests/mir_verifier_header_smoke.cpp
)
target_link_libraries(gti_mir_header_smoke PRIVATE gti_compiler)
add_test(NAME mir_header_self_containment COMMAND gti_mir_header_smoke)

add_executable("gti_pack_fold_syntax_tests"
               "${PROJECT_SOURCE_DIR}/tests/pack_fold_syntax_tests.cpp")
target_link_libraries("gti_pack_fold_syntax_tests" PRIVATE gti_compiler)
add_test(NAME pack_fold_syntax COMMAND gti_pack_fold_syntax_tests)

add_executable("gti_parser_conditional_recovery_tests"
               "${PROJECT_SOURCE_DIR}/tests/parser_conditional_recovery_tests.cpp")
target_link_libraries("gti_parser_conditional_recovery_tests"
                      PRIVATE gti_compiler)
add_test(NAME parser_conditional_recovery
         COMMAND gti_parser_conditional_recovery_tests)
set_tests_properties(parser_conditional_recovery PROPERTIES TIMEOUT 10)

add_executable("gti_optimizer_tests" "${PROJECT_SOURCE_DIR}/tests/optimizer_tests.cpp")
target_link_libraries("gti_optimizer_tests" PRIVATE gti_compiler)
add_test(NAME optimizer_foundation COMMAND gti_optimizer_tests)

add_executable("gti_optimizer_safety_tests"
               "${PROJECT_SOURCE_DIR}/tests/optimizer_safety_tests.cpp")
target_link_libraries("gti_optimizer_safety_tests" PRIVATE gti_compiler)
add_test(NAME optimizer_safety COMMAND gti_optimizer_safety_tests)

add_executable("gti_optimizer_analysis_tests"
               "${PROJECT_SOURCE_DIR}/tests/optimizer_analysis_tests.cpp")
target_link_libraries("gti_optimizer_analysis_tests" PRIVATE gti_compiler)
add_test(NAME optimizer_analyses COMMAND gti_optimizer_analysis_tests)

add_executable("gti_failure_metadata_tests"
               "${PROJECT_SOURCE_DIR}/tests/failure_metadata_tests.cpp")
target_link_libraries("gti_failure_metadata_tests" PRIVATE gti_compiler)
add_test(NAME failure_metadata COMMAND gti_failure_metadata_tests)

add_executable("gti_mir_program_initialization_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_program_initialization_tests.cpp")
target_link_libraries("gti_mir_program_initialization_tests"
                      PRIVATE gti_compiler)
add_test(NAME mir_program_initialization
         COMMAND gti_mir_program_initialization_tests)

add_executable("gti_mir_hosted_startup_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_hosted_startup_tests.cpp")
target_link_libraries("gti_mir_hosted_startup_tests" PRIVATE gti_compiler)
add_test(NAME mir_hosted_startup COMMAND gti_mir_hosted_startup_tests)

add_executable("gti_raw_pointer_tests" "${PROJECT_SOURCE_DIR}/tests/raw_pointer_tests.cpp")
target_link_libraries("gti_raw_pointer_tests" PRIVATE gti_cpp_backend)
add_test(NAME raw_pointer_pipeline COMMAND gti_raw_pointer_tests)

add_executable("gti_layout_query_tests" "${PROJECT_SOURCE_DIR}/tests/layout_query_tests.cpp")
target_link_libraries("gti_layout_query_tests" PRIVATE gti_cpp_backend)
add_test(NAME layout_query_pipeline COMMAND gti_layout_query_tests)

add_executable("gti_bindgen_tests" "${PROJECT_SOURCE_DIR}/tests/bindgen_tests.cpp")
target_link_libraries("gti_bindgen_tests" PRIVATE gti_bindgen gti_compiler)
add_test(NAME bindgen_pipeline COMMAND gti_bindgen_tests)

add_executable("gti_native_record_tests" "${PROJECT_SOURCE_DIR}/tests/native_record_tests.cpp")
target_link_libraries("gti_native_record_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_record_pipeline COMMAND gti_native_record_tests)

add_executable("gti_native_header_tests" "${PROJECT_SOURCE_DIR}/tests/native_header_tests.cpp")
target_link_libraries("gti_native_header_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_header_pipeline COMMAND gti_native_header_tests)

add_executable("gti_native_callback_tests"
               "${PROJECT_SOURCE_DIR}/tests/native_callback_tests.cpp")
target_link_libraries("gti_native_callback_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_callback_pipeline COMMAND gti_native_callback_tests)

add_executable("gti_managed_thread_tests"
               "${PROJECT_SOURCE_DIR}/tests/managed_thread_tests.cpp")
target_link_libraries("gti_managed_thread_tests" PRIVATE gti_cpp_backend)
add_test(NAME managed_thread_semantic_ir COMMAND gti_managed_thread_tests)

add_executable("gti_defined_integer_tests"
               "${PROJECT_SOURCE_DIR}/tests/defined_integer_tests.cpp")
target_link_libraries("gti_defined_integer_tests" PRIVATE gti_cpp_backend)
add_test(NAME defined_integer_arithmetic COMMAND gti_defined_integer_tests)

add_executable("gti_binary64_tests" "${PROJECT_SOURCE_DIR}/tests/binary64_tests.cpp")
target_link_libraries("gti_binary64_tests" PRIVATE gti_cpp_backend)
add_test(NAME binary64_pipeline COMMAND gti_binary64_tests)

add_executable("gti_sum_type_tests" "${PROJECT_SOURCE_DIR}/tests/sum_type_tests.cpp")
target_link_libraries("gti_sum_type_tests" PRIVATE gti_cpp_backend)
add_test(NAME sum_type_pipeline COMMAND gti_sum_type_tests)

add_executable("gti_mir_backend_tests" "${PROJECT_SOURCE_DIR}/tests/mir_backend_tests.cpp")
target_link_libraries("gti_mir_backend_tests" PRIVATE gti_cpp_backend)
add_test(NAME mir_backend_first_family COMMAND gti_mir_backend_tests)

add_executable("gti_lowered_program_tests"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_tests.cpp"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_contract_client.cpp")
target_link_libraries("gti_lowered_program_tests" PRIVATE gti_compiler)
add_test(NAME lowered_program_contract COMMAND gti_lowered_program_tests)

add_executable("gti_lowered_program_census"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_census.cpp"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_contract_client.cpp")
target_link_libraries("gti_lowered_program_census" PRIVATE gti_compiler)

add_executable("gti_backend_lowered_program_rejection_tests"
               "${PROJECT_SOURCE_DIR}/tests/backend_lowered_program_rejection_tests.cpp")
target_link_libraries("gti_backend_lowered_program_rejection_tests"
                      PRIVATE gti_cpp_backend)
add_test(NAME backend_lowered_program_rejection
         COMMAND gti_backend_lowered_program_rejection_tests)

add_executable("gti_cpp_mir_program_plan_tests"
               "${PROJECT_SOURCE_DIR}/tests/cpp_mir_program_plan_tests.cpp")
target_link_libraries("gti_cpp_mir_program_plan_tests"
                      PRIVATE gti_cpp_backend)
add_test(NAME cpp_mir_program_plan COMMAND gti_cpp_mir_program_plan_tests)

add_executable("gti_cpp_mir_control_flow_tests"
               "${PROJECT_SOURCE_DIR}/tests/cpp_mir_control_flow_tests.cpp")
target_link_libraries("gti_cpp_mir_control_flow_tests"
                      PRIVATE gti_cpp_backend)
add_test(NAME cpp_mir_control_flow COMMAND gti_cpp_mir_control_flow_tests)

set(GTI_CPP_MIR_BODY_EMITTER_TEST_SUITES
    preflight
    text
    lifecycle
    corpus)
set(GTI_CPP_MIR_BODY_EMITTER_TEST_TARGETS)
set(GTI_CPP_MIR_BODY_EMITTER_CENSUS_ARGUMENTS)
foreach(suite IN LISTS GTI_CPP_MIR_BODY_EMITTER_TEST_SUITES)
  set(target "gti_cpp_mir_body_emitter_${suite}_tests")
  add_executable("${target}"
                 "${PROJECT_SOURCE_DIR}/tests/cpp_mir_body_emitter_${suite}_tests.cpp")
  target_link_libraries("${target}" PRIVATE gti_cpp_backend)
  list(APPEND GTI_CPP_MIR_BODY_EMITTER_TEST_TARGETS "${target}")
  list(APPEND GTI_CPP_MIR_BODY_EMITTER_CENSUS_ARGUMENTS
       "${suite}=$<TARGET_FILE:${target}>")
endforeach()
add_custom_target(
  gti_cpp_mir_body_emitter_tests
  DEPENDS ${GTI_CPP_MIR_BODY_EMITTER_TEST_TARGETS}
)
add_test(NAME cpp_mir_body_emitter_preflight
         COMMAND gti_cpp_mir_body_emitter_preflight_tests)
add_test(NAME cpp_mir_body_emitter_text
         COMMAND gti_cpp_mir_body_emitter_text_tests)
add_test(NAME cpp_mir_body_emitter_lifecycle
         COMMAND gti_cpp_mir_body_emitter_lifecycle_tests)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME cpp_mir_body_emitter_census
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/named_test_census.py"
            "${PROJECT_SOURCE_DIR}/tests/cpp_mir_body_emitter_test_manifest.json"
            ${GTI_CPP_MIR_BODY_EMITTER_CENSUS_ARGUMENTS}
  )
else()
  message(FATAL_ERROR
          "Python 3 is required to enforce the emitter-test census")
endif()

add_executable(
  "gti_cpp_mir_representation_snapshot_tests"
  "${PROJECT_SOURCE_DIR}/tests/cpp_mir_representation_snapshot_tests.cpp"
)
target_link_libraries("gti_cpp_mir_representation_snapshot_tests"
                      PRIVATE gti_cpp_backend)
add_test(NAME cpp_mir_representation_snapshot
         COMMAND gti_cpp_mir_representation_snapshot_tests)

add_executable("gti_failure_descriptor_backend_tests"
               "${PROJECT_SOURCE_DIR}/tests/failure_descriptor_backend_tests.cpp")
target_link_libraries("gti_failure_descriptor_backend_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME failure_descriptor_backend
  COMMAND gti_failure_descriptor_backend_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/failure_descriptor_backend.gti"
)

add_executable("gti_mir_backend_scalar_cfg_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_cfg_tests.cpp")
target_link_libraries("gti_mir_backend_scalar_cfg_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME mir_backend_scalar_cfg
  COMMAND gti_mir_backend_scalar_cfg_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_cfg.gti"
)

add_executable("gti_mir_backend_scalar_direct_call_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_direct_call_tests.cpp")
target_link_libraries("gti_mir_backend_scalar_direct_call_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME mir_backend_scalar_direct_call
  COMMAND gti_mir_backend_scalar_direct_call_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_direct_call.gti"
)

add_executable("gti_mir_backend_class_default_cleanup_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_backend_class_default_cleanup_tests.cpp")
target_link_libraries("gti_mir_backend_class_default_cleanup_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME mir_backend_class_default_cleanup
  COMMAND gti_mir_backend_class_default_cleanup_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_class_default_cleanup.gti"
)

add_executable("gti_mir_backend_owned_lifecycle_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_backend_owned_lifecycle_tests.cpp")
target_link_libraries("gti_mir_backend_owned_lifecycle_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME mir_backend_owned_lifecycle
  COMMAND gti_mir_backend_owned_lifecycle_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_owned_lifecycle.gti"
)

add_executable("gti_mir_backend_scalar_failure_callgraph_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_failure_callgraph_tests.cpp")
target_link_libraries("gti_mir_backend_scalar_failure_callgraph_tests"
                      PRIVATE gti_cpp_backend)
add_test(
  NAME mir_backend_scalar_failure_callgraph
  COMMAND gti_mir_backend_scalar_failure_callgraph_tests
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_reverse_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_recursive_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_cleanup_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_checked_ctor_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_checked_dtor_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_virtual_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_lambda_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_dynamic_global_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure_free_helper_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_c_linkage_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_class_field_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_lifecycle_user_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_array_user_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_generic_owner_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_no_detector_entry_near_miss.gti"
          "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_nonowning_user.gti"
)

add_executable("gti_layout_query_native_smoke"
               "${PROJECT_SOURCE_DIR}/tests/layout_query_native_smoke.cpp")
target_link_libraries("gti_layout_query_native_smoke" PRIVATE gti_compiler)
add_test(NAME layout_query_native_boundary
         COMMAND gti_layout_query_native_smoke)

add_executable("gti_prefix_storage_evidence"
               "${PROJECT_SOURCE_DIR}/tests/prefix_storage_native_evidence.cpp")
target_link_libraries("gti_prefix_storage_evidence" PRIVATE gti_cpp_backend)
add_test(NAME prefix_storage_native_evidence
         COMMAND gti_prefix_storage_evidence "${CMAKE_CXX_COMPILER}")
set_tests_properties(
  prefix_storage_native_evidence
  PROPERTIES TIMEOUT 600
             ENVIRONMENT
               "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
)

add_executable("gti_optimizer_safety_native_evidence"
               "${PROJECT_SOURCE_DIR}/tests/optimizer_safety_native_evidence.cpp")
target_link_libraries("gti_optimizer_safety_native_evidence"
                      PRIVATE gti_cpp_backend)
add_test(NAME optimizer_safety_native_evidence
         COMMAND gti_optimizer_safety_native_evidence
                 "${CMAKE_CXX_COMPILER}")
set_tests_properties(
  optimizer_safety_native_evidence
  PROPERTIES TIMEOUT 600
             ENVIRONMENT
               "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
)

if(Python3_Interpreter_FOUND)
  add_test(
    NAME cmake_test_ownership
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cmake_test_ownership_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME cpp_mir_body_emitter_corpus
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cpp_mir_body_emitter_corpus_test.py"
            "$<TARGET_FILE:gti_cpp_mir_body_emitter_corpus_tests>"
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/lowered_program_census_baseline.json"
            --jobs=4
  )
  set_tests_properties(cpp_mir_body_emitter_corpus PROPERTIES PROCESSORS 4)
  add_test(
    NAME cpp_mir_body_emitter_corpus_protocol
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cpp_mir_body_emitter_corpus_coordinator_test.py"
            "${PROJECT_SOURCE_DIR}/tests/cpp_mir_body_emitter_corpus_test.py"
  )
  add_test(
    NAME backend_lowered_program_boundary
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/backend_boundary_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME lowered_program_source_boundary
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lowered_program_boundary_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME lowered_program_corpus_census
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lowered_program_census_test.py"
            "$<TARGET_FILE:gti_lowered_program_census>"
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/lowered_program_census_baseline.json"
  )
  set_tests_properties(lowered_program_corpus_census PROPERTIES TIMEOUT 900)
  add_test(
    NAME runtime_failure_subprocess
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/runtime_failure_test.py"
            "$<TARGET_FILE:gti_runtime_failure_helper>"
  )
  add_test(
    NAME release_version_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/release_version_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/check_release_version.py"
  )
  add_test(
    NAME nightly_asset_sync
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/nightly_asset_sync_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/sync_nightly_assets.py"
  )
  add_test(
    NAME mir_census_regression
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_census_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/mir_census_baseline.json"
  )
  add_test(
    NAME mir_cutover_corpus_oracle
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_cutover_corpus_oracle_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/mir_census_baseline.json"
            "$<TARGET_FILE:gti_runtime>"
            "--jobs=4"
  )
  set_tests_properties(mir_cutover_corpus_oracle
                       PROPERTIES TIMEOUT 900 PROCESSORS 4)
  add_test(
    NAME tree_sitter_queries
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tree_sitter_query_test.py"
            "${PROJECT_SOURCE_DIR}/tree-sitter-gti/src/node-types.json"
            "${PROJECT_SOURCE_DIR}/queries/gti"
  )
  add_test(
    NAME output_determinism
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/determinism_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/07-generics.gti"
  )
  add_test(
    NAME compiler_debug_tooling
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compiler_debug_tooling_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME compiler_failure_reducer
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compiler_failure_reducer_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(compiler_failure_reducer PROPERTIES TIMEOUT 600)
  add_test(
    NAME typed_program_model
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/typed_program_model_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME typed_differential_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/typed_differential_smoke_test.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(
    typed_differential_smoke
    PROPERTIES TIMEOUT 600
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME stdlib_surface_manifest
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stdlib_surface_manifest_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME generic_algorithm_cluster
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_cluster_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(
    generic_algorithm_cluster
    PROPERTIES TIMEOUT 600
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME allocator_storage_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/allocator_storage_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/allocator_storage.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/allocator_storage_alignment.cpp"
  )
  set_tests_properties(
    allocator_storage_runtime
    PROPERTIES TIMEOUT 600
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME arena_pool_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/arena_pool_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/arena_pool.gti"
  )
  set_tests_properties(
    arena_pool_runtime
      PROPERTIES TIMEOUT 600
                 ENVIRONMENT
                   "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
    )
    add_test(
      NAME mut_cursor_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mut_cursor_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(
      mut_cursor_runtime
      PROPERTIES TIMEOUT 600
                 ENVIRONMENT
                   "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
    )
    add_test(
      NAME stdlib_surface
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/stdlib_surface_test.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
            "$<TARGET_FILE:gti_runtime>"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(stdlib_surface PROPERTIES TIMEOUT 1200 PROCESSORS 2)
  add_test(
    NAME stdlib_include_sweep
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/debug/stdlib_include_sweep.py"
            --gti "$<TARGET_FILE:gti>"
  )
  set_tests_properties(stdlib_include_sweep PROPERTIES TIMEOUT 600)
  # Sharded by (optimization, standard) configuration; the shard union is
  # the full 2x2 matrix, and the script with no --shard still runs it all.
  foreach(shard RANGE 1 4)
    add_test(
      NAME defined_integer_runtime_${shard}of4
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/defined_integer_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/examples/46-defined-integer-arithmetic.gti"
              --shard ${shard}/4
    )
  endforeach()
  add_test(
    NAME binary64_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/binary64_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/47-binary64.gti"
  )
  add_test(
    NAME constexpr_aggregate_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_aggregate_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_aggregate_positive.gti"
  )
  add_test(
    NAME sum_types_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/sum_types_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/56-sum-types.gti"
  )
  add_test(
    NAME issues_122_124_field_initializers_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issues_122_124_field_initializers_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/issues_122_124_field_initializers.gti"
  )
  set_tests_properties(issues_122_124_field_initializers_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME expected_wrong_state_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_wrong_state_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME expected_propagation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_propagation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME returned_class_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/returned_class_field_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME reference_parameter_range_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/reference_parameter_range_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME mutable_range_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mutable_range_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME span_owner_dependent_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/span_owner_dependent_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/span_owner_dependent.gti"
  )
  add_test(
    NAME evaluation_order_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/evaluation_order_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME optimizer_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/optimizer_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/optimizer_scalar_promotion.gti"
            "${CMAKE_CXX_COMPILER}"
            "${PROJECT_SOURCE_DIR}"
            "$<TARGET_FILE:gti_runtime>"
  )
  add_test(
    NAME default_parameters_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/default_parameter_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/default_parameters.gti"
  )
  add_test(
    NAME mir_backend_first_family_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_first_family.gti"
  )
  add_test(
    NAME global_class_storage_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/global_class_storage_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/global_class_storage.gti"
  )
  add_test(
    NAME failure_descriptor_backend_compile
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/failure_descriptor_backend_compile_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/failure_descriptor_backend.gti"
            "${CMAKE_CXX_COMPILER}"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME mir_backend_scalar_cfg_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_cfg_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_cfg.gti"
  )
  add_test(
    NAME mir_backend_scalar_direct_call_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_direct_call_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_direct_call.gti"
  )
  add_test(
    NAME mir_backend_class_default_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_class_default_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_class_default_cleanup.gti"
  )
  set_tests_properties(mir_backend_class_default_cleanup_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME inline_lambda_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/inline_lambda_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/inline_lambda_argument.gti"
  )
  set_tests_properties(inline_lambda_argument_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME defaulted_lifecycle_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/defaulted_lifecycle_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/defaulted_lifecycle.gti"
  )
  set_tests_properties(
    defaulted_lifecycle_runtime
    PROPERTIES TIMEOUT 180
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME mir_backend_owned_lifecycle_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_owned_lifecycle_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_owned_lifecycle.gti"
  )
  set_tests_properties(mir_backend_owned_lifecycle_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME mir_class_value_lifecycle_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_class_value_lifecycle_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(mir_class_value_lifecycle_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME fixed_array_class_lifecycle_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fixed_array_class_lifecycle_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/fixed_array_class_lifecycle.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/fixed_array_class_lifecycle_negative.gti"
    )
    set_tests_properties(fixed_array_class_lifecycle_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME fixed_array_oob_write_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/fixed_array_oob_write_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(fixed_array_oob_write_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME fixed_array_multidimensional_index_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/fixed_array_multidimensional_index_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(fixed_array_multidimensional_index_runtime
                         PROPERTIES TIMEOUT 360)
    add_test(
      NAME issue_143_polymorphic_class_array_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/issue_143_polymorphic_class_array_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/issue_143_polymorphic_class_array.gti"
  )
  set_tests_properties(issue_143_polymorphic_class_array_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME issue_123_cleanup_temporary_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_123_cleanup_temporary_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issue_123_cleanup_temporary_argument_runtime
                       PROPERTIES TIMEOUT 240)
  add_test(
    NAME issue_126_array_element_assignment_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_126_array_element_assignment_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issue_126_array_element_assignment_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME issue_127_constructor_temporary_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_127_constructor_temporary_field_runtime_test.py"
            "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(issue_127_constructor_temporary_field_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME constructor_constructed_field_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/constructor_constructed_field_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(constructor_constructed_field_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME issues_117_120_regressions
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/issues_117_120_regression_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issues_117_120_regressions PROPERTIES TIMEOUT 180)
  add_test(
    NAME issue_144_class_array_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_144_class_array_field_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issue_144_class_array_field_runtime
                       PROPERTIES TIMEOUT 240)
  add_test(
    NAME issue_146_class_value_positions_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_146_class_value_positions_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issue_146_class_value_positions_runtime
                       PROPERTIES TIMEOUT 240)
  add_test(
    NAME issue_147_stored_reference_field_read_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issue_147_stored_reference_field_read_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/issue_147_stored_reference_field_read.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/issue_147_stored_reference_field_write.gti"
  )
  set_tests_properties(issue_147_stored_reference_field_read_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME reference_receiver_compound_assign_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/reference_receiver_compound_assign_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reference_receiver_compound_assign.gti"
  )
  set_tests_properties(reference_receiver_compound_assign_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME subscript_store_read_order_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/subscript_store_read_order_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(subscript_store_read_order_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(
    NAME owner_tied_reference_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owner_tied_reference_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/owner_tied_reference_argument.gti"
  )
  set_tests_properties(owner_tied_reference_argument_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME generic_symbolic_auto_local_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
            "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/generic_symbolic_auto_local.gti"
    )
  set_tests_properties(generic_symbolic_auto_local_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME explicit_function_value_generics_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/explicit_function_value_generics_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/explicit_function_value_generics.gti"
  )
  set_tests_properties(explicit_function_value_generics_runtime
                       PROPERTIES TIMEOUT 240)
  add_test(
    NAME generic_callable_concepts_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_callable_concepts_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/generic_callable_concepts.gti"
  )
  set_tests_properties(generic_callable_concepts_runtime
                       PROPERTIES TIMEOUT 300)
    add_test(
      NAME owner_tied_by_value_argument_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/owner_tied_by_value_argument_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/owner_tied_by_value_argument.gti"
    )
    set_tests_properties(owner_tied_by_value_argument_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME moved_loop_local_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/moved_loop_local_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/moved_loop_local.gti"
    )
    set_tests_properties(moved_loop_local_runtime PROPERTIES TIMEOUT 180)
    add_test(
      NAME mixed_capture_callable_template_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mixed_capture_callable_template_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(mixed_capture_callable_template_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME class_move_assignment_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/class_move_assignment_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(class_move_assignment_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME class_prvalue_assignment_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/class_prvalue_assignment_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures"
    )
    set_tests_properties(class_prvalue_assignment_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME mutable_temporary_receiver_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mutable_temporary_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(mutable_temporary_receiver_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME nested_reference_return_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/nested_reference_return_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(nested_reference_return_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME class_payload_enum_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/class_payload_enum_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(class_payload_enum_runtime
                         PROPERTIES TIMEOUT 180)
    add_test(
      NAME argument_reference_return_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/argument_reference_return_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(argument_reference_return_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME temporary_argument_borrow_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/temporary_argument_borrow_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(temporary_argument_borrow_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME field_subscript_store_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/field_subscript_store_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(field_subscript_store_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME disjoint_field_projection_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/disjoint_field_projection_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(disjoint_field_projection_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME retained_borrowed_call_result_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/retained_borrowed_call_result_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(retained_borrowed_call_result_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME nested_borrow_chain_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/nested_borrow_chain_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(nested_borrow_chain_runtime
                         PROPERTIES TIMEOUT 300)
    add_test(
      NAME mut_reference_retained_chain_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mut_reference_retained_chain_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(mut_reference_retained_chain_runtime
                         PROPERTIES TIMEOUT 300)
    # Sharded by (optimization, standard) configuration; three shards of two
    # configurations each, union = the full six-configuration matrix.
    foreach(shard RANGE 1 3)
    add_test(
    NAME mir_backend_scalar_failure_callgraph_runtime_${shard}of3
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_backend_scalar_failure_callgraph_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_success.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_all_operations_success.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_failure.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_division_zero.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_division_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negative_shift.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_wide_shift.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_subtraction_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_multiplication_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_remainder_zero.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negative_right_shift.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_wide_right_shift.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_negation_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_conversion_wide.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_conversion_negative.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_addition_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_subtraction_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_unsigned_multiplication_overflow.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/mir_backend_scalar_failure_callgraph_array_user_near_miss.gti"
            --shard ${shard}/3
  )
  set_tests_properties(mir_backend_scalar_failure_callgraph_runtime_${shard}of3
                       PROPERTIES TIMEOUT 300 PROCESSORS 2)
  endforeach()
  add_test(
    NAME native_record_c_oracle
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/native_record_c_oracle_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(native_record_c_oracle PROPERTIES TIMEOUT 180)
  add_test(
    NAME native_header_c_cpp_oracle
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/native_header_c_cpp_oracle_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(native_header_c_cpp_oracle PROPERTIES TIMEOUT 180)
  add_test(
    NAME glfw_3_4_c_oracle
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/glfw_3_4_c_oracle_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/glfw_3_4_surface.gti"
  )
  set_tests_properties(glfw_3_4_c_oracle PROPERTIES TIMEOUT 300)
  add_test(
    NAME llvm_link_surface
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/check_llvm_link_surface.py"
            "${CMAKE_BINARY_DIR}"
  )
  add_test(
    NAME llvm_link_surface_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_link_surface_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/check_llvm_link_surface.py"
  )
  add_test(
    NAME cli_workflow
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_smoke_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME project_cli_workflow
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/project_cli_smoke_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME benchmark_harness
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/benchmark_compare_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/benchmark_compare.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
  )
  set_tests_properties(
    benchmark_harness
    PROPERTIES TIMEOUT 300
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME runtime_performance_workloads
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/runtime_performance_workloads_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/benchmark_compare.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
  )
  set_tests_properties(
    runtime_performance_workloads
    PROPERTIES TIMEOUT 300
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
  add_test(
    NAME random_engine_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/random_engine_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/random_engine.gti"
  )
  add_test(
    NAME filesystem_path_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/filesystem_path_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/filesystem_path.gti"
  )
  add_test(
    NAME string_owned_output_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/string_owned_output_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/string_owned_output.gti"
  )
  add_test(
    NAME managed_thread_end_to_end
    COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/managed_thread_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(managed_thread_end_to_end PROPERTIES TIMEOUT 300)
    add_test(
      NAME domain_operator_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/domain_operator_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(domain_operator_runtime PROPERTIES TIMEOUT 300)

    if(GTI_RELEASE_BUILD)
      # Release binaries deliberately omit source-tree fallback paths so the
      # installed-toolchain checks exercise relocatable discovery. Build-tree
    # tests still run before installation and therefore need an explicit
    # test-only resource layout.
    set_tests_properties(
      driver_pipeline
      output_determinism binary64_runtime sum_types_runtime
      constexpr_aggregate_runtime
      issues_122_124_field_initializers_runtime
      stdlib_include_sweep
      defined_integer_runtime_1of4 defined_integer_runtime_2of4
      defined_integer_runtime_3of4 defined_integer_runtime_4of4
      expected_wrong_state_runtime expected_propagation_runtime
      returned_class_field_runtime
      reference_parameter_range_runtime mutable_range_runtime
      span_owner_dependent_runtime
      evaluation_order_runtime optimizer_runtime
      native_record_c_oracle native_header_c_cpp_oracle glfw_3_4_c_oracle
      cli_workflow project_cli_workflow
      mir_backend_first_family_runtime global_class_storage_runtime
      failure_descriptor_backend_compile
      mir_backend_scalar_cfg_runtime mir_backend_scalar_direct_call_runtime
        mir_backend_class_default_cleanup_runtime
        inline_lambda_argument_runtime
        mir_backend_owned_lifecycle_runtime
        mir_class_value_lifecycle_runtime fixed_array_class_lifecycle_runtime
        fixed_array_oob_write_runtime
        fixed_array_multidimensional_index_runtime
        issue_123_cleanup_temporary_argument_runtime
        issue_126_array_element_assignment_runtime
        issue_127_constructor_temporary_field_runtime
        constructor_constructed_field_runtime
        issues_117_120_regressions
        issue_143_polymorphic_class_array_runtime
        issue_144_class_array_field_runtime
        issue_146_class_value_positions_runtime
      issue_147_stored_reference_field_read_runtime
        reference_receiver_compound_assign_runtime
        subscript_store_read_order_runtime
        owner_tied_reference_argument_runtime
        generic_symbolic_auto_local_runtime
        explicit_function_value_generics_runtime
        generic_callable_concepts_runtime
        owner_tied_by_value_argument_runtime
        moved_loop_local_runtime
        mixed_capture_callable_template_runtime
        class_move_assignment_runtime
        class_prvalue_assignment_runtime
        mutable_temporary_receiver_runtime
        nested_reference_return_runtime
        class_payload_enum_runtime
        argument_reference_return_runtime
        temporary_argument_borrow_runtime
        field_subscript_store_runtime
        disjoint_field_projection_runtime
        retained_borrowed_call_result_runtime
        nested_borrow_chain_runtime
        mut_reference_retained_chain_runtime
        mir_backend_scalar_failure_callgraph_runtime_1of3
        mir_backend_scalar_failure_callgraph_runtime_2of3
        mir_backend_scalar_failure_callgraph_runtime_3of3
        random_engine_runtime default_parameters_runtime
      compiler_debug_tooling compiler_failure_reducer filesystem_path_runtime
      string_owned_output_runtime managed_thread_end_to_end
      domain_operator_runtime
      PROPERTIES ENVIRONMENT
        "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
    )
  endif()

  if(TARGET gti_lsp)
    add_test(
      NAME lsp_protocol
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME block_comment_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/block_comment_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME lsp_scalability_benchmark_smoke
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/lsp_benchmark.py"
        --lsp "$<TARGET_FILE:gti_lsp>"
        --stdlib "${PROJECT_SOURCE_DIR}/stdlib"
        --output "${PROJECT_BINARY_DIR}/lsp-benchmark-smoke"
        --dependencies 8 --smoke
    )
    if(GTI_RELEASE_BUILD)
      set_tests_properties(
        lsp_protocol block_comment_lsp lsp_scalability_benchmark_smoke
        PROPERTIES ENVIRONMENT
          "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib"
      )
    endif()
  endif()
endif()
