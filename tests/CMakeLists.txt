function(add_returnguard_smoke_test name source mode)
    add_test(
        NAME ${name}
        COMMAND returnguard
            --mode=${mode}
            "${CMAKE_CURRENT_SOURCE_DIR}/cases/${source}"
            --
            -std=c17
            -I"${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
endfunction()

add_returnguard_smoke_test(returnguard.macros macros.c practical)
add_returnguard_smoke_test(returnguard.enums enums.c practical)
add_returnguard_smoke_test(returnguard.integers integers.c strict)
add_returnguard_smoke_test(returnguard.if_chains if_chains.c practical)
add_returnguard_smoke_test(returnguard.switches switches.c practical)
add_returnguard_smoke_test(returnguard.ignored ignored.c ignored-only)
