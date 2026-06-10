#pragma once

#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Config/llvm-config.h>

#include <string>

#if LLVM_VERSION_MAJOR < 19
namespace llvm {

inline std::string toString(const APSInt& value, unsigned radix, bool signed_value) {
    return llvm::toString(static_cast<const APInt&>(value), radix, signed_value, false);
}

} // namespace llvm
#endif
