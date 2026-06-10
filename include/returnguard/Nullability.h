#pragma once

#if defined(__clang__)
#define RETURNGUARD_NULLABLE _Nullable
#else
#define RETURNGUARD_NULLABLE
#endif
