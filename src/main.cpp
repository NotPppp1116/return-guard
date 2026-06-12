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
#include <fstream>
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

llvm::cl::list<std::string> contract_options(
    "contract",
    llvm::cl::desc("Add a failure contract as qualified_name=null, qualified_name=negative, "
                   "qualified_name=nonzero, or qualified_name=byte-count:N"),
    llvm::cl::ZeroOrMore,
    llvm::cl::cat(category));

llvm::cl::list<std::string> contract_file_options(
    "contract-file",
    llvm::cl::desc("Read failure contracts from a file using qualified_name=null style lines"),
    llvm::cl::ZeroOrMore,
    llvm::cl::cat(category));

llvm::cl::list<std::string> function_config_options(
    "function-config",
    llvm::cl::desc(
        "Read function policy lines: 'contract name null|negative|nonzero|byte-count:N' or "
        "'lifetime name alloc|free|realloc'"),
    llvm::cl::ZeroOrMore,
    llvm::cl::cat(category));

struct FunctionConfig {
    std::vector<std::string> contracts;
    std::vector<std::string> lifetime_roles;
};

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

bool valid_contract_spec(llvm::StringRef value) {
    const auto [name, predicate] = value.split('=');
    llvm::StringRef trimmed_predicate = predicate.trim();
    if (name.trim().empty()) {
        return false;
    }
    if (trimmed_predicate == "null" || trimmed_predicate == "negative" ||
        trimmed_predicate == "nonzero" || trimmed_predicate == "non-zero") {
        return true;
    }

    llvm::StringRef byte_count_argument = trimmed_predicate;
    if (!byte_count_argument.consume_front("byte-count:") &&
        !byte_count_argument.consume_front("byte_count:")) {
        return false;
    }
    unsigned ignored = 0;
    return !byte_count_argument.empty() && !byte_count_argument.getAsInteger(10, ignored);
}

bool valid_lifetime_spec(llvm::StringRef value) {
    const auto [name, role] = value.split('=');
    return !name.trim().empty() &&
           (role.trim() == "alloc" || role.trim() == "free" || role.trim() == "realloc");
}

void parse_function_config_line(llvm::StringRef text, const std::string& path,
                                unsigned line_number, FunctionConfig& config) {
    text = text.split('#').first.trim();
    if (text.empty()) {
        return;
    }

    const auto [kind, rest] = text.split(' ');
    const auto [name, value] = rest.trim().split(' ');
    if (kind == "contract") {
        const std::string spec = (name.trim() + "=" + value.trim()).str();
        if (valid_contract_spec(spec)) {
            config.contracts.push_back(spec);
            return;
        }
    }
    if (kind == "lifetime") {
        const std::string spec = (name.trim() + "=" + value.trim()).str();
        if (valid_lifetime_spec(spec)) {
            config.lifetime_roles.push_back(spec);
            return;
        }
    }

    llvm::errs() << "returnguard: invalid function policy in " << path << ':' << line_number
                 << " (expected 'contract name null|negative|nonzero' or "
                    "'contract name byte-count:N' or 'lifetime name alloc|free|realloc')\n";
    std::exit(2);
}

FunctionConfig collect_function_config() {
    FunctionConfig config;
    for (const std::string& path : function_config_options) {
        std::ifstream input(path);
        if (!input) {
            llvm::errs() << "returnguard: could not read --function-config='" << path << "'\n";
            std::exit(2);
        }

        std::string line;
        unsigned line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            parse_function_config_line(line, path, line_number, config);
        }
    }
    return config;
}

std::vector<std::string> collect_contracts() {
    std::vector<std::string> contracts(contract_options.begin(), contract_options.end());

    for (const std::string& path : contract_file_options) {
        std::ifstream input(path);
        if (!input) {
            llvm::errs() << "returnguard: could not read --contract-file='" << path << "'\n";
            std::exit(2);
        }

        std::string line;
        unsigned line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            llvm::StringRef text(line);
            text = text.split('#').first.trim();
            if (text.empty()) {
                continue;
            }
            if (!valid_contract_spec(text)) {
                llvm::errs() << "returnguard: invalid contract in " << path << ':' << line_number
                             << " (expected qualified_name=null, qualified_name=negative, "
                                "qualified_name=nonzero, or qualified_name=byte-count:N)\n";
                std::exit(2);
            }
            contracts.push_back(text.str());
        }
    }

    for (const std::string& contract : contracts) {
        if (!valid_contract_spec(contract)) {
            llvm::errs() << "returnguard: invalid --contract='" << contract
                         << "' (expected qualified_name=null, qualified_name=negative, "
                            "qualified_name=nonzero, or qualified_name=byte-count:N)\n";
            std::exit(2);
        }
    }

    return contracts;
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

    FunctionConfig function_config = collect_function_config();
    std::vector<std::string> contracts = collect_contracts();
    contracts.insert(contracts.end(), function_config.contracts.begin(),
                     function_config.contracts.end());

    returnguard::set_options({
        .mode = parse_mode(mode_option),
        .analyze_headers = analyze_headers,
        .include_operators = include_operators,
        .include_reference_returns = include_reference_returns,
        .explicit_void_is_handled = explicit_void_is_handled,
        .fail_on_diagnostics = fail_on_diagnostics,
        .color = !no_color,
        .contracts = std::move(contracts),
        .lifetime_roles = std::move(function_config.lifetime_roles),
    });

    clang::tooling::ClangTool tool(
        parser->getCompilations(),
        parser->getSourcePathList());
    tool.appendArgumentsAdjuster(parser->getArgumentsAdjuster());
    tool.appendArgumentsAdjuster(returnguard::resource_directory_adjuster());

    returnguard::ActionFactory factory;
    return tool.run(&factory);
}
