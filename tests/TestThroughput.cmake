# One suite occupies exactly its bounded case-worker allocation in CTest.
# This is test orchestration only; no case, mode, assertion or timeout is removed.
set(GTI_TEST_CASE_JOBS "2" CACHE STRING "Bounded workers per parallel test suite")
if(NOT GTI_TEST_CASE_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "GTI_TEST_CASE_JOBS must be a positive integer")
endif()

if(Python3_Interpreter_FOUND)
  set(gti_parallel_case_suites
    abstract_generic_failure_contract_runtime
    associative_constructor_runtime associative_hash_contracts associative_map
    associative_ownership_runtime borrowing_composition charconv_runtime
    cleanup_bound_borrow_runtime complete_containers composable_io_runtime
    constructed_callback_argument_runtime constructor_argument_staging_runtime
    constructor_field_publication_runtime constructor_passive_field_rollback_runtime
    consuming_call_assignment_runtime consuming_lambda_runtime consuming_methods_runtime consuming_owned_pointee_runtime
    consuming_receiver_runtime consuming_retained_field_replace_runtime
    consuming_retained_field_runtime consuming_retained_lambda_runtime
    contextual_numeric_literals_runtime discarded_nominal_result_runtime
    expected_class_return_runtime expected_filesystem_prerequisites_runtime expected_passive_publication_runtime
    expected_payload_pair_arguments_runtime expected_payload_pending_cleanup_runtime
    extensible_formatting_runtime fresh_callable_later_argument_failure_runtime
    generated_base_constructor_runtime generic_algorithm_breadth
    generic_callable_adapter_runtime generic_callable_transport_runtime hosted_constructor_identity_runtime
    generic_enum_instances_runtime
    hosted_exit_status_runtime
    http_url_operators immovable_storage_runtime integral_enum_formatting_runtime
    lambda_pack_invocation_runtime nested_borrow_fields nested_guarded_expected_runtime
    native_c_scalar_runtime nested_raw_pointer_runtime
    nested_nominal_result_runtime owned_expected_error_runtime
    owned_pointee_projection_runtime owned_unexpected_pending_cleanup_runtime
    owning_function_resource_result_runtime owning_function_runtime
    pack_prefix_constructor_failure_runtime private_constructor_placement_runtime
    programmable_allocators_runtime raw_owner_witness_runtime raw_pointer_conversion_runtime
    restoring_exchange_runtime restoring_exchange_shapes_runtime restoring_indexed_runtime
    retained_owner_member_pack_runtime shared_accessor_range_runtime
    shared_ownership_runtime shared_ownership_threads
    storage_final_address_runtime string_operations_runtime string_ordering_runtime
    structural_relocation_runtime temporary_owner_borrow_runtime unordered_map
    stdlib_surface
  )
  foreach(suite IN LISTS gti_parallel_case_suites)
    if(NOT TEST "${suite}")
      message(FATAL_ERROR "Parallel case suite is not registered: ${suite}")
    endif()
    set_property(TEST "${suite}" APPEND PROPERTY ENVIRONMENT
      "GTI_TEST_CASE_JOBS=${GTI_TEST_CASE_JOBS}")
    set_tests_properties("${suite}" PROPERTIES PROCESSORS "${GTI_TEST_CASE_JOBS}")
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
  add_test(NAME test_case_jobs
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/case_jobs_test.py")
  add_test(NAME test_throughput_registration
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/test_throughput_registration_test.py"
      "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
endif()

# Full is a census of every registered gate, never a hand-maintained subset.
# Fast is deliberately partial feedback, not completion/release evidence.
get_property(gti_throughput_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${gti_throughput_tests} APPEND PROPERTY LABELS "coverage-full")
set(gti_fast_feedback_tests
  compiler_library_boundary cpp_backend_library_boundary driver_library_boundary
  project_model executable_entry_semantics include_suggestions
  raw_pointer_pipeline raw_borrow_contract raw_access_contract
  mir_full_expression failure_metadata mir_program_initialization mir_hosted_startup
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
