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
            --mode "${mode}"
            --warning-count "${warning_count}"
            --expect "${expected_text}"
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
add_returnguard_test(returnguard.if_chains if_chains.c practical 1 "missing 32")
add_returnguard_test(returnguard.direct_if direct_if.c practical 1 "missing 4, 32")
add_returnguard_test(returnguard.reassignment reassignment.c practical 1 "stored but never used")
add_returnguard_test(returnguard.nested_calls nested_calls.c strict 2 "consumed but not verified")
add_returnguard_test(returnguard.nested_storage_and_switch nested_storage_and_switch.c strict 2 "consumed but not verified")
add_returnguard_test(returnguard.comma_discard comma_discard.c ignored-only 1 "is not handled")
add_returnguard_test(returnguard.comma_value comma_value.c practical 1 "missing 32")
add_returnguard_test(returnguard.compound_assignment compound_assignment.c strict 1 "consumed but not verified")
add_returnguard_test(returnguard.annotation_redeclaration annotation_redeclaration.c practical 1 "missing 4, 32")
add_returnguard_test(returnguard.switches switches.c strict 1 "open-ended")
add_returnguard_test(returnguard.ignored ignored.c ignored-only 1 "return value of 'annotated_status'")
add_returnguard_test(
    returnguard.fail_on_diagnostics ignored.c ignored-only 1
    "return value of 'annotated_status'"
    --fail-on-diagnostics
    --diagnostic-kind error
    --expected-exit-code 1)

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
