add_returnguard_test(
    returnguard.null_state_cpp null_state.cpp practical 4
    "is dereferenced without a prior null check")

add_returnguard_test(
    returnguard.cross_function_globals cross_function_globals.c practical 3
    "missing STATUS_FATAL"
    --expect "cross_function_chain_missing"
    --expect "global_backed_result_missing"
    --expect "callback_result_missing"
    --expect "STATUS_UNKNOWN")
