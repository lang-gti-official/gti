# One suite occupies exactly its bounded case-worker allocation in CTest.
# This is test orchestration only; no case, mode, assertion or timeout is removed.
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
  generic_algorithm_breadth complete_containers)

if(Python3_Interpreter_FOUND)
  set(gti_parallel_case_suites
    callable_return_type_runtime
    borrowed_enum_field_cleanup_runtime
    borrowed_enum_scalar_sibling_runtime
    borrowed_enum_terminal_contexts_runtime
    contextual_borrowed_construction_runtime
    enum_replacement_runtime
    enum_selector_runtime
    protected_member_runtime
    out_of_class_cleanup
    implicit_receiver_owned_assignment_runtime
    aggregate_std_runtime
    operator_relay_runtime
    ordered_search_runtime
    abstract_generic_failure_contract_runtime
    associative_constructor_runtime associative_hash_contracts associative_map
    associative_ownership_runtime borrow_origin_runtime borrowing_composition
    borrowing_callbacks_runtime charconv_runtime
    cleanup_bound_borrow_runtime complete_containers composable_io_runtime
    conditional_capability_runtime conditional_converted_receiver_runtime
    conditional_owning_argument_runtime borrowed_callable_conditionals_runtime
    borrowed_argument_fallible_condition_runtime
    borrowed_argument_nested_owning_argument_runtime
    borrowed_argument_inner_temporary_runtime
    conditional_cleanup_free_call_result_runtime
    conditional_checked_arm_input_runtime
    conditional_cleanup_free_nested_join_runtime
    conditional_cleanup_free_receiver_runtime
    conditional_moved_local_join_runtime
    moved_local_field_read_runtime
    conditional_prepared_input_result_runtime
    constructed_callback_argument_runtime constructor_argument_staging_runtime
    constructor_field_publication_runtime constructor_member_call_field_runtime
    constructor_virtual_call_field_runtime virtual_temporary_receiver_runtime
    constructor_null_native_runtime constructor_passive_field_rollback_runtime
    contiguous_iterator_completion container_iterator_movement
    paired_span_iterator_completion cursor_carrier_publication
    range_endpoint_parking_runtime user_dual_range_lifetime
    user_owning_reference_replacement
    class_conditional_join_runtime
    comma_class_results_runtime
    construction_assignment_runtime
    consuming_call_assignment_runtime consuming_lambda_runtime consuming_methods_runtime consuming_owned_pointee_runtime
    consuming_prepared_receiver_runtime
    consuming_receiver_runtime consuming_retained_field_replace_runtime
    consuming_conditional_receiver_runtime consuming_returned_result_runtime
    consuming_generic_owner_receiver_runtime
    consuming_retained_field_runtime consuming_retained_lambda_runtime
    contextual_numeric_literals_runtime discarded_nominal_result_runtime
    dormant_capability_runtime
    expected_class_argument_runtime expected_class_assignment_runtime
    expected_boundary_replacement_runtime
    raw_receiver_owned_argument_runtime
    expected_cleanup_free_copy_runtime expected_cleanup_free_move_runtime
    expected_call_reassignment_runtime expected_lambda_class_argument_runtime
    expected_member_argument_runtime
    expected_conditional_assignment_runtime expected_conditional_arms_runtime
    expected_class_return_runtime expected_constructor_return_runtime expected_consuming_return_runtime
    mixed_constructor_parameter_runtime
    moved_passive_constructor_field_runtime
    deferred_bound_field_runtime
    passive_text_view_class_runtime
    view_fields_runtime text_reference_accessors_runtime
    view_field_payloads_runtime
    recursive_text_relays_runtime
    declaration_owned_defaults_runtime
    base_rollback_record_runtime virtual_expected_owning_return_runtime
    reference_owned_field_replacement_runtime
    raw_owned_field_replacement_runtime
    shared_getter_worker_loans_runtime
    expected_field_and_join_runtime expected_scoped_return_runtime
    expected_temporary_observation_runtime
    expected_filesystem_prerequisites_runtime expected_passive_publication_runtime
    expected_payload_pair_arguments_runtime expected_payload_pending_cleanup_runtime
    expected_payload_short_circuit_runtime
    extensible_formatting_runtime fresh_callable_later_argument_failure_runtime
    failure_free_passive_moves_runtime
    generated_base_constructor_runtime generated_constructor_order_runtime generic_algorithm_breadth
    generic_callable_adapter_runtime generic_callable_transport_runtime generic_contract_runtime
    coverage_composition_runtime
    generic_enum_instances_runtime generic_payload_enum_runtime hosted_constructor_identity_runtime
    hosted_exit_status_runtime
    returned_field_address_runtime
    http_url_operators immovable_storage_runtime integral_enum_formatting_runtime
    grouped_lambda_runtime
    lambda_base_reference_runtime lambda_owning_transport_runtime lambda_pack_invocation_runtime
    lambda_class_result_consumers_runtime
    nested_borrow_fields nested_guarded_expected_runtime
    native_c_scalar_runtime nested_raw_pointer_runtime
    nested_nominal_result_runtime operator_place_composition_runtime owned_expected_error_runtime
    object_construction_runtime object_relocation_runtime
    owned_pointee_projection_runtime owned_unexpected_pending_cleanup_runtime
    owning_function_resource_result_runtime owning_function_runtime
    owned_argument_member_initializer_runtime
    owning_expected_field_runtime
    owning_conditional_join_runtime
    owning_temporary_field_read_runtime
    plain_call_member_initializer_runtime
    short_circuit_field_view_runtime
    short_circuit_temporary_argument_runtime
    short_circuit_payload_enum_argument_runtime
    pack_prefix_constructor_failure_runtime private_constructor_placement_runtime
    programmable_allocators_runtime prvalue_direct_initialization_runtime
    raw_owner_witness_runtime raw_pointer_conversion_runtime
    reference_class_assignment_runtime
    constructor_borrow_replacement_runtime
    constructor_text_reference_runtime
    constructor_copy_after_formal_write_runtime
    assignment_result_projection_runtime
    assignment_result_whole_value_runtime
    allocation_default_argument_runtime
    restoring_exchange_runtime restoring_exchange_shapes_runtime restoring_indexed_runtime
    retained_owner_member_pack_runtime shared_accessor_range_runtime
    shared_ownership_runtime shared_ownership_threads scoped_parallel_threads
    source_storage_drain_runtime
    source_prefix_owner_runtime source_resource_prefix_runtime
    source_text_address_runtime source_prefix_string_runtime
    many_fallible_scope_guards_runtime
    optimizer_raw_context_runtime optimizer_object_context_runtime
    inherited_cleanup_context_runtime
    cleanup_context_scope_runtime
    storage_final_address_runtime string_operations_runtime string_ordering_runtime
    structural_relocation_runtime temporary_owner_borrow_runtime unordered_map
    interface_static_runtime
    uninitialized_carrier_runtime
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
  # Long single-process gates get the same cold-tree ordering hint. A fresh
  # CTest tree knows no durations, so without a hint these start late and run
  # alone at the end. Values are measured seconds from a full run, used only
  # for scheduling order; no case, assertion or timeout changes.
  set(gti_long_gate_costs
    failure_metadata 520
    cpp_mir_body_emitter_corpus 500
    allocator_raw_audit_independence 290
    operator_relay_pipeline 285
    filesystem_operations_runtime 220
    language_coverage_corpus 170
    complete_containers_collection 165
    class_expression_consumers_runtime 155
    aggregate_projection_runtime 140
    panic_runtime 140
    container_contract_failure 115
    cli_workflow 95
    mir_cutover_corpus_oracle 85
    constructor_pack_field_runtime 85
    expected_transform_error_runtime 85
    pack_parameter_runtime 85
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
      set_tests_properties("${gti_long_gate}" PROPERTIES COST "${gti_long_gate_cost}")
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
