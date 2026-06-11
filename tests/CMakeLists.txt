find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(add_returnguard_test name source mode warning_count expected_text)
    add_test(
        NAME ${name}
        COMMAND
            ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/run_expect.py"
            --tool "$<TARGET_FILE:returnguard>"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/${source}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/include"
            --include "${CMAKE_SOURCE_DIR}/include"
            --mode "${mode}"
            --warning-count "${warning_count}"
            --expect "${expected_text}"
            ${ARGN}
    )
endfunction()

function(add_returnguard_clean_test name source mode)
    add_test(
        NAME ${name}
        COMMAND
            ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/run_expect.py"
            --tool "$<TARGET_FILE:returnguard>"
            --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/${source}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/include"
            --include "${CMAKE_SOURCE_DIR}/include"
            --mode "${mode}"
            --warning-count 0
            ${ARGN}
    )
endfunction()

add_returnguard_test(
    returnguard.macros macros.c practical 1 "missing 32"
    --expect "macros.c:"
    --expect "int status = CALL_STATUS();"
    --reject "status_api.h:")
add_returnguard_test(returnguard.enums enums.c practical 1 "missing STATUS_DENIED")
add_returnguard_test(returnguard.integers integers.c strict 1 "missing 32")
add_returnguard_test(returnguard.inference inference.c strict 2 "no final else")
add_returnguard_test(returnguard.inference_complex inference_complex.c strict 6 "missing")
add_returnguard_test(returnguard.inference_cfg inference_cfg.c strict 1 "no final else")
add_returnguard_test(returnguard.return_flow_cfg return_flow_cfg.c strict 3 "CFG-proven")
add_returnguard_test(returnguard.if_chains if_chains.c practical 1 "missing 32")
add_returnguard_test(returnguard.direct_if direct_if.c practical 1 "missing 4, 32")
add_returnguard_test(returnguard.loop_conditions loop_conditions.c practical 2 "missing 32")
add_returnguard_test(returnguard.assignment_conditions assignment_conditions.c practical 2 "missing 32")
add_returnguard_test(
    returnguard.conditional_expressions conditional_expressions.c strict 1
    "consumed but not verified")
add_returnguard_test(returnguard.guard_clauses guard_clauses.c strict 1 "missing 4, 32")
add_returnguard_test(returnguard.complex_guard_clauses complex_guard_clauses.c strict 1 "missing 4, 32")
add_returnguard_test(returnguard.noreturn_guards noreturn_guards.c strict 1 "missing 1")
add_returnguard_test(
    returnguard.aliases aliases.c practical 2 "missing 4, 32"
    --expect "stored but never used")
add_returnguard_test(returnguard.reassignment reassignment.c practical 1 "stored but never used")
add_returnguard_test(returnguard.forwarded forwarded.c strict 1 "consumed but not verified")
add_returnguard_test(returnguard.nested_calls nested_calls.c strict 2 "consumed but not verified")
add_returnguard_test(
    returnguard.nested_storage_and_switch nested_storage_and_switch.c strict 2
    "consumed but not verified")
add_returnguard_test(returnguard.comma_discard comma_discard.c ignored-only 1 "is not handled")
add_returnguard_test(returnguard.comma_value comma_value.c practical 1 "missing 32")
add_returnguard_test(returnguard.compound_assignment compound_assignment.c strict 1 "consumed but not verified")
add_returnguard_test(returnguard.annotation_redeclaration annotation_redeclaration.c practical 1 "missing 4, 32")
add_returnguard_test(returnguard.switches switches.c strict 1 "open-ended")
add_returnguard_test(returnguard.ignored ignored.c ignored-only 1 "return value of 'annotated_status'")
add_returnguard_test(returnguard.must_check must_check.c practical 2 "must_check_status")
add_returnguard_test(returnguard.nodiscard nodiscard.cpp practical 1 "nodiscard_status")
add_returnguard_clean_test(returnguard.bool_predicate bool_predicate.c practical)
add_returnguard_test(returnguard.bool_predicate_strict bool_predicate.c strict 1 "ready")
add_returnguard_clean_test(returnguard.bool_binding_condition bool_binding_condition.c practical)
add_returnguard_test(returnguard.cpp_calls cpp_calls.cpp practical 1 "missing 4, 32")
add_returnguard_clean_test(returnguard.std_value_helpers std_value_helpers.cpp strict)
add_returnguard_test(returnguard.pointer_flow pointer_flow.c practical 1 "missing 4, 32")
add_returnguard_test(returnguard.safety safety.c practical 6 "returnguard safety")
add_returnguard_test(returnguard.safety_cpp safety.cpp practical 6 "returnguard safety")
add_returnguard_clean_test(returnguard.safety_header_default safety_header_default.c practical)
add_returnguard_test(
    returnguard.safety_header_explicit safety_header_default.c practical 1
    "address of stack-allocated variable returned"
    --tool-arg=--analyze-headers)
add_returnguard_test(returnguard.safety_lifetime safety_lifetime.c practical 9 "returnguard safety")
add_returnguard_test(returnguard.safety_lifetime_cpp safety_lifetime.cpp practical 4 "returnguard safety")
add_returnguard_test(returnguard.kernel_lifetime kernel_lifetime.c practical 2 "returnguard safety")
add_returnguard_test(
    returnguard.function_config_lifetime function_config_lifetime.c practical 2
    "returnguard safety"
    --tool-arg=--function-config=${CMAKE_CURRENT_SOURCE_DIR}/cases/function_config.rg)
