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

llvm::cl::opt<std::string> instrument_output(
    "instrument-output",
    llvm::cl::desc(
        "Write one transformed source file with fail-closed call checks"),
    llvm::cl::init(""),
    llvm::cl::cat(category));

llvm::cl::opt<std::string> site_map_output(
    "site-map-output",
    llvm::cl::desc(
        "Write JSON metadata mapping instrumented site IDs to source locations"),
    llvm::cl::init(""),
    llvm::cl::cat(category));

llvm::cl::opt<std::string> site_root(
    "site-root",
    llvm::cl::desc(
        "Normalize site-map source paths relative to this project root"),
    llvm::cl::init(""),
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

    if (!instrument_output.empty() && parser->getSourcePathList().size() != 1U) {
        llvm::errs()
            << "returnguard: --instrument-output requires exactly one source file\n";
        return 2;
    }
    if (!site_map_output.empty() && instrument_output.empty()) {
        llvm::errs()
            << "returnguard: --site-map-output requires --instrument-output\n";
        return 2;
    }
    if (!site_root.empty() && site_map_output.empty()) {
        llvm::errs()
            << "returnguard: --site-root requires --site-map-output\n";
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
        .instrument_output = instrument_output,
        .site_map_output = site_map_output,
        .site_root = site_root,
    });

    clang::tooling::ClangTool tool(
        parser->getCompilations(),
        parser->getSourcePathList());
    tool.appendArgumentsAdjuster(parser->getArgumentsAdjuster());
    tool.appendArgumentsAdjuster(returnguard::resource_directory_adjuster());

    returnguard::ActionFactory factory;
    return tool.run(&factory);
}
