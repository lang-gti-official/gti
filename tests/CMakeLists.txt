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
add_executable(gti_runtime_filesystem_tests runtime_filesystem_tests.cpp)
target_compile_features(gti_runtime_filesystem_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_filesystem_tests PRIVATE gti_runtime)
add_test(NAME filesystem_runtime_boundary COMMAND gti_runtime_filesystem_tests)
add_executable(gti_runtime_charconv_tests runtime_charconv_tests.cpp)
target_compile_features(gti_runtime_charconv_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_charconv_tests PRIVATE gti_runtime)
add_test(NAME charconv_runtime_boundary COMMAND gti_runtime_charconv_tests)
add_executable(gti_mir_full_expression_tests mir_full_expression_tests.cpp)
target_link_libraries(gti_mir_full_expression_tests PRIVATE gti_compiler)
add_test(NAME mir_full_expression COMMAND gti_mir_full_expression_tests)

add_executable("gti_runtime_thread_tests" "${PROJECT_SOURCE_DIR}/tests/runtime_thread_tests.cpp")
target_include_directories(
  "gti_runtime_thread_tests"
  PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src"
)
target_compile_features("gti_runtime_thread_tests" PRIVATE cxx_std_23)
target_link_libraries("gti_runtime_thread_tests" PRIVATE gti_runtime)
add_test(NAME managed_thread_runtime COMMAND gti_runtime_thread_tests)

