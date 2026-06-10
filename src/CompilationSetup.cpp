#include "CompilationSetup.hpp"

#include <returnguard/BuildConfig.hpp>

#include <clang/Tooling/JSONCompilationDatabase.h>
#include <llvm/ADT/StringRef.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace returnguard {
namespace {

namespace fs = std::filesystem;

bool has_explicit_compilation_database(llvm::ArrayRef<const char*> arguments) {
    for (size_t index = 1; index < arguments.size(); ++index) {
        const llvm::StringRef argument(arguments[index]);
        if (argument == "--") {
            return true;
        }
        if (argument == "-p" || argument.starts_with("-p=")) {
            return true;
        }
    }
    return false;
}

fs::path normalized_path(fs::path path) {
    std::error_code error;
    if (path.is_relative()) {
        path = fs::absolute(path, error);
        if (error) {
            return path.lexically_normal();
        }
    }

    fs::path normalized = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

bool is_source_file(const fs::path& path) {
    static constexpr std::array<std::string_view, 6> extensions = {
        ".c", ".cc", ".cpp", ".cxx", ".m", ".mm",
    };

    std::string extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::vector<fs::path> source_files(llvm::ArrayRef<const char*> arguments) {
    std::vector<fs::path> result;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const llvm::StringRef argument(arguments[index]);
        if (argument == "--") {
            break;
        }
        if (argument.empty() || argument.starts_with("-")) {
            continue;
        }

        const fs::path path = normalized_path(argument.str());
        std::error_code error;
        if (is_source_file(path) && fs::is_regular_file(path, error) && !error) {
            result.push_back(path);
        }
    }
    return result;
}

void add_candidate(
    std::vector<fs::path>& candidates,
    std::unordered_set<std::string>& seen,
    const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        return;
    }

    const fs::path normalized = normalized_path(path);
    if (seen.insert(normalized.string()).second) {
        candidates.push_back(normalized);
    }
}

void add_candidates_from_root(
    std::vector<fs::path>& candidates,
    std::unordered_set<std::string>& seen,
    const fs::path& root) {
    static constexpr std::array<std::string_view, 8> build_directories = {
        "build",
        "build-debug",
        "build-release",
        "build-relwithdebinfo",
        "cmake-build-debug",
        "cmake-build-release",
        "out",
        "out/build",
    };

    add_candidate(candidates, seen, root / "compile_commands.json");
    for (const std::string_view directory : build_directories) {
        add_candidate(
            candidates,
            seen,
            root / fs::path(std::string(directory)) / "compile_commands.json");
    }

    std::error_code error;
    for (fs::directory_iterator iterator(root, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_directory(error) || error) {
            continue;
        }

        const std::string name = iterator->path().filename().string();
        if (name.starts_with("build-") || name.starts_with("cmake-build-")) {
            add_candidate(
                candidates,
                seen,
                iterator->path() / "compile_commands.json");
        }
    }
}

std::vector<fs::path> database_candidates(const std::vector<fs::path>& sources) {
    std::vector<fs::path> candidates;
    std::unordered_set<std::string> seen_candidates;
    std::unordered_set<std::string> seen_roots;

    std::vector<fs::path> starts;
    starts.reserve(sources.size() + 1);
    for (const fs::path& source : sources) {
        starts.push_back(source.parent_path());
    }

    std::error_code error;
    const fs::path current = fs::current_path(error);
    if (!error) {
        starts.push_back(current);
    }

    for (const fs::path& start : starts) {
        for (fs::path root = normalized_path(start); !root.empty(); root = root.parent_path()) {
            if (seen_roots.insert(root.string()).second) {
                add_candidates_from_root(candidates, seen_candidates, root);
            }
            if (root == root.root_path() || root.parent_path() == root) {
                break;
            }
        }
    }
    return candidates;
}

bool database_contains_source(
    const fs::path& database_path,
    const std::unordered_set<std::string>& normalized_sources) {
    std::string error_message;
    std::unique_ptr<clang::tooling::JSONCompilationDatabase> database =
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            database_path.string(),
            error_message,
            clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!database) {
        return false;
    }

    for (const clang::tooling::CompileCommand& command :
         database->getAllCompileCommands()) {
        fs::path file(command.Filename);
        if (file.is_relative()) {
            file = fs::path(command.Directory) / file;
        }
        if (normalized_sources.contains(normalized_path(file).string())) {
            return true;
        }
    }
    return false;
}

bool has_resource_directory(
    const clang::tooling::CommandLineArguments& arguments) {
    auto first = arguments.begin();
    if (first != arguments.end()) {
        ++first;
    }

    return std::any_of(
        first,
        arguments.end(),
        [](const std::string& argument) {
            const llvm::StringRef value(argument);
            return value == "-resource-dir" || value.starts_with("-resource-dir=");
        });
}

} // namespace

std::optional<std::string> discover_compilation_database(
    llvm::ArrayRef<const char*> arguments) {
    if (has_explicit_compilation_database(arguments)) {
        return std::nullopt;
    }

    const std::vector<fs::path> sources = source_files(arguments);
    if (sources.empty()) {
        return std::nullopt;
    }

    std::unordered_set<std::string> normalized_sources;
    for (const fs::path& source : sources) {
        normalized_sources.insert(source.string());
    }

    for (const fs::path& candidate : database_candidates(sources)) {
        if (database_contains_source(candidate, normalized_sources)) {
            return candidate.parent_path().string();
        }
    }
    return std::nullopt;
}

clang::tooling::ArgumentsAdjuster resource_directory_adjuster() {
    const std::string resource_directory = RETURNGUARD_RESOURCE_DIR;
    return [resource_directory](
               const clang::tooling::CommandLineArguments& arguments,
               llvm::StringRef) {
        if (resource_directory.empty() || arguments.empty() ||
            has_resource_directory(arguments)) {
            return arguments;
        }

        clang::tooling::CommandLineArguments adjusted = arguments;
        adjusted.insert(
            adjusted.begin() + 1,
            "-resource-dir=" + resource_directory);
        return adjusted;
    };
}

} // namespace returnguard
