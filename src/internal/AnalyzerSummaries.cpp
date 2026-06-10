#include "Analyzer.hpp"

#include "DomainUtils.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Analysis/CallGraph.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <vector>

namespace returnguard::internal {
namespace {

const clang::FunctionDecl* canonical_definition(const clang::FunctionDecl* function) {
    if (function == nullptr) {
        return nullptr;
    }

    const clang::FunctionDecl* definition = nullptr;
    if (!function->hasBody(definition) || definition == nullptr) {
        return nullptr;
    }
    return definition->getCanonicalDecl();
}

bool merge_domain_values(Domain& destination, const Domain& source) {
    bool changed = false;
    if (destination.type_name.empty() && !source.type_name.empty()) {
        destination.type_name = source.type_name;
        changed = true;
    }
    destination.inferred_from_body =
        destination.inferred_from_body || source.inferred_from_body;

    for (const DomainValue& value : source.values) {
        Domain before = destination;
        if (value.labels.empty()) {
            add_domain_value(destination, value.value, "");
        } else {
            for (const std::string& label : value.labels) {
                add_domain_value(destination, value.value, label);
            }
        }

        if (before.values.size() != destination.values.size()) {
            changed = true;
            continue;
        }
        for (std::size_t index = 0; index < before.values.size(); ++index) {
            if (before.values[index].labels != destination.values[index].labels) {
                changed = true;
                break;
            }
        }
    }
    return changed;
}

void enqueue_function(
    const clang::FunctionDecl* function,
    std::deque<const clang::FunctionDecl*>& worklist,
    std::unordered_set<const clang::FunctionDecl*>& queued) {
    if (function != nullptr && queued.insert(function).second) {
        worklist.push_back(function);
    }
}

} // namespace

void Analyzer::prepare_translation_unit() {
    if (summaries_prepared_ || summaries_building_) {
        return;
    }

    summaries_building_ = true;
    summary_functions_.clear();
    summary_callers_.clear();
    domain_cache_.clear();
    domain_complete_.clear();
    nullable_cache_.clear();

    clang::CallGraph call_graph;
    call_graph.addToCallGraph(context_.getTranslationUnitDecl());

    std::unordered_set<const clang::FunctionDecl*> seen_functions;
    for (const auto& entry : call_graph) {
        const clang::CallGraphNode* node = entry.second.get();
        const auto* declaration =
            llvm::dyn_cast_or_null<clang::FunctionDecl>(node->getDecl());
        const clang::FunctionDecl* caller = canonical_definition(declaration);
        if (caller == nullptr) {
            continue;
        }

        if (seen_functions.insert(caller).second) {
            summary_functions_.push_back(caller);
        }

        for (const clang::CallGraphNode::CallRecord& record : node->callees()) {
            const auto* callee_declaration =
                llvm::dyn_cast_or_null<clang::FunctionDecl>(record.Callee->getDecl());
            if (callee_declaration == nullptr) {
                continue;
            }
            const clang::FunctionDecl* callee =
                callee_declaration->getCanonicalDecl();
            std::vector<const clang::FunctionDecl*>& callers =
                summary_callers_[callee];
            if (std::find(callers.begin(), callers.end(), caller) == callers.end()) {
                callers.push_back(caller);
            }
        }
    }

    for (const clang::FunctionDecl* function : summary_functions_) {
        Domain annotated = annotation_domain(function);
        Domain initial = annotated.finite
                             ? std::move(annotated)
                             : type_domain(function->getReturnType());
        domain_complete_[function] = initial.finite;
        domain_cache_[function] = std::move(initial);
        nullable_cache_[function] = false;
    }

    std::deque<const clang::FunctionDecl*> worklist;
    std::unordered_set<const clang::FunctionDecl*> queued;
    for (const clang::FunctionDecl* function : summary_functions_) {
        enqueue_function(function, worklist, queued);
    }

    collecting_domain_values_ = true;
    while (!worklist.empty()) {
        const clang::FunctionDecl* function = worklist.front();
        worklist.pop_front();
        queued.erase(function);

        Domain inferred = infer_function_domain_once(function);
        if (!merge_domain_values(domain_cache_[function], inferred)) {
            continue;
        }

        const auto callers = summary_callers_.find(function);
        if (callers == summary_callers_.end()) {
            continue;
        }
        for (const clang::FunctionDecl* caller : callers->second) {
            enqueue_function(caller, worklist, queued);
        }
    }
    collecting_domain_values_ = false;

    for (const clang::FunctionDecl* function : summary_functions_) {
        const Domain annotated = annotation_domain(function);
        const Domain by_type = type_domain(function->getReturnType());
        domain_complete_[function] =
            annotated.finite || by_type.finite ||
            !domain_cache_[function].values.empty();
    }

    validating_domain_completeness_ = true;
    bool completeness_changed = true;
    while (completeness_changed) {
        completeness_changed = false;
        for (const clang::FunctionDecl* function : summary_functions_) {
            if (!domain_complete_[function]) {
                continue;
            }

            const Domain annotated = annotation_domain(function);
            const Domain by_type = type_domain(function->getReturnType());
            if (annotated.finite || by_type.finite) {
                continue;
            }

            if (!infer_function_domain_once(function).finite) {
                domain_complete_[function] = false;
                completeness_changed = true;
            }
        }
    }
    validating_domain_completeness_ = false;

    for (const clang::FunctionDecl* function : summary_functions_) {
        domain_cache_[function].finite = domain_complete_[function];
    }

    worklist.clear();
    queued.clear();
    for (const clang::FunctionDecl* function : summary_functions_) {
        enqueue_function(function, worklist, queued);
    }

    while (!worklist.empty()) {
        const clang::FunctionDecl* function = worklist.front();
        worklist.pop_front();
        queued.erase(function);

        if (nullable_cache_[function]) {
            continue;
        }

        nullable_recompute_target_ = function;
        active_nullable_checks_.clear();
        const bool inferred =
            is_nullable_function_impl(function, active_nullable_checks_);
        nullable_recompute_target_ = nullptr;
        active_nullable_checks_.clear();

        if (!inferred) {
            continue;
        }
        nullable_cache_[function] = true;

        const auto callers = summary_callers_.find(function);
        if (callers == summary_callers_.end()) {
            continue;
        }
        for (const clang::FunctionDecl* caller : callers->second) {
            enqueue_function(caller, worklist, queued);
        }
    }

    nullable_recompute_target_ = nullptr;
    summaries_building_ = false;
    summaries_prepared_ = true;
}

} // namespace returnguard::internal
