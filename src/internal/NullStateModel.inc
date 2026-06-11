enum class Origin {
    No,
    Yes,
    Maybe,
};

enum class NullState {
    MaybeNull,
    NonNull,
    Null,
};

struct PointerFact {
    Origin origin = Origin::No;
    NullState null_state = NullState::MaybeNull;

    bool operator==(const PointerFact&) const = default;
};

NullState join_null_state(NullState lhs, NullState rhs) {
    return lhs == rhs ? lhs : NullState::MaybeNull;
}

PointerFact join_fact(PointerFact lhs, PointerFact rhs) {
    if (lhs.origin == Origin::No && rhs.origin == Origin::No) {
        return {};
    }

    if (lhs.origin == Origin::No) {
        rhs.origin = Origin::Maybe;
        return rhs;
    }
    if (rhs.origin == Origin::No) {
        lhs.origin = Origin::Maybe;
        return lhs;
    }

    return {
        .origin = lhs.origin == Origin::Yes && rhs.origin == Origin::Yes
                      ? Origin::Yes
                      : Origin::Maybe,
        .null_state = join_null_state(lhs.null_state, rhs.null_state),
    };
}

struct State {
    std::unordered_map<const clang::ValueDecl*, PointerFact> values;
    std::unordered_map<const clang::Expr*, PointerFact> expr_values;
    const clang::ValueDecl* refinement_subject = nullptr;
    const clang::Expr* refinement_expr_subject = nullptr;

    bool operator==(const State&) const = default;

    [[nodiscard]] PointerFact fact(const clang::ValueDecl* declaration) const {
        const auto iterator = values.find(declaration);
        return iterator == values.end() ? PointerFact{} : iterator->second;
    }

    [[nodiscard]] PointerFact fact_for_expr(const clang::Expr* expression) const {
        expression = strip_expr(expression);
        if (expression == nullptr) return {};
        const auto iterator = expr_values.find(expression);
        return iterator == expr_values.end() ? PointerFact{} : iterator->second;
    }

    void set(const clang::ValueDecl* declaration, PointerFact value) {
        if (declaration == nullptr || value.origin == Origin::No) {
            values.erase(declaration);
            return;
        }
        values[declaration] = value;
    }

    void set_expr(const clang::Expr* expression, PointerFact value) {
        expression = strip_expr(expression);
        if (expression == nullptr || value.origin == Origin::No) {
            expr_values.erase(expression);
            return;
        }
        expr_values[expression] = value;
    }

    void prepare_refinement(const clang::ValueDecl* subject) {
        refinement_subject = subject;
    }

    void prepare_refinement_expr(const clang::Expr* subject) {
        refinement_expr_subject = strip_expr(subject);
    }

    void refine(NullState null_state) {
        const clang::ValueDecl* subject = refinement_subject;
        const clang::Expr* subject_expr = refinement_expr_subject;
        refinement_subject = nullptr;
        refinement_expr_subject = nullptr;

        if (subject != nullptr) {
            PointerFact subject_fact = fact(subject);
            if (subject_fact.origin == Origin::Maybe) {
                subject_fact.null_state = null_state;
                set(subject, subject_fact);
                return;
            }
        }
        if (subject_expr != nullptr) {
            PointerFact subject_fact = fact_for_expr(subject_expr);
            if (subject_fact.origin == Origin::Maybe) {
                subject_fact.null_state = null_state;
                set_expr(subject_expr, subject_fact);
                return;
            }
        }

        for (auto& [declaration, value] : values) {
            (void)declaration;
            if (value.origin == Origin::Yes) {
                value.null_state = null_state;
            }
        }
        for (auto& [expression, value] : expr_values) {
            (void)expression;
            if (value.origin == Origin::Yes) {
                value.null_state = null_state;
            }
        }
    }

    bool join(const State& other) {
        bool changed = false;
        std::unordered_set<const clang::ValueDecl*> declarations;
        declarations.reserve(values.size() + other.values.size());

        for (const auto& [declaration, unused] : values) {
            (void)unused;
            declarations.insert(declaration);
        }
        for (const auto& [declaration, unused] : other.values) {
            (void)unused;
            declarations.insert(declaration);
        }

        for (const clang::ValueDecl* declaration : declarations) {
            const PointerFact old = fact(declaration);
            const PointerFact merged = join_fact(old, other.fact(declaration));
            changed = changed || old != merged;
            set(declaration, merged);
        }

        std::unordered_set<const clang::Expr*> expressions;
        expressions.reserve(expr_values.size() + other.expr_values.size());
        for (const auto& [expr, unused] : expr_values) {
            (void)unused;
            expressions.insert(expr);
        }
        for (const auto& [expr, unused] : other.expr_values) {
            (void)unused;
            expressions.insert(expr);
        }

        for (const clang::Expr* expression : expressions) {
            const PointerFact old = fact_for_expr(expression);
            const PointerFact merged = join_fact(old, other.fact_for_expr(expression));
            changed = changed || old != merged;
            set_expr(expression, merged);
        }
        return changed;
    }
};

