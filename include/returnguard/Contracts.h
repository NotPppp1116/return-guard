#pragma once

#if defined(__clang__)
#define RETURNGUARD_FAILS_NULL __attribute__((annotate("returnguard.failure:null")))
#define RETURNGUARD_FAILS_NEGATIVE __attribute__((annotate("returnguard.failure:negative")))
#else
#define RETURNGUARD_FAILS_NULL
#define RETURNGUARD_FAILS_NEGATIVE
#endif
