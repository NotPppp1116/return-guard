#include "ContractPolicy.hpp"

#include "AstUtils.hpp"

#include <returnguard/Options.hpp>

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringRef.h>

#include <initializer_list>
#include <optional>
#include <string>

// Built-in policy stays focused on stable stdio/POSIX/Wayland-style system APIs, not
// project-specific names.

namespace returnguard::internal {
namespace {

const clang::DeclContext* skip_linkage_contexts(const clang::DeclContext* context) {
    while (context != nullptr && context->getDeclKind() == clang::Decl::LinkageSpec) {
        context = context->getParent();
    }
    return context;
}

bool is_global_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context = skip_linkage_contexts(function.getDeclContext());
    return context != nullptr && context->isTranslationUnit();
}

bool is_std_context(const clang::FunctionDecl& function) {
    const clang::DeclContext* context = skip_linkage_contexts(function.getDeclContext());

    while (context != nullptr) {
        const auto* namespace_declaration = llvm::dyn_cast<clang::NamespaceDecl>(context);
        if (namespace_declaration == nullptr) {
            return false;
        }

        if (namespace_declaration->getName() == "std") {
            const clang::DeclContext* parent =
                skip_linkage_contexts(namespace_declaration->getParent());
            return parent != nullptr && parent->isTranslationUnit();
        }

        if (!namespace_declaration->isInline()) {
            return false;
        }

        context = skip_linkage_contexts(namespace_declaration->getParent());
    }

    return false;
}

bool has_any_name(const clang::FunctionDecl& function,
                  std::initializer_list<llvm::StringRef> names) {
    for (llvm::StringRef name : names) {
        if (has_identifier_name(&function, name)) {
            return true;
        }
    }
    return false;
}

std::optional<FailurePredicate> annotation_contract(const clang::FunctionDecl& function) {
    for (const clang::FunctionDecl* redeclaration : function.redecls()) {
        for (const clang::AnnotateAttr* attribute :
             redeclaration->specific_attrs<clang::AnnotateAttr>()) {
            const llvm::StringRef annotation = attribute->getAnnotation();

            if (annotation == "returnguard.failure:null") {
                return FailurePredicate::Null;
            }

            if (annotation == "returnguard.failure:negative") {
                return FailurePredicate::Negative;
            }
        }
    }

    return std::nullopt;
}

std::optional<FailurePredicate> parse_predicate(llvm::StringRef value) {
    value = value.trim();
    if (value == "null") {
        return FailurePredicate::Null;
    }
    if (value == "negative") {
        return FailurePredicate::Negative;
    }
    return std::nullopt;
}

bool contract_name_matches(const clang::FunctionDecl& function, llvm::StringRef name) {
    name = name.trim();
    if (name.empty()) {
        return false;
    }

    if (function.getQualifiedNameAsString() == name.str()) {
        return true;
    }
    return function.getIdentifier() != nullptr && function.getName() == name;
}

std::optional<FailurePredicate> configured_contract(const clang::FunctionDecl& function) {
    for (const std::string& contract : returnguard::options().contracts) {
        const auto [name, predicate_text] = llvm::StringRef(contract).split('=');
        const std::optional<FailurePredicate> predicate = parse_predicate(predicate_text);
        if (predicate.has_value() && contract_name_matches(function, name)) {
            return predicate;
        }
    }
    return std::nullopt;
}

std::optional<unsigned> parse_byte_count_parameter_index(llvm::StringRef value) {
    value = value.trim();
    llvm::StringRef argument;
    if (value.consume_front("byte-count:")) {
        argument = value;
    } else if (value.consume_front("byte_count:")) {
        argument = value;
    } else {
        return std::nullopt;
    }

    unsigned parsed = 0;
    if (argument.getAsInteger(10, parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<unsigned> configured_byte_count_parameter_index(const clang::FunctionDecl& function) {
    for (const std::string& contract : returnguard::options().contracts) {
        const auto [name, predicate_text] = llvm::StringRef(contract).split('=');
        const std::optional<unsigned> index = parse_byte_count_parameter_index(predicate_text);
        if (index.has_value() && contract_name_matches(function, name)) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<unsigned>
builtin_byte_count_parameter_index(const clang::FunctionDecl& function,
                                   const clang::SourceManager& source_manager) {
    if (!source_manager.isInSystemHeader(function.getLocation()) || !is_global_context(function)) {
        return std::nullopt;
    }

    if (has_any_name(function, {
                                   "getrandom",
                               })) {
        return 1U;
    }

    if (has_any_name(function, {
                                   "pread",
                                   "pread64",
                                   "pwrite",
                                   "pwrite64",
                                   "read",
                                   "recv",
                                   "recvfrom",
                                   "send",
                                   "sendto",
                                   "write",
                               })) {
        return 2U;
    }
    return std::nullopt;
}

} // namespace

std::optional<FailurePredicate> failure_contract(const clang::FunctionDecl& function,
                                                 const clang::SourceManager& source_manager) {
    if (const std::optional<FailurePredicate> annotation = annotation_contract(function)) {
        return annotation;
    }
    if (const std::optional<FailurePredicate> configured = configured_contract(function)) {
        return configured;
    }
    if (configured_byte_count_parameter_index(function).has_value() ||
        builtin_byte_count_parameter_index(function, source_manager).has_value()) {
        return FailurePredicate::Negative;
    }

    if (!source_manager.isInSystemHeader(function.getLocation())) {
        return std::nullopt;
    }

    const bool global = is_global_context(function);
    const bool standard = is_std_context(function);

    if ((global || standard) && has_any_name(function, {
                                                           "aligned_alloc",
                                                           "calloc",
                                                           "ctermid",
                                                           "fopen",
                                                           "freopen",
                                                           "fgets",
                                                           "fmemopen",
                                                           "malloc",
                                                           "open_memstream",
                                                           "realloc",
                                                           "reallocarray",
                                                           "tmpfile",
                                                       })) {
        return FailurePredicate::Null;
    }

    if (global && has_any_name(function, {
                                             "fdopen",
                                             "dlsym",
                                             "getenv",
                                             "getlogin",
                                             "getcwd",
                                             "get_current_dir_name",
                                             "memchr",
                                             "memmem",
                                             "popen",
                                             "opendir",
                                             "fdopendir",
                                             "dlopen",
                                             "mkdtemp",
                                             "readdir",
                                             "realpath",
                                             "secure_getenv",
                                             "strcasestr",
                                             "strchr",
                                             "strpbrk",
                                             "strrchr",
                                             "strdup",
                                             "strndup",
                                             "strsep",
                                             "strstr",
                                             "strtok",
                                             "strtok_r",
                                             "ptsname",
                                             "ttyname",
                                             "wl_client_create",
                                             "wl_display_create",
                                             "wl_event_loop_add_fd",
                                             "wl_event_loop_add_idle",
                                             "wl_event_loop_add_signal",
                                             "wl_event_loop_add_timer",
                                             "wl_registry_bind",
                                             "wl_resource_create",
                                         })) {
        return FailurePredicate::Null;
    }

    if ((global || standard) && has_any_name(function, {
                                                           "printf",
                                                           "fprintf",
                                                           "sprintf",
                                                           "snprintf",
                                                           "vprintf",
                                                           "vfprintf",
                                                           "vsprintf",
                                                           "vsnprintf",
                                                           "putchar",
                                                           "putc",
                                                           "puts",
                                                           "fclose",
                                                           "fflush",
                                                           "fputc",
                                                           "fputs",
                                                           "remove",
                                                           "rename",
                                                       })) {
        return FailurePredicate::Negative;
    }

    if (global && has_any_name(function, {
                                             "open",           "open64",
                                             "openat",         "openat64",
                                             "creat",          "creat64",
                                             "close",          "fsync",
                                             "fdatasync",      "ftruncate",
                                             "truncate",       "dup",
                                             "dup2",           "dup3",
                                             "setuid",         "setgid",
                                             "seteuid",        "setegid",
                                             "setreuid",       "setregid",
                                             "setresuid",      "setresgid",
                                             "setgroups",      "initgroups",
                                             "chroot",         "pivot_root",
                                             "prctl",          "capset",
                                             "munmap",         "mprotect",
                                             "msync",          "mlock",
                                             "mlockall",       "shm_open",
                                             "shm_unlink",
                                             "fork",           "vfork",
                                             "clone",          "clone3",
                                             "execve",         "execv",
                                             "execvp",         "execvpe",
                                             "execl",          "execlp",
                                             "execle",         "wait",
                                             "waitpid",        "waitid",
                                             "dirfd",          "fileno",
                                             "ftell",          "ftello",
                                             "getline",        "getdelim",
                                             "lseek",          "lseek64",
                                             "mkstemp",        "mkostemp",
                                             "mkstemps",       "mkostemps",
                                             "system",
                                             "getentropy",
                                             "socket",         "socketpair",
                                             "bind",           "listen",
                                             "connect",        "accept",
                                             "accept4",        "shutdown",
                                             "getsockname",    "getpeername",
                                             "setsockopt",     "getsockopt",
                                             "pipe",           "pipe2",
                                             "eventfd",        "eventfd_read",
                                             "eventfd_write",  "inotify_init",
                                             "inotify_init1",  "epoll_create",
                                             "epoll_create1",  "epoll_ctl",
                                             "timerfd_create", "timerfd_settime",
                                             "signalfd",       "chdir",
                                             "fchdir",         "mkdir",
                                             "rmdir",          "unlink",
                                             "unlinkat",       "link",
                                             "linkat",         "symlink",
                                             "symlinkat",      "readlink",
                                             "readlinkat",     "chmod",
                                             "fchmod",         "fchmodat",
                                             "chown",          "fchown",
                                             "lchown",         "fchownat",
                                             "access",         "faccessat",
                                             "stat",           "lstat",
                                             "fstat",          "fstatat",
                                             "poll",           "ppoll",
                                             "select",         "pselect",
                                             "sigaction",      "sigprocmask",
                                             "kill",           "ioctl",
                                             "fcntl",          "flock",
                                             "wl_array_add",   "wl_display_dispatch",
                                             "wl_display_dispatch_pending",
                                             "wl_display_flush",
                                             "wl_display_read_events",
                                             "wl_display_roundtrip",
                                             "wl_event_source_fd_update",
                                             "wl_event_source_remove",
                                             "wl_event_source_timer_update",
                                             "wl_proxy_add_listener",
                                         })) {
        return FailurePredicate::Negative;
    }

    return std::nullopt;
}

std::optional<unsigned> byte_count_parameter_index(const clang::FunctionDecl& function,
                                                   const clang::SourceManager& source_manager) {
    if (const std::optional<unsigned> configured =
            configured_byte_count_parameter_index(function)) {
        return configured;
    }
    return builtin_byte_count_parameter_index(function, source_manager);
}

} // namespace returnguard::internal
