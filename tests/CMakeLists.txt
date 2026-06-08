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
    )
endfunction()

add_returnguard_test(
    returnguard.macros macros.c practical 1 "missing 32")
add_returnguard_test(
    returnguard.enums enums.c practical 1 "missing STATUS_DENIED")
add_returnguard_test(
    returnguard.integers integers.c strict 1 "missing 32")
add_returnguard_test(
    returnguard.if_chains if_chains.c practical 1 "missing 32")
add_returnguard_test(
    returnguard.direct_if direct_if.c practical 1 "missing 4, 32")
add_returnguard_test(
    returnguard.switches switches.c strict 1 "open-ended")
add_returnguard_test(
    returnguard.ignored ignored.c ignored-only 1 "return value of 'annotated_status'")