add_executable("gti_runtime_cthread_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_tests.cpp")
target_include_directories("gti_runtime_cthread_tests"
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features("gti_runtime_cthread_tests" PRIVATE cxx_std_20)
target_link_libraries("gti_runtime_cthread_tests" PRIVATE gti_runtime)
add_test(NAME cthread_runtime_boundary COMMAND gti_runtime_cthread_tests)
set_tests_properties(cthread_runtime_boundary PROPERTIES TIMEOUT 60)

add_executable(gti_runtime_cthread_sleep_tests
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_sleep_tests.cpp")
target_include_directories(gti_runtime_cthread_sleep_tests
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_cthread_sleep_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_cthread_sleep_tests PRIVATE gti_runtime)
add_test(NAME cthread_sleep_boundary COMMAND gti_runtime_cthread_sleep_tests)
set_tests_properties(cthread_sleep_boundary PROPERTIES TIMEOUT 60)

add_executable(gti_runtime_cthread_mutex_tests
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_mutex_tests.cpp")
target_include_directories(gti_runtime_cthread_mutex_tests
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_cthread_mutex_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_cthread_mutex_tests PRIVATE gti_runtime)
add_test(NAME cthread_mutex_boundary COMMAND gti_runtime_cthread_mutex_tests)
set_tests_properties(cthread_mutex_boundary PROPERTIES TIMEOUT 60)

add_executable(gti_runtime_cthread_condition_tests
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_condition_tests.cpp")
target_include_directories(gti_runtime_cthread_condition_tests
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_cthread_condition_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_cthread_condition_tests PRIVATE gti_runtime)
add_test(NAME cthread_condition_boundary COMMAND gti_runtime_cthread_condition_tests)
set_tests_properties(cthread_condition_boundary PROPERTIES TIMEOUT 60)

add_executable(gti_runtime_cthread_once_tests
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_once_tests.cpp")
target_include_directories(gti_runtime_cthread_once_tests
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_cthread_once_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_cthread_once_tests PRIVATE gti_runtime)
add_test(NAME cthread_once_boundary COMMAND gti_runtime_cthread_once_tests)
set_tests_properties(cthread_once_boundary PROPERTIES TIMEOUT 60)

add_executable("gti_runtime_chrono_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_chrono_tests.cpp")
target_include_directories(gti_runtime_chrono_tests
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_chrono_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_chrono_tests PRIVATE gti_runtime)
add_test(NAME chrono_runtime_boundary COMMAND gti_runtime_chrono_tests)

add_executable("gti_runtime_io_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_io_tests.cpp")
target_compile_features("gti_runtime_io_tests" PRIVATE cxx_std_23)
target_link_libraries("gti_runtime_io_tests" PRIVATE gti_runtime)
add_test(NAME runtime_io_boundary COMMAND gti_runtime_io_tests)

add_executable("gti_runtime_process_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_process_tests.cpp")
target_compile_features("gti_runtime_process_tests" PRIVATE cxx_std_20)
target_link_libraries("gti_runtime_process_tests" PRIVATE gti_runtime)
add_test(NAME runtime_process_boundary COMMAND gti_runtime_process_tests)

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

add_test(
  NAME diagnostic_registry_generator
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/diagnostic_registry_generator_test.py"
    "${PROJECT_SOURCE_DIR}/scripts/generate_diagnostics.py"
    "${PROJECT_SOURCE_DIR}/diagnostics/semantic.json"
)

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

add_executable(gti_executable_entry_tests
               "${PROJECT_SOURCE_DIR}/tests/executable_entry_tests.cpp")
target_link_libraries(gti_executable_entry_tests PRIVATE gti_driver)
add_test(NAME executable_entry_semantics COMMAND gti_executable_entry_tests)

add_executable("gti_project_tests" "${PROJECT_SOURCE_DIR}/tests/project_tests.cpp")
target_link_libraries("gti_project_tests" PRIVATE gti_driver)
add_test(NAME project_model COMMAND gti_project_tests)

add_executable(gti_designated_construction_tests designated_construction_tests.cpp)
target_link_libraries(gti_designated_construction_tests PRIVATE gti_cpp_backend)
add_test(NAME designated_construction_pipeline COMMAND gti_designated_construction_tests)
add_test(NAME designated_construction_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/designated_construction_runtime_test.py" $<TARGET_FILE:gti>)
add_test(NAME designated_construction_lsp
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/designated_construction_lsp_test.py" $<TARGET_FILE:gti_lsp>)

add_executable(gti_collection_initialization_tests collection_initialization_tests.cpp)
target_link_libraries(gti_collection_initialization_tests PRIVATE gti_cpp_backend)
add_test(NAME collection_initialization_pipeline COMMAND gti_collection_initialization_tests)
add_test(NAME collection_initialization_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/collection_initialization_runtime_test.py" $<TARGET_FILE:gti>)
add_test(NAME collection_initialization_lsp
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/collection_initialization_lsp_test.py" $<TARGET_FILE:gti_lsp>)
add_test(NAME member_requirement_lsp
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/member_requirement_lsp_test.py" $<TARGET_FILE:gti_lsp>)
add_test(NAME nested_borrow_fields_lsp
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_borrow_fields_lsp_test.py" $<TARGET_FILE:gti_lsp>)
add_test(NAME borrowing_composition_lsp
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/borrowing_composition_lsp_test.py" $<TARGET_FILE:gti_lsp>)
add_executable(gti_borrowing_composition_tests borrowing_composition_tests.cpp)
target_link_libraries(gti_borrowing_composition_tests PRIVATE gti_cpp_backend)
add_test(NAME borrowing_composition_pipeline COMMAND gti_borrowing_composition_tests)
add_executable(gti_shared_ownership_tests shared_ownership_tests.cpp)
target_link_libraries(gti_shared_ownership_tests PRIVATE gti_compiler)
add_test(NAME shared_ownership_pipeline COMMAND gti_shared_ownership_tests)

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

add_executable("gti_source_text_tests" "${PROJECT_SOURCE_DIR}/tests/source_text_tests.cpp")
target_link_libraries("gti_source_text_tests" PRIVATE gti_compiler)
add_test(NAME source_text_pipeline COMMAND gti_source_text_tests)

add_executable("gti_source_unit_services_tests"
               "${PROJECT_SOURCE_DIR}/tests/source_unit_services_tests.cpp")
target_link_libraries("gti_source_unit_services_tests" PRIVATE gti_compiler)
add_test(NAME source_unit_services COMMAND gti_source_unit_services_tests)

add_executable("gti_optional_compiler_tests"
               "${PROJECT_SOURCE_DIR}/tests/optional_compiler_tests.cpp")
target_link_libraries("gti_optional_compiler_tests" PRIVATE gti_cpp_backend)
target_precompile_headers("gti_optional_compiler_tests"
                          REUSE_FROM gti_compiler_test_pch)
add_test(NAME optional_compiler_contract COMMAND gti_optional_compiler_tests)

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

add_executable(gti_optimizer_conformance_tests
               "${PROJECT_SOURCE_DIR}/tests/optimizer_conformance_tests.cpp")
target_link_libraries(gti_optimizer_conformance_tests PRIVATE gti_compiler)
add_test(NAME optimizer_conformance COMMAND gti_optimizer_conformance_tests)
add_executable(gti_optimizer_devirtualization_tests
               "${PROJECT_SOURCE_DIR}/tests/optimizer_devirtualization_tests.cpp")
target_link_libraries(gti_optimizer_devirtualization_tests PRIVATE gti_compiler)
add_test(NAME optimizer_devirtualization COMMAND gti_optimizer_devirtualization_tests)
add_test(NAME optimizer_devirtualization_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/optimizer_devirtualization_runtime_test.py"
                 "$<TARGET_FILE:gti>"
                 "${PROJECT_SOURCE_DIR}/tests/fixtures/optimizer_exact_receiver.gti"
                 "${CMAKE_CXX_COMPILER}")
set_tests_properties(optimizer_devirtualization_runtime PROPERTIES TIMEOUT 180)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
  add_test(NAME optimizer_coverage_growth
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/optimizer_coverage_growth_test.py"
                   --cxx "${CMAKE_CXX_COMPILER}"
                   --source "${PROJECT_SOURCE_DIR}")
endif()

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

add_executable(gti_cthread_tests "${PROJECT_SOURCE_DIR}/tests/cthread_tests.cpp")
target_link_libraries(gti_cthread_tests PRIVATE gti_cpp_backend)
add_test(NAME cthread_semantic_ir COMMAND gti_cthread_tests)

add_executable(gti_unsafe_function_tests
               "${PROJECT_SOURCE_DIR}/tests/unsafe_function_tests.cpp")
target_link_libraries(gti_unsafe_function_tests PRIVATE gti_cpp_backend)
add_test(NAME unsafe_function_contract COMMAND gti_unsafe_function_tests)

add_executable("gti_managed_thread_tests"
               "${PROJECT_SOURCE_DIR}/tests/managed_thread_tests.cpp")
target_link_libraries("gti_managed_thread_tests" PRIVATE gti_cpp_backend)
add_test(NAME managed_thread_semantic_ir COMMAND gti_managed_thread_tests)

add_executable("gti_function_item_tests"
               "${PROJECT_SOURCE_DIR}/tests/function_item_tests.cpp")
target_link_libraries("gti_function_item_tests" PRIVATE gti_cpp_backend)
add_test(NAME function_item_semantic_ir COMMAND gti_function_item_tests)

add_executable(gti_function_signature_tests
               "${PROJECT_SOURCE_DIR}/tests/function_signature_tests.cpp")
target_link_libraries(gti_function_signature_tests PRIVATE gti_cpp_backend)
add_test(NAME function_signature_types COMMAND gti_function_signature_tests)
add_executable(gti_stored_callable_tests stored_callable_tests.cpp)
target_link_libraries(gti_stored_callable_tests PRIVATE gti_cpp_backend)
add_test(NAME stored_callable_types COMMAND gti_stored_callable_tests)
add_executable(gti_owning_function_tests owning_function_tests.cpp)
target_link_libraries(gti_owning_function_tests PRIVATE gti_cpp_backend)
add_test(NAME owning_function_types COMMAND gti_owning_function_tests)

add_executable(gti_consuming_receiver_tests consuming_receiver_tests.cpp)
target_link_libraries(gti_consuming_receiver_tests PRIVATE gti_cpp_backend)
add_test(NAME consuming_receiver_types COMMAND gti_consuming_receiver_tests)

add_executable("gti_atomic_tests"
               "${PROJECT_SOURCE_DIR}/tests/atomic_tests.cpp")
target_link_libraries("gti_atomic_tests" PRIVATE gti_cpp_backend)
add_test(NAME atomic_semantic_ir COMMAND gti_atomic_tests)

add_executable("gti_mutex_tests"
               "${PROJECT_SOURCE_DIR}/tests/mutex_tests.cpp")
target_link_libraries("gti_mutex_tests" PRIVATE gti_cpp_backend)
add_test(NAME mutex_semantic_ir COMMAND gti_mutex_tests)

add_executable(gti_source_location_tests source_location_tests.cpp)
target_link_libraries(gti_source_location_tests PRIVATE gti_cpp_backend)
add_test(NAME source_location_semantic_ir COMMAND gti_source_location_tests)

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
add_test(NAME native_opaque_pointer_results
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_opaque_pointer_results_test.py" $<TARGET_FILE:gti>)
add_test(NAME declaration_field_failure
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/declaration_field_failure_test.py" $<TARGET_FILE:gti>)
add_test(NAME cpp_mir_body_emitter_lifecycle
         COMMAND gti_cpp_mir_body_emitter_lifecycle_tests)
add_executable(gti_expected_payload_pair_emitter_tests expected_payload_pair_emitter_tests.cpp)
target_link_libraries(gti_expected_payload_pair_emitter_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_payload_pair_emitter COMMAND gti_expected_payload_pair_emitter_tests)
add_executable(gti_expected_passive_publication_tests expected_passive_publication_tests.cpp)
target_link_libraries(gti_expected_passive_publication_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_passive_publication COMMAND gti_expected_passive_publication_tests)
add_executable(gti_abstract_generic_failure_contract_tests abstract_generic_failure_contract_tests.cpp)
target_link_libraries(gti_abstract_generic_failure_contract_tests PRIVATE gti_cpp_backend)
add_test(NAME abstract_generic_failure_contract COMMAND gti_abstract_generic_failure_contract_tests)
add_executable(gti_retained_generic_lifecycle_tests retained_generic_lifecycle_tests.cpp)
target_link_libraries(gti_retained_generic_lifecycle_tests PRIVATE gti_cpp_backend)
add_test(NAME retained_generic_lifecycle COMMAND gti_retained_generic_lifecycle_tests)

add_executable(gti_owned_pointee_projection_tests owned_pointee_projection_tests.cpp)
target_link_libraries(gti_owned_pointee_projection_tests PRIVATE gti_cpp_backend)
add_test(NAME owned_pointee_projection COMMAND gti_owned_pointee_projection_tests)

add_executable(gti_consuming_owned_pointee_tests consuming_owned_pointee_tests.cpp)
target_link_libraries(gti_consuming_owned_pointee_tests PRIVATE gti_cpp_backend)
add_test(NAME consuming_owned_pointee COMMAND gti_consuming_owned_pointee_tests)
add_executable(gti_optional_empty_prefix_tests optional_empty_prefix_tests.cpp)
target_link_libraries(gti_optional_empty_prefix_tests PRIVATE gti_cpp_backend)
add_test(NAME optional_empty_prefix COMMAND gti_optional_empty_prefix_tests)
add_executable(gti_field_array_empty_state_tests field_array_empty_state_tests.cpp)
target_link_libraries(gti_field_array_empty_state_tests PRIVATE gti_cpp_backend)
add_test(NAME field_array_empty_state COMMAND gti_field_array_empty_state_tests)
add_executable(gti_expected_payload_pending_cleanup_tests
               expected_payload_pending_cleanup_tests.cpp)
target_link_libraries(gti_expected_payload_pending_cleanup_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_payload_pending_cleanup
         COMMAND gti_expected_payload_pending_cleanup_tests)
add_executable(gti_temporary_owner_borrow_tests temporary_owner_borrow_tests.cpp)
target_link_libraries(gti_temporary_owner_borrow_tests PRIVATE gti_cpp_backend)
add_test(NAME temporary_owner_borrow COMMAND gti_temporary_owner_borrow_tests)
add_executable(gti_owned_expected_error_tests owned_expected_error_tests.cpp)
target_link_libraries(gti_owned_expected_error_tests PRIVATE gti_cpp_backend)
add_test(NAME owned_expected_error COMMAND gti_owned_expected_error_tests)
add_executable(gti_expected_transform_error_tests expected_transform_error_tests.cpp)
target_link_libraries(gti_expected_transform_error_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_transform_error COMMAND gti_expected_transform_error_tests)
add_executable(gti_owned_unexpected_pending_cleanup_tests owned_unexpected_pending_cleanup_tests.cpp)
target_link_libraries(gti_owned_unexpected_pending_cleanup_tests PRIVATE gti_cpp_backend)
add_test(NAME owned_unexpected_pending_cleanup COMMAND gti_owned_unexpected_pending_cleanup_tests)
add_executable(gti_private_constructor_placement_tests private_constructor_placement_tests.cpp)
target_link_libraries(gti_private_constructor_placement_tests PRIVATE gti_cpp_backend)
add_test(NAME private_constructor_placement COMMAND gti_private_constructor_placement_tests)
add_executable(gti_nested_guarded_expected_tests nested_guarded_expected_tests.cpp)
target_link_libraries(gti_nested_guarded_expected_tests PRIVATE gti_cpp_backend)
add_test(NAME nested_guarded_expected COMMAND gti_nested_guarded_expected_tests)
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
    NAME optimized_test_compiler_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/optimized_test_compiler_policy_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME cthread_runtime_firewall
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_boundary_test.py"
            "$<TARGET_FILE:gti_runtime_cthread_tests>"
  )
  set_tests_properties(cthread_runtime_firewall PROPERTIES TIMEOUT 60)
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
    NAME cmath_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cmath_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(cmath_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME http_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/http_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(http_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME practical_examples
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/practical_examples_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(practical_examples PROPERTIES TIMEOUT 600)
  add_test(
    NAME process_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/process_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "$<TARGET_FILE:gti_runtime>"
  )
  set_tests_properties(process_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME json_stdlib
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/json_stdlib_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(json_stdlib PROPERTIES TIMEOUT 2400)
  add_test(
    NAME cthread_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cthread_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(cthread_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME cthread_sleep_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/cthread_sleep_test.py"
                   "$<TARGET_FILE:gti>" "${CMAKE_CXX_COMPILER}")
  set_tests_properties(cthread_sleep_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME cthread_mutex_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/cthread_mutex_test.py"
                   "$<TARGET_FILE:gti>" "${CMAKE_CXX_COMPILER}")
  set_tests_properties(cthread_mutex_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME cthread_condition_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/cthread_condition_test.py"
                   "$<TARGET_FILE:gti>" "${CMAKE_CXX_COMPILER}")
  set_tests_properties(cthread_condition_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME cthread_once_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/cthread_once_test.py"
                   "$<TARGET_FILE:gti>" "${CMAKE_CXX_COMPILER}")
  set_tests_properties(cthread_once_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME cthread_create_join_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/cthread_create_join_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(cthread_create_join_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME unsafe_function_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/unsafe_function_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(unsafe_function_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME string_index_range_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/string_index_range_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(string_index_range_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME string_c_string_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/string_c_string_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(string_c_string_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME forward_list_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/forward_list_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(forward_list_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME pack_parameter_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/pack_parameter_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(pack_parameter_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME unique_owner_access_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/unique_owner_access_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(unique_owner_access_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME unique_scalar_allocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/unique_scalar_allocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(unique_scalar_allocation_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME unique_class_allocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/unique_class_allocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(unique_class_allocation_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME inherited_unique_allocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/inherited_unique_allocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(inherited_unique_allocation_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME unique_pointee_cleanup_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/unique_pointee_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(unique_pointee_cleanup_failure_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME aggregate_cleanup_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aggregate_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(aggregate_cleanup_failure_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME source_destructor_cleanup_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/source_destructor_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(source_destructor_cleanup_failure_runtime PROPERTIES TIMEOUT 300)

  add_test(
    NAME recursive_cleanup_failure_runtime
    COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/recursive_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(recursive_cleanup_failure_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME checked_owner_replacement_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/checked_owner_replacement_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(checked_owner_replacement_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME constructor_owned_call_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_owned_call_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_owned_call_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME collection_cleanup_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/collection_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(collection_cleanup_failure_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME release_version_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/release_version_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/check_release_version.py"
  )
  add_test(
    NAME repository_hosting
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/repository_hosting_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME ci_resources
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ci_resources_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/configure_ci_resources.sh"
  )
  add_test(
    NAME neovim_local_installer
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/neovim_local_installer_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/install_neovim_local.py"
  )
  add_test(
    NAME native_packages
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/native_packages_test.py"
  )
  add_test(
    NAME toy_engine_initializers
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/toy_engine_initializers_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME gitea_release_asset_sync
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/gitea_release_asset_sync_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/sync_gitea_release_assets.py"
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
    NAME generic_algorithm_breadth
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_breadth_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(generic_algorithm_breadth PROPERTIES TIMEOUT 600)
  add_test(
    NAME generic_algorithm_errors
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_errors_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(generic_algorithm_errors PROPERTIES TIMEOUT 180)
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
    NAME numeric_boundaries_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/numeric_boundaries_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(numeric_boundaries_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME container_swap_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/container_swap_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(container_swap_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME constexpr_aggregate_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_aggregate_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_aggregate_positive.gti"
  )
  add_test(
    NAME constexpr_text_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_text_positive.gti"
  )
  add_test(
    NAME constexpr_array_mir_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_array_mir_positive.gti"
  )
  add_test(
    NAME constexpr_readonly_references_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_readonly_references.gti"
  )
  add_test(
    NAME constexpr_library_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_library_positive.gti"
  )
  add_test(
    NAME constexpr_dependent_branches_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_dependent_branches.gti"
  )
  add_test(
    NAME constexpr_operators_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_operators.gti"
  )
  add_test(
    NAME constexpr_asset_table_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_asset_table.gti"
  )
  add_test(
    NAME constexpr_record_mir_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_record_mir_positive.gti"
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
    NAME optional_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/optional_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(optional_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME expected_propagation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_propagation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME expected_transform_error_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_transform_error_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(expected_transform_error_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME expected_payload_range_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_payload_range_runtime_test.py"
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
      NAME generic_clone_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_clone_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(generic_clone_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME issues_117_120_regressions
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/issues_117_120_regression_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issues_117_120_regressions PROPERTIES TIMEOUT 180)
  add_test(
    NAME issues_208_210_211_regressions
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issues_208_210_211_regression_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issues_208_210_211_regressions PROPERTIES TIMEOUT 300)
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
    NAME requires_disjunction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/requires_disjunction.gti"
  )
  set_tests_properties(requires_disjunction_runtime PROPERTIES TIMEOUT 180)
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
    NAME associative_hash_contracts
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/associative_hash_contracts_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(associative_hash_contracts PROPERTIES TIMEOUT 300)
  foreach(associative_suite IN ITEMS associative_map unordered_map
      associative_ownership_runtime associative_constructor_runtime
      nested_borrow_fields borrowing_composition container_contract_failure)
    add_test(
      NAME "${associative_suite}"
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/${associative_suite}_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties("${associative_suite}" PROPERTIES TIMEOUT 600)
  endforeach()
  add_test(
    NAME string_ordering_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/string_ordering_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(string_ordering_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME string_operations_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/string_operations_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(string_operations_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME string_operations_errors
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/string_operations_errors_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(string_operations_errors PROPERTIES TIMEOUT 300)
  add_test(NAME expected_filesystem_prerequisites_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/expected_filesystem_prerequisites_runtime_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(expected_filesystem_prerequisites_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME expected_passive_publication_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/expected_passive_publication_runtime_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(expected_passive_publication_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME charconv_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/charconv_runtime_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(charconv_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME complete_containers
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/complete_containers_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(complete_containers PROPERTIES TIMEOUT 900)
  add_test(NAME complete_containers_collection
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/complete_containers_collection_test.py" "$<TARGET_FILE:gti>")
  set_tests_properties(complete_containers_collection PROPERTIES TIMEOUT 600)
  add_test(
    NAME filesystem_operations_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/filesystem_operations_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(filesystem_operations_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME encoding_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/encoding_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(encoding_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME source_location_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/source_location_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/stdlib_surface_source_location.gti"
  )
  set_tests_properties(source_location_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME mutex_cleanup_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mutex_cleanup_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(mutex_cleanup_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME function_item_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/function_item_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/function_items.gti"
  )
  set_tests_properties(function_item_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME function_signature_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/function_signatures.gti"
            --include-o1
  )
  set_tests_properties(function_signature_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME signature_pack_deduction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/signature_pack_deduction.gti"
            --include-o1
  )
  set_tests_properties(signature_pack_deduction_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME class_signature_pattern_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/class_signature_pattern.gti"
            --include-o1
  )
  set_tests_properties(class_signature_pattern_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME stored_callable_invocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stored_callable_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(stored_callable_invocation_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME return_result_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/return_result_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(return_result_cleanup_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME contextual_numeric_literals_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/contextual_numeric_literals_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(contextual_numeric_literals_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME implicit_return_move_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/implicit_return_move_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/implicit_return_move.gti"
  )
  set_tests_properties(implicit_return_move_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME polymorphic_owner_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/polymorphic_owner_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(polymorphic_owner_cleanup_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME lambda_pack_invocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lambda_pack_invocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(lambda_pack_invocation_runtime PROPERTIES TIMEOUT 1200)
  add_test(
    NAME owning_function_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owning_function_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owning_function_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME consuming_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_receiver_runtime PROPERTIES TIMEOUT 480)
  add_test(
    NAME consuming_methods_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_methods_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_methods_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME consuming_lambda_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_lambda_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_lambda_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owned_pointee_projection_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owned_pointee_projection_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owned_pointee_projection_runtime PROPERTIES TIMEOUT 240)
  add_test(
    NAME shared_ownership_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_ownership_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(shared_ownership_runtime PROPERTIES TIMEOUT 480)
  add_test(
    NAME shared_ownership_failure
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_ownership_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(shared_ownership_failure PROPERTIES TIMEOUT 480)
  add_test(
    NAME shared_ownership_threads
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_ownership_threads_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(shared_ownership_threads PROPERTIES TIMEOUT 480)
  add_test(
    NAME http_url_operators
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/http_url_operators_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(http_url_operators PROPERTIES TIMEOUT 480)
  add_test(
    NAME consuming_owned_pointee_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_owned_pointee_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_owned_pointee_runtime PROPERTIES TIMEOUT 480)
  add_test(
    NAME retained_owner_member_pack_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/retained_owner_member_pack_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(retained_owner_member_pack_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME hosted_constructor_identity_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/hosted_constructor_identity_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(hosted_constructor_identity_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME discarded_nominal_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/discarded_nominal_result_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(discarded_nominal_result_runtime PROPERTIES TIMEOUT 240)
  add_test(
    NAME nested_nominal_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/nested_nominal_result_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(nested_nominal_result_runtime PROPERTIES TIMEOUT 480)
  add_test(
    NAME generic_callable_adapter_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_callable_adapter_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(generic_callable_adapter_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructed_callback_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructed_callback_argument_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructed_callback_argument_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME pack_prefix_constructor_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/pack_prefix_constructor_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(pack_prefix_constructor_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructor_field_publication_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_field_publication_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_field_publication_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME fresh_callable_later_argument_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fresh_callable_later_argument_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(fresh_callable_later_argument_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME consuming_retained_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_retained_field_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_retained_field_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME consuming_retained_field_replace_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_retained_field_replace_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_retained_field_replace_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME consuming_retained_lambda_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_retained_lambda_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_retained_lambda_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME expected_payload_pair_arguments_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_payload_pair_arguments_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(expected_payload_pair_arguments_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME expected_payload_pending_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_payload_pending_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(expected_payload_pending_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME temporary_owner_borrow_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/temporary_owner_borrow_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(temporary_owner_borrow_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owned_expected_error_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owned_expected_error_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owned_expected_error_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owned_unexpected_pending_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owned_unexpected_pending_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owned_unexpected_pending_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME private_constructor_placement_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/private_constructor_placement_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(private_constructor_placement_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME nested_guarded_expected_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/nested_guarded_expected_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(nested_guarded_expected_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owning_function_policy_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owning_function_policy_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owning_function_policy_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME abstract_generic_failure_contract_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/abstract_generic_failure_contract_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(abstract_generic_failure_contract_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owning_function_resource_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owning_function_resource_result_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owning_function_resource_result_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructor_argument_staging_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_argument_staging_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_argument_staging_runtime PROPERTIES TIMEOUT 480)
  add_test(
    NAME void_return_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/void_return_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(void_return_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME function_signature_lsp
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/function_signature_lsp_test.py"
            "$<TARGET_FILE:gti_lsp>"
  )
  set_tests_properties(function_signature_lsp PROPERTIES TIMEOUT 60)
  add_test(
    NAME executable_entry_cli_lsp
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/executable_entry_test.py"
            "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_lsp>"
  )
  set_tests_properties(executable_entry_cli_lsp PROPERTIES TIMEOUT 180)
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
      NAME generic_owner_callable_variants_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_owner_callable_variants_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/generic_owner_callable_variants.gti"
    )
    set_tests_properties(generic_owner_callable_variants_runtime
                         PROPERTIES TIMEOUT 300)
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
    NAME temporary_constructor_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/temporary_constructor_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(temporary_constructor_argument_runtime
                       PROPERTIES TIMEOUT 300)
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
      NAME payload_enum_containment_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/payload_enum_containment_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(payload_enum_containment_runtime
                         PROPERTIES TIMEOUT 240)
    add_test(
      NAME recursive_payload_sum_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/recursive_payload_sum_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(recursive_payload_sum_runtime PROPERTIES TIMEOUT 300)
    add_test(
      NAME borrowed_payload_pattern_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/borrowed_payload_pattern_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(borrowed_payload_pattern_runtime PROPERTIES TIMEOUT 300)
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
      NAME mutable_stored_dependency_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mutable_stored_dependency_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/mutable_stored_dependency.gti"
    )
    set_tests_properties(mutable_stored_dependency_runtime
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
      NAME fallible_borrow_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/fallible_borrow_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/fallible_borrow.gti"
    )
    set_tests_properties(fallible_borrow_runtime PROPERTIES TIMEOUT 300)
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
    NAME objective_cpp_bridge
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/objective_cpp_bridge_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/objective-cpp-bridge"
  )
  set_tests_properties(objective_cpp_bridge
                       PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
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
    NAME compile_time_harness
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compile_time_compare_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/compile_time_compare.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
  )
  set_tests_properties(
    compile_time_harness
    PROPERTIES TIMEOUT 240
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
    NAME language_coverage_fixed_corpus
    COMMAND "${PROJECT_SOURCE_DIR}/tests/coverage/run_coverage.sh"
            "$<TARGET_FILE:gti>"
            "${PROJECT_BINARY_DIR}/language-coverage-fixed-corpus"
            --fixed-baseline
  )
  set_tests_properties(
    language_coverage_fixed_corpus
    PROPERTIES TIMEOUT 900
               PROCESSORS 2
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>;GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
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
      NAME managed_thread_tsan
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/managed_thread_tsan_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/managed_thread_class_result.gti"
              "${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(managed_thread_tsan PROPERTIES TIMEOUT 300)
    add_test(
      NAME atomic_end_to_end
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/atomic_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_scalar.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_concurrent.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_orders.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_fetch.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_fetch_overflow.gti"
    )
    set_tests_properties(atomic_end_to_end PROPERTIES TIMEOUT 600)
    add_test(
      NAME atomic_tsan
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/atomic_tsan_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_concurrent.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_orders.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_fetch.gti"
              "${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(atomic_tsan PROPERTIES TIMEOUT 600)
    add_test(
      NAME mutex_end_to_end
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mutex_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/mutex_basic.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/mutex_concurrent.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/mutex_move_only.gti"
    )
    set_tests_properties(mutex_end_to_end PROPERTIES TIMEOUT 300)
    add_test(
      NAME mutex_tsan
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mutex_tsan_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/mutex_concurrent.gti"
              "${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(mutex_tsan PROPERTIES TIMEOUT 300)
    add_test(
      NAME domain_operator_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/domain_operator_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(domain_operator_runtime PROPERTIES TIMEOUT 300)
    add_test(
      NAME float_text_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/float_text_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(float_text_runtime PROPERTIES TIMEOUT 300)


  if(TARGET gti_lsp)
    add_test(
      NAME lsp_project_facts
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_project_facts_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME lsp_protocol
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME atomic_lsp
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>" atomic
    )
    add_test(
      NAME mutex_lsp
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>" mutex
    )
    add_test(
      NAME expected_payload_range_lsp
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>" expected-payload-range
    )
    add_test(
      NAME expected_transform_error_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/expected_transform_error_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME borrowed_payload_pattern_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/borrowed_payload_pattern_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME block_comment_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/block_comment_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(NAME unsafe_function_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/unsafe_function_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME cthread_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/cthread_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(
      NAME source_text_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/source_text_lsp_test.py"
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
  endif()
endif()

# Release binaries intentionally contain no source-tree resource fallback.
# Every build-tree test runs before staging, including newly registered CLI/LSP
# tests. Derive the inventory instead of maintaining an incomplete name list.
# Installed-consumer checks run outside this directory without these overrides.
if(GTI_RELEASE_BUILD)
  get_property(gti_build_tree_tests DIRECTORY PROPERTY TESTS)
  set_property(TEST ${gti_build_tree_tests} APPEND PROPERTY ENVIRONMENT
    "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib"
    "GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include"
    "GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
    "GTI_VENDOR_INCLUDE=${PROJECT_SOURCE_DIR}/vendor/expected_lite/include"
  )
endif()
