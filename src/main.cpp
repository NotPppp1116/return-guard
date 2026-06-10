#include "CompilationSetup.hpp"

#include <returnguard/Frontend.hpp>
#include <returnguard/Options.hpp>

#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

llvm::cl::OptionCategory category("returnguard options");

llvm::cl::opt<std::string> mode_option(
    "mode",
    llvm::cl::desc("Checking policy: practical, strict, or ignored-only"),
    llvm::cl::init("practical"),
    llvm::cl::cat(category));

llvm::cl::opt<bool> analyze_headers(
    "analyze-headers",
    llvm::cl::desc("Also diagnose call sites written in headers"),
    llvm::cl::init(false),
    llvm::cl::cat(category));

llvm::cl::opt<bool> include_operators(
    "include-operators",
    llvm::cl::desc("Analyze overloaded operator calls"),
    llvm::cl::init(false),
    llvm::cl::cat(category));

llvm::cl::opt<bool> include_reference_returns(
    "include-reference-returns",
    llvm::cl::desc("Analyze functions returning references"),
    llvm::cl::init(false),
    llvm::cl::cat(category));

llvm::cl::opt<bool> explicit_void_is_handled(
    "explicit-void-is-handled",
    llvm::cl::desc("Treat '(void)function()' as an intentional discard"),
    llvm::cl::init(true),
    llvm::cl::cat(category));

llvm::cl::opt<bool> fail_on_diagnostics(
    "fail-on-diagnostics",
    llvm::cl::desc("Emit ReturnGuard findings as errors and return a nonzero status"),
    llvm::cl::init(false),
    llvm::cl::cat(category));

llvm::cl::opt<bool> no_color(
    "no-color",
    llvm::cl::desc("Disable colored diagnostics"),
    llvm::cl::init(false),
    llvm::cl::cat(category));

returnguard::Mode parse_mode(llvm::StringRef value) {
    if (value == "practical") {
        return returnguard::Mode::Practical;
    }
    if (value == "strict") {
        return returnguard::Mode::Strict;
    }
    if (value == "ignored-only") {
        return returnguard::Mode::IgnoredOnly;
    }

    llvm::errs() << "returnguard: invalid --mode='" << value
                 << "' (expected practical, strict, or ignored-only)\n";
    std::exit(2);
}

} // namespace

int main(int argc, const char** argv) {
    llvm::InitLLVM init_llvm(argc, argv);

    std::vector<std::string> argument_storage(argv, argv + argc);
    if (const auto build_path = returnguard::discover_compilation_database(
            llvm::ArrayRef<const char*>(argv, static_cast<size_t>(argc)))) {
        argument_storage.insert(argument_storage.begin() + 1, "-p=" + *build_path);
    }

    std::vector<const char*> parser_arguments;
    parser_arguments.reserve(argument_storage.size());
    for (const std::string& argument : argument_storage) {
        parser_arguments.push_back(argument.c_str());
    }
    int parser_argument_count = static_cast<int>(parser_arguments.size());

    auto parser = clang::tooling::CommonOptionsParser::create(
        parser_argument_count,
        parser_arguments.data(),
        category,
        llvm::cl::OneOrMore);
    if (!parser) {
        llvm::errs() << llvm::toString(parser.takeError()) << '\n';
        return 2;
    }

    returnguard::set_options({
        .mode = parse_mode(mode_option),
        .analyze_headers = analyze_headers,
        .include_operators = include_operators,
        .include_reference_returns = include_reference_returns,
        .explicit_void_is_handled = explicit_void_is_handled,
        .fail_on_diagnostics = fail_on_diagnostics,
        .color = !no_color,
    });

    clang::tooling::ClangTool tool(
        parser->getCompilations(),
        parser->getSourcePathList());
    tool.appendArgumentsAdjuster(parser->getArgumentsAdjuster());
    tool.appendArgumentsAdjuster(returnguard::resource_directory_adjuster());

    returnguard::ActionFactory factory;
    return tool.run(&factory);
}
