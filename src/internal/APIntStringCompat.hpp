#pragma once

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Config/llvm-config.h>

#include <string>

#if LLVM_VERSION_MAJOR < 19
namespace llvm {
inline std::string toString(const APInt& value, unsigned radix, bool signed_value) {
    SmallString<64> buffer;
    value.toString(buffer, radix, signed_value);
    return std::string(buffer);
}
} // namespace llvm
#endif