bool is_source(const clang::Expr* expression, const NullSource& source) {
    if (expression == nullptr) return false;
    expression = strip_expr(expression);
    if (source.expr != nullptr) {
        const clang::Expr* stored = strip_expr(source.expr);
        if (stored != nullptr && expression == stored) {
            return true;
        }
    }
    if (source.decl != nullptr) {
        if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            if (ref->getDecl() == source.decl) {
                return true;
            }
        }
    }
    return false;
}

bool is_null_pointer_constant(
    const clang::Expr* expression,
    const clang::ASTContext& context) {
    return expression != nullptr &&
           expression->isNullPointerConstant(
               const_cast<clang::ASTContext&>(context),
               clang::Expr::NPC_ValueDependentIsNotNull) !=
               clang::Expr::NPCK_NotNull;
}

const clang::ValueDecl* lvalue_declaration(const clang::Expr* expression) {
    expression = strip_expr(expression);
    const auto* reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression);
    return reference == nullptr
               ? nullptr
               : llvm::dyn_cast<clang::ValueDecl>(reference->getDecl());
}

bool same_lvalue_or_expression(const clang::Expr* lhs, const clang::Expr* rhs) {
    lhs = strip_expr(lhs);
    rhs = strip_expr(rhs);
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    const clang::ValueDecl* lhs_decl = lvalue_declaration(lhs);
    const clang::ValueDecl* rhs_decl = lvalue_declaration(rhs);
    if (lhs_decl != nullptr || rhs_decl != nullptr) {
        return lhs_decl != nullptr && lhs_decl == rhs_decl;
    }
    return lhs == rhs;
}

std::optional<NullState> null_state_for_conditional_branch(
    const clang::Expr* condition,
    const clang::Expr* branch_expression,
    bool true_branch,
    const clang::ASTContext& context) {
    condition = strip_expr(condition);
    branch_expression = strip_expr(branch_expression);
    if (condition == nullptr || branch_expression == nullptr) {
        return std::nullopt;
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(condition);
        unary != nullptr && unary->getOpcode() == clang::UO_LNot) {
        return null_state_for_conditional_branch(
            unary->getSubExpr(), branch_expression, !true_branch, context);
    }

    if (same_lvalue_or_expression(condition, branch_expression) &&
        condition->getType()->isPointerType()) {
        return true_branch ? NullState::NonNull : NullState::Null;
    }

    const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(condition);
    if (binary == nullptr ||
        (binary->getOpcode() != clang::BO_EQ && binary->getOpcode() != clang::BO_NE)) {
        return std::nullopt;
    }

    const clang::Expr* subject = nullptr;
    if (is_null_pointer_constant(binary->getRHS(), context)) {
        subject = binary->getLHS();
    } else if (is_null_pointer_constant(binary->getLHS(), context)) {
        subject = binary->getRHS();
    }
    if (subject == nullptr || !same_lvalue_or_expression(subject, branch_expression)) {
        return std::nullopt;
    }

    const bool nonnull_when_true = binary->getOpcode() == clang::BO_NE;
    return true_branch == nonnull_when_true ? NullState::NonNull : NullState::Null;
}

PointerFact fact_for_expression(
    const clang::Expr* expression,
    const State& state,
    const NullSource& target,
    const clang::ASTContext& context);

PointerFact fact_for_conditional_branch(
    const clang::Expr* condition,
    const clang::Expr* branch_expression,
    bool true_branch,
    const State& state,
    const NullSource& target,
    const clang::ASTContext& context) {
    PointerFact fact = fact_for_expression(branch_expression, state, target, context);
    if (fact.origin == Origin::No) {
        return fact;
    }
    if (std::optional<NullState> refined =
            null_state_for_conditional_branch(condition, branch_expression, true_branch, context)) {
        fact.null_state = *refined;
    }
    return fact;
}

