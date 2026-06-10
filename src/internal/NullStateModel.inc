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
    if (lhs.origin != rhs.origin) {
        return {
            .origin = Origin::Maybe,
            .null_state = NullState::MaybeNull,
        };
    }
    return {
        .origin = lhs.origin,
        .null_state = join_null_state(lhs.null_state, rhs.null_state),
    };
}

struct State {
    std::unordered_map<const clang::ValueDecl*, PointerFact> values;

    bool operator==(const State&) const = default;

    [[nodiscard]] PointerFact fact(const clang::ValueDecl* declaration) const {
        const auto iterator = values.find(declaration);
        return iterator == values.end() ? PointerFact{} : iterator->second;
    }

    void set(const clang::ValueDecl* declaration, PointerFact value) {
        if (declaration == nullptr || value.origin == Origin::No) {
            values.erase(declaration);
            return;
        }
        values[declaration] = value;
    }

    void refine(NullState null_state) {
        for (auto& [declaration, value] : values) {
            (void)declaration;
            if (value.origin == Origin::Yes) {
                value.null_state = null_state;
            } else if (value.origin == Origin::Maybe) {
                value.null_state = NullState::MaybeNull;
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
        return changed;
    }
};

bool is_null_pointer_constant(
    const clang::Expr* expression,
    const clang::ASTContext& context) {
    return expression != nullptr &&
           expression->isNullPointerConstant(
               const_cast<clang::ASTContext&>(context),
               clang::Expr::NPC_ValueDependentIsNotNull) !=
               clang::Expr::NPCK_NotNull;
}

PointerFact fact_for_expression(
    const clang::Expr* expression,
    const State& state,
    const clang::CallExpr& target) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return {};
    }
    if (expression == &target) {
        return {
            .origin = Origin::Yes,
            .null_state = NullState::MaybeNull,
        };
    }
    if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
        return state.fact(llvm::dyn_cast<clang::ValueDecl>(reference->getDecl()));
    }
    if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
        return fact_for_expression(cast->getSubExpr(), state, target);
    }
    if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
        return fact_for_expression(cleanup->getSubExpr(), state, target);
    }
    if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
        return fact_for_expression(opaque->getSourceExpr(), state, target);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
        return fact_for_expression(temporary->getSubExpr(), state, target);
    }
    if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
        return fact_for_expression(temporary->getSubExpr(), state, target);
    }
    if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
        if (binary->getOpcode() == clang::BO_Assign ||
            binary->getOpcode() == clang::BO_Comma) {
            return fact_for_expression(binary->getRHS(), state, target);
        }
        if (binary->getOpcode() == clang::BO_Add ||
            binary->getOpcode() == clang::BO_Sub) {
            const PointerFact lhs = fact_for_expression(binary->getLHS(), state, target);
            const PointerFact rhs = fact_for_expression(binary->getRHS(), state, target);
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
            fact_for_expression(conditional->getTrueExpr(), state, target),
            fact_for_expression(conditional->getFalseExpr(), state, target));
    }
    if (const auto* conditional = llvm::dyn_cast<clang::BinaryConditionalOperator>(expression)) {
        return join_fact(
            fact_for_expression(conditional->getCommon(), state, target),
            fact_for_expression(conditional->getFalseExpr(), state, target));
    }
    return {};
}

struct BranchConstraint {
    bool recognized = false;
    NullState true_state = NullState::MaybeNull;
    NullState false_state = NullState::MaybeNull;
};

BranchConstraint invert_constraint(BranchConstraint constraint) {
    if (constraint.recognized) {
        std::swap(constraint.true_state, constraint.false_state);
    }
    return constraint;
}

BranchConstraint condition_constraint(
    const clang::Expr* expression,
    const State& state,
    const clang::CallExpr& target,
    const clang::ASTContext& context) {
    expression = strip_expr(expression);
    if (expression == nullptr) {
        return {};
    }

    if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression);
        unary != nullptr && unary->getOpcode() == clang::UO_LNot) {
        return invert_constraint(condition_constraint(
            unary->getSubExpr(), state, target, context));
    }

    const PointerFact direct = fact_for_expression(expression, state, target);
    if (direct.origin == Origin::Yes && expression->getType()->isPointerType()) {
        return {
            .recognized = true,
            .true_state = NullState::NonNull,
            .false_state = NullState::Null,
        };
    }

    const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression);
    if (binary == nullptr ||
        (binary->getOpcode() != clang::BO_EQ &&
         binary->getOpcode() != clang::BO_NE)) {
        return {};
    }

    const PointerFact lhs = fact_for_expression(binary->getLHS(), state, target);
    const PointerFact rhs = fact_for_expression(binary->getRHS(), state, target);
    const bool target_and_null =
        (lhs.origin == Origin::Yes && is_null_pointer_constant(binary->getRHS(), context)) ||
        (rhs.origin == Origin::Yes && is_null_pointer_constant(binary->getLHS(), context));
    if (!target_and_null) {
        return {};
    }

    if (binary->getOpcode() == clang::BO_NE) {
        return {
            .recognized = true,
            .true_state = NullState::NonNull,
            .false_state = NullState::Null,
        };
    }
    return {
        .recognized = true,
        .true_state = NullState::Null,
        .false_state = NullState::NonNull,
    };
}

const clang::ValueDecl* lvalue_declaration(const clang::Expr* expression) {
    expression = strip_expr(expression);
    const auto* reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression);
    return reference == nullptr
               ? nullptr
               : llvm::dyn_cast<clang::ValueDecl>(reference->getDecl());
}
