#pragma once

#include <clang/Tooling/ArgumentsAdjusters.h>
#include <llvm/ADT/ArrayRef.h>

#include <optional>
#include <string>

namespace returnguard {

std::optional<std::string> discover_compilation_database(
    llvm::ArrayRef<const char*> arguments);

clang::tooling::ArgumentsAdjuster resource_directory_adjuster();
clang::tooling::ArgumentsAdjuster compiler_diagnostic_adjuster();
clang::tooling::ArgumentsAdjuster analyzer_argument_adjuster();

} // namespace returnguard
