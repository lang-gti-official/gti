add_test(NAME stdlib_module_boundaries COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/stdlib_module_boundaries_test.py" $<TARGET_FILE:gti>)
set_tests_properties(stdlib_module_boundaries PROPERTIES TIMEOUT 120)

add_executable(gti_generic_method_tooling_tests generic_method_tooling_tests.cpp)
target_link_libraries(gti_generic_method_tooling_tests PRIVATE gti_compiler)
add_test(NAME generic_method_tooling COMMAND gti_generic_method_tooling_tests)

add_executable(gti_source_owner_tests source_owner_tests.cpp)
target_link_libraries(gti_source_owner_tests PRIVATE gti_compiler)
add_test(NAME source_owner_registration COMMAND gti_source_owner_tests)

add_executable(gti_driver_module_facts_tests driver_module_facts_tests.cpp)
target_link_libraries(gti_driver_module_facts_tests PRIVATE gti_driver)
add_test(NAME driver_module_facts COMMAND gti_driver_module_facts_tests)
add_test(NAME bindgen_module_cli COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/bindgen_module_cli_test.py" $<TARGET_FILE:gti>)

add_executable(gti_include_removal_tests include_removal_tests.cpp)
target_link_libraries(gti_include_removal_tests PRIVATE gti_compiler)
add_test(NAME include_removal COMMAND gti_include_removal_tests)
add_executable(gti_module_editor_recovery_tests module_editor_recovery_tests.cpp)
target_link_libraries(gti_module_editor_recovery_tests PRIVATE gti_compiler)
add_test(NAME module_editor_recovery COMMAND gti_module_editor_recovery_tests)
add_executable(gti_module_query_tests module_query_tests.cpp)
target_link_libraries(gti_module_query_tests PRIVATE gti_compiler)
add_test(NAME module_queries COMMAND gti_module_query_tests)
add_executable(gti_persistent_declaration_tests persistent_declaration_tests.cpp)
target_link_libraries(gti_persistent_declaration_tests PRIVATE gti_compiler)
add_test(NAME persistent_declaration_identity COMMAND gti_persistent_declaration_tests)
add_test(NAME persistent_declaration_process COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/persistent_declaration_process_test.py"
  $<TARGET_FILE:gti_persistent_declaration_tests>)
add_executable(gti_module_identity_tests module_identity_tests.cpp)
target_link_libraries(gti_module_identity_tests PRIVATE gti_compiler)
add_test(NAME module_semantic_identity COMMAND gti_module_identity_tests)
add_executable(gti_module_preamble_tests module_preamble_tests.cpp)
target_link_libraries(gti_module_preamble_tests PRIVATE gti_compiler)
add_test(NAME module_preamble_identity COMMAND gti_module_preamble_tests)
add_test(NAME module_system_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_system_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(module_system_runtime PROPERTIES TIMEOUT 240)
add_test(NAME module_initialization_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_initialization_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(module_initialization_runtime PROPERTIES TIMEOUT 240)
add_test(NAME module_cpp_boundary
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_cpp_boundary_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(module_cpp_boundary PROPERTIES TIMEOUT 180)
if(TARGET gti_lsp)
  add_test(NAME module_map_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_map_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(module_map_lsp PROPERTIES TIMEOUT 90)
  add_test(NAME module_navigation_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_navigation_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(module_navigation_lsp PROPERTIES TIMEOUT 120)
  add_test(NAME include_removal_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/include_removal_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(include_removal_lsp PROPERTIES TIMEOUT 120)
endif()

add_executable(gti_thread_local_syntax_tests thread_local_syntax_tests.cpp)
target_link_libraries(gti_thread_local_syntax_tests PRIVATE gti_compiler)
add_test(NAME thread_local_syntax COMMAND gti_thread_local_syntax_tests)
add_executable(gti_thread_local_semantic_tests thread_local_semantic_tests.cpp)
target_link_libraries(gti_thread_local_semantic_tests PRIVATE gti_compiler)
add_test(NAME thread_local_semantic COMMAND gti_thread_local_semantic_tests)
if(TARGET gti_lsp)
  add_test(NAME thread_local_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/thread_local_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(thread_local_lsp PROPERTIES TIMEOUT 90)
  add_test(NAME consteval_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/consteval_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(consteval_lsp PROPERTIES TIMEOUT 90)
endif()

add_executable(gti_foreign_variable_borrow_tests foreign_variable_borrow_tests.cpp)
target_link_libraries(gti_foreign_variable_borrow_tests PRIVATE gti_compiler)
add_test(NAME foreign_variable_borrow COMMAND gti_foreign_variable_borrow_tests)

add_executable(gti_foreign_variable_tests foreign_variable_tests.cpp)
target_link_libraries(gti_foreign_variable_tests PRIVATE gti_cpp_backend)
add_test(NAME foreign_variable_pipeline COMMAND gti_foreign_variable_tests)
add_test(NAME native_variable_bindgen
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_bindgen_runtime_test.py"
    $<TARGET_FILE:gti> --example native-variable-bindings)
set_tests_properties(native_variable_bindgen PROPERTIES TIMEOUT 600)

# Preserve the build-tree location of test executables and corpus tools while
# moving their target ownership out of the root project file.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

# The native-backend tests compile emitted IR with the clang and opt of the
# configured LLVM. An installed LLVM provides them beside its CMake package; a
# bundled LLVM provides them as its own targets, built with the tests so the
# same evidence runs against the pinned release LLVM.
if(GTI_BUNDLE_LLVM AND TARGET clang AND TARGET opt)
  add_custom_target(gti_llvm_test_tools ALL)
  add_dependencies(gti_llvm_test_tools clang opt)
  set(GTI_LLVM_TEST_CLANG "$<TARGET_FILE:clang>")
  set(GTI_LLVM_TEST_OPT "$<TARGET_FILE:opt>")
else()
  find_program(GTI_LLVM_TEST_CLANG NAMES clang HINTS "${LLVM_TOOLS_BINARY_DIR}" NO_DEFAULT_PATH)
  set(GTI_LLVM_TEST_OPT "${LLVM_TOOLS_BINARY_DIR}/opt")
endif()

if(TARGET gti_lsp)
  add_executable(gti_lsp_repair_contract_tests lsp_repair_contract_tests.cpp)
  target_include_directories(gti_lsp_repair_contract_tests SYSTEM PRIVATE
    ${GTI_LLVM_INCLUDE_DIRS})
  target_link_libraries(gti_lsp_repair_contract_tests PRIVATE
    gti_driver gti_lsp_state Threads::Threads)
  target_compile_definitions(gti_lsp_repair_contract_tests PRIVATE
    GTI_VERSION="${PROJECT_VERSION}"
    GTI_BUILD_STDLIB_ROOT="${GTI_BUILD_STDLIB_ROOT}")
  add_test(NAME lsp_repair_groups COMMAND gti_lsp_repair_contract_tests)
endif()

add_executable(gti_code_snippet_tests code_snippet_tests.cpp)
target_link_libraries(gti_code_snippet_tests PRIVATE gti_compiler)
add_test(NAME code_snippets COMMAND gti_code_snippet_tests)
add_executable(gti_macro_expansion_query_tests macro_expansion_query_tests.cpp)
target_link_libraries(gti_macro_expansion_query_tests PRIVATE gti_compiler)
add_test(NAME macro_expansion_queries COMMAND gti_macro_expansion_query_tests)
add_executable(gti_macro_fresh_callable_tests macro_fresh_callable_tests.cpp)
target_link_libraries(gti_macro_fresh_callable_tests PRIVATE gti_compiler)
add_test(NAME macro_fresh_callables COMMAND gti_macro_fresh_callable_tests)
set_tests_properties(macro_fresh_callables PROPERTIES TIMEOUT 60)
add_test(NAME macro_fresh_callable_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
    $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/macro_fresh_callables.gti"
    --ownership-checks)
set_tests_properties(macro_fresh_callable_runtime PROPERTIES TIMEOUT 1200)
add_test(NAME macro_result_failures
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/macro_result_failure_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(macro_result_failures PROPERTIES TIMEOUT 1200)
add_executable(gti_macro_result_parser_tests macro_result_parser_tests.cpp)
target_link_libraries(gti_macro_result_parser_tests PRIVATE gti_compiler)
add_test(NAME macro_result_parser COMMAND gti_macro_result_parser_tests)
find_program(GTI_MACRO_SYNTAX_NEOVIM_EXECUTABLE nvim)
if(GTI_MACRO_SYNTAX_NEOVIM_EXECUTABLE)
  add_test(NAME macro_neovim_syntax
    COMMAND "${CMAKE_COMMAND}" -E env
      "GTI_TEST_REPOSITORY=${PROJECT_SOURCE_DIR}"
      "XDG_STATE_HOME=${PROJECT_BINARY_DIR}/nvim-test/state"
      "XDG_CACHE_HOME=${PROJECT_BINARY_DIR}/nvim-test/cache"
      "XDG_DATA_HOME=${PROJECT_BINARY_DIR}/nvim-test/data"
      "NVIM_LOG_FILE=${PROJECT_BINARY_DIR}/nvim-macro-syntax.log"
      "${GTI_MACRO_SYNTAX_NEOVIM_EXECUTABLE}" --headless -u NONE -i NONE
      -l "${PROJECT_SOURCE_DIR}/tests/macro_syntax_test.lua")
  set_tests_properties(macro_neovim_syntax PROPERTIES TIMEOUT 30)
endif()
add_executable(gti_expression_text_tests expression_text_tests.cpp)
add_executable(gti_macro_value_tests macro_value_tests.cpp)
target_link_libraries(gti_macro_value_tests PRIVATE gti_compiler)
add_test(NAME macro_values_semantic_ir COMMAND gti_macro_value_tests)
add_test(NAME macro_values_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
    $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/macro_values.gti" --ownership-checks)
set_tests_properties(macro_values_runtime PROPERTIES TIMEOUT 1200)
target_link_libraries(gti_expression_text_tests PRIVATE gti_compiler)
add_test(NAME expression_text_semantic_ir COMMAND gti_expression_text_tests)
add_test(NAME expression_text_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/expression_text_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(expression_text_runtime PROPERTIES TIMEOUT 1200)
if(TARGET gti_lsp)
  add_test(NAME expression_text_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/expression_text_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(expression_text_lsp PROPERTIES TIMEOUT 180)
endif()
if(TARGET gti_lsp)
  add_test(NAME lsp_code_snippets
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_code_snippets_test.py"
      $<TARGET_FILE:gti_lsp> $<TARGET_FILE:gti>)
  set_tests_properties(lsp_code_snippets PROPERTIES TIMEOUT 180)
endif()

add_executable(gti_foreign_variable_syntax_tests foreign_variable_syntax_tests.cpp)
target_link_libraries(gti_foreign_variable_syntax_tests PRIVATE gti_compiler)
add_test(NAME foreign_variable_syntax COMMAND gti_foreign_variable_syntax_tests)
if(TARGET gti_lsp)
  add_test(NAME foreign_variable_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/foreign_variable_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(foreign_variable_lsp PROPERTIES TIMEOUT 90)
endif()

add_executable(gti_protected_member_tests protected_member_tests.cpp)
target_link_libraries(gti_protected_member_tests PRIVATE gti_compiler)
add_test(NAME protected_members COMMAND gti_protected_member_tests)
set_tests_properties(protected_members PROPERTIES TIMEOUT 30)
add_test(NAME protected_member_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/protected_member_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(protected_member_runtime PROPERTIES TIMEOUT 600)
if(TARGET gti_lsp)
  add_test(NAME protected_member_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/protected_member_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(protected_member_lsp PROPERTIES TIMEOUT 90)
endif()

add_executable(gti_nested_type_syntax_tests nested_type_syntax_tests.cpp)
target_link_libraries(gti_nested_type_syntax_tests PRIVATE gti_compiler)
add_test(NAME nested_type_syntax COMMAND gti_nested_type_syntax_tests)
set_tests_properties(nested_type_syntax PROPERTIES TIMEOUT 10)
add_test(NAME nested_type_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_type_runtime_test.py"
    $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
set_tests_properties(nested_type_runtime PROPERTIES TIMEOUT 600)
if(TARGET gti_lsp)
  add_test(NAME nested_type_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_type_lsp_test.py"
      $<TARGET_FILE:gti_lsp>)
  set_tests_properties(nested_type_lsp PROPERTIES TIMEOUT 90)
endif()

add_executable(gti_qualified_type_member_tests qualified_type_member_tests.cpp)
target_link_libraries(gti_qualified_type_member_tests PRIVATE gti_compiler)
add_test(NAME qualified_type_members COMMAND gti_qualified_type_member_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME qualified_type_member_runtime
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/qualified_type_member_runtime_test.py"
      $<TARGET_FILE:gti>
      "${PROJECT_SOURCE_DIR}/examples/132-qualified-type-members.gti")
  set_tests_properties(qualified_type_member_runtime PROPERTIES TIMEOUT 600)
  if(TARGET gti_lsp)
    add_test(NAME qualified_type_member_lsp
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/qualified_type_member_lsp_test.py"
        $<TARGET_FILE:gti_lsp>)
    set_tests_properties(qualified_type_member_lsp PROPERTIES TIMEOUT 60)
  endif()
endif()

add_executable(gti_local_reference_inference_tests local_reference_inference_tests.cpp)
target_link_libraries(gti_local_reference_inference_tests PRIVATE gti_cpp_backend)
add_test(NAME local_reference_inference COMMAND gti_local_reference_inference_tests)
add_executable(gti_bounded_array_borrows_tests bounded_array_borrows_tests.cpp)
target_link_libraries(gti_bounded_array_borrows_tests PRIVATE gti_cpp_backend)
add_test(NAME bounded_array_borrows COMMAND gti_bounded_array_borrows_tests)
add_test(NAME bounded_array_borrows_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/bounded_array_borrows_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(bounded_array_borrows_runtime PROPERTIES TIMEOUT 300)
add_test(NAME local_reference_inference_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/local_reference_inference_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(local_reference_inference_runtime PROPERTIES TIMEOUT 300)
if(TARGET gti_lsp)
  add_test(NAME local_reference_inference_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/local_reference_inference_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(local_reference_inference_lsp PROPERTIES TIMEOUT 120)
endif()

add_executable(gti_prvalue_direct_initialization_tests
               prvalue_direct_initialization_tests.cpp)
target_link_libraries(gti_prvalue_direct_initialization_tests PRIVATE gti_cpp_backend)
add_test(NAME prvalue_direct_initialization
         COMMAND gti_prvalue_direct_initialization_tests)
add_test(NAME prvalue_direct_initialization_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/prvalue_direct_initialization_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(prvalue_direct_initialization_runtime PROPERTIES TIMEOUT 600)
if(TARGET gti_lsp)
  add_test(NAME prvalue_direct_initialization_lsp
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/prvalue_direct_initialization_lsp_test.py"
                   "$<TARGET_FILE:gti_lsp>")
  set_tests_properties(prvalue_direct_initialization_lsp PROPERTIES TIMEOUT 90)
endif()

add_executable(gti_generic_enum_declaration_tests generic_enum_declaration_tests.cpp)
target_link_libraries(gti_generic_enum_declaration_tests PRIVATE gti_compiler)
add_test(NAME generic_enum_declaration COMMAND gti_generic_enum_declaration_tests)

add_executable(gti_generic_payload_enum_backend_tests generic_payload_enum_backend_tests.cpp)
target_link_libraries(gti_generic_payload_enum_backend_tests PRIVATE gti_cpp_backend)
add_test(NAME generic_payload_enum_backend COMMAND gti_generic_payload_enum_backend_tests)

add_executable(gti_operator_relay_tests operator_relay_tests.cpp)
target_link_libraries(gti_operator_relay_tests PRIVATE gti_cpp_backend)
add_test(NAME operator_relay_pipeline COMMAND gti_operator_relay_tests)
add_test(NAME operator_relay_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/operator_relay_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(operator_relay_runtime PROPERTIES TIMEOUT 300)
add_test(NAME existing_value_allocation_failure_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/existing_value_allocation_failure_test.py" $<TARGET_FILE:gti>)
set_tests_properties(existing_value_allocation_failure_runtime PROPERTIES TIMEOUT 180)
if(TARGET gti_lsp)
  add_test(NAME operator_relay_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/operator_relay_lsp_test.py" $<TARGET_FILE:gti_lsp>)
endif()

add_executable(gti_native_c_scalar_target_tests native_c_scalar_target_tests.cpp)
target_link_libraries(gti_native_c_scalar_target_tests PRIVATE gti_compiler)
add_test(NAME native_c_scalar_target COMMAND gti_native_c_scalar_target_tests)
add_executable(gti_native_c_scalar_tests native_c_scalar_tests.cpp)
target_link_libraries(gti_native_c_scalar_tests PRIVATE gti_cpp_backend)
add_test(NAME native_c_scalar_pipeline COMMAND gti_native_c_scalar_tests)

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
add_executable(gti_runtime_charconv_tests runtime_charconv_tests.cpp
               runtime_charconv_parser_tests.cpp)
if(GTI_HAS_STD_FLOAT_FROM_CHARS)
  target_compile_definitions(gti_runtime_charconv_tests PRIVATE
                             GTI_HAS_STD_FLOAT_FROM_CHARS=1)
endif()
if(NOT GTI_HAS_STD_FLOAT_FROM_CHARS OR GTI_FORCE_PORTABLE_CHARCONV)
  target_compile_definitions(gti_runtime_charconv_tests PRIVATE
                             GTI_TEST_PORTABLE_CHARCONV=1)
endif()
target_compile_features(gti_runtime_charconv_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_charconv_tests PRIVATE gti_runtime)
add_test(NAME charconv_runtime_boundary COMMAND gti_runtime_charconv_tests)
add_executable(gti_mir_full_expression_tests mir_full_expression_tests.cpp)
target_link_libraries(gti_mir_full_expression_tests PRIVATE gti_compiler)
add_test(NAME mir_full_expression COMMAND gti_mir_full_expression_tests)
add_executable(gti_loop_break_reinitialization_tests
               loop_break_reinitialization_tests.cpp)
target_link_libraries(gti_loop_break_reinitialization_tests PRIVATE gti_compiler)
add_test(NAME loop_break_reinitialization
         COMMAND gti_loop_break_reinitialization_tests)
add_test(NAME loop_break_reinitialization_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/loop_break_reinitialization_runtime_test.py"
                 "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(loop_break_reinitialization_runtime PROPERTIES TIMEOUT 300)
add_executable(gti_constructor_pack_field_tests constructor_pack_field_tests.cpp)
target_link_libraries(gti_constructor_pack_field_tests PRIVATE gti_cpp_backend)
add_test(NAME constructor_pack_field COMMAND gti_constructor_pack_field_tests)
add_test(NAME constructor_pack_field_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/constructor_pack_field_runtime_test.py"
                 "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(constructor_pack_field_runtime PROPERTIES TIMEOUT 600)
add_executable(gti_base_constructor_stage_tests base_constructor_stage_tests.cpp)
target_link_libraries(gti_base_constructor_stage_tests PRIVATE gti_cpp_backend)
add_test(NAME base_constructor_stage COMMAND gti_base_constructor_stage_tests)
add_executable(gti_expected_constructor_field_tests expected_constructor_field_tests.cpp)
target_link_libraries(gti_expected_constructor_field_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_constructor_field COMMAND gti_expected_constructor_field_tests)
add_test(NAME generated_constructor_order_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/generated_constructor_order_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(generated_constructor_order_runtime PROPERTIES TIMEOUT 300)
add_test(NAME base_constructor_stage_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/base_constructor_stage_runtime_test.py"
                 "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(base_constructor_stage_runtime PROPERTIES TIMEOUT 600)
add_executable(gti_operator_place_composition_tests operator_place_composition_tests.cpp)
target_link_libraries(gti_operator_place_composition_tests PRIVATE gti_cpp_backend)
add_test(NAME operator_place_composition_pipeline COMMAND gti_operator_place_composition_tests)
add_executable(gti_ephemeral_carrier_mir_tests ephemeral_carrier_mir_tests.cpp)
add_executable(gti_fresh_callable_borrow_tests fresh_callable_borrow_tests.cpp)
target_link_libraries(gti_fresh_callable_borrow_tests PRIVATE gti_compiler)
add_test(NAME fresh_callable_borrow COMMAND gti_fresh_callable_borrow_tests)
add_executable(gti_borrow_carrier_receiver_tests borrow_carrier_receiver_tests.cpp)
target_link_libraries(gti_borrow_carrier_receiver_tests PRIVATE gti_compiler)
add_test(NAME borrow_carrier_receivers COMMAND gti_borrow_carrier_receiver_tests)
add_test(NAME borrow_carrier_receivers_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/borrow_carrier_receiver_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(borrow_carrier_receivers_runtime PROPERTIES TIMEOUT 600)
add_executable(gti_borrowing_callbacks_mir_tests borrowing_callbacks_mir_tests.cpp)
target_link_libraries(gti_borrowing_callbacks_mir_tests PRIVATE gti_compiler)
add_test(NAME borrowing_callbacks_mir COMMAND gti_borrowing_callbacks_mir_tests)
add_test(NAME borrowing_callback_alias_proof
         COMMAND gti_borrowing_callbacks_mir_tests --alias-proof-only)
add_test(NAME borrowing_callback_relay_proof
         COMMAND gti_borrowing_callbacks_mir_tests --relay-proof-only)
add_test(NAME borrowing_callback_restoration_return
         COMMAND gti_borrowing_callbacks_mir_tests --restoration-proof-only)
add_executable(gti_resource_memory_runtime_tests resource_memory_runtime_tests.cpp)
target_link_libraries(gti_resource_memory_runtime_tests PRIVATE gti_runtime)
add_test(NAME resource_memory_runtime COMMAND gti_resource_memory_runtime_tests)
add_executable(gti_process_provider_target_tests process_provider_target_tests.cpp)
target_link_libraries(gti_process_provider_target_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME process_provider_target_contract COMMAND gti_process_provider_target_tests)
add_executable(gti_storage_adoption_tests storage_adoption_tests.cpp)
target_link_libraries(gti_storage_adoption_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME storage_adoption_contract COMMAND gti_storage_adoption_tests)

add_executable(gti_object_lifetime_tests object_lifetime_tests.cpp)
target_link_libraries(gti_object_lifetime_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME object_lifetime_contract COMMAND gti_object_lifetime_tests)
add_executable(gti_text_address_bridge_tests text_address_bridge_tests.cpp)
target_link_libraries(gti_text_address_bridge_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME text_address_bridge_contract COMMAND gti_text_address_bridge_tests)
add_executable(gti_source_prefix_owner_tests source_prefix_owner_tests.cpp)
target_link_libraries(gti_source_prefix_owner_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME source_prefix_owner_contract COMMAND gti_source_prefix_owner_tests)
add_executable(gti_uninitialized_carrier_tests uninitialized_carrier_tests.cpp)
target_link_libraries(gti_uninitialized_carrier_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME uninitialized_carrier_contract COMMAND gti_uninitialized_carrier_tests)
add_executable(gti_source_sparse_storage_tests source_sparse_storage_tests.cpp)
target_link_libraries(gti_source_sparse_storage_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME source_sparse_storage_contract COMMAND gti_source_sparse_storage_tests)
add_executable(gti_conditional_capability_tests conditional_capability_tests.cpp)
target_link_libraries(gti_conditional_capability_tests PRIVATE gti_compiler)
add_test(NAME conditional_capability_contract COMMAND gti_conditional_capability_tests)
add_test(NAME conditional_capability_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/conditional_capability_runtime_test.py"
    "$<TARGET_FILE:gti>")
set_tests_properties(conditional_capability_runtime PROPERTIES TIMEOUT 300)
if(TARGET gti_lsp)
  add_test(NAME conditional_capability_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/conditional_capability_lsp_test.py"
      "$<TARGET_FILE:gti_lsp>")
  set_tests_properties(conditional_capability_lsp PROPERTIES TIMEOUT 90)
endif()
add_test(NAME source_sparse_storage_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_sparse_storage_runtime_test.py"
    "$<TARGET_FILE:gti_source_sparse_storage_tests>" "${CMAKE_CXX_COMPILER}"
    "$<TARGET_FILE:gti_runtime>")
set_tests_properties(source_sparse_storage_runtime PROPERTIES TIMEOUT 300)
add_executable(gti_resource_protocol_tests resource_protocol_tests.cpp)
target_link_libraries(gti_resource_protocol_tests PRIVATE gti_compiler gti_cpp_backend)
add_test(NAME resource_protocol_contract COMMAND gti_resource_protocol_tests)
target_link_libraries(gti_ephemeral_carrier_mir_tests PRIVATE gti_compiler)
add_test(NAME ephemeral_carrier_mir COMMAND gti_ephemeral_carrier_mir_tests)

add_executable("gti_runtime_thread_tests" "${PROJECT_SOURCE_DIR}/tests/runtime_thread_tests.cpp")
target_include_directories(
  "gti_runtime_thread_tests"
  PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src"
)
target_compile_features("gti_runtime_thread_tests" PRIVATE cxx_std_23)
target_link_libraries("gti_runtime_thread_tests" PRIVATE gti_runtime)
add_test(NAME managed_thread_runtime COMMAND gti_runtime_thread_tests)

add_executable(gti_runtime_thread_local_tests runtime_thread_local_tests.cpp)
target_compile_features(gti_runtime_thread_local_tests PRIVATE cxx_std_23)
target_link_libraries(gti_runtime_thread_local_tests PRIVATE gti_runtime)
add_test(NAME thread_local_runtime_registry COMMAND gti_runtime_thread_local_tests)

add_executable(gti_runtime_scoped_thread_tests runtime_scoped_thread_tests.cpp)
target_include_directories(gti_runtime_scoped_thread_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features(gti_runtime_scoped_thread_tests PRIVATE cxx_std_23)
target_link_libraries(gti_runtime_scoped_thread_tests PRIVATE gti_runtime)
add_test(NAME scoped_thread_runtime COMMAND gti_runtime_scoped_thread_tests)
add_test(NAME scoped_thread_join_boundary
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/runtime_scoped_thread_boundary_test.py"
          "$<TARGET_FILE:gti_runtime_scoped_thread_tests>")
set_tests_properties(scoped_thread_runtime scoped_thread_join_boundary
                     PROPERTIES TIMEOUT 60)
add_executable(gti_runtime_scoped_thread_header_smoke
               runtime_scoped_thread_header_smoke.c)
target_compile_features(gti_runtime_scoped_thread_header_smoke PRIVATE c_std_17)
target_link_libraries(gti_runtime_scoped_thread_header_smoke PRIVATE gti_runtime)
add_test(NAME scoped_thread_c_header COMMAND gti_runtime_scoped_thread_header_smoke)
set_tests_properties(scoped_thread_c_header PROPERTIES TIMEOUT 60)

add_executable("gti_runtime_cthread_tests"
               "${PROJECT_SOURCE_DIR}/tests/runtime_cthread_tests.cpp")
target_include_directories("gti_runtime_cthread_tests"
                           PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_features("gti_runtime_cthread_tests" PRIVATE cxx_std_20)
target_link_libraries("gti_runtime_cthread_tests" PRIVATE gti_runtime)
add_test(NAME cthread_runtime_boundary COMMAND gti_runtime_cthread_tests)
add_executable(gti_runtime_thread_outcome_tests runtime_thread_outcome_tests.cpp
               fixtures/runtime_thread_outcome_c.c)
target_compile_features(gti_runtime_thread_outcome_tests PRIVATE cxx_std_20 c_std_17)
target_link_libraries(gti_runtime_thread_outcome_tests PRIVATE gti_runtime)
if(Python3_Interpreter_FOUND)
  add_test(NAME runtime_thread_outcome_boundary
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/runtime_thread_outcome_test.py"
            "$<TARGET_FILE:gti_runtime_thread_outcome_tests>")
  set_tests_properties(runtime_thread_outcome_boundary PROPERTIES TIMEOUT 120)
endif()
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

add_executable(gti_runtime_environment_tests runtime_environment_tests.cpp)
target_compile_features(gti_runtime_environment_tests PRIVATE cxx_std_20)
target_link_libraries(gti_runtime_environment_tests PRIVATE gti_runtime Threads::Threads)
add_test(NAME environment_runtime_boundary COMMAND gti_runtime_environment_tests)
add_executable(gti_runtime_environment_header_smoke runtime_environment_header_smoke.c)
target_compile_features(gti_runtime_environment_header_smoke PRIVATE c_std_17)
target_link_libraries(gti_runtime_environment_header_smoke PRIVATE gti_runtime)
add_test(NAME environment_c_header COMMAND gti_runtime_environment_header_smoke)
add_executable(gti_environment_target_tests environment_target_tests.cpp)
target_link_libraries(gti_environment_target_tests PRIVATE gti_compiler)
add_test(NAME environment_target_capability COMMAND gti_environment_target_tests)

add_executable("gti_runtime_failure_header_smoke"
               "${PROJECT_SOURCE_DIR}/tests/runtime_failure_header_smoke.cpp")
target_compile_features("gti_runtime_failure_header_smoke" PRIVATE cxx_std_20)
target_link_libraries("gti_runtime_failure_header_smoke"
                      PRIVATE gti_compiler gti_runtime)
add_test(NAME runtime_failure_header_coexistence
         COMMAND gti_runtime_failure_header_smoke)

add_executable("gti_embedding_header_c_smoke"
               "${PROJECT_SOURCE_DIR}/tests/embedding_header_c_smoke.c")
target_include_directories(
  "gti_embedding_header_c_smoke"
  PRIVATE "${PROJECT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/runtime/include"
)
target_compile_features("gti_embedding_header_c_smoke" PRIVATE c_std_17)
add_test(NAME embedding_header_c17_oracle COMMAND gti_embedding_header_c_smoke)

add_executable("gti_embedding_header_cpp_smoke"
               "${PROJECT_SOURCE_DIR}/tests/embedding_header_cpp_smoke.cpp")
target_include_directories(
  "gti_embedding_header_cpp_smoke"
  PRIVATE "${PROJECT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/runtime/include"
)
target_compile_features("gti_embedding_header_cpp_smoke" PRIVATE cxx_std_20)
add_test(NAME embedding_header_cpp20_oracle
         COMMAND gti_embedding_header_cpp_smoke)

add_executable("gti_embedding_foundation_tests"
               "${PROJECT_SOURCE_DIR}/tests/embedding_foundation_tests.cpp")
target_link_libraries("gti_embedding_foundation_tests" PRIVATE gti_driver)
add_test(NAME embedding_foundation COMMAND gti_embedding_foundation_tests)

add_executable("gti_runtime_failure_helper"
               "${PROJECT_SOURCE_DIR}/tests/runtime_failure_helper.cpp")
target_link_libraries("gti_runtime_failure_helper"
                      PRIVATE gti_runtime Threads::Threads)

add_executable("gti_allocation_abort_helper"
               "${PROJECT_SOURCE_DIR}/tests/allocation_abort_helper.cpp")
target_link_libraries("gti_allocation_abort_helper"
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

add_executable("gti_llvm_backend_library_smoke"
               "${PROJECT_SOURCE_DIR}/tests/llvm_backend_library_smoke.cpp")
target_link_libraries("gti_llvm_backend_library_smoke"
                      PRIVATE gti_llvm_backend)
add_test(NAME llvm_backend_library_boundary
         COMMAND gti_llvm_backend_library_smoke)

add_executable("gti_driver_library_smoke" "${PROJECT_SOURCE_DIR}/tests/driver_library_smoke.cpp")
target_link_libraries("gti_driver_library_smoke" PRIVATE gti_driver)
add_test(NAME driver_library_boundary COMMAND gti_driver_library_smoke)

add_executable("gti_driver_tests" "${PROJECT_SOURCE_DIR}/tests/driver_tests.cpp")
target_link_libraries("gti_driver_tests" PRIVATE gti_driver)
add_test(NAME driver_pipeline COMMAND gti_driver_tests)

add_executable(gti_library_bundle_tests library_bundle_tests.cpp)
target_link_libraries(gti_library_bundle_tests PRIVATE gti_driver)
add_test(NAME library_bundle COMMAND gti_library_bundle_tests)
add_executable(gti_compiled_scalar_library_tests compiled_scalar_library_tests.cpp)
target_link_libraries(gti_compiled_scalar_library_tests PRIVATE gti_driver)
add_test(NAME compiled_scalar_library COMMAND gti_compiled_scalar_library_tests "$<TARGET_FILE:gti_runtime>" "${PROJECT_SOURCE_DIR}/examples/compiled-scalar-library")
add_test(NAME compiled_scalar_library_cost COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/compiled_scalar_cost_test.py"
  "$<TARGET_FILE:gti_compiled_scalar_library_tests>" "$<TARGET_FILE:gti_runtime>"
  "${PROJECT_SOURCE_DIR}/examples/compiled-scalar-library"
  --output "${CMAKE_BINARY_DIR}/compiled-scalar-cost.json")
add_executable(gti_compiled_primitive_cost_producer compiled_primitive_cost_producer.cpp)
target_link_libraries(gti_compiled_primitive_cost_producer PRIVATE gti_driver)
add_test(NAME compiled_primitive_cost
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_primitive_cost_test.py"
  "$<TARGET_FILE:gti_compiled_primitive_cost_producer>" "$<TARGET_FILE:gti_runtime>"
  --output "${CMAKE_BINARY_DIR}/compiled-primitive-cost.json")
set_tests_properties(compiled_primitive_cost PROPERTIES TIMEOUT 300)
add_test(NAME compiled_library_project
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_library_project_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-library-project")
set_tests_properties(compiled_library_project PROPERTIES TIMEOUT 600)
add_test(NAME compiled_libraries_project
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_libraries_project_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-libraries-project")
set_tests_properties(compiled_libraries_project PROPERTIES TIMEOUT 600)
add_test(NAME compiled_primitive_library
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_primitive_library_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-primitive-library")
set_tests_properties(compiled_primitive_library PROPERTIES TIMEOUT 600)
add_executable(gti_llvm_instrumented_source llvm_instrumented_source.cpp)
target_include_directories(gti_llvm_instrumented_source SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_instrumented_source PRIVATE gti_driver gti_llvm_backend)
add_executable(gti_llvm_cache_tests llvm_cache_tests.cpp)
target_link_libraries(gti_llvm_cache_tests PRIVATE gti_driver)
target_compile_definitions(gti_llvm_cache_tests PRIVATE
  GTI_LLVM_CACHE_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/llvm_cache")
add_test(NAME llvm_cache_round_trip COMMAND gti_llvm_cache_tests)
set_tests_properties(llvm_cache_round_trip PROPERTIES TIMEOUT 180)
add_test(NAME llvm_debug_artifacts
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/llvm_debug_artifacts_test.py" "$<TARGET_FILE:gti>")
set_tests_properties(llvm_debug_artifacts PROPERTIES TIMEOUT 240)
if(APPLE)
  add_test(NAME cpp_debug_artifacts
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_debug_artifacts_test.py"
            "$<TARGET_FILE:gti>" --backend cpp)
  set_tests_properties(cpp_debug_artifacts PROPERTIES TIMEOUT 240)
endif()
# Explicit matching-runtime evidence, separate from default/rejection coverage.
set(GTI_LLVM_RUNTIME_TEST_COMPILER "" CACHE FILEPATH
    "Matching Clang++ for the native runtime identity fixture")
if(GTI_LLVM_RUNTIME_TEST_COMPILER)
  add_test(NAME shared_ownership_instrumented
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/shared_ownership_instrumented_test.py"
            "$<TARGET_FILE:gti_llvm_instrumented_source>" "${PROJECT_SOURCE_DIR}"
            "${GTI_LLVM_RUNTIME_TEST_COMPILER}")
  set_tests_properties(shared_ownership_instrumented PROPERTIES TIMEOUT 600)
  add_test(NAME compact_owner_layout_instrumented
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compact_owner_layout_runtime_test.py"
            "$<TARGET_FILE:gti>" --instrumented-emitter "$<TARGET_FILE:gti_llvm_instrumented_source>"
            --clang "${GTI_LLVM_RUNTIME_TEST_COMPILER}")
  set_tests_properties(compact_owner_layout_instrumented PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_native_runtime_identity
    COMMAND gti_llvm_cache_tests --runtime-probe "${GTI_LLVM_RUNTIME_TEST_COMPILER}")
  set_tests_properties(llvm_native_runtime_identity PROPERTIES TIMEOUT 180)
endif()
add_test(NAME llvm_cache_component_mapping
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/llvm_cache_component_mapping_test.py"
          "${CMAKE_BINARY_DIR}" "${CMAKE_SYSTEM_PROCESSOR}")
set_tests_properties(llvm_cache_component_mapping PROPERTIES TIMEOUT 30)
add_test(NAME llvm_cache_project_workflows
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/llvm_cache_project_test.py" "$<TARGET_FILE:gti>")
set_tests_properties(llvm_cache_project_workflows PROPERTIES TIMEOUT 300)
add_test(NAME llvm_cache_installed_toolchain
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/llvm_cache_installed_test.py"
          "${PROJECT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
set_tests_properties(llvm_cache_installed_toolchain PROPERTIES TIMEOUT 600)
add_executable(gti_test_runner_tests test_runner_tests.cpp)
target_link_libraries(gti_test_runner_tests PRIVATE gti_driver)
add_test(NAME test_runner COMMAND gti_test_runner_tests)
set_tests_properties(test_runner PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
add_executable(gti_test_report_tests test_report_tests.cpp)
target_link_libraries(gti_test_report_tests PRIVATE gti_driver)
add_test(NAME test_report COMMAND "${Python3_EXECUTABLE}"
  "${PROJECT_SOURCE_DIR}/tests/test_report_test.py" "$<TARGET_FILE:gti_test_report_tests>")
set_tests_properties(test_report PROPERTIES TIMEOUT 30)
add_executable(gti_process_capture_failure_tests process_capture_failure_tests.cpp)
target_link_libraries(gti_process_capture_failure_tests PRIVATE gti_driver ${CMAKE_DL_LIBS})
add_test(NAME process_capture_failure COMMAND gti_process_capture_failure_tests)
set_tests_properties(process_capture_failure PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
add_executable(gti_process_group_identity_tests process_group_identity_tests.cpp)
target_link_libraries(gti_process_group_identity_tests PRIVATE gti_driver ${CMAKE_DL_LIBS})
add_test(NAME process_group_identity COMMAND gti_process_group_identity_tests)
set_tests_properties(process_group_identity PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
add_test(NAME source_debug_info
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_debug_info_test.py" $<TARGET_FILE:gti>)
set_tests_properties(source_debug_info PROPERTIES TIMEOUT 180)
find_program(GTI_LLDB_EXECUTABLE NAMES lldb)
if(GTI_LLDB_EXECUTABLE)
  add_test(NAME source_debug_lldb
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_debug_info_test.py" $<TARGET_FILE:gti> --lldb "${GTI_LLDB_EXECUTABLE}")
  set_tests_properties(source_debug_lldb PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

add_executable(gti_include_suggestions_tests
               "${PROJECT_SOURCE_DIR}/tests/include_suggestions_tests.cpp")
target_link_libraries(gti_include_suggestions_tests PRIVATE gti_driver)
add_test(NAME include_suggestions COMMAND gti_include_suggestions_tests)

add_executable(gti_executable_entry_tests
               "${PROJECT_SOURCE_DIR}/tests/executable_entry_tests.cpp")
target_link_libraries(gti_executable_entry_tests PRIVATE gti_driver)
add_test(NAME executable_entry_semantics COMMAND gti_executable_entry_tests)

add_executable("gti_project_tests" "${PROJECT_SOURCE_DIR}/tests/project_tests.cpp")
target_link_libraries("gti_project_tests" PRIVATE gti_driver)
add_test(NAME project_model COMMAND gti_project_tests)
add_executable(gti_registry_tests registry_tests.cpp)
target_link_libraries(gti_registry_tests PRIVATE gti_driver)
add_test(NAME package_registry_model COMMAND gti_registry_tests)
if(TARGET gti_lsp)
  add_test(NAME package_registry_cli
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/registry_cli_test.py" $<TARGET_FILE:gti> $<TARGET_FILE:gti_lsp>)
else()
  add_test(NAME package_registry_cli
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/registry_cli_test.py" $<TARGET_FILE:gti>)
endif()
set_tests_properties(package_registry_cli PROPERTIES TIMEOUT 240)

add_executable(gti_registry_auth_tests registry_auth_tests.cpp)
target_include_directories(gti_registry_auth_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/driver")
target_link_libraries(gti_registry_auth_tests PRIVATE gti_driver)
add_test(NAME package_registry_auth_model COMMAND gti_registry_auth_tests)
add_test(NAME package_registry_auth_cli
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/registry_auth_cli_test.py"
    $<TARGET_FILE:gti> $<TARGET_FILE:gti_registry_auth_tests>)
set_tests_properties(package_registry_auth_cli PROPERTIES TIMEOUT 90)

add_executable(gti_designated_construction_tests designated_construction_tests.cpp)
target_link_libraries(gti_designated_construction_tests PRIVATE gti_cpp_backend)
add_test(NAME designated_construction_pipeline COMMAND gti_designated_construction_tests)
add_test(NAME designated_construction_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/designated_construction_runtime_test.py" $<TARGET_FILE:gti>)
if(TARGET gti_lsp)
  add_test(NAME designated_construction_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/designated_construction_lsp_test.py" $<TARGET_FILE:gti_lsp>)
endif()

add_executable(gti_collection_initialization_tests collection_initialization_tests.cpp)
target_link_libraries(gti_collection_initialization_tests PRIVATE gti_cpp_backend)
add_test(NAME collection_initialization_pipeline COMMAND gti_collection_initialization_tests)
add_test(NAME collection_initialization_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/collection_initialization_runtime_test.py" $<TARGET_FILE:gti>)
if(TARGET gti_lsp)
  add_test(NAME collection_initialization_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/collection_initialization_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME member_requirement_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/member_requirement_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME source_modules_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_modules_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME module_discovery_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_discovery_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(module_discovery_lsp PROPERTIES TIMEOUT 120)
  add_test(NAME out_of_class_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/out_of_class_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(out_of_class_lsp PROPERTIES TIMEOUT 120)
  add_test(NAME nested_borrow_fields_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_borrow_fields_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME borrowing_composition_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/borrowing_composition_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME cleanup_bound_borrow_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cleanup_bound_borrow_lsp_test.py" $<TARGET_FILE:gti_lsp>)
endif()
add_test(NAME source_modules_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_modules_runtime_test.py" $<TARGET_FILE:gti>)
add_test(NAME module_discovery_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/module_discovery_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(module_discovery_runtime PROPERTIES TIMEOUT 180)
add_test(NAME out_of_class_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/out_of_class_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(out_of_class_runtime PROPERTIES TIMEOUT 600)
add_test(NAME out_of_class_cleanup
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/out_of_class_cleanup_test.py" $<TARGET_FILE:gti>)
set_tests_properties(out_of_class_cleanup PROPERTIES TIMEOUT 600)
add_test(NAME explicit_default_constructors
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/explicit_default_constructors_test.py" $<TARGET_FILE:gti>)
set_tests_properties(explicit_default_constructors PROPERTIES TIMEOUT 600)
if(GTI_BUILD_LSP)
  add_test(NAME explicit_default_constructors_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/explicit_default_constructors_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(explicit_default_constructors_lsp PROPERTIES TIMEOUT 120)
endif()
add_test(NAME out_of_class_performance
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/out_of_class_performance_test.py" $<TARGET_FILE:gti>
          "${PROJECT_BINARY_DIR}/Testing/out-of-class-performance")
set_tests_properties(out_of_class_performance PROPERTIES TIMEOUT 180)
add_executable(gti_borrowing_composition_tests borrowing_composition_tests.cpp)
target_link_libraries(gti_borrowing_composition_tests PRIVATE gti_cpp_backend)
add_test(NAME borrowing_composition_pipeline COMMAND gti_borrowing_composition_tests)
add_test(NAME shared_getter_worker_loans_runtime
  COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/shared_getter_worker_loans_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(shared_getter_worker_loans_runtime PROPERTIES TIMEOUT 600)
add_executable(gti_cleanup_bound_borrow_tests cleanup_bound_borrow_tests.cpp)
target_link_libraries(gti_cleanup_bound_borrow_tests PRIVATE gti_compiler)
add_test(NAME cleanup_bound_borrow COMMAND gti_cleanup_bound_borrow_tests)
add_executable(gti_cleanup_bound_borrow_mutation_tests
               cleanup_bound_borrow_mutation_tests.cpp)
target_link_libraries(gti_cleanup_bound_borrow_mutation_tests PRIVATE gti_compiler)
add_test(NAME cleanup_bound_borrow_mutations
         COMMAND gti_cleanup_bound_borrow_mutation_tests)
add_executable(gti_cleanup_bound_backend_mutation_tests
               cleanup_bound_backend_mutation_tests.cpp)
target_link_libraries(gti_cleanup_bound_backend_mutation_tests PRIVATE gti_cpp_backend)
add_test(NAME cleanup_bound_backend_mutations
         COMMAND gti_cleanup_bound_backend_mutation_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME process_resource_policy_harness
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/process_resource_policy_harness_test.py")
  add_test(NAME process_resource_policy
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/process_resource_policy_test.py"
                   $<TARGET_FILE:gti>)
  set_tests_properties(process_resource_policy PROPERTIES TIMEOUT 300)
  add_test(NAME cleanup_bound_borrow_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cleanup_bound_borrow_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(cleanup_bound_borrow_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME immovable_storage_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/immovable_storage_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(immovable_storage_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME source_storage_drain_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_storage_drain_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(source_storage_drain_runtime PROPERTIES TIMEOUT 240)
  add_test(NAME source_prefix_owner_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_prefix_owner_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(source_prefix_owner_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME source_resource_prefix_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_resource_prefix_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(source_resource_prefix_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME source_text_address_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_text_address_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(source_text_address_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME source_prefix_string_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/source_prefix_string_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(source_prefix_string_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME inherited_cleanup_context_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/inherited_cleanup_context_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(inherited_cleanup_context_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME cleanup_context_scope_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cleanup_context_scope_runtime_test.py"
      $<TARGET_FILE:gti> "${CMAKE_CXX_COMPILER}" $<TARGET_FILE:gti_runtime>
      $<TARGET_FILE:gti_runtime_thread_tests>)
  set_tests_properties(cleanup_context_scope_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME propagation_only_context_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/propagation_only_context_runtime_test.py"
      $<TARGET_FILE:gti>)
  set_tests_properties(propagation_only_context_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME many_fallible_scope_guards_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/many_fallible_scope_guards_runtime_test.py"
      $<TARGET_FILE:gti>)
  set_tests_properties(many_fallible_scope_guards_runtime PROPERTIES TIMEOUT 300)

  add_test(NAME object_destroy_scalar_runtime
           COMMAND ${Python3_EXECUTABLE}
                   "${PROJECT_SOURCE_DIR}/tests/object_destroy_scalar_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(object_destroy_scalar_runtime PROPERTIES TIMEOUT 240)
  add_test(NAME object_construction_runtime
           COMMAND ${Python3_EXECUTABLE}
                   "${PROJECT_SOURCE_DIR}/tests/object_construction_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(object_construction_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME uninitialized_carrier_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/uninitialized_carrier_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(uninitialized_carrier_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME object_relocation_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/object_relocation_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(object_relocation_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME programmable_allocators_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/programmable_allocators_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(programmable_allocators_runtime PROPERTIES TIMEOUT 900)
  add_test(NAME borrowed_carrier_return_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_carrier_return_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(borrowed_carrier_return_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME allocator_raw_audit_independence
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/allocator_raw_audit_independence_test.py" $<TARGET_FILE:gti>)
  # 36 compilations, two thirds of them auditing every trusted body: about
  # 280 s on an Apple M-series host and 674 s on an idle 8-vCPU Linux
  # runner. See COMPILE_TIMEOUT in the test.
  set_tests_properties(allocator_raw_audit_independence PROPERTIES TIMEOUT 2400)
endif()
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
add_executable(gti_constructor_formatting_tests constructor_formatting_tests.cpp)
target_link_libraries(gti_constructor_formatting_tests PRIVATE gti_compiler)
add_test(NAME constructor_formatting COMMAND gti_constructor_formatting_tests)
add_executable(gti_lambda_formatting_tests lambda_formatting_tests.cpp)
target_link_libraries(gti_lambda_formatting_tests PRIVATE gti_compiler)
add_test(NAME lambda_formatting COMMAND gti_lambda_formatting_tests)
add_executable(gti_integral_enum_tests integral_enum_tests.cpp)
target_link_libraries(gti_integral_enum_tests PRIVATE gti_cpp_backend)
add_test(NAME integral_enum COMMAND gti_integral_enum_tests)
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
  option(GTI_COMPILER_TEST_ISOLATE_CASES
         "Run each named compiler test in a fresh process" OFF)
  set(GTI_COMPILER_TEST_PROCESS_OPTIONS)
  if(GTI_COMPILER_TEST_ISOLATE_CASES)
    list(APPEND GTI_COMPILER_TEST_PROCESS_OPTIONS --isolate-tests)
  endif()
  add_test(
    NAME compiler_pipeline
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compiler_test_aggregate.py"
            ${GTI_COMPILER_TEST_PROCESS_OPTIONS}
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

add_executable("gti_polymorphic_storage_compiler_tests"
               "${PROJECT_SOURCE_DIR}/tests/polymorphic_storage_compiler_tests.cpp")
target_link_libraries("gti_polymorphic_storage_compiler_tests" PRIVATE gti_cpp_backend)
target_precompile_headers("gti_polymorphic_storage_compiler_tests"
                          REUSE_FROM gti_compiler_test_pch)
add_test(NAME polymorphic_storage_compiler_contract
         COMMAND gti_polymorphic_storage_compiler_tests)

add_executable(gti_prefix_constructor_tests prefix_constructor_tests.cpp)
target_link_libraries(gti_prefix_constructor_tests PRIVATE gti_cpp_backend)
add_test(NAME prefix_constructor_contract COMMAND gti_prefix_constructor_tests)
add_test(NAME vector_empty_constructor_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/vector_empty_constructor_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(vector_empty_constructor_runtime PROPERTIES TIMEOUT 240)
add_test(NAME vector_prepared_constructor_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/vector_prepared_constructor_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(vector_prepared_constructor_runtime PROPERTIES TIMEOUT 240)
add_test(NAME prefix_generated_transport_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/prefix_generated_transport_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(prefix_generated_transport_runtime PROPERTIES TIMEOUT 240)

add_executable(gti_carrier_shell_reference_tests carrier_shell_reference_tests.cpp)
target_link_libraries(gti_carrier_shell_reference_tests PRIVATE gti_cpp_backend)
target_precompile_headers(gti_carrier_shell_reference_tests
                          REUSE_FROM gti_compiler_test_pch)
add_test(NAME carrier_shell_references COMMAND gti_carrier_shell_reference_tests)
add_test(NAME carrier_reference_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/carrier_reference_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(carrier_reference_runtime PROPERTIES TIMEOUT 180)
add_test(NAME shared_accessor_range_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/shared_accessor_range_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(shared_accessor_range_runtime PROPERTIES TIMEOUT 240)
add_test(NAME accessor_text_carrier_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/accessor_text_carrier_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(accessor_text_carrier_runtime PROPERTIES TIMEOUT 240)

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
add_executable(gti_optimizer_raw_context_tests optimizer_raw_context_tests.cpp)
target_link_libraries(gti_optimizer_raw_context_tests PRIVATE gti_compiler)
add_test(NAME optimizer_raw_context COMMAND gti_optimizer_raw_context_tests)
add_test(NAME returned_field_address_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/returned_field_address_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(returned_field_address_runtime PROPERTIES TIMEOUT 1200)
add_test(NAME optimizer_terminal_context_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/optimizer_terminal_context_runtime_test.py"
    "$<TARGET_FILE:gti>")
set_tests_properties(optimizer_terminal_context_runtime PROPERTIES TIMEOUT 300)
add_test(NAME wrapping_context_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/wrapping_context_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(wrapping_context_runtime PROPERTIES TIMEOUT 300)
add_executable(gti_optimizer_intrinsic_context_tests optimizer_intrinsic_context_tests.cpp)
target_link_libraries(gti_optimizer_intrinsic_context_tests PRIVATE gti_compiler)
add_test(NAME optimizer_intrinsic_context COMMAND gti_optimizer_intrinsic_context_tests)
add_executable(gti_optimizer_object_context_tests optimizer_object_context_tests.cpp)
target_link_libraries(gti_optimizer_object_context_tests PRIVATE gti_compiler)
add_test(NAME optimizer_object_context COMMAND gti_optimizer_object_context_tests)
add_test(NAME optimizer_object_context_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/optimizer_object_context_runtime_test.py"
    $<TARGET_FILE:gti>)
# Keep the full 57-fixture/eight-mode matrix: all 456 compile/run cases.
# GNU 15.2 Linux completed 435 cases at the old 300s aggregate bound. Allow
# its sanitized tail without changing per-case compile/execution deadlines.
set_tests_properties(optimizer_object_context_runtime PROPERTIES TIMEOUT 600)
add_test(NAME optimizer_raw_context_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/optimizer_raw_context_runtime_test.py"
    $<TARGET_FILE:gti>)
set_tests_properties(optimizer_raw_context_runtime PROPERTIES TIMEOUT 300)

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
add_test(NAME optimizer_devirtualization_parameter_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/optimizer_devirtualization_runtime_test.py"
                 "$<TARGET_FILE:gti>"
                 "${PROJECT_SOURCE_DIR}/tests/fixtures/optimizer_exact_parameter_receiver.gti"
                 "${CMAKE_CXX_COMPILER}"
                 "exact-parameter-receiver")
set_tests_properties(optimizer_devirtualization_parameter_runtime
                     PROPERTIES TIMEOUT 180)
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
add_test(NAME cpp_identifier_relocation
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/cpp_identifier_relocation_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(cpp_identifier_relocation PROPERTIES TIMEOUT 300)

add_executable("gti_mir_shared_failure_cleanup_tests"
               "${PROJECT_SOURCE_DIR}/tests/mir_shared_failure_cleanup_tests.cpp")
target_link_libraries("gti_mir_shared_failure_cleanup_tests" PRIVATE gti_compiler)
add_test(NAME mir_shared_failure_cleanup
         COMMAND gti_mir_shared_failure_cleanup_tests)

add_executable(gti_mir_thread_local_tests mir_thread_local_tests.cpp)
target_link_libraries(gti_mir_thread_local_tests PRIVATE gti_compiler)
add_test(NAME mir_thread_local COMMAND gti_mir_thread_local_tests)
add_executable(gti_mir_persistent_storage_tests mir_persistent_storage_tests.cpp)
target_link_libraries(gti_mir_persistent_storage_tests PRIVATE gti_compiler)
add_test(NAME mir_persistent_storage COMMAND gti_mir_persistent_storage_tests)

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
add_executable(gti_raw_borrow_tests "${PROJECT_SOURCE_DIR}/tests/raw_borrow_tests.cpp")
target_link_libraries(gti_raw_borrow_tests PRIVATE gti_cpp_backend)
add_test(NAME raw_borrow_contract COMMAND gti_raw_borrow_tests)
add_executable(gti_body_analysis_order_tests "${PROJECT_SOURCE_DIR}/tests/body_analysis_order_tests.cpp")
target_link_libraries(gti_body_analysis_order_tests PRIVATE gti_compiler)
add_test(NAME body_analysis_order COMMAND gti_body_analysis_order_tests)
add_executable(gti_trusted_body_analysis_tests "${PROJECT_SOURCE_DIR}/tests/trusted_body_analysis_tests.cpp")
target_link_libraries(gti_trusted_body_analysis_tests PRIVATE gti_compiler)
# Each entry has independent complete/reachable and preamble-cache comparisons.
# Keep all entries and fixture checks, but do not put the entire growing corpus
# under one platform-dependent aggregate deadline.
file(GLOB gti_trusted_body_entries CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/examples/*.gti"
  "${PROJECT_SOURCE_DIR}/stdlib/tests/*.gti")
list(SORT gti_trusted_body_entries)
foreach(gti_trusted_partition RANGE 0 3)
  set(gti_trusted_partition_entries)
  set(gti_trusted_entry_index 0)
  foreach(gti_trusted_entry IN LISTS gti_trusted_body_entries)
    math(EXPR gti_trusted_entry_partition "${gti_trusted_entry_index} % 4")
    if(gti_trusted_entry_partition EQUAL gti_trusted_partition)
      list(APPEND gti_trusted_partition_entries "${gti_trusted_entry}")
    endif()
    math(EXPR gti_trusted_entry_index "${gti_trusted_entry_index} + 1")
  endforeach()
  if(NOT gti_trusted_partition_entries)
    message(FATAL_ERROR "trusted-body analysis partition is empty")
  endif()
  add_test(NAME trusted_body_analysis_${gti_trusted_partition}
    COMMAND "${CMAKE_COMMAND}" -E env $<TARGET_FILE:gti_trusted_body_analysis_tests>
            "${PROJECT_SOURCE_DIR}"
            ${gti_trusted_partition_entries})
  set_tests_properties(trusted_body_analysis_${gti_trusted_partition}
    PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()
add_executable(gti_instance_reuse_tests "${PROJECT_SOURCE_DIR}/tests/instance_reuse_tests.cpp")
target_link_libraries(gti_instance_reuse_tests PRIVATE gti_compiler)
add_test(NAME instance_reuse COMMAND gti_instance_reuse_tests "${PROJECT_SOURCE_DIR}")
set_tests_properties(instance_reuse PROPERTIES TIMEOUT 900)
add_executable(gti_reflection_tests "${PROJECT_SOURCE_DIR}/tests/reflection_tests.cpp")
target_link_libraries(gti_reflection_tests PRIVATE gti_compiler)
add_test(NAME reflection_semantics COMMAND gti_reflection_tests)
add_test(NAME reflection_method_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/reflection_method_runtime_test.py"
          "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(reflection_method_runtime PROPERTIES TIMEOUT 300)
if(TARGET gti_lsp)
  add_test(NAME reflection_method_lsp
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/reflection_method_lsp_test.py" "$<TARGET_FILE:gti_lsp>")
  set_tests_properties(reflection_method_lsp PROPERTIES TIMEOUT 60)
endif()
add_executable(gti_general_requires_tests general_requires_tests.cpp)
target_link_libraries(gti_general_requires_tests PRIVATE gti_compiler)
add_test(NAME general_requires_semantics
  COMMAND gti_general_requires_tests "${PROJECT_SOURCE_DIR}")
set_tests_properties(general_requires_semantics PROPERTIES TIMEOUT 120)
add_test(NAME general_requires_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/examples/173-general-requires.gti" --include-o1)
set_tests_properties(general_requires_runtime PROPERTIES TIMEOUT 180)
add_executable(gti_callable_return_type_tests "${PROJECT_SOURCE_DIR}/tests/callable_return_type_tests.cpp")
target_link_libraries(gti_callable_return_type_tests PRIVATE gti_cpp_backend)
add_test(NAME callable_return_type COMMAND gti_callable_return_type_tests)
add_executable(gti_class_argument_profile_tests "${PROJECT_SOURCE_DIR}/tests/class_argument_profile_tests.cpp")
target_link_libraries(gti_class_argument_profile_tests PRIVATE gti_cpp_backend)
add_test(NAME class_argument_profiles COMMAND gti_class_argument_profile_tests)
add_test(NAME class_argument_profiles_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generic_symbolic_auto_local_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/examples/168-source-value-profiles.gti" --include-o1)
set_tests_properties(class_argument_profiles_runtime PROPERTIES TIMEOUT 180)
add_test(NAME empty_return_construction_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/empty_return_construction_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(empty_return_construction_runtime PROPERTIES TIMEOUT 180)
add_test(NAME source_expected_production_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/source_expected_production.gti" --ownership-checks)
set_tests_properties(source_expected_production_runtime PROPERTIES TIMEOUT 300)
add_test(NAME source_expected_example_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/examples/169-source-expected.gti" --ownership-checks)
set_tests_properties(source_expected_example_runtime PROPERTIES TIMEOUT 300)
add_test(NAME global_borrow_carriers_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/global_borrow_carriers.gti" --ownership-checks)
set_tests_properties(global_borrow_carriers_runtime PROPERTIES TIMEOUT 300)
add_test(NAME readonly_range_over_mutable_result_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/readonly_range_over_mutable_result.gti" --ownership-checks)
set_tests_properties(readonly_range_over_mutable_result_runtime PROPERTIES TIMEOUT 300)
add_test(NAME parameter_payload_reference_returns_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/parameter_payload_reference_returns.gti" --ownership-checks)
set_tests_properties(parameter_payload_reference_returns_runtime PROPERTIES TIMEOUT 300)
add_test(NAME raw_pointer_slot_references_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/raw_pointer_slot_references.gti" --ownership-checks)
set_tests_properties(raw_pointer_slot_references_runtime PROPERTIES TIMEOUT 300)
add_test(NAME use_based_accessor_selection_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/use_based_accessor_selection.gti" --ownership-checks)
set_tests_properties(use_based_accessor_selection_runtime PROPERTIES TIMEOUT 300)
add_test(NAME reflected_contextual_construction_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/reflected_contextual_construction.gti" --ownership-checks)
set_tests_properties(reflected_contextual_construction_runtime PROPERTIES TIMEOUT 300)
add_test(NAME callable_return_type_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/callable_return_type_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(callable_return_type_runtime PROPERTIES TIMEOUT 300)
if(TARGET gti_lsp)
  add_test(NAME callable_return_type_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/callable_return_type_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(callable_return_type_lsp PROPERTIES TIMEOUT 60)
endif()
add_executable(gti_raw_access_tests "${PROJECT_SOURCE_DIR}/tests/raw_access_tests.cpp")
target_link_libraries(gti_raw_access_tests PRIVATE gti_compiler)
add_test(NAME raw_access_contract COMMAND gti_raw_access_tests)
add_executable(gti_raw_audit_certainty_tests "${PROJECT_SOURCE_DIR}/tests/raw_audit_certainty_tests.cpp")
target_link_libraries(gti_raw_audit_certainty_tests PRIVATE gti_compiler)
add_test(NAME raw_audit_certainty COMMAND gti_raw_audit_certainty_tests)
add_executable(gti_raw_query_budget_tests "${PROJECT_SOURCE_DIR}/tests/raw_query_budget_tests.cpp")
target_link_libraries(gti_raw_query_budget_tests PRIVATE gti_compiler)
add_test(NAME raw_query_budget_contract COMMAND gti_raw_query_budget_tests)
set_tests_properties(raw_query_budget_contract PROPERTIES TIMEOUT 240)
add_executable(gti_raw_pointer_conversion_tests "${PROJECT_SOURCE_DIR}/tests/raw_pointer_conversion_tests.cpp")
target_link_libraries(gti_raw_pointer_conversion_tests PRIVATE gti_cpp_backend)
add_test(NAME raw_pointer_conversion_contract COMMAND gti_raw_pointer_conversion_tests)
add_executable(gti_borrow_origin_syntax_tests "${PROJECT_SOURCE_DIR}/tests/borrow_origin_syntax_tests.cpp")
target_link_libraries(gti_borrow_origin_syntax_tests PRIVATE gti_compiler)
add_test(NAME borrow_origin_syntax COMMAND gti_borrow_origin_syntax_tests)

add_executable(gti_discardable_result_tests "${PROJECT_SOURCE_DIR}/tests/discardable_result_tests.cpp")
target_link_libraries(gti_discardable_result_tests PRIVATE gti_compiler)
add_test(NAME discardable_result COMMAND gti_discardable_result_tests)
add_test(NAME stdlib_discardable
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/stdlib_discardable_test.py"
          "$<TARGET_FILE:gti>")
set_tests_properties(stdlib_discardable PROPERTIES TIMEOUT 300)
if(TARGET gti_lsp)
  add_test(NAME aggregate_std_lsp
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aggregate_std_lsp_test.py"
            $<TARGET_FILE:gti_lsp>)
  set_tests_properties(aggregate_std_lsp PROPERTIES TIMEOUT 120)
  add_test(NAME raw_pointer_audit_policy
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_pointer_audit_policy_test.py" $<TARGET_FILE:gti> $<TARGET_FILE:gti_lsp>)
  set_tests_properties(raw_pointer_audit_policy PROPERTIES TIMEOUT 240)
  add_test(NAME raw_pointer_conversion_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_pointer_conversion_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME raw_pointer_provenance_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_pointer_provenance_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  add_test(NAME nested_raw_pointer_lsp
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_raw_pointer_lsp_test.py" $<TARGET_FILE:gti_lsp>)
endif()

add_executable("gti_layout_query_tests" "${PROJECT_SOURCE_DIR}/tests/layout_query_tests.cpp")
target_link_libraries("gti_layout_query_tests" PRIVATE gti_cpp_backend)
add_test(NAME layout_query_pipeline COMMAND gti_layout_query_tests)

add_executable("gti_bindgen_tests" "${PROJECT_SOURCE_DIR}/tests/bindgen_tests.cpp")
target_link_libraries("gti_bindgen_tests" PRIVATE gti_bindgen gti_compiler)
add_test(NAME bindgen_pipeline COMMAND gti_bindgen_tests)
add_test(NAME native_enum_bindgen
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_bindgen_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(native_enum_bindgen PROPERTIES TIMEOUT 300)
add_test(NAME native_typedef_bindgen
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_bindgen_runtime_test.py" $<TARGET_FILE:gti> --example native-callback-slots)
set_tests_properties(native_typedef_bindgen PROPERTIES TIMEOUT 300)
add_test(NAME native_union_bindgen
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_bindgen_runtime_test.py" $<TARGET_FILE:gti> --example native-union-bindings)
set_tests_properties(native_union_bindgen PROPERTIES TIMEOUT 300)
add_test(NAME native_array_index_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_array_index_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(native_array_index_runtime PROPERTIES TIMEOUT 300)

add_executable("gti_native_record_tests" "${PROJECT_SOURCE_DIR}/tests/native_record_tests.cpp")
target_link_libraries("gti_native_record_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_record_pipeline COMMAND gti_native_record_tests)
add_executable(gti_call_result_publication_tests call_result_publication_tests.cpp)
target_link_libraries(gti_call_result_publication_tests PRIVATE gti_cpp_backend)
add_test(NAME call_result_publication COMMAND gti_call_result_publication_tests)
add_test(NAME call_result_publication_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/call_result_publication_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(call_result_publication_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_class_return_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_class_return_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_class_return_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_scoped_return_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_scoped_return_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_scoped_return_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_class_argument_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_class_argument_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_class_argument_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_cleanup_free_copy_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_cleanup_free_copy_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_cleanup_free_copy_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_cleanup_free_move_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_cleanup_free_move_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_cleanup_free_move_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_lambda_class_argument_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_lambda_class_argument_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_lambda_class_argument_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_call_reassignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_call_reassignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_call_reassignment_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_member_argument_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_member_argument_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_member_argument_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_constructor_return_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_constructor_return_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_constructor_return_runtime PROPERTIES TIMEOUT 240)
add_test(NAME mixed_constructor_parameter_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/mixed_constructor_parameter_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(mixed_constructor_parameter_runtime PROPERTIES TIMEOUT 240)
add_test(NAME moved_passive_constructor_field_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/moved_passive_constructor_field_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(moved_passive_constructor_field_runtime PROPERTIES TIMEOUT 240)
add_test(NAME deferred_bound_field_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/deferred_bound_field_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(deferred_bound_field_runtime PROPERTIES TIMEOUT 240)
add_test(NAME passive_text_view_class_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/passive_text_view_class_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(passive_text_view_class_runtime PROPERTIES TIMEOUT 240)
add_test(NAME passive_function_item_class_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/passive_function_item_class_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(passive_function_item_class_runtime PROPERTIES TIMEOUT 240)
add_test(NAME passive_class_move_and_copy_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/passive_class_move_and_copy_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(passive_class_move_and_copy_runtime PROPERTIES TIMEOUT 240)
add_test(NAME view_fields_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/view_fields_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(view_fields_runtime PROPERTIES TIMEOUT 240)
add_test(NAME text_reference_accessors_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/text_reference_accessors_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(text_reference_accessors_runtime PROPERTIES TIMEOUT 240)
add_test(NAME text_storage_provenance_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/text_storage_provenance_runtime_test.py"
                 $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
set_tests_properties(text_storage_provenance_runtime PROPERTIES TIMEOUT 300)
add_test(NAME formal_text_field_relays_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/formal_text_field_relays_test.py"
                 $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
set_tests_properties(formal_text_field_relays_runtime PROPERTIES TIMEOUT 300)
add_test(NAME projected_reference_bindings_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/projected_reference_bindings_test.py"
                 $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
set_tests_properties(projected_reference_bindings_runtime PROPERTIES TIMEOUT 300)

add_test(NAME alias_construction_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/alias_construction_runtime_test.py"
                 "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
set_tests_properties(alias_construction_runtime PROPERTIES TIMEOUT 300)
add_test(NAME view_field_payloads_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/view_field_payloads_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(view_field_payloads_runtime PROPERTIES TIMEOUT 240)
add_test(NAME view_field_conversions_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/view_field_conversions_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(view_field_conversions_runtime PROPERTIES TIMEOUT 240)
add_test(NAME dependent_reference_results_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/dependent_reference_results_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(dependent_reference_results_runtime PROPERTIES TIMEOUT 240)
add_test(NAME view_field_nesting_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/view_field_nesting_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(view_field_nesting_runtime PROPERTIES TIMEOUT 240)
add_test(NAME recursive_text_relays_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/recursive_text_relays_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(recursive_text_relays_runtime PROPERTIES TIMEOUT 240)
add_test(NAME failure_free_passive_moves_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/failure_free_passive_moves_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(failure_free_passive_moves_runtime PROPERTIES TIMEOUT 240)
add_test(NAME failure_free_passive_constructors_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/failure_free_passive_constructors_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(failure_free_passive_constructors_runtime PROPERTIES TIMEOUT 240)
add_test(NAME constructor_copy_after_formal_write_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/constructor_copy_after_formal_write_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(constructor_copy_after_formal_write_runtime PROPERTIES TIMEOUT 240)
add_test(NAME assignment_result_projection_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/assignment_result_projection_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(assignment_result_projection_runtime PROPERTIES TIMEOUT 240)
add_test(NAME class_store_destinations_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/class_store_destinations_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(class_store_destinations_runtime PROPERTIES TIMEOUT 240)
add_test(NAME array_field_constructor_elements_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/array_field_constructor_elements_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(array_field_constructor_elements_runtime PROPERTIES TIMEOUT 240)
add_test(NAME assignment_result_whole_value_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/assignment_result_whole_value_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(assignment_result_whole_value_runtime PROPERTIES TIMEOUT 240)
add_test(NAME declaration_owned_defaults_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/declaration_owned_defaults_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(declaration_owned_defaults_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_consuming_return_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_consuming_return_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_consuming_return_runtime PROPERTIES TIMEOUT 240)
add_test(NAME base_rollback_record_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/base_rollback_record_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(base_rollback_record_runtime PROPERTIES TIMEOUT 240)
add_test(NAME virtual_expected_owning_return_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/virtual_expected_owning_return_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(virtual_expected_owning_return_runtime PROPERTIES TIMEOUT 240)
add_test(NAME reference_owned_field_replacement_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/reference_owned_field_replacement_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(reference_owned_field_replacement_runtime PROPERTIES TIMEOUT 240)
add_test(NAME payload_enum_reference_assignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/payload_enum_reference_assignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(payload_enum_reference_assignment_runtime PROPERTIES TIMEOUT 240)
add_test(NAME raw_owned_field_replacement_runtime
         COMMAND ${Python3_EXECUTABLE}
                 "${PROJECT_SOURCE_DIR}/tests/raw_owned_field_replacement_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(raw_owned_field_replacement_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_temporary_observation_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_temporary_observation_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_temporary_observation_runtime PROPERTIES TIMEOUT 240)
add_test(NAME owning_temporary_field_read_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/owning_temporary_field_read_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(owning_temporary_field_read_runtime PROPERTIES TIMEOUT 240)
add_test(NAME construction_assignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/construction_assignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(construction_assignment_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_field_and_join_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_field_and_join_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_field_and_join_runtime PROPERTIES TIMEOUT 240)
add_test(NAME owned_argument_member_initializer_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/owned_argument_member_initializer_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(owned_argument_member_initializer_runtime PROPERTIES TIMEOUT 240)
add_test(NAME owning_expected_field_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/owning_expected_field_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(owning_expected_field_runtime PROPERTIES TIMEOUT 240)
add_test(NAME comma_class_results_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/comma_class_results_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(comma_class_results_runtime PROPERTIES TIMEOUT 240)
add_test(NAME plain_call_member_initializer_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/plain_call_member_initializer_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(plain_call_member_initializer_runtime PROPERTIES TIMEOUT 240)
add_test(NAME class_conditional_join_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/class_conditional_join_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(class_conditional_join_runtime PROPERTIES TIMEOUT 240)
add_test(NAME short_circuit_field_view_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/short_circuit_field_view_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(short_circuit_field_view_runtime PROPERTIES TIMEOUT 240)
add_test(NAME owning_conditional_join_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/owning_conditional_join_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(owning_conditional_join_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_payload_short_circuit_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_payload_short_circuit_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_payload_short_circuit_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_class_assignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_class_assignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_class_assignment_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_conditional_assignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_conditional_assignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_conditional_assignment_runtime PROPERTIES TIMEOUT 240)
add_test(NAME expected_conditional_arms_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/expected_conditional_arms_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(expected_conditional_arms_runtime PROPERTIES TIMEOUT 240)
add_test(NAME borrowed_callable_conditionals_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/borrowed_callable_conditionals_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(borrowed_callable_conditionals_runtime PROPERTIES TIMEOUT 240)
add_test(NAME aggregate_std_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/aggregate_std_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(aggregate_std_runtime PROPERTIES TIMEOUT 240)
if(TARGET gti_lsp)
  add_test(NAME borrowed_callable_conditionals_lsp
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_callable_conditionals_lsp_test.py"
            $<TARGET_FILE:gti_lsp>)
  set_tests_properties(borrowed_callable_conditionals_lsp PROPERTIES TIMEOUT 120)
  add_test(NAME borrowing_callbacks_lsp
    COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/tests/borrowing_callbacks_lsp_test.py"
            $<TARGET_FILE:gti_lsp>)
  set_tests_properties(borrowing_callbacks_lsp PROPERTIES TIMEOUT 120)
endif()
add_test(NAME consuming_call_assignment_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/consuming_call_assignment_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(consuming_call_assignment_runtime PROPERTIES TIMEOUT 240)

add_executable(gti_initialization_and_native_identity_tests
  "${PROJECT_SOURCE_DIR}/tests/initialization_and_native_identity_tests.cpp")
target_link_libraries(gti_initialization_and_native_identity_tests PRIVATE gti_cpp_backend)
add_test(NAME initialization_and_native_identity COMMAND gti_initialization_and_native_identity_tests)
if(TARGET gti_lsp)
  add_test(NAME implicit_return_construction_lsp
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/implicit_return_construction_lsp_test.py" $<TARGET_FILE:gti_lsp>)
  set_tests_properties(implicit_return_construction_lsp PROPERTIES TIMEOUT 60)
endif()
add_executable(gti_conversion_operator_tests
  "${PROJECT_SOURCE_DIR}/tests/conversion_operator_tests.cpp")
target_link_libraries(gti_conversion_operator_tests PRIVATE gti_cpp_backend)
add_test(NAME conversion_operators COMMAND gti_conversion_operator_tests)
add_executable(gti_protocol_concept_tests
  "${PROJECT_SOURCE_DIR}/tests/protocol_concept_tests.cpp")
target_link_libraries(gti_protocol_concept_tests PRIVATE gti_cpp_backend)
add_test(NAME protocol_concepts COMMAND gti_protocol_concept_tests)
if(TARGET gti_lsp)
  add_test(NAME conformance_header_lsp
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/conformance_header_lsp_test.py"
                   "$<TARGET_FILE:gti_lsp>")
  set_tests_properties(conformance_header_lsp PROPERTIES TIMEOUT 120)
endif()
add_executable(gti_tryable_propagation_tests
  "${PROJECT_SOURCE_DIR}/tests/tryable_propagation_tests.cpp")
target_link_libraries(gti_tryable_propagation_tests PRIVATE gti_cpp_backend)
add_test(NAME tryable_propagation COMMAND gti_tryable_propagation_tests)
add_executable(gti_complete_borrow_summary_tests complete_borrow_summary_tests.cpp)
target_link_libraries(gti_complete_borrow_summary_tests PRIVATE gti_compiler)
add_test(NAME complete_borrow_summaries COMMAND gti_complete_borrow_summary_tests)
add_executable(gti_ownerless_text_contracts_tests ownerless_text_contracts_tests.cpp)
target_link_libraries(gti_ownerless_text_contracts_tests PRIVATE gti_compiler)
add_test(NAME ownerless_text_contracts COMMAND gti_ownerless_text_contracts_tests)
add_executable(gti_view_field_tests view_field_tests.cpp)
target_link_libraries(gti_view_field_tests PRIVATE gti_compiler)
add_test(NAME view_fields COMMAND gti_view_field_tests)
add_executable(gti_reference_payload_semantic_tests reference_payload_semantic_tests.cpp)
target_link_libraries(gti_reference_payload_semantic_tests PRIVATE gti_compiler)
add_test(NAME reference_payload_semantics COMMAND gti_reference_payload_semantic_tests)
add_test(NAME constructor_null_native_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/constructor_null_native_runtime_test.py" $<TARGET_FILE:gti>)
add_test(NAME payload_fresh_class_operands_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/payload_fresh_class_operands_runtime_test.py"
          $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}/tests/fixtures/payload_fresh_class_operands.gti")
set_tests_properties(constructor_null_native_runtime PROPERTIES TIMEOUT 300)

add_executable("gti_native_header_tests" "${PROJECT_SOURCE_DIR}/tests/native_header_tests.cpp")
target_link_libraries("gti_native_header_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_header_pipeline COMMAND gti_native_header_tests)

add_executable("gti_native_callback_tests"
               "${PROJECT_SOURCE_DIR}/tests/native_callback_tests.cpp")
target_link_libraries("gti_native_callback_tests" PRIVATE gti_cpp_backend)
add_test(NAME native_callback_pipeline COMMAND gti_native_callback_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME native_indirect_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/native_indirect_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(native_indirect_runtime PROPERTIES TIMEOUT 900)
endif()

add_executable(gti_cthread_tests "${PROJECT_SOURCE_DIR}/tests/cthread_tests.cpp")
target_link_libraries(gti_cthread_tests PRIVATE gti_cpp_backend)
add_test(NAME cthread_semantic_ir COMMAND gti_cthread_tests)

add_executable(gti_unsafe_function_tests
               "${PROJECT_SOURCE_DIR}/tests/unsafe_function_tests.cpp")
target_link_libraries(gti_unsafe_function_tests PRIVATE gti_cpp_backend)
add_test(NAME unsafe_function_contract COMMAND gti_unsafe_function_tests)

add_executable(gti_unsafe_expression_tests unsafe_expression_tests.cpp)
target_link_libraries(gti_unsafe_expression_tests PRIVATE gti_cpp_backend)
add_test(NAME unsafe_expression_contract COMMAND gti_unsafe_expression_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME unsafe_expression_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unsafe_expression_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(unsafe_expression_runtime PROPERTIES TIMEOUT 900)
endif()

add_executable(gti_runtime_object_layout_tests runtime_object_layout_tests.cpp)
target_link_libraries(gti_runtime_object_layout_tests PRIVATE gti_cpp_backend)
add_test(NAME runtime_object_layout_contract COMMAND gti_runtime_object_layout_tests)

add_executable("gti_managed_thread_tests"
               "${PROJECT_SOURCE_DIR}/tests/managed_thread_tests.cpp")
target_link_libraries("gti_managed_thread_tests" PRIVATE gti_cpp_backend)
add_test(NAME managed_thread_semantic_ir COMMAND gti_managed_thread_tests)

add_executable(gti_scoped_thread_semantic_tests scoped_thread_semantic_tests.cpp)
target_link_libraries(gti_scoped_thread_semantic_tests PRIVATE gti_cpp_backend)
add_test(NAME scoped_thread_semantic_shapes COMMAND gti_scoped_thread_semantic_tests)

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
add_executable(gti_borrowing_capture_tests borrowing_capture_tests.cpp)
target_link_libraries(gti_borrowing_capture_tests PRIVATE gti_cpp_backend)
add_test(NAME borrowing_capture_semantics COMMAND gti_borrowing_capture_tests)
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
add_test(NAME mutex_shell_recipes COMMAND gti_mutex_tests --shell-recipes)

add_executable(gti_source_location_tests source_location_tests.cpp)
target_link_libraries(gti_source_location_tests PRIVATE gti_cpp_backend)
add_test(NAME source_location_semantic_ir COMMAND gti_source_location_tests)

add_executable(gti_simd_tests simd_tests.cpp)
target_link_libraries(gti_simd_tests PRIVATE gti_cpp_backend)
add_test(NAME simd_semantic_ir COMMAND gti_simd_tests)
add_test(NAME simd_runtime
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/simd_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(simd_runtime PROPERTIES TIMEOUT 600 PROCESSORS 2)

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
add_test(NAME payload_borrow_chains_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/payload_borrow_chains_runtime_test.py"
                 "$<TARGET_FILE:gti>"
                 "${PROJECT_SOURCE_DIR}/examples/184-payload-borrow-chains.gti")
set_tests_properties(payload_borrow_chains_runtime PROPERTIES TIMEOUT 120)

add_executable("gti_mir_backend_tests" "${PROJECT_SOURCE_DIR}/tests/mir_backend_tests.cpp")
target_link_libraries("gti_mir_backend_tests" PRIVATE gti_cpp_backend)
add_test(NAME mir_backend_first_family COMMAND gti_mir_backend_tests)

add_executable("gti_lowered_program_tests"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_tests.cpp"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_contract_client.cpp")
target_link_libraries("gti_lowered_program_tests" PRIVATE gti_compiler)
add_test(NAME lowered_program_contract COMMAND gti_lowered_program_tests)
add_executable(gti_llvm_dispatch_plan_tests llvm_dispatch_plan_tests.cpp)
target_link_libraries(gti_llvm_dispatch_plan_tests PRIVATE gti_compiler)
add_test(NAME llvm_dispatch_plans COMMAND gti_llvm_dispatch_plan_tests)
set_tests_properties(llvm_dispatch_plans PROPERTIES TIMEOUT 120)
add_executable(gti_field_symbol_identity_tests field_symbol_identity_tests.cpp)
target_link_libraries(gti_field_symbol_identity_tests PRIVATE gti_compiler)
add_test(NAME field_symbol_identity COMMAND gti_field_symbol_identity_tests)
add_executable(gti_constructor_transport_tests constructor_transport_tests.cpp)
target_link_libraries(gti_constructor_transport_tests PRIVATE gti_compiler)
add_test(NAME constructor_transport COMMAND gti_constructor_transport_tests)
add_test(NAME generic_callable_transport_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generic_callable_transport_runtime_test.py" "$<TARGET_FILE:gti>")
set_tests_properties(generic_callable_transport_runtime PROPERTIES TIMEOUT 360)
add_test(NAME generic_branch_locals_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/generic_branch_locals_runtime_test.py"
                 "$<TARGET_FILE:gti>")
add_executable(gti_generic_initialization_tests generic_initialization_tests.cpp)
target_link_libraries(gti_generic_initialization_tests PRIVATE gti_compiler)
add_test(NAME generic_initialization COMMAND gti_generic_initialization_tests)
add_executable(gti_generic_layout_tests generic_layout_tests.cpp)
target_link_libraries(gti_generic_layout_tests PRIVATE gti_compiler)
add_test(NAME generic_layout COMMAND gti_generic_layout_tests)
add_test(NAME generic_instance_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/generic_instance_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(generic_instance_runtime PROPERTIES TIMEOUT 600)

add_executable("gti_lowered_program_census"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_census.cpp"
               "${PROJECT_SOURCE_DIR}/tests/lowered_program_contract_client.cpp")
target_link_libraries("gti_lowered_program_census" PRIVATE gti_compiler)

add_executable("gti_backend_lowered_program_rejection_tests"
               "${PROJECT_SOURCE_DIR}/tests/backend_lowered_program_rejection_tests.cpp")
target_link_libraries("gti_backend_lowered_program_rejection_tests"
                      PRIVATE gti_cpp_backend gti_llvm_backend)
add_test(NAME backend_lowered_program_rejection
         COMMAND gti_backend_lowered_program_rejection_tests)

add_executable("gti_llvm_backend_foundation_tests"
               "${PROJECT_SOURCE_DIR}/tests/llvm_backend_foundation_tests.cpp")
target_link_libraries("gti_llvm_backend_foundation_tests"
                      PRIVATE gti_llvm_backend)
add_test(NAME llvm_backend_foundation COMMAND gti_llvm_backend_foundation_tests)

add_executable("gti_llvm_admission_tests"
               "${PROJECT_SOURCE_DIR}/tests/llvm_admission_tests.cpp")
target_link_libraries("gti_llvm_admission_tests" PRIVATE gti_llvm_backend)
add_test(NAME llvm_admission COMMAND gti_llvm_admission_tests)

add_executable(gti_llvm_atomic_contract_tests llvm_atomic_contract_tests.cpp)
target_include_directories(gti_llvm_atomic_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_atomic_contract_tests PRIVATE gti_llvm_backend gti_driver)
add_test(NAME llvm_atomic_contracts COMMAND gti_llvm_atomic_contract_tests "${PROJECT_SOURCE_DIR}")
add_executable(gti_llvm_mutex_contract_tests llvm_mutex_contract_tests.cpp)
target_include_directories(gti_llvm_mutex_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_mutex_contract_tests PRIVATE gti_llvm_backend gti_driver gti_runtime)
add_test(NAME llvm_mutex_contracts COMMAND gti_llvm_mutex_contract_tests "${PROJECT_SOURCE_DIR}")
add_test(NAME llvm_mutex_hierarchy_contracts
  COMMAND gti_llvm_mutex_contract_tests "${PROJECT_SOURCE_DIR}" --hierarchy-contracts)
add_executable("gti_llvm_coverage_evidence"
               "${PROJECT_SOURCE_DIR}/tests/llvm_coverage_evidence.cpp")
target_link_libraries("gti_llvm_coverage_evidence" PRIVATE gti_llvm_backend gti_driver)
target_include_directories("gti_llvm_coverage_evidence" SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_include_directories("gti_llvm_coverage_evidence"
                           PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies("gti_llvm_coverage_evidence" gti_build_identity_refresh)
target_compile_definitions(
  "gti_llvm_coverage_evidence"
  PRIVATE
    GTI_COVERAGE_BUILD_CONFIG="$<CONFIG>"
    GTI_COVERAGE_SYSTEM_NAME="${CMAKE_SYSTEM_NAME}"
    GTI_COVERAGE_SYSTEM_PROCESSOR="${CMAKE_SYSTEM_PROCESSOR}"
    GTI_COVERAGE_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    GTI_COVERAGE_LLVM_VERSION="${GTI_LLVM_VERSION}"
)

set(GTI_LLVM_EVIDENCE_REGISTRY
    "${CMAKE_BINARY_DIR}/gti_llvm_evidence_registry.tsv")
function(gti_register_llvm_evidence_manifest test_name script_path)
  if("${test_name}" MATCHES "[\t\r\n]" OR
     "${script_path}" MATCHES "[\t\r\n]")
    message(FATAL_ERROR "LLVM evidence registry fields cannot contain tabs or newlines")
  endif()
  get_property(registered GLOBAL PROPERTY GTI_LLVM_EVIDENCE_TESTS)
  if("${test_name}" IN_LIST registered)
    message(FATAL_ERROR "Duplicate LLVM evidence test registration: ${test_name}")
  endif()
  set_property(GLOBAL APPEND PROPERTY GTI_LLVM_EVIDENCE_TESTS "${test_name}")
  get_property(contents GLOBAL PROPERTY GTI_LLVM_EVIDENCE_REGISTRY_CONTENTS)
  string(APPEND contents "${test_name}\t${script_path}\n")
  set_property(GLOBAL PROPERTY GTI_LLVM_EVIDENCE_REGISTRY_CONTENTS
                               "${contents}")
endfunction()

function(gti_write_llvm_evidence_registry)
  get_property(contents GLOBAL PROPERTY GTI_LLVM_EVIDENCE_REGISTRY_CONTENTS)
  if("${contents}" STREQUAL "")
    message(FATAL_ERROR "LLVM evidence registry cannot be empty")
  endif()
  file(GENERATE OUTPUT "${GTI_LLVM_EVIDENCE_REGISTRY}"
                CONTENT "${contents}")
endfunction()
cmake_language(DEFER CALL gti_write_llvm_evidence_registry)

include(llvm_c04_evaluation.cmake)

# O15 helper checks remain component evidence; production debug/mode wiring is
# separately gated and is not enabled by these private source registrations.
add_executable(gti_llvm_debug_info_tests llvm_debug_info_tests.cpp)
target_include_directories(gti_llvm_debug_info_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_debug_info_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_debug_info COMMAND gti_llvm_debug_info_tests)
set_tests_properties(llvm_debug_info PROPERTIES TIMEOUT 120)

add_executable(gti_llvm_instrumentation_tests llvm_instrumentation_tests.cpp)
target_include_directories(gti_llvm_instrumentation_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_instrumentation_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_instrumentation COMMAND gti_llvm_instrumentation_tests)
set_tests_properties(llvm_instrumentation PROPERTIES TIMEOUT 120)

# O15 SIMD uses the same verified MIR capability and private static LLVM closure
# as production Body emission. The SDK tools must match the configured LLVM.
add_executable(gti_llvm_simd_lowering_tests llvm_simd_lowering_tests.cpp)
target_include_directories(gti_llvm_simd_lowering_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_simd_lowering_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_simd_lowering
         COMMAND gti_llvm_simd_lowering_tests "${CMAKE_CURRENT_BINARY_DIR}/llvm-simd-component.ll")
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_simd_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_simd_execution_test.py"
                   --emitter "$<TARGET_FILE:gti_llvm_simd_lowering_tests>"
                   --clang "${GTI_LLVM_TEST_CLANG}" --opt "${GTI_LLVM_TEST_OPT}"
                   --output-root "${CMAKE_BINARY_DIR}/Testing/llvm-simd-execution")
  set_tests_properties(llvm_simd_execution PROPERTIES TIMEOUT 1200 PROCESSORS 1)
  add_test(NAME llvm_simd_source
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_simd_source_test.py"
                   --gti "$<TARGET_FILE:gti>" --gti-build-cache "${CMAKE_BINARY_DIR}/CMakeCache.txt"
                   --output-root "${CMAKE_BINARY_DIR}/Testing/llvm-simd-source")
  set_tests_properties(llvm_simd_source PROPERTIES TIMEOUT 1200 PROCESSORS 1)
  gti_register_llvm_evidence_manifest(
    llvm_simd_source "${PROJECT_SOURCE_DIR}/tests/llvm_simd_source_test.py")
endif()

add_executable(gti_llvm_optimization_pipeline_tests llvm_optimization_pipeline_tests.cpp)
target_include_directories(gti_llvm_optimization_pipeline_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_optimization_pipeline_tests PRIVATE gti_llvm_backend gti_driver gti_runtime)
add_test(NAME llvm_optimization_pipeline COMMAND gti_llvm_optimization_pipeline_tests)
add_executable(gti_llvm_checked_reduction_tests llvm_checked_reduction_tests.cpp)
target_include_directories(gti_llvm_checked_reduction_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_checked_reduction_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_checked_reductions COMMAND gti_llvm_checked_reduction_tests)
add_executable(gti_llvm_checked_range_tests llvm_checked_range_tests.cpp)
target_include_directories(gti_llvm_checked_range_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_checked_range_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_checked_ranges COMMAND gti_llvm_checked_range_tests)
add_executable(gti_llvm_session_tests llvm_session_tests.cpp)
target_include_directories(gti_llvm_session_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_session_tests PRIVATE gti_llvm_backend gti_driver gti_runtime)
add_test(NAME llvm_target_layout_abi COMMAND gti_llvm_session_tests)
add_test(NAME llvm_polymorphic_storage_layout
  COMMAND gti_llvm_session_tests --polymorphic-storage-layout "${PROJECT_SOURCE_DIR}")
set_tests_properties(llvm_polymorphic_storage_layout PROPERTIES TIMEOUT 120)
add_test(NAME llvm_payload_unique_owner
  COMMAND gti_llvm_session_tests --payload-unique-owner "${PROJECT_SOURCE_DIR}")
set_tests_properties(llvm_payload_unique_owner PROPERTIES TIMEOUT 120)

add_executable(gti_llvm_hosted_contract_tests llvm_hosted_contract_tests.cpp)
target_include_directories(gti_llvm_hosted_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_hosted_contract_tests PRIVATE gti_llvm_backend gti_driver gti_runtime)
add_test(NAME llvm_hosted_contracts
         COMMAND gti_llvm_hosted_contract_tests "${PROJECT_SOURCE_DIR}")
set_tests_properties(llvm_hosted_contracts PROPERTIES TIMEOUT 180)
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_hosted_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_hosted_execution_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_hosted_execution PROPERTIES TIMEOUT 300)
  add_test(NAME llvm_cleanup_context_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_cleanup_context_execution_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_cleanup_context_execution PROPERTIES TIMEOUT 300)
  add_test(NAME llvm_hosted_cleanup_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_hosted_cleanup_execution_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
  set_tests_properties(llvm_hosted_cleanup_execution PROPERTIES TIMEOUT 300)
  gti_register_llvm_evidence_manifest(
    llvm_hosted_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_hosted_execution_test.py"
  )
endif()

add_executable(gti_llvm_dispatch_layout_tests llvm_dispatch_layout_tests.cpp)
add_dependencies(gti_llvm_dispatch_layout_tests gti_runtime)
target_include_directories(gti_llvm_dispatch_layout_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_dispatch_layout_tests PRIVATE gti_llvm_backend gti_driver)
add_test(NAME llvm_dispatch_layout COMMAND gti_llvm_dispatch_layout_tests)
set_tests_properties(llvm_dispatch_layout PROPERTIES TIMEOUT 120)

add_executable(gti_llvm_polymorphic_storage_tests llvm_polymorphic_storage_tests.cpp)
target_include_directories(gti_llvm_polymorphic_storage_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_polymorphic_storage_tests PRIVATE gti_llvm_backend gti_driver)
add_test(NAME llvm_polymorphic_storage_mechanisms COMMAND gti_llvm_polymorphic_storage_tests)
set_tests_properties(llvm_polymorphic_storage_mechanisms PROPERTIES TIMEOUT 120)

# Private component execution for cleanup composition only. This deliberately
# does not enter the LLVM evidence manifest or public admission/coverage gates.
add_library(gti_llvm_cleanup_release_oracle STATIC
  llvm_cleanup_release_oracle.cpp)
add_executable(gti_llvm_cleanup_composition_execution_tests
  llvm_cleanup_composition_execution_tests.cpp)
add_dependencies(gti_llvm_cleanup_composition_execution_tests
  gti_llvm_cleanup_release_oracle)
target_include_directories(gti_llvm_cleanup_composition_execution_tests SYSTEM
  PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_cleanup_composition_execution_tests
  PRIVATE gti_llvm_backend gti_driver gti_runtime)
add_test(NAME llvm_cleanup_composition_execution
  COMMAND gti_llvm_cleanup_composition_execution_tests
          "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:gti_runtime>"
          "$<TARGET_FILE:gti_llvm_cleanup_release_oracle>")
set_tests_properties(llvm_cleanup_composition_execution PROPERTIES TIMEOUT 180)
add_executable(gti_llvm_native_c_abi_tests llvm_native_c_abi_tests.cpp)
add_executable(gti_llvm_embedding_emitter llvm_embedding_emitter.cpp)
target_link_libraries(gti_llvm_embedding_emitter PRIVATE gti_driver gti_llvm_backend)
add_executable(gti_runtime_embedding_tests runtime_embedding_tests.cpp)
target_include_directories(gti_runtime_embedding_tests PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_link_libraries(gti_runtime_embedding_tests PRIVATE gti_runtime)
target_compile_options(gti_runtime_embedding_tests PRIVATE -UNDEBUG)
add_test(NAME runtime_embedding_context COMMAND gti_runtime_embedding_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_embedding_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_embedding_execution_test.py"
      "$<TARGET_FILE:gti_llvm_embedding_emitter>" --runtime "$<TARGET_FILE:gti_runtime>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-embedding-evidence")
  gti_register_llvm_evidence_manifest(llvm_embedding_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_embedding_execution_test.py")
  add_test(NAME llvm_embedding_project
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_embedding_project_test.py"
      "$<TARGET_FILE:gti>" --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-embedding-project-evidence")
  set_tests_properties(llvm_embedding_project PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_embedding_cost
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_embedding_cost_test.py"
      "$<TARGET_FILE:gti_llvm_embedding_emitter>" --runtime "$<TARGET_FILE:gti_runtime>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-embedding-cost")
endif()
target_include_directories(gti_llvm_native_c_abi_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_native_c_abi_tests PRIVATE gti_llvm_backend)
add_dependencies(gti_llvm_native_c_abi_tests gti_runtime)
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_native_c_abi
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_abi_oracle_test.py"
      "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-abi-evidence")
  set_tests_properties(llvm_native_c_abi PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_native_c_contract_preflight
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_contract_oracle_test.py"
      "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --runtime "$<TARGET_FILE:gti_runtime>" --preflight --profile scalar
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-preflight-evidence")
  add_test(NAME llvm_native_c_scalar_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_contract_oracle_test.py"
      "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --compiler "$<TARGET_FILE:gti>" --profile scalar
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-scalar-evidence")
  add_test(NAME llvm_native_c_counted_input
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_text_oracle_test.py"
      "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --runtime "$<TARGET_FILE:gti_runtime>" --compiler "$<TARGET_FILE:gti>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-counted-evidence")
  set_tests_properties(llvm_native_c_contract_preflight
    llvm_native_c_scalar_execution llvm_native_c_counted_input PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_native_c_indirect_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_indirect_oracle_test.py"
      "$<TARGET_FILE:gti>" --emitter "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-indirect-evidence")
  set_tests_properties(llvm_native_c_indirect_execution PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_native_c_aggregate_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_aggregate_oracle_test.py"
      "$<TARGET_FILE:gti>" --emitter "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-aggregate-evidence")
  set_tests_properties(llvm_native_c_aggregate_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(llvm_native_c_aggregate_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_aggregate_oracle_test.py")
  add_test(NAME llvm_native_c_storage_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_storage_oracle_test.py"
      "$<TARGET_FILE:gti>" --emitter "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-storage-evidence")
  set_tests_properties(llvm_native_c_storage_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(llvm_native_c_storage_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_storage_oracle_test.py")
  add_test(NAME llvm_native_c_callback_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_callback_execution_test.py"
      "$<TARGET_FILE:gti>" --emitter "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-callback-execution-evidence")
  set_tests_properties(llvm_native_c_callback_execution PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_native_c_outbound_boundary
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_outbound_boundary_test.py"
      "$<TARGET_FILE:gti>" --cxx "${CMAKE_CXX_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-outbound-boundary-evidence")
  set_tests_properties(llvm_native_c_outbound_boundary PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_native_c_confined_execution
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_confined_execution_test.py"
      "$<TARGET_FILE:gti>" --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-confined-evidence")
  set_tests_properties(llvm_native_c_confined_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(llvm_native_c_callback_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_callback_execution_test.py")
  add_test(NAME llvm_native_c_callback_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_cost_test.py"
      "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --callback
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-callback-cost-evidence")
  set_tests_properties(llvm_native_c_callback_cost PROPERTIES TIMEOUT 180)
  add_test(NAME llvm_native_c_storage_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_cost_test.py"
      "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --cc "${CMAKE_C_COMPILER}" --storage
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-storage-cost")
  set_tests_properties(llvm_native_c_storage_cost PROPERTIES TIMEOUT 180)
  add_test(NAME llvm_native_c_indirect_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_cost_test.py"
      "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --indirect --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-indirect-cost")
  set_tests_properties(llvm_native_c_indirect_cost PROPERTIES TIMEOUT 180)
  add_test(NAME llvm_raw_pointer_array_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_cost_test.py"
      "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --pointer-array --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-raw-pointer-array-cost")
  set_tests_properties(llvm_raw_pointer_array_cost PROPERTIES TIMEOUT 180)
  add_test(NAME llvm_native_c_aggregate_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_cost_test.py"
      "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_native_c_abi_tests>"
      --aggregate --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-native-c-aggregate-cost")
  set_tests_properties(llvm_native_c_aggregate_cost PROPERTIES TIMEOUT 180)
  gti_register_llvm_evidence_manifest(llvm_native_c_indirect_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_indirect_oracle_test.py")
  gti_register_llvm_evidence_manifest(llvm_native_c_scalar_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_contract_oracle_test.py")
  gti_register_llvm_evidence_manifest(llvm_native_c_counted_input
    "${PROJECT_SOURCE_DIR}/tests/llvm_native_c_text_oracle_test.py")
endif()

add_executable(gti_runtime_execution_tests runtime_execution_tests.cpp)
target_link_libraries(gti_runtime_execution_tests PRIVATE gti_runtime)
add_test(NAME runtime_execution_boundary COMMAND gti_runtime_execution_tests)
add_executable(gti_llvm_thread_lifecycle_contract_tests llvm_thread_lifecycle_contract_tests.cpp)
target_include_directories(gti_llvm_thread_lifecycle_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
# Header-only C record layout comparison; the backend links no runtime here.
target_include_directories(gti_llvm_thread_lifecycle_contract_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/runtime/include")
target_link_libraries(gti_llvm_thread_lifecycle_contract_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_thread_lifecycle_contracts
         COMMAND gti_llvm_thread_lifecycle_contract_tests "${PROJECT_SOURCE_DIR}")
set_tests_properties(llvm_thread_lifecycle_contracts PROPERTIES TIMEOUT 180)
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_cthread_startup_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_cthread_startup_execution_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_cthread_startup_execution PROPERTIES TIMEOUT 300)
  gti_register_llvm_evidence_manifest(
    llvm_cthread_startup_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_cthread_startup_execution_test.py")
  add_test(NAME llvm_thread_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_thread_execution_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}" --cxx "${CMAKE_CXX_COMPILER}")
  set_tests_properties(llvm_thread_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_thread_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_thread_execution_test.py")
endif()
add_executable(gti_llvm_tls_contract_tests llvm_tls_contract_tests.cpp)
target_include_directories(gti_llvm_tls_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_tls_contract_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_tls_contracts
         COMMAND gti_llvm_tls_contract_tests "${PROJECT_SOURCE_DIR}")
set_tests_properties(llvm_tls_contracts PROPERTIES TIMEOUT 180)
if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_tls_cleanup_context
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_tls_cleanup_context_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_tls_cleanup_context PROPERTIES TIMEOUT 300)
endif()

add_executable(gti_llvm_clang_coexistence_smoke llvm_clang_coexistence_smoke.cpp)
target_link_libraries(gti_llvm_clang_coexistence_smoke PRIVATE gti_llvm_backend gti_bindgen)
add_test(NAME llvm_clang_coexistence COMMAND gti_llvm_clang_coexistence_smoke)
set_tests_properties(llvm_clang_coexistence PROPERTIES TIMEOUT 30)

add_executable(gti_llvm_memory_contract_tests llvm_memory_contract_tests.cpp)
target_include_directories(gti_llvm_memory_contract_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_memory_contract_tests PRIVATE gti_llvm_backend)
add_test(NAME llvm_memory_contracts COMMAND gti_llvm_memory_contract_tests "${PROJECT_SOURCE_DIR}")
add_executable(gti_llvm_memory_mechanism_tests llvm_memory_mechanism_tests.cpp)
target_include_directories(gti_llvm_memory_mechanism_tests SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
target_link_libraries(gti_llvm_memory_mechanism_tests PRIVATE gti_llvm_backend gti_driver)
add_test(NAME llvm_memory_mechanisms COMMAND gti_llvm_memory_mechanism_tests)
set_tests_properties(llvm_memory_mechanisms PROPERTIES TIMEOUT 120)

if(Python3_Interpreter_FOUND)
  add_test(NAME llvm_checked_reduction_runtime
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_checked_reduction_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(llvm_checked_reduction_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_checked_range_runtime
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_checked_range_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(llvm_checked_range_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME llvm_scalar_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_scalar_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_scalar_execution PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_atomic_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_atomic_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_atomic_execution PROPERTIES TIMEOUT 600)
  if(GTI_NATIVE_SYNCHRONIZATION_SERVICES)
    add_test(NAME runtime_synchronization
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/runtime_synchronization_test.py"
              "${PROJECT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/runtime-synchronization-evidence"
              --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}")
    set_tests_properties(runtime_synchronization PROPERTIES TIMEOUT 180)
    if(GTI_LLVM_TEST_CLANG)
      add_test(NAME llvm_atomic_wait_service
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_atomic_wait_service_test.py"
                "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:gti_llvm_atomic_contract_tests>"
                "${CMAKE_BINARY_DIR}/atomic-wait-service-evidence"
                --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
                --llvm-cc "${GTI_LLVM_TEST_CLANG}")
      set_tests_properties(llvm_atomic_wait_service PROPERTIES TIMEOUT 240)
      add_test(NAME llvm_mutex_service
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_mutex_service_test.py"
                "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:gti_llvm_mutex_contract_tests>"
                "${CMAKE_BINARY_DIR}/mutex-service-evidence"
                --public-gti "$<TARGET_FILE:gti>"
                --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
                --llvm-cc "${GTI_LLVM_TEST_CLANG}")
      set_tests_properties(llvm_mutex_service PROPERTIES TIMEOUT 240)
      gti_register_llvm_evidence_manifest(
        llvm_mutex_service "${PROJECT_SOURCE_DIR}/tests/llvm_mutex_service_test.py")
    endif()
  endif()
  gti_register_llvm_evidence_manifest(
    llvm_scalar_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_scalar_execution_test.py"
  )
  gti_register_llvm_evidence_manifest(
    llvm_atomic_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_atomic_execution_test.py"
  )
  add_test(NAME llvm_place_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_place_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_place_execution PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_raw_pointer_array_execution
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_raw_pointer_array_test.py"
      "$<TARGET_FILE:gti>" --cc "${CMAKE_C_COMPILER}"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/llvm-raw-pointer-array-evidence")
  set_tests_properties(llvm_raw_pointer_array_execution PROPERTIES TIMEOUT 600)
  gti_register_llvm_evidence_manifest(llvm_raw_pointer_array_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_raw_pointer_array_test.py")
  gti_register_llvm_evidence_manifest(
    llvm_place_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_place_execution_test.py"
  )
  add_test(NAME llvm_class_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_class_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_class_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_class_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_class_execution_test.py"
  )
  add_test(
    NAME llvm_coverage_evidence
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_coverage_evidence_test.py"
      "$<TARGET_FILE:gti_llvm_coverage_evidence>" "${CMAKE_CTEST_COMMAND}"
      "${CMAKE_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
      "${GTI_LLVM_EVIDENCE_REGISTRY}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_readiness_baseline.json"
  )
  set_tests_properties(llvm_coverage_evidence PROPERTIES TIMEOUT 600)
  add_test(
    NAME llvm_readiness_collection_controls
    COMMAND
      "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_readiness_collect_test.py"
      "$<TARGET_FILE:gti_llvm_coverage_evidence>" "${PROJECT_SOURCE_DIR}"
      "${CMAKE_BINARY_DIR}" "${GTI_LLVM_EVIDENCE_REGISTRY}"
      "${PROJECT_SOURCE_DIR}/tests/llvm_readiness_baseline.json"
      "${CMAKE_CTEST_COMMAND}"
  )
  set_tests_properties(llvm_readiness_collection_controls PROPERTIES TIMEOUT 180)
  add_test(
    NAME llvm_canonical_host_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_canonical_host_test.py"
            "$<TARGET_FILE:gti_llvm_coverage_evidence>" "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" "${GTI_LLVM_EVIDENCE_REGISTRY}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_readiness_baseline.json" "${CMAKE_CTEST_COMMAND}"
  )
  set_tests_properties(llvm_canonical_host_controls PROPERTIES TIMEOUT 240)
  # Alternative source-library legality/verified-MIR contracts ONLY. This is
  # deliberately not a native execution manifest or a hosted coverage witness.
  add_test(
    NAME llvm_hosted_cleanup_source_contract
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_hosted_cleanup_evidence_test.py"
            "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_llvm_coverage_evidence>"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
  )
  set_tests_properties(llvm_hosted_cleanup_source_contract PROPERTIES TIMEOUT 900)
  add_test(
    NAME llvm_case_context_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_case_context_test.py"
            "$<TARGET_FILE:gti_llvm_coverage_evidence>" "${PROJECT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
  )
  set_tests_properties(llvm_case_context_controls PROPERTIES TIMEOUT 180)
  add_test(
    NAME llvm_scalar_context_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_scalar_context_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(llvm_scalar_context_controls PROPERTIES TIMEOUT 180)
  add_test(
    NAME stdlib_surface_context_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/stdlib_surface_context_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(stdlib_surface_context_controls PROPERTIES TIMEOUT 180)

  add_test(NAME llvm_dispatch_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_dispatch_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_dispatch_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_dispatch_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_dispatch_execution_test.py"
  )
  add_test(NAME llvm_placement_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_placement_execution_test.py"
                   $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_placement_execution PROPERTIES TIMEOUT 1200)
  add_test(NAME llvm_payload_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_payload_execution_test.py"
                   $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_payload_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_payload_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_payload_execution_test.py"
  )
  add_test(NAME llvm_callable_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_callable_execution_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_callable_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_callable_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_callable_execution_test.py"
  )
  add_test(NAME llvm_callable_owned_composition
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_callable_owned_composition_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_callable_owned_composition PROPERTIES TIMEOUT 300)
  add_test(NAME llvm_callable_null_owner_rollback
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_callable_null_owner_rollback_test.py"
                   "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_callable_null_owner_rollback PROPERTIES TIMEOUT 300)
  add_test(NAME llvm_memory_execution
           COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/llvm_memory_execution_test.py"
                   $<TARGET_FILE:gti> "${PROJECT_SOURCE_DIR}")
  set_tests_properties(llvm_memory_execution PROPERTIES TIMEOUT 1200)
  gti_register_llvm_evidence_manifest(
    llvm_memory_execution
    "${PROJECT_SOURCE_DIR}/tests/llvm_memory_execution_test.py"
  )
  if(GTI_BUILD_LSP)
    add_test(NAME array_publication_lsp
             COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/array_publication_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(array_publication_lsp PROPERTIES TIMEOUT 120)
  endif()
  add_test(NAME runtime_abi_inventory
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/runtime_abi_inventory_test.py"
                   "${PROJECT_SOURCE_DIR}"
                   "${CMAKE_BINARY_DIR}/gti_runtime_abi_inventory.json")
endif()

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
add_test(NAME implicit_receiver_owned_assignment_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/implicit_receiver_owned_assignment_runtime_test.py"
          $<TARGET_FILE:gti>)
set_tests_properties(implicit_receiver_owned_assignment_runtime PROPERTIES TIMEOUT 600)
add_test(NAME constructor_passive_field_rollback_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/constructor_passive_field_rollback_runtime_test.py"
          $<TARGET_FILE:gti>)
set_tests_properties(constructor_passive_field_rollback_runtime PROPERTIES TIMEOUT 600)
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
add_executable(gti_user_dual_range_backend_tests user_dual_range_backend_tests.cpp)
target_link_libraries(gti_user_dual_range_backend_tests PRIVATE gti_cpp_backend)
add_test(NAME user_dual_range_backend COMMAND gti_user_dual_range_backend_tests)
add_executable(gti_text_view_iterator_compiler_tests text_view_iterator_compiler_tests.cpp)
target_link_libraries(gti_text_view_iterator_compiler_tests PRIVATE gti_cpp_backend)
add_test(NAME text_view_iterator_compiler COMMAND gti_text_view_iterator_compiler_tests)
add_executable(gti_generic_discovery_tests generic_discovery_tests.cpp)
target_link_libraries(gti_generic_discovery_tests PRIVATE gti_compiler)
add_test(NAME generic_discovery COMMAND gti_generic_discovery_tests)
set_tests_properties(generic_discovery PROPERTIES TIMEOUT 60)
add_test(NAME generic_discovery_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/generic_discovery_runtime_test.py"
          "$<TARGET_FILE:gti>")
set_tests_properties(generic_discovery_runtime PROPERTIES TIMEOUT 240)

add_executable(gti_owned_pointee_projection_tests owned_pointee_projection_tests.cpp)
target_link_libraries(gti_owned_pointee_projection_tests PRIVATE gti_cpp_backend)
add_test(NAME owned_pointee_projection COMMAND gti_owned_pointee_projection_tests)

add_executable(gti_consuming_owned_pointee_tests consuming_owned_pointee_tests.cpp)
target_link_libraries(gti_consuming_owned_pointee_tests PRIVATE gti_cpp_backend)
add_test(NAME consuming_owned_pointee COMMAND gti_consuming_owned_pointee_tests)
add_executable(gti_optional_empty_prefix_tests optional_empty_prefix_tests.cpp)
target_link_libraries(gti_optional_empty_prefix_tests PRIVATE gti_cpp_backend)
add_test(NAME optional_empty_prefix COMMAND gti_optional_empty_prefix_tests)
add_executable(gti_enum_replacement_tests enum_replacement_tests.cpp)
target_link_libraries(gti_enum_replacement_tests PRIVATE gti_cpp_backend)
add_test(NAME enum_replacement COMMAND gti_enum_replacement_tests)
add_test(NAME enum_replacement_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/enum_replacement_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(enum_replacement_runtime PROPERTIES TIMEOUT 600)

add_executable(gti_borrowed_payload_copy_tests borrowed_payload_copy_tests.cpp)
target_link_libraries(gti_borrowed_payload_copy_tests PRIVATE gti_cpp_backend)
add_test(NAME borrowed_payload_copy COMMAND gti_borrowed_payload_copy_tests)

add_executable(gti_enum_selector_tests enum_selector_tests.cpp)
target_link_libraries(gti_enum_selector_tests PRIVATE gti_cpp_backend)
add_test(NAME enum_selector COMMAND gti_enum_selector_tests)
add_test(NAME enum_selector_runtime
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/enum_selector_runtime_test.py" $<TARGET_FILE:gti>)
set_tests_properties(enum_selector_runtime PROPERTIES TIMEOUT 300)
add_executable(gti_field_array_empty_state_tests field_array_empty_state_tests.cpp)
target_link_libraries(gti_field_array_empty_state_tests PRIVATE gti_cpp_backend)
add_test(NAME field_array_empty_state COMMAND gti_field_array_empty_state_tests)
add_executable(gti_passive_fixed_array_tests passive_fixed_array_tests.cpp)
target_link_libraries(gti_passive_fixed_array_tests PRIVATE gti_cpp_backend)
add_test(NAME passive_fixed_array_values COMMAND gti_passive_fixed_array_tests)
add_executable(gti_class_field_value_tests class_field_value_tests.cpp)
target_link_libraries(gti_class_field_value_tests PRIVATE gti_cpp_backend)
add_test(NAME class_field_values COMMAND gti_class_field_value_tests)
add_executable(gti_conditional_class_publication_tests conditional_class_publication_tests.cpp)
target_link_libraries(gti_conditional_class_publication_tests PRIVATE gti_cpp_backend)
add_test(NAME conditional_class_publication COMMAND gti_conditional_class_publication_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME class_expression_publication_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/class_expression_publication_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(class_expression_publication_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME class_expression_consumers_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/class_expression_consumers_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}" --jobs 2)
  set_tests_properties(class_expression_consumers_runtime
    PROPERTIES TIMEOUT 600 PROCESSORS 2)
  add_test(NAME class_field_value_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/class_field_value_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(class_field_value_runtime PROPERTIES TIMEOUT 300)
endif()
add_executable(gti_aggregate_projection_tests aggregate_projection_tests.cpp)
target_link_libraries(gti_aggregate_projection_tests PRIVATE gti_cpp_backend)
add_test(NAME aggregate_projection COMMAND gti_aggregate_projection_tests)
if(Python3_Interpreter_FOUND)
  add_test(NAME aggregate_projection_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aggregate_projection_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aggregate_projection_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME passive_fixed_array_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/passive_fixed_array_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(passive_fixed_array_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME fresh_array_index_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fresh_array_index_runtime_test.py"
            "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(fresh_array_index_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME coverage_native_fixture_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/coverage/native_fixture_policy_test.py")
endif()
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
add_executable(gti_expected_ergonomics_tests expected_ergonomics_tests.cpp)
target_link_libraries(gti_expected_ergonomics_tests PRIVATE gti_cpp_backend)
add_test(NAME expected_ergonomics COMMAND gti_expected_ergonomics_tests)
add_executable(gti_borrowed_expected_extraction_tests borrowed_expected_extraction_tests.cpp)
target_link_libraries(gti_borrowed_expected_extraction_tests PRIVATE gti_cpp_backend)
add_test(NAME borrowed_expected_extraction COMMAND gti_borrowed_expected_extraction_tests)
add_executable(gti_borrow_origin_tests borrow_origin_tests.cpp)
target_link_libraries(gti_borrow_origin_tests PRIVATE gti_cpp_backend)
add_test(NAME borrow_origin COMMAND gti_borrow_origin_tests)
add_executable(gti_panic_contract_tests panic_contract_tests.cpp)
target_link_libraries(gti_panic_contract_tests PRIVATE gti_cpp_backend)
add_test(NAME panic_contracts COMMAND gti_panic_contract_tests)
add_executable(gti_deferred_assertion_tests deferred_assertion_tests.cpp)
target_link_libraries(gti_deferred_assertion_tests PRIVATE gti_cpp_backend)
add_test(NAME deferred_assertions COMMAND gti_deferred_assertion_tests)
add_executable(gti_owned_unexpected_pending_cleanup_tests owned_unexpected_pending_cleanup_tests.cpp)
target_link_libraries(gti_owned_unexpected_pending_cleanup_tests PRIVATE gti_cpp_backend)
add_test(NAME owned_unexpected_pending_cleanup COMMAND gti_owned_unexpected_pending_cleanup_tests)
add_executable(gti_private_constructor_placement_tests private_constructor_placement_tests.cpp)
target_link_libraries(gti_private_constructor_placement_tests PRIVATE gti_cpp_backend)
add_test(NAME private_constructor_placement COMMAND gti_private_constructor_placement_tests)
add_executable(gti_nested_guarded_expected_tests nested_guarded_expected_tests.cpp)
target_link_libraries(gti_nested_guarded_expected_tests PRIVATE gti_cpp_backend)
add_test(NAME nested_guarded_expected COMMAND gti_nested_guarded_expected_tests)
add_executable(gti_structural_relocation_tests structural_relocation_tests.cpp)
target_link_libraries(gti_structural_relocation_tests PRIVATE gti_cpp_backend)
add_test(NAME structural_relocation COMMAND gti_structural_relocation_tests)

add_executable(gti_compact_owner_layout_tests compact_owner_layout_tests.cpp)
target_link_libraries(gti_compact_owner_layout_tests PRIVATE gti_cpp_backend)
add_test(NAME compact_owner_layout COMMAND gti_compact_owner_layout_tests)
add_executable(gti_restoring_exchange_tests restoring_exchange_tests.cpp)
target_link_libraries(gti_restoring_exchange_tests PRIVATE gti_cpp_backend)
add_test(NAME restoring_exchange COMMAND gti_restoring_exchange_tests)
add_test(NAME restoring_exchange_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/restoring_exchange_runtime_test.py"
          $<TARGET_FILE:gti>)
set_tests_properties(restoring_exchange_runtime PROPERTIES TIMEOUT 600)
add_test(NAME restoring_exchange_shapes_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/restoring_exchange_shapes_runtime_test.py"
          $<TARGET_FILE:gti>)
# Its GCC sanitized build may take minutes under a parallel suite;
# see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
set_tests_properties(restoring_exchange_shapes_runtime PROPERTIES TIMEOUT 1800)
add_test(NAME restoring_indexed_runtime
  COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/restoring_indexed_runtime_test.py"
          $<TARGET_FILE:gti>)
set_tests_properties(restoring_indexed_runtime PROPERTIES TIMEOUT 600)
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
add_test(NAME type_only_alias_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/type_only_alias_runtime_test.py"
                 "$<TARGET_FILE:gti>")
set_tests_properties(type_only_alias_runtime PROPERTIES TIMEOUT 300)

add_test(NAME nested_receiver_array_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/nested_receiver_array_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(nested_receiver_array_runtime PROPERTIES TIMEOUT 300)

add_executable(gti_issues_30_33_tests issues_30_33_tests.cpp)
target_link_libraries(gti_issues_30_33_tests PRIVATE gti_cpp_backend)
add_test(NAME issues_30_33 COMMAND gti_issues_30_33_tests)
add_test(NAME issues_30_33_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/issues_30_33_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(issues_30_33_runtime PROPERTIES TIMEOUT 300)
if(GTI_BUILD_LSP)
  add_test(NAME issues_30_33_lsp
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/issues_30_33_lsp_test.py"
                   $<TARGET_FILE:gti> $<TARGET_FILE:gti_lsp>)
endif()

add_test(NAME passive_record_runtime
         COMMAND "${Python3_EXECUTABLE}"
                 "${PROJECT_SOURCE_DIR}/tests/passive_record_runtime_test.py"
                 $<TARGET_FILE:gti>)
set_tests_properties(passive_record_runtime PROPERTIES TIMEOUT 300)

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
               "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
               "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
)

if(Python3_Interpreter_FOUND)
  add_test(
    NAME bounded_log
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/bounded_log_test.py"
  )
  set_tests_properties(bounded_log PROPERTIES TIMEOUT 30)
  add_test(
    NAME agent_workflow
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/agent_workflow_test.py"
  )
  add_test(NAME rollup
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/rollup_test.py")
  add_test(NAME preview_workflow
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/preview_workflow_test.py")
  # Only combined preview candidates register this aggregate gate. Ordinary
  # PR/main suites and their acceptance requirements are unchanged.
  if(EXISTS "${PROJECT_SOURCE_DIR}/.gti-preview.json")
    add_test(NAME preview_admission
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/preview_examples.py"
              "${PROJECT_BINARY_DIR}" --manifest)
    set_tests_properties(preview_admission PROPERTIES TIMEOUT 10800 RUN_SERIAL TRUE)
  endif()
  add_test(
    NAME build_window
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/build_window_test.py"
  )
  set_tests_properties(build_window PROPERTIES TIMEOUT 120)
  add_test(
    NAME remote_runner
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/remote_runner_test.py"
  )
  set_tests_properties(remote_runner PROPERTIES TIMEOUT 120)
  add_test(
    NAME cmake_test_ownership
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cmake_test_ownership_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  # Every owner and consumer in tests/ownership.toml must name a test that
  # this build registers. The Git-based coverage checks run in CI's workflow
  # regression job instead.
  add_test(
    NAME test_ownership_map
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ownership_map_test.py"
            --inventory-only --ctest "${CMAKE_CTEST_COMMAND}"
            --build "${PROJECT_BINARY_DIR}"
  )
  set_tests_properties(test_ownership_map PROPERTIES TIMEOUT 120)
  add_test(
    NAME plan_checks
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/plan_checks_test.py"
  )
  set_tests_properties(plan_checks PROPERTIES TIMEOUT 60)
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
            "${CMAKE_BINARY_DIR}/gti_llvm_backend_sources.txt"
  )
  add_test(
    NAME llvm_backend_dependency_controls
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_backend_boundary_test.py"
  )
  add_test(
    NAME llvm_coverage_exhaustiveness
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_coverage_exhaustiveness_test.py"
            "${CMAKE_CXX_COMPILER}" "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME backend_execution_protocol
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/backend_execution_test.py"
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
  set_tests_properties(lowered_program_corpus_census PROPERTIES TIMEOUT 1800)
  add_test(
    NAME runtime_failure_subprocess
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/runtime_failure_test.py"
            "$<TARGET_FILE:gti_runtime_failure_helper>"
  )
  add_test(
    NAME allocation_abort_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/allocation_abort_runtime_test.py"
            "$<TARGET_FILE:gti_allocation_abort_helper>"
  )
  add_test(
    NAME panic_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/panic_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(panic_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME deferred_assertions_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/deferred_assertion_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(deferred_assertions_runtime PROPERTIES TIMEOUT 1200)
  add_test(NAME deferred_assertions_cost
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/deferred_assertion_cost_test.py"
      "$<TARGET_FILE:gti>")
  set_tests_properties(deferred_assertions_cost PROPERTIES TIMEOUT 300)
  if(TARGET gti_lsp)
    add_test(NAME deferred_assertions_lsp
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/deferred_assertion_lsp_test.py"
        "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(deferred_assertions_lsp PROPERTIES TIMEOUT 90)
  endif()
  add_test(
    NAME panic_adoption_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/panic_adoption_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(panic_adoption_runtime PROPERTIES TIMEOUT 600)
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
    NAME environment_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/environment_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "$<TARGET_FILE:gti_runtime>"
  )
  set_tests_properties(environment_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME stdlib_adoption_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stdlib_adoption_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(stdlib_adoption_runtime PROPERTIES TIMEOUT 1200)
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
  add_test(NAME thread_confined_callback_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/thread_confined_callback_test.py"
                   "$<TARGET_FILE:gti>" "${CMAKE_CXX_COMPILER}")
  set_tests_properties(thread_confined_callback_runtime PROPERTIES TIMEOUT 600)
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
  add_test(NAME runtime_object_layout_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/runtime_object_layout_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(runtime_object_layout_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME consteval_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/consteval_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(consteval_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME general_object_layout_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/general_object_layout_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(general_object_layout_runtime PROPERTIES TIMEOUT 300)
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
  add_test(NAME c_string_import_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/c_string_import_runtime_test.py" $<TARGET_FILE:gti>)
  set_tests_properties(c_string_import_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME forward_list_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/forward_list_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(forward_list_runtime PROPERTIES TIMEOUT 600)
  foreach(iterator_gate IN ITEMS
      forward_list_iterator_runtime
      contiguous_iterator_completion
      paired_span_iterator_completion
      storage_lane_iterator_runtime
      cursor_carrier_publication
      map_mutable_entry_projection
      user_dual_range_lifetime
      user_owning_reference_replacement
      range_endpoint_parking_runtime
      text_view_iterator_runtime
      container_iterator_movement)
    add_test(
      NAME ${iterator_gate}
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/${iterator_gate}_test.py"
              "$<TARGET_FILE:gti>")
    set_tests_properties(${iterator_gate} PROPERTIES TIMEOUT 600)
  endforeach()
  add_test(
    NAME pack_parameter_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/pack_parameter_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(pack_parameter_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME enum_pack_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/enum_pack_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(enum_pack_runtime PROPERTIES TIMEOUT 180)
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
    NAME process_allocation_elision
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/process_allocation_elision_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(process_allocation_elision PROPERTIES TIMEOUT 300)
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
    NAME generated_base_constructor_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generated_base_constructor_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(generated_base_constructor_runtime PROPERTIES TIMEOUT 300)

  add_test(
    NAME expected_boundary_replacement_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_boundary_replacement_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(expected_boundary_replacement_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME raw_receiver_owned_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/raw_receiver_owned_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(raw_receiver_owned_argument_runtime PROPERTIES TIMEOUT 300)
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
    NAME shared_failure_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_failure_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(shared_failure_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME shared_exit_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_exit_cleanup_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(shared_exit_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME declaration_construction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/declaration_construction_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(declaration_construction_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME implicit_return_construction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/implicit_return_construction_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(implicit_return_construction_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME conversion_operators_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conversion_operators_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conversion_operators_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME protocol_concepts_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/protocol_concepts_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(protocol_concepts_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME associated_type_families_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/associated_type_families_runtime_test.py"
            "$<TARGET_FILE:gti>" --output-dir
            "${PROJECT_BINARY_DIR}/test-artifacts/associated-type-families"
  )
  set_tests_properties(associated_type_families_runtime PROPERTIES TIMEOUT 900)
  if(TARGET gti_lsp)
    add_test(
      NAME associated_type_families_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/associated_type_families_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(associated_type_families_lsp PROPERTIES TIMEOUT 60)
  endif()
  add_test(
    NAME consuming_payload_patterns_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_payload_patterns_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_payload_patterns_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME move_only_payload_transport_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/move_only_payload_transport_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(move_only_payload_transport_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME tryable_propagation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tryable_propagation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(tryable_propagation_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME experimental_optional_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/experimental_optional_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(experimental_optional_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME source_optional_protocol_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/experimental_optional_runtime_test.py"
            "$<TARGET_FILE:gti>" --stable
  )
  set_tests_properties(source_optional_protocol_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME payload_enum_carrier_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/payload_enum_carrier_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(payload_enum_carrier_runtime PROPERTIES TIMEOUT 900)

  add_test(
    NAME composed_payload_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/composed_payload_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(composed_payload_runtime PROPERTIES TIMEOUT 900)

  add_test(
    NAME payload_enum_propagation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/payload_enum_propagation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(payload_enum_propagation_runtime PROPERTIES TIMEOUT 900)

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
    NAME cut_release_policy
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cut_release_test.py"
  )
  add_test(
    NAME repository_hosting
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/repository_hosting_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME ci_sanitizer_capability
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/sanitizer_support_test.py"
  )
  add_test(
    NAME named_test_shards
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/named_test_shards_test.py"
  )
  add_test(
    NAME ci_resources
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ci_resources_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/configure_ci_resources.sh"
  )
  add_test(
    NAME ci_cache
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ci_cache_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/configure_ci_cache.sh"
  )
  add_test(
    NAME ci_release
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ci_release_test.py"
            "${PROJECT_SOURCE_DIR}"
            --cmake "${CMAKE_COMMAND}"
  )
  add_test(NAME preview_release
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/preview_release_test.py")
  add_test(
    NAME release_train
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/release_train_test.py"
  )
  add_test(
    NAME native_framework_nonapple
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/native_framework_nonapple_test.py"
            "${PROJECT_SOURCE_DIR}/src/driver/build.cpp"
            "${CMAKE_CXX_COMPILER}"
  )
  add_test(
    NAME neovim_local_installer
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/neovim_local_installer_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/install_neovim_local.py"
  )
  add_test(
    NAME macos_runtime_bundle
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/macos_runtime_bundle_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/bundle_macos_runtime.py"
  )
  if(APPLE AND GTI_BUILD_LSP)
    add_test(
      NAME macos_installed_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/macos_installed_runtime_test.py"
              "${PROJECT_BINARY_DIR}"
    )
  endif()
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
    NAME release_asset_sync
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/release_asset_sync_test.py"
            "${PROJECT_SOURCE_DIR}/scripts/sync_release_assets.py"
  )
  add_test(
    NAME mir_census_profile_controls
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mir_census_profile_test.py"
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
            "${PROJECT_SOURCE_DIR}/examples/007-generics.gti"
  )
  add_test(NAME mir_field_determinism
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/determinism_test.py"
      "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}/tests/coverage/reproducers/a13-mir-field-symbol-order.gti" --mir-runs 30)
  add_test(NAME mir_associative_determinism
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/determinism_test.py"
      "$<TARGET_FILE:gti>" "${PROJECT_SOURCE_DIR}/examples/087-associative-containers.gti" --mir-runs 6)
  set_tests_properties(mir_field_determinism mir_associative_determinism PROPERTIES TIMEOUT 180)
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
  )
  add_test(
    NAME feature_interaction_model
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/feature_interaction_model_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(
    NAME feature_interaction_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/feature_interaction_smoke_test.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(
    feature_interaction_smoke
    PROPERTIES TIMEOUT 600
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
  )
  add_test(
    NAME stdlib_surface_manifest
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stdlib_surface_manifest_test.py"
            "${PROJECT_SOURCE_DIR}"
  )
  add_test(NAME native_fixture_build
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/native_fixture_build_test.py")
  add_test(
    NAME stdlib_modules
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stdlib_modules_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(stdlib_modules PROPERTIES TIMEOUT 600)
  # Preserve the complete four-fixture/eight-mode matrix without an outer
  # deadline shorter than one of its permitted sanitized compilations.
  foreach(standard IN ITEMS 20 23)
    add_test(
      NAME generic_algorithm_breadth_cpp${standard}
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_breadth_test.py"
              "$<TARGET_FILE:gti>"
              --mode "c++${standard}-O0"
              --mode "c++${standard}-O1"
              --mode "c++${standard}-O3"
    )
    set_tests_properties(generic_algorithm_breadth_cpp${standard}
                         PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
  endforeach()
  add_test(
    NAME generic_algorithm_breadth_no_elide
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_breadth_test.py"
            "$<TARGET_FILE:gti>" --mode no-elide
  )
  set_tests_properties(generic_algorithm_breadth_no_elide
                       PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
  add_test(
    NAME generic_algorithm_breadth_sanitized
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/generic_algorithm_breadth_test.py"
            "$<TARGET_FILE:gti>" --mode sanitized
  )
  set_tests_properties(generic_algorithm_breadth_sanitized
                       PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
  add_test(
    NAME ordered_search_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ordered_search_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(
    ordered_search_runtime
    PROPERTIES TIMEOUT 600
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
  )
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
                   "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
                   "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
              "${PROJECT_SOURCE_DIR}/examples/046-defined-integer-arithmetic.gti"
              --shard ${shard}/4
    )
  endforeach()
  add_test(
    NAME checked_integer_overflow_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/checked_integer_overflow_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(checked_integer_overflow_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME binary64_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/binary64_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/047-binary64.gti"
  )
  add_test(
    NAME numeric_boundaries_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/numeric_boundaries_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(numeric_boundaries_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME optimizer_mutable_call_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/optimizer_mutable_call_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(optimizer_mutable_call_runtime PROPERTIES TIMEOUT 240)
  add_test(
    NAME optimizer_initializer_publication_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/optimizer_initializer_publication_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(optimizer_initializer_publication_runtime PROPERTIES TIMEOUT 300)
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
    NAME reflection_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/reflection_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_basic.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_generic_encoder.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_field_names.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_type_bridge.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_mutation.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_empty_and_nested.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_generic_struct.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_sequences.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_binary_writer.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_attributes.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_construct.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_callables.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_receiver_effects.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_deferred_range.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_defaults.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_class_assertions.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/reflection_type_facts.gti"
  )
  set_tests_properties(reflection_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME json_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/reflection_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/stdlib_surface_json.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/json_parity.gti"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/json_config.gti"
  )
  set_tests_properties(json_runtime PROPERTIES TIMEOUT 1200)
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
    NAME constexpr_mutation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_mutation.gti"
  )
  add_test(
    NAME constexpr_results_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_results.gti"
  )
  add_test(
    NAME constexpr_allocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_allocation.gti"
            --ownership-checks
  )
  add_test(
    NAME constexpr_payload_enum_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_payload_enum.gti"
            --ownership-checks
  )
  add_test(
    NAME constexpr_tryable_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_tryable.gti"
            --ownership-checks
  )
  add_test(
    NAME fresh_callable_borrow_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/fresh_callable_borrows.gti"
            --ownership-checks
  )
  add_test(
    NAME constexpr_allocation_example
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/153-constexpr-allocation.gti"
            --ownership-checks
  )
  add_test(
    NAME constexpr_enum_builders_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_enum_builders.gti"
  )
  add_test(
    NAME constexpr_builders_example
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/142-constexpr-builders.gti"
  )
  add_test(
    NAME constexpr_chrono_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constexpr_text_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/constexpr_chrono.gti"
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
            "${PROJECT_SOURCE_DIR}/examples/056-sum-types.gti"
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
    NAME vector_construction_only_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/vector_construction_only_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(vector_construction_only_runtime PROPERTIES TIMEOUT 240)
  add_test(
    NAME vector_scalar_constructor_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/vector_scalar_constructor_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(vector_scalar_constructor_runtime PROPERTIES TIMEOUT 240)
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
    NAME expected_ergonomics_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/expected_ergonomics_runtime_test.py"
            "$<TARGET_FILE:gti>")
  # Its GCC sanitized build may take minutes under a parallel suite;
  # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
  set_tests_properties(expected_ergonomics_runtime PROPERTIES TIMEOUT 1800)
  add_test(
    NAME borrowed_expected_extraction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_expected_extraction_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(borrowed_expected_extraction_runtime PROPERTIES TIMEOUT 600)
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
    NAME passive_range_temporary_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/passive_range_temporary_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME shared_payload_sibling_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/shared_payload_sibling_runtime_test.py"
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
    NAME cpp_program_linkage
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cpp_program_linkage_test.py"
            "$<TARGET_FILE:gti>"
            "${CMAKE_CXX_COMPILER}"
            "${PROJECT_SOURCE_DIR}"
            "$<TARGET_FILE:gti_runtime>"
  )
  set_tests_properties(cpp_program_linkage PROPERTIES TIMEOUT 120)
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
      NAME constructor_nested_argument_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/constructor_nested_argument_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(constructor_nested_argument_runtime
                         PROPERTIES TIMEOUT 600)
    add_test(
      NAME generic_clone_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_clone_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(generic_clone_runtime PROPERTIES TIMEOUT 240)
    add_test(NAME extensible_formatting_runtime
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/extensible_formatting_runtime_test.py"
        "$<TARGET_FILE:gti>")
    set_tests_properties(extensible_formatting_runtime PROPERTIES TIMEOUT 240)
    add_test(NAME format_output_matrix_runtime
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/format_output_matrix_runtime_test.py"
        "$<TARGET_FILE:gti>"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/format_output_matrix.expected")
    set_tests_properties(format_output_matrix_runtime PROPERTIES TIMEOUT 600)
    add_test(NAME integral_enum_formatting_runtime
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/integral_enum_formatting_runtime_test.py"
        "$<TARGET_FILE:gti>")
    set_tests_properties(integral_enum_formatting_runtime PROPERTIES TIMEOUT 300)
    add_test(NAME composable_io_runtime
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/composable_io_runtime_test.py"
        "$<TARGET_FILE:gti>")
    # Its GCC sanitized build may take minutes under a parallel suite;
    # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
    set_tests_properties(composable_io_runtime PROPERTIES TIMEOUT 1800)
    if(TARGET gti_lsp)
      add_test(NAME extensible_formatting_lsp
        COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/extensible_formatting_lsp_test.py"
          "$<TARGET_FILE:gti_lsp>")
      add_test(NAME integral_enum_lsp
        COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/integral_enum_lsp_test.py"
          "$<TARGET_FILE:gti_lsp>")
      add_test(NAME generic_enum_lsp
        COMMAND "${Python3_EXECUTABLE}"
          "${PROJECT_SOURCE_DIR}/tests/generic_enum_lsp_test.py"
          "$<TARGET_FILE:gti_lsp>")
      set_tests_properties(generic_enum_lsp PROPERTIES TIMEOUT 180)
    endif()
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
    NAME issues_212_214_regressions
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/issues_212_214_regression_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures"
  )
  set_tests_properties(issues_212_214_regressions PROPERTIES TIMEOUT 300)
  add_test(
    NAME borrowed_receiver_carriers_regressions
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_receiver_carriers_regression_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures")
  set_tests_properties(borrowed_receiver_carriers_regressions PROPERTIES TIMEOUT 600)
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
    NAME cleanup_free_moved_formal_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cleanup_free_moved_formal_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/nested_cleanup_free_moved_formal.gti"
  )
  set_tests_properties(cleanup_free_moved_formal_runtime
                       PROPERTIES TIMEOUT 240)
  add_test(
    NAME subscript_store_read_order_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/subscript_store_read_order_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(subscript_store_read_order_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(NAME operator_place_composition_runtime
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/operator_place_composition_runtime_test.py"
      "$<TARGET_FILE:gti>")
  set_tests_properties(operator_place_composition_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME coverage_composition_runtime
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/coverage_composition_runtime_test.py"
      "$<TARGET_FILE:gti>")
  set_tests_properties(coverage_composition_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME operator_place_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/operator_place_failure_runtime_test.py"
      "$<TARGET_FILE:gti>")
  set_tests_properties(operator_place_failure_runtime PROPERTIES TIMEOUT 180)
  if(TARGET gti_lsp)
    add_test(NAME operator_place_composition_lsp
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/operator_place_composition_lsp_test.py"
        "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(operator_place_composition_lsp PROPERTIES TIMEOUT 60)
  endif()
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
  # Its GCC sanitized build may take minutes under a parallel suite;
  # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
  set_tests_properties(string_operations_runtime PROPERTIES TIMEOUT 1800)
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
  # Its GCC sanitized build may take minutes under a parallel suite;
  # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
  set_tests_properties(charconv_runtime PROPERTIES TIMEOUT 1800)
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
  # Its GCC sanitized build may take minutes under a parallel suite;
  # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
  set_tests_properties(filesystem_operations_runtime PROPERTIES TIMEOUT 1800)
  add_test(
    NAME encoding_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/encoding_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  # Its GCC sanitized build may take minutes under a parallel suite;
  # see SANITIZED_BUILD_TIMEOUT in tests/sanitizer_support.py.
  set_tests_properties(encoding_runtime PROPERTIES TIMEOUT 1800)
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
    NAME mutex_move_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mutex_move_cleanup_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(mutex_move_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME function_item_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/function_item_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/function_items.gti"
  )
  set_tests_properties(function_item_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME generic_contract_runtime
           COMMAND "${Python3_EXECUTABLE}"
                   "${PROJECT_SOURCE_DIR}/tests/generic_contract_runtime_test.py"
                   "$<TARGET_FILE:gti>")
  set_tests_properties(generic_contract_runtime PROPERTIES TIMEOUT 300)
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
    NAME polymorphic_constructor_rollback_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/polymorphic_constructor_rollback_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(polymorphic_constructor_rollback_runtime PROPERTIES TIMEOUT 180)
  add_test(
    NAME interface_static_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/interface_static_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(interface_static_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME interface_static_performance
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/interface_static_performance_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_BINARY_DIR}/Testing/interface-static-cost"
  )
  set_tests_properties(interface_static_performance PROPERTIES TIMEOUT 180)
  add_test(
    NAME lambda_pack_invocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lambda_pack_invocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(lambda_pack_invocation_runtime PROPERTIES TIMEOUT 1200)
  add_test(NAME borrowing_callbacks_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowing_callbacks_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(borrowing_callbacks_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME lambda_base_reference_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lambda_base_reference_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(lambda_base_reference_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME lambda_owning_transport_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lambda_owning_transport_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(lambda_owning_transport_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME grouped_lambda_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/grouped_lambda_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(grouped_lambda_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME owning_function_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owning_function_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owning_function_runtime PROPERTIES TIMEOUT 900)
  add_test(
    NAME conditional_owning_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_owning_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_owning_argument_runtime PROPERTIES
                       TIMEOUT 600)
  add_test(
    NAME borrowed_argument_fallible_condition_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_argument_fallible_condition_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_argument_fallible_condition_runtime PROPERTIES
                       TIMEOUT 600)
  add_test(
    NAME borrowed_argument_nested_owning_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_argument_nested_owning_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_argument_nested_owning_argument_runtime
                       PROPERTIES TIMEOUT 600)
  add_test(
    NAME borrowed_argument_inner_temporary_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_argument_inner_temporary_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_argument_inner_temporary_runtime PROPERTIES
                       TIMEOUT 600)
  add_test(
    NAME conditional_converted_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_converted_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_converted_receiver_runtime PROPERTIES
                       TIMEOUT 600)
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
    NAME consuming_returned_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_returned_result_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_returned_result_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME consuming_prepared_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_prepared_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_prepared_receiver_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME consuming_conditional_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_conditional_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_conditional_receiver_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_prepared_input_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_prepared_input_result_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_prepared_input_result_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_cleanup_free_call_result_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_cleanup_free_call_result_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_cleanup_free_call_result_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_checked_arm_input_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_checked_arm_input_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_checked_arm_input_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_cleanup_free_nested_join_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_cleanup_free_nested_join_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_cleanup_free_nested_join_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_cleanup_free_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_cleanup_free_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_cleanup_free_receiver_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME lambda_class_result_consumers_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lambda_class_result_consumers_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(lambda_class_result_consumers_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME conditional_moved_local_join_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/conditional_moved_local_join_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(conditional_moved_local_join_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME moved_local_field_read_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/moved_local_field_read_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(moved_local_field_read_runtime PROPERTIES TIMEOUT 300)
  add_test(
    NAME consuming_generic_owner_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/consuming_generic_owner_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(consuming_generic_owner_receiver_runtime PROPERTIES TIMEOUT 300)
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
    NAME scoped_parallel_threads
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/scoped_parallel_threads_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(scoped_parallel_threads PROPERTIES TIMEOUT 480)
  add_test(
    NAME scoped_concurrency_installed
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/scoped_concurrency_install_test.py"
            "${PROJECT_BINARY_DIR}"
  )
  set_tests_properties(scoped_concurrency_installed PROPERTIES TIMEOUT 300)
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
    NAME hosted_exit_status_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/hosted_exit_status_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(hosted_exit_status_runtime PROPERTIES TIMEOUT 480)
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
  add_test(NAME storage_final_address_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/storage_final_address_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(storage_final_address_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME raw_owner_witness_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_owner_witness_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(raw_owner_witness_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME raw_pointer_conversion_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_pointer_conversion_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(raw_pointer_conversion_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME native_c_scalar_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/native_c_scalar_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(native_c_scalar_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME nested_raw_pointer_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_raw_pointer_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(nested_raw_pointer_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME nested_raw_pointer_native_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/nested_raw_pointer_native_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(nested_raw_pointer_native_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME raw_record_containers_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_record_containers_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(raw_record_containers_runtime PROPERTIES TIMEOUT 600)
  add_test(NAME raw_native_record_transport_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/raw_native_record_transport_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(raw_native_record_transport_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructor_field_publication_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_field_publication_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_field_publication_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructor_member_call_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_member_call_field_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_member_call_field_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME constructor_virtual_call_field_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/constructor_virtual_call_field_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(constructor_virtual_call_field_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME virtual_temporary_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/virtual_temporary_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(virtual_temporary_receiver_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME fresh_callable_later_argument_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/fresh_callable_later_argument_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(fresh_callable_later_argument_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME borrowed_enum_field_cleanup_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_enum_field_cleanup_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_enum_field_cleanup_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME borrowed_enum_terminal_contexts_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_enum_terminal_contexts_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_enum_terminal_contexts_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME borrowed_enum_scalar_sibling_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/borrowed_enum_scalar_sibling_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(borrowed_enum_scalar_sibling_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME contextual_borrowed_construction_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/contextual_borrowed_construction_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(contextual_borrowed_construction_runtime PROPERTIES TIMEOUT 600)
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
    NAME structural_relocation_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/structural_relocation_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(structural_relocation_runtime PROPERTIES TIMEOUT 600)

  add_test(NAME compact_owner_layout_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/compact_owner_layout_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(compact_owner_layout_runtime PROPERTIES TIMEOUT 600)
  if(TARGET gti_lsp)
    add_test(
      NAME structural_relocation_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/structural_relocation_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(structural_relocation_lsp PROPERTIES TIMEOUT 120)
  endif()
  if(TARGET gti_lsp)
    add_test(NAME restoring_exchange_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/restoring_exchange_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(restoring_exchange_lsp PROPERTIES TIMEOUT 120)
  endif()
  add_test(
    NAME owning_function_policy_failure_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/owning_function_policy_failure_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(owning_function_policy_failure_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME inline_polymorphic_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/inline_polymorphic_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(inline_polymorphic_runtime PROPERTIES TIMEOUT 900)
  if(UNIX)
    add_test(
      NAME owned_inline_consumption_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/owned_inline_consumption_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(owned_inline_consumption_runtime PROPERTIES TIMEOUT 600)
  endif()
  add_test(
    NAME inline_closure_owner_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/inline_closure_owner_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(inline_closure_owner_runtime PROPERTIES TIMEOUT 600)
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
  if(TARGET gti_lsp)
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
      NAME missing_definition_cli_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/missing_definition_test.py"
              "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(missing_definition_cli_lsp PROPERTIES TIMEOUT 180)
  endif()
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
      NAME constructor_borrow_replacement_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/constructor_borrow_replacement_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(constructor_borrow_replacement_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME constructor_text_reference_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/constructor_text_reference_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(constructor_text_reference_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME allocation_default_argument_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/allocation_default_argument_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(allocation_default_argument_runtime PROPERTIES TIMEOUT 240)
    add_test(
      NAME reference_class_assignment_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/reference_class_assignment_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(reference_class_assignment_runtime PROPERTIES TIMEOUT 300)
    add_test(
      NAME mutable_temporary_receiver_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/mutable_temporary_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(mutable_temporary_receiver_runtime
                       PROPERTIES TIMEOUT 180)
  add_test(
    NAME ephemeral_carrier_receiver_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ephemeral_carrier_receiver_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(ephemeral_carrier_receiver_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(
    NAME temporary_constructor_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/temporary_constructor_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(temporary_constructor_argument_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(
    NAME short_circuit_class_temporary_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/short_circuit_class_temporary_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(short_circuit_class_temporary_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(
    NAME short_circuit_temporary_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/short_circuit_temporary_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(short_circuit_temporary_argument_runtime
                       PROPERTIES TIMEOUT 300)
  add_test(
    NAME short_circuit_payload_enum_argument_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/short_circuit_payload_enum_argument_runtime_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(short_circuit_payload_enum_argument_runtime
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
      NAME generic_payload_enum_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_payload_enum_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(generic_payload_enum_runtime PROPERTIES TIMEOUT 300)
    add_test(
      NAME generic_enum_instances_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/generic_enum_instances_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(generic_enum_instances_runtime PROPERTIES TIMEOUT 600)
    add_test(
      NAME ordered_borrow_composition_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/ordered_borrow_composition_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(ordered_borrow_composition_runtime PROPERTIES TIMEOUT 180)
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
    set_tests_properties(borrowed_payload_pattern_runtime PROPERTIES TIMEOUT 600)
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
    add_test(NAME borrow_origin_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/borrow_origin_runtime_test.py"
              "$<TARGET_FILE:gti>")
    set_tests_properties(borrow_origin_runtime PROPERTIES TIMEOUT 600)
    add_test(NAME owner_chain_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/owner_chain_runtime_test.py"
              "$<TARGET_FILE:gti>")
    set_tests_properties(owner_chain_runtime PROPERTIES TIMEOUT 300)
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
    NAME llvm_backend_cli
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/llvm_backend_cli_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(llvm_backend_cli PROPERTIES TIMEOUT 120)
  add_test(
    NAME cli_workflow
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_smoke_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME cli_hardlink_fixture
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/cli_hardlink_fixture_test.py"
  )
  add_test(
    NAME cli_color
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_color_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(cli_color PROPERTIES TIMEOUT 180)
  add_executable(gti_cli_progress_probe
    cli_progress_probe.cpp
    "${PROJECT_SOURCE_DIR}/src/cli/build_progress.cpp"
    "${PROJECT_SOURCE_DIR}/src/cli/terminal_style.cpp")
  target_include_directories(gti_cli_progress_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src/cli")
  target_include_directories(gti_cli_progress_probe SYSTEM PRIVATE ${GTI_LLVM_INCLUDE_DIRS})
  target_link_libraries(gti_cli_progress_probe PRIVATE LLVMSupport Threads::Threads)
  add_test(
    NAME cli_progress
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_progress_test.py"
            "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_cli_progress_probe>"
  )
  set_tests_properties(cli_progress PROPERTIES TIMEOUT 180)
  add_test(
    NAME cli_native_dependency_progress
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_native_dependency_progress_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(cli_native_dependency_progress PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
  add_test(
    NAME cli_install
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/cli_install_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(cli_install PROPERTIES TIMEOUT 300)
  add_test(
    NAME project_cli_workflow
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/project_cli_smoke_test.py"
            "$<TARGET_FILE:gti>"
  )
  add_test(
    NAME annotated_test_suites
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/annotated_test_suite_test.py"
            "$<TARGET_FILE:gti>"
  )
  set_tests_properties(annotated_test_suites PROPERTIES TIMEOUT 600)
  add_test(NAME test_command_controls
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/test_command_controls_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(test_command_controls PROPERTIES TIMEOUT 600)
  add_test(NAME testing_assertions_runtime
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/testing_assertions_runtime_test.py"
            "$<TARGET_FILE:gti>")
  set_tests_properties(testing_assertions_runtime PROPERTIES TIMEOUT 600)
  add_test(
    NAME scoped_configuration_defines
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/scoped_configuration_defines_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/examples/scoped-configuration"
  )
  set_tests_properties(scoped_configuration_defines PROPERTIES TIMEOUT 180)
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
  )
  add_test(
    NAME single_body_compile_scaling
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/single_body_compile_scaling_test.py"
            "$<TARGET_FILE:gti>"
  )
  # It compares compile CPU time across body sizes; concurrent tests on a
  # small runner skew that ratio, so it runs alone.
  set_tests_properties(single_body_compile_scaling PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
  add_test(
    NAME single_body_scaling_measurement
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/single_body_scaling_measurement_test.py"
  )
  set_tests_properties(single_body_scaling_measurement PROPERTIES TIMEOUT 30)

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
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
  )
  add_test(
    NAME random_engine_runtime
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/random_engine_runtime_test.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/random_engine.gti"
  )
  add_test(
    NAME language_coverage_corpus
    COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/coverage/gate.py"
            "$<TARGET_FILE:gti>"
            "${PROJECT_BINARY_DIR}/language-coverage-corpus"
            --jobs 2
  )
  set_tests_properties(
    language_coverage_corpus
    PROPERTIES TIMEOUT 900
               PROCESSORS 2
               ENVIRONMENT
                 "GTI_STDLIB_PATH=${PROJECT_SOURCE_DIR}/stdlib;GTI_RUNTIME_INCLUDE=${PROJECT_SOURCE_DIR}/runtime/include;GTI_RUNTIME_LIBRARY=$<TARGET_FILE:gti_runtime>"
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
    add_test(NAME thread_local_end_to_end
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/thread_local_runtime_test.py"
        "$<TARGET_FILE:gti>")
    set_tests_properties(thread_local_end_to_end PROPERTIES TIMEOUT 900)
    add_test(NAME thread_local_owner_assignment
      COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/thread_local_owner_assignment_test.py"
        "$<TARGET_FILE:gti>")
    set_tests_properties(thread_local_owner_assignment PROPERTIES TIMEOUT 300)
    add_test(
      NAME managed_thread_tsan
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/managed_thread_tsan_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/managed_thread_class_result.gti"
              "${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(managed_thread_tsan PROPERTIES TIMEOUT 300)
    foreach(fixture shared_mutex_threads transfer_function_threads
                    shared_ownership_threads weak_ownership_threads
                    shared_ownership_release_race
                    scoped_parallel_threads scoped_parallel_cleanup)
      add_test(
        NAME ${fixture}_tsan
        COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/managed_thread_tsan_test.py"
                "$<TARGET_FILE:gti>"
                "${PROJECT_SOURCE_DIR}/tests/fixtures/${fixture}.gti"
                "${CMAKE_CXX_COMPILER}"
                "$<TARGET_FILE:gti_llvm_instrumented_source>"
                "${LLVM_TOOLS_BINARY_DIR}/clang++"
      )
      set_tests_properties(${fixture}_tsan PROPERTIES TIMEOUT 300)
    endforeach()
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
      NAME atomic_wait_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/atomic_wait_runtime_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_wait.gti"
              "${PROJECT_SOURCE_DIR}/examples/076-scalar-atomics.gti"
    )
    set_tests_properties(atomic_wait_runtime PROPERTIES TIMEOUT 600)
    add_test(
      NAME atomic_tsan
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/atomic_tsan_test.py"
              "$<TARGET_FILE:gti>"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_concurrent.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_orders.gti"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_fetch.gti"
              "${CMAKE_CXX_COMPILER}"
              "${PROJECT_SOURCE_DIR}/tests/fixtures/atomic_wait.gti"
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
      NAME dormant_capability_runtime
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/dormant_capability_runtime_test.py"
              "$<TARGET_FILE:gti>"
    )
    set_tests_properties(dormant_capability_runtime PROPERTIES TIMEOUT 300)
    if(GTI_ENABLE_THREAD_SANITIZER_TESTS)
      set(gti_tsan_supported 1)
    else()
      set(gti_tsan_supported 0)
    endif()
    set_tests_properties(
      cthread_runtime
      cthread_sleep_runtime
      cthread_mutex_runtime
      cthread_condition_runtime
      cthread_once_runtime
      cthread_create_join_runtime
      managed_thread_tsan
      shared_mutex_threads_tsan
      transfer_function_threads_tsan
      shared_ownership_threads_tsan
      weak_ownership_threads_tsan
      shared_ownership_release_race_tsan
      scoped_parallel_threads_tsan
      scoped_parallel_cleanup_tsan
      atomic_tsan
      mutex_tsan
      PROPERTIES ENVIRONMENT "GTI_TSAN_SUPPORTED=${gti_tsan_supported}"
    )
    set_tests_properties(
      managed_thread_tsan shared_mutex_threads_tsan transfer_function_threads_tsan
      shared_ownership_threads_tsan weak_ownership_threads_tsan
      shared_ownership_release_race_tsan
      scoped_parallel_threads_tsan scoped_parallel_cleanup_tsan
      atomic_tsan mutex_tsan
      PROPERTIES SKIP_RETURN_CODE 77
    )
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
      NAME diagnostic_explanations
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/diagnostic_explanations_test.py"
              "$<TARGET_FILE:gti>" "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(diagnostic_explanations PROPERTIES TIMEOUT 120)
    add_test(
      NAME lsp_repair_contract
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_repair_contract_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(lsp_repair_contract PROPERTIES TIMEOUT 120)
    add_test(
      NAME lsp_include_suggestions
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_include_suggestions_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(lsp_include_suggestions PROPERTIES TIMEOUT 120)
    add_test(
      NAME lsp_project_facts
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_project_facts_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME lsp_configuration
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_configuration_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(lsp_configuration PROPERTIES TIMEOUT 120)
    add_test(
      NAME lsp_test_target
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/lsp_test_target_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(lsp_test_target PROPERTIES TIMEOUT 90)
    find_program(GTI_NEOVIM_EXECUTABLE nvim)
    if(GTI_NEOVIM_EXECUTABLE)
      add_test(NAME neovim_gtiup
        COMMAND "${CMAKE_COMMAND}" -E env
                "GTI_TEST_REPOSITORY=${PROJECT_SOURCE_DIR}"
                "${GTI_NEOVIM_EXECUTABLE}" --headless -u NONE -i NONE
                -l "${PROJECT_SOURCE_DIR}/tests/nvim_gtiup_test.lua")
      add_test(
        NAME neovim_lsp_configuration
        COMMAND "${CMAKE_COMMAND}" -E env
                "GTI_TEST_REPOSITORY=${PROJECT_SOURCE_DIR}"
                "GTI_TEST_LSP=$<TARGET_FILE:gti_lsp>"
                "GTI_TEST_COMPILER=$<TARGET_FILE:gti>"
                "XDG_STATE_HOME=${PROJECT_BINARY_DIR}/nvim-test/state"
                "XDG_CACHE_HOME=${PROJECT_BINARY_DIR}/nvim-test/cache"
                "XDG_DATA_HOME=${PROJECT_BINARY_DIR}/nvim-test/data"
                "NVIM_LOG_FILE=${PROJECT_BINARY_DIR}/nvim-test.log"
                "${GTI_NEOVIM_EXECUTABLE}" --headless -u NONE -i NONE
                -l "${PROJECT_SOURCE_DIR}/tests/nvim_lsp_configuration_test.lua"
      )
      set_tests_properties(neovim_lsp_configuration PROPERTIES TIMEOUT 90
                                                            SKIP_RETURN_CODE 77)
    endif()
    add_test(
      NAME lsp_protocol
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME interface_static_lsp
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>" interface-static
    )
    set_tests_properties(interface_static_lsp PROPERTIES TIMEOUT 120)
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
      NAME concurrency_composition_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/concurrency_composition_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(concurrency_composition_lsp PROPERTIES TIMEOUT 120)
    add_test(
      NAME expected_payload_range_lsp
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/lsp_smoke_test.py"
              "$<TARGET_FILE:gti_lsp>" expected-payload-range
    )
    add_test(
      NAME optional_contract_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/optional_contract_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME source_expected_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/source_expected_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    set_tests_properties(source_expected_lsp PROPERTIES TIMEOUT 120)
    add_test(
      NAME expected_transform_error_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/expected_transform_error_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(
      NAME borrowed_expected_extraction_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/borrowed_expected_extraction_lsp_test.py"
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
    add_test(
      NAME comment_placement_lsp
      COMMAND "${Python3_EXECUTABLE}"
              "${PROJECT_SOURCE_DIR}/tests/comment_placement_lsp_test.py"
              "$<TARGET_FILE:gti_lsp>"
    )
    add_test(NAME discardable_result_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/discardable_result_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME unsafe_function_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/unsafe_function_lsp_test.py"
                   "$<TARGET_FILE:gti_lsp>")
    add_test(NAME export_placement_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/export_placement_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME diagnostic_recovery_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/diagnostic_recovery_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(diagnostic_recovery_lsp PROPERTIES TIMEOUT 60)
    add_test(NAME runtime_object_layout_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/runtime_object_layout_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME resource_protocol_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/resource_protocol_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME structural_class_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/structural_class_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME cthread_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/cthread_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME generic_advisory_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/generic_advisory_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME generic_contract_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/generic_contract_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    add_test(NAME general_requires_lsp
             COMMAND "${Python3_EXECUTABLE}"
                     "${PROJECT_SOURCE_DIR}/tests/general_requires_lsp_test.py"
                     "$<TARGET_FILE:gti_lsp>")
    set_tests_properties(general_requires_lsp PROPERTIES TIMEOUT 60)
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


add_test(NAME compiled_type_forwarding
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_type_forwarding_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-type-forwarding")
set_tests_properties(compiled_type_forwarding PROPERTIES TIMEOUT 600)
add_test(NAME compiled_namespaces
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_namespaces_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_CURRENT_BINARY_DIR}/compiled-namespaces")
set_tests_properties(compiled_namespaces PROPERTIES TIMEOUT 600)
add_test(NAME compiled_value_types
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_value_types_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-value-types")
set_tests_properties(compiled_value_types PROPERTIES TIMEOUT 600)
add_test(NAME compiled_borrow_library
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_borrow_library_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-borrow-library")
set_tests_properties(compiled_borrow_library PROPERTIES TIMEOUT 600)
add_executable(gti_compiled_borrow_contract_tests compiled_borrow_contract_tests.cpp)
target_link_libraries(gti_compiled_borrow_contract_tests PRIVATE gti_compiler)
add_test(NAME compiled_borrow_contract COMMAND gti_compiled_borrow_contract_tests)
add_executable(gti_compiled_interface_types_tests compiled_interface_types_tests.cpp)
target_link_libraries(gti_compiled_interface_types_tests PRIVATE gti_compiler)
add_test(NAME compiled_interface_types COMMAND gti_compiled_interface_types_tests)

add_test(NAME compiled_dependency_project
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/compiled_dependency_project_test.py"
  "$<TARGET_FILE:gti>" --output-dir "${CMAKE_BINARY_DIR}/compiled-dependency-project")
set_tests_properties(compiled_dependency_project PROPERTIES TIMEOUT 600)

include("${PROJECT_SOURCE_DIR}/tests/TestThroughput.cmake")

# Every build-tree test runs the compiler's self-checks, such as the
# lowered-program construction seal, that ordinary compiles skip. Tests that
# exercise a self-check also enable it explicitly, so they do not depend on
# this inventory.
get_property(gti_validation_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${gti_validation_tests} APPEND PROPERTY ENVIRONMENT
  "GTI_COMPILER_VALIDATION=1")

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
  )
endif()