add_returnguard_test(
    returnguard.complex_array_index_and_oob complex_array_index_and_oob.c practical 1
    "nullable_bytes")
add_returnguard_test(
    returnguard.complex_double_free_and_uaf complex_double_free_and_uaf.c practical 2
    "potential double free"
    --expect "use of memory after it was freed")
add_returnguard_test(
    returnguard.complex_function_pointer_and_callbacks
    complex_function_pointer_and_callbacks.c practical 1 "nullable_cb")
add_returnguard_test(
    returnguard.complex_memcpy_aliasing complex_memcpy_aliasing.c practical 1
    "nullable_intptr")
add_returnguard_test(
    returnguard.complex_nested_member_return complex_nested_member_return.c practical 1
    "nullable_top")
add_returnguard_test(
    returnguard.complex_pointer_copy_and_return complex_pointer_copy_and_return.c practical 1
    "copy_and_return")
add_returnguard_clean_test(
    returnguard.complex_pointer_to_pointer_aliasing complex_pointer_to_pointer_aliasing.c
    practical)
add_returnguard_test(
    returnguard.complex_shift_overflow complex_shift_overflow.c practical 1
    "signed left shift")
add_returnguard_test(
    returnguard.null_state null_state.c practical 10
    "is dereferenced without a prior null check"
    --expect "pointer returned here is marked nullable")
add_returnguard_test(
    returnguard.null_state_contract null_state_contract.c practical 2
    "potentially-null return value"
    --tool-arg=--contract=configured_failure=null)
add_returnguard_test(
    returnguard.null_state_inference null_state_inference.c practical 2
    "is dereferenced without a prior null check")
add_returnguard_test(
    returnguard.null_state_strict null_state_strict.c strict 2
    "is dereferenced without a prior null check"
    --expect "consumed but not verified")
add_returnguard_test(
    returnguard.fail_on_diagnostics ignored.c ignored-only 1
    "return value of 'annotated_status'"
    --fail-on-diagnostics
    --diagnostic-kind error
    --expected-exit-code 1)

include("${CMAKE_CURRENT_SOURCE_DIR}/null_state_cpp_test.cmake")

set(RETURNGUARD_PROJECT_RUNNER
    "${CMAKE_SOURCE_DIR}/scripts/returnguard_project.py")

add_test(
    NAME returnguard.project_runner
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_project_runner.py"
        --runner "${RETURNGUARD_PROJECT_RUNNER}"
)

add_test(
    NAME returnguard.project_runner_scaling
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_project_runner_scaling.py"
        --runner "${RETURNGUARD_PROJECT_RUNNER}"
)

add_test(
    NAME returnguard.automatic_compilation_setup
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_auto_compilation_setup.py"
        --tool "$<TARGET_FILE:returnguard>"
        --compiler "${CMAKE_C_COMPILER}"
)

add_test(
    NAME returnguard.invalid_instrumentation_contract
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_invalid_instrumentation_contract.py"
        --tool "$<TARGET_FILE:returnguard>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation_invalid_contract.c"
        --include "${CMAKE_SOURCE_DIR}/include"
)

add_test(
    NAME returnguard.site_metadata
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_site_metadata.py"
        --tool "$<TARGET_FILE:returnguard>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation.c"
        --invalid-source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation_invalid_contract.c"
        --include "${CMAKE_SOURCE_DIR}/include"
)

add_test(
    NAME returnguard.contract_policy
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_contract_policy.py"
        --tool "$<TARGET_FILE:returnguard>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation_system_namespace.cpp"
        --include "${CMAKE_SOURCE_DIR}/include"
        --compiler "${CMAKE_CXX_COMPILER}"
)

add_test(
    NAME returnguard.readonly_launcher
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_readonly_launcher.py"
        --launcher "${CMAKE_SOURCE_DIR}/scripts/returnguard_compiler_launcher.py"
        --tool "$<TARGET_FILE:returnguard>"
        --compiler "${CMAKE_C_COMPILER}"
        --include "${CMAKE_SOURCE_DIR}/include"
        --runtime "$<TARGET_FILE:returnguard_runtime>"
)

add_test(
    NAME returnguard.allocator_policy
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_allocator_policy.py"
        --tool "$<TARGET_FILE:returnguard>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation_allocator_policy.c"
        --include "${CMAKE_SOURCE_DIR}/include"
        --compiler "${CMAKE_C_COMPILER}"
)

add_test(
    NAME returnguard.overload_metadata
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_overload_metadata.py"
        --tool "$<TARGET_FILE:returnguard>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/instrumentation_overloads.cpp"
        --include "${CMAKE_SOURCE_DIR}/include"
        --compiler "${CMAKE_CXX_COMPILER}"
)

add_test(
    NAME returnguard.runtime_registry_shutdown
    COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_registry_shutdown.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/cases/runtime_registry_shutdown.c"
        --include "${CMAKE_SOURCE_DIR}/include"
        --compiler "${CMAKE_C_COMPILER}"
        --runtime "$<TARGET_FILE:returnguard_runtime>"
)