PointerFact fact_for_expression(
    const clang::Expr* expression,
    const State& state,
    const NullSource& target,
    const clang::ASTContext& context) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return {};
    }
    if (is_source(expression, target)) {
        return {
            .origin = Origin::Yes,
            .null_state = NullState::MaybeNull,
        };
    }
    // Check for any expression-level facts (members, array slots, etc.).
    {
        const PointerFact expr_fact = state.fact_for_expr(expression);
        if (expr_fact.origin != Origin::No) {
            return expr_fact;
        }
    }
    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        return state.fact(llvm::dyn_cast<clang::ValueDecl>(reference->getDecl()));
    }
    if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
        return fact_for_expression(cast->getSubExpr(), state, target, context);
    }
    if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
        return fact_for_expression(cleanup->getSubExpr(), state, target, context);
    }
    if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
        return fact_for_expression(opaque->getSourceExpr(), state, target, context);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
        return fact_for_expression(temporary->getSubExpr(), state, target, context);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
        return fact_for_expression(temporary->getSubExpr(), state, target, context);
    }
    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        if (binary->getOpcode() == clang::BO_Assign ||
            binary->getOpcode() == clang::BO_Comma) {
            return fact_for_expression(binary->getRHS(), state, target, context);
        }
        if (binary->getOpcode() == clang::BO_Add ||
            binary->getOpcode() == clang::BO_Sub) {
            const PointerFact lhs = fact_for_expression(binary->getLHS(), state, target, context);
            const PointerFact rhs = fact_for_expression(binary->getRHS(), state, target, context);
            if (lhs.origin == Origin::No) {
                return rhs;
            }
            if (rhs.origin == Origin::No) {
                return lhs;
            }
            return join_fact(lhs, rhs);
        }
    }
    if (const auto* conditional = llvm::dyn_cast<clang::ConditionalOperator>(expression)) {
        return join_fact(
            fact_for_conditional_branch(conditional->getCond(), conditional->getTrueExpr(), true,
                                        state, target, context),
            fact_for_conditional_branch(conditional->getCond(), conditional->getFalseExpr(), false,
                                        state, target, context));
    }
    if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(expression)) {
        return join_fact(
            fact_for_conditional_branch(conditional->getCond(), conditional->getCommon(), true,
                                        state, target, context),
            fact_for_conditional_branch(conditional->getCond(), conditional->getFalseExpr(), false,
                                        state, target, context));
    }
    return {};
}

struct BranchConstraint {
    bool recognized = false;
    const clang::ValueDecl* subject = nullptr;
    NullState true_state = NullState::MaybeNull;
    NullState false_state = NullState::MaybeNull;
};

BranchConstraint make_constraint(
    State& state,
    const clang::ValueDecl* subject,
    NullState true_state,
    NullState false_state) {
    state.prepare_refinement(subject);
    return {
        .recognized = true,
        .subject = subject,
        .true_state = true_state,
        .false_state = false_state,
    };
}

BranchConstraint make_constraint(
    State& state,
    const clang::Expr* subject_expr,
    NullState true_state,
    NullState false_state) {
    state.prepare_refinement_expr(subject_expr);
    return {
        .recognized = true,
        .subject = nullptr,
        .true_state = true_state,
        .false_state = false_state,
    };
}

BranchConstraint invert_constraint(BranchConstraint constraint) {
    if (constraint.recognized) {
        std::swap(constraint.true_state, constraint.false_state);
    }
    return constraint;
}

BranchConstraint condition_constraint(
    const clang::Expr* expression,
    State& state,
    const NullSource& target,
    const clang::ASTContext& context) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return {};
    }

    // Support boolean variables initialized from pointer-null comparisons, e.g.
    //  bool missing = ptr == NULL;
    //  if (missing) { ... }
    // In that case, analyze the initializer expression as the condition.
    if (const auto* ref = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        if (const auto* var = llvm::dyn_cast<clang::VarDecl>(ref->getDecl())) {
            if (var->getInit() != nullptr) {
                return condition_constraint(var->getInit(), state, target, context);
            }
        }
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression);
        unary != nullptr && unary->getOpcode() == clang::UO_LNot) {
        return invert_constraint(condition_constraint(
            unary->getSubExpr(), state, target, context));
    }

    const PointerFact direct = fact_for_expression(expression, state, target, context);
    if (direct.origin != Origin::No && expression->getType()->isPointerType()) {
        return make_constraint(
            state,
            lvalue_declaration(expression),
            NullState::NonNull,
            NullState::Null);
    }

    const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
    if (binary == nullptr ||
        (binary->getOpcode() != clang::BO_EQ &&
         binary->getOpcode() != clang::BO_NE)) {
        return {};
    }

    const PointerFact lhs = fact_for_expression(binary->getLHS(), state, target, context);
    const PointerFact rhs = fact_for_expression(binary->getRHS(), state, target, context);
    const clang::Expr* subject_expression = nullptr;
    if (lhs.origin != Origin::No &&
        is_null_pointer_constant(binary->getRHS(), context)) {
        subject_expression = binary->getLHS();
    } else if (rhs.origin != Origin::No &&
               is_null_pointer_constant(binary->getLHS(), context)) {
        subject_expression = binary->getRHS();
    }
    if (subject_expression == nullptr) {
        return {};
    }

    const clang::ValueDecl* subject_decl = lvalue_declaration(subject_expression);
    if (subject_decl != nullptr) {
        if (binary->getOpcode() == clang::BO_NE) {
            return make_constraint(
                state,
                subject_decl,
                NullState::NonNull,
                NullState::Null);
        }
        return make_constraint(
            state,
            subject_decl,
            NullState::Null,
            NullState::NonNull);
    }

    if (binary->getOpcode() == clang::BO_NE) {
        return make_constraint(
            state,
            subject_expression,
            NullState::NonNull,
            NullState::Null);
    }
    return make_constraint(
        state,
        subject_expression,
        NullState::Null,
        NullState::NonNull);
}
