#pragma once

#if defined(__clang__)
#define RETURNGUARD_FAILS_NULL __attribute__((annotate("returnguard.failure:null")))
#define RETURNGUARD_FAILS_NEGATIVE __attribute__((annotate("returnguard.failure:negative")))
#define RETURNGUARD_FAILS_NONZERO __attribute__((annotate("returnguard.failure:nonzero")))
#else
#define RETURNGUARD_FAILS_NULL
#define RETURNGUARD_FAILS_NEGATIVE
#define RETURNGUARD_FAILS_NONZERO
#endif
