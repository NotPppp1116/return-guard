#include "CompilationSetup.hpp"

#include <returnguard/Options.hpp>

#include <array>
#include <string>
#include <string_view>

namespace returnguard {

clang::tooling::ArgumentsAdjuster compiler_diagnostic_adjuster() {
    return [](const clang::tooling::CommandLineArguments& arguments, llvm::StringRef) {
        clang::tooling::CommandLineArguments adjusted = arguments;
        static constexpr std::array<std::string_view, 5> warnings = {
            "array-bounds",
            "return-stack-address",
            "shift-count-overflow",
            "shift-overflow",
            "null-dereference",
        };
        for (const std::string_view warning : warnings) {
            adjusted.push_back("-W" + std::string(warning));
            if (options().fail_on_diagnostics) {
                adjusted.push_back("-Werror=" + std::string(warning));
            }
        }
        return adjusted;
    };
}

clang::tooling::ArgumentsAdjuster analyzer_argument_adjuster() {
    return [](const clang::tooling::CommandLineArguments& arguments, llvm::StringRef) {
        return arguments;
    };
}

} // namespace returnguard
