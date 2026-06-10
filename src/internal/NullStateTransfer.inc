class Transfer final {
    public:
        Transfer(
                State& state,
                const NullSource& target,
                std::unordered_set<const clang::Expr*>* unsafe,
                clang::ASTContext& context)
                : state_(state), target_(target), unsafe_(unsafe), context_(context) {}

    void process(const clang::Stmt* statement) {
        if (statement == nullptr) {
            return;
        }

        if (const auto* declarations = llvm::dyn_cast<clang::DeclStmt>(statement)) {
            for (const clang::Decl* declaration : declarations->decls()) {
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                if (variable == nullptr) {
                    continue;
                }

                if (variable->getInit() == nullptr ||
                    !variable->getType()->isPointerType()) {
                    state_.set(variable, {});
                    continue;
                }
                state_.set(variable, evaluate(variable->getInit()));
            }
            return;
        }

        if (const auto* return_statement = llvm::dyn_cast<clang::ReturnStmt>(statement)) {
            evaluate(return_statement->getRetValue());
            return;
        }

        if (const auto* expression = llvm::dyn_cast<clang::Expr>(statement)) {
            evaluate(expression);
        }
    }

    PointerFact evaluate(const clang::Expr* expression) {
        expression = strip_expr(expression);
        if (expression == nullptr) {
            return {};
        }

        // Treat explicit null pointer constants as definite null sources.
        if (is_null_pointer_constant(expression, context_)) {
            return {
                .origin = Origin::Yes,
                .null_state = NullState::Null,
            };
        }

        if (is_source(expression, target_)) {
            if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
                for (const clang::Expr* argument : call->arguments()) {
                    evaluate(argument);
                }
            }
            return {
                .origin = Origin::Yes,
                .null_state = NullState::MaybeNull,
            };
        }

        // Prefer expression-level facts for members/array slots that were
        // recorded during assignment handling.
        {
            const PointerFact expr_fact = state_.fact_for_expr(expression);
            if (expr_fact.origin != Origin::No) {
                return expr_fact;
            }
        }

        if (const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(expression)) {
            return state_.fact(llvm::dyn_cast<clang::ValueDecl>(reference->getDecl()));
        }
        if (const auto* cast = llvm::dyn_cast<clang::CastExpr>(expression)) {
            return evaluate(cast->getSubExpr());
        }
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expression)) {
            return evaluate(cleanup->getSubExpr());
        }
        if (const auto* opaque = llvm::dyn_cast<clang::OpaqueValueExpr>(expression)) {
            return evaluate(opaque->getSourceExpr());
        }
        if (const auto* temporary = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expression)) {
            return evaluate(temporary->getSubExpr());
        }
        if (const auto* temporary = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expression)) {
            return evaluate(temporary->getSubExpr());
        }
        if (llvm::isa<clang::UnaryExprOrTypeTraitExpr>(expression)) {
            return {};
        }

        if (const auto* member = llvm::dyn_cast<clang::MemberExpr>(expression)) {
            const PointerFact base = evaluate(member->getBase());
            if (member->isArrow()) {
                diagnose(expression, base);
            }
            const PointerFact expr_fact = state_.fact_for_expr(expression);
            if (expr_fact.origin != Origin::No) {
                return expr_fact;
            }
            return {};
        }

        if (const auto* subscript = llvm::dyn_cast<clang::ArraySubscriptExpr>(expression)) {
            const PointerFact base = evaluate(subscript->getBase());
            diagnose(expression, base);
            evaluate(subscript->getIdx());
            const PointerFact expr_fact = state_.fact_for_expr(expression);
            if (expr_fact.origin != Origin::No) {
                return expr_fact;
            }
            return {};
        }

        if (const auto* unary = llvm::dyn_cast<clang::UnaryOperator>(expression)) {
            if (unary->getOpcode() == clang::UO_AddrOf) {
                return {};
            }

            PointerFact operand = evaluate(unary->getSubExpr());
            if (unary->getOpcode() == clang::UO_Deref) {
                diagnose(expression, operand);
                return {};
            }
            if (unary->getOpcode() == clang::UO_Extension) {
                return operand;
            }
            return {};
        }

        if (const auto* binary = llvm::dyn_cast<clang::BinaryOperator>(expression)) {
            if (binary->getOpcode() == clang::BO_Assign) {
                const PointerFact value = evaluate(binary->getRHS());
                evaluate_assignment_target(binary->getLHS(), value);
                if (const clang::ValueDecl* declaration = lvalue_declaration(binary->getLHS())) {
                    state_.set(
                        declaration,
                        declaration->getType()->isPointerType()
                            ? value
                            : PointerFact{});
                }
                return value;
            }

            if (binary->getOpcode() == clang::BO_Comma) {
                evaluate(binary->getLHS());
                return evaluate(binary->getRHS());
            }

            if (binary->getOpcode() == clang::BO_LAnd ||
                binary->getOpcode() == clang::BO_LOr) {
                return fact_for_expression(expression, state_, target_);
            }

            PointerFact lhs = evaluate(binary->getLHS());
            PointerFact rhs = evaluate(binary->getRHS());
            if (binary->getOpcode() == clang::BO_Add ||
                binary->getOpcode() == clang::BO_Sub) {
                if (lhs.origin == Origin::No) {
                    return rhs;
                }
                if (rhs.origin == Origin::No) {
                    return lhs;
                }
                return join_fact(lhs, rhs);
            }
            return {};
        }

        if (llvm::isa<clang::ConditionalOperator>(expression) ||
            llvm::isa<clang::BinaryConditionalOperator>(expression)) {
            return fact_for_expression(expression, state_, target_);
        }

        if (const auto* call = llvm::dyn_cast<clang::CallExpr>(expression)) {
            if (call->getDirectCallee() == nullptr) {
                const PointerFact callee = evaluate(call->getCallee());
                diagnose(expression, callee);
            }
            for (const clang::Expr* argument : call->arguments()) {
                evaluate(argument);
            }
            return {};
        }

        for (const clang::Stmt* child : expression->children()) {
            if (const auto* child_expression = llvm::dyn_cast_or_null<clang::Expr>(child)) {
                evaluate(child_expression);
            }
        }
        return {};
    }

  private:
    void evaluate_assignment_target(const clang::Expr* expression, const PointerFact& value) {
        expression = strip_expr(expression);
        if (expression == nullptr) {
            return;
        }
        if (llvm::isa<clang::DeclRefExpr>(expression)) {
            return;
        }
        if (expression->getType() != nullptr && expression->getType()->isPointerType()) {
            state_.set_expr(expression, value);
        }
        evaluate(expression);
    }

    void diagnose(const clang::Expr* expression, PointerFact pointer) {
        if (unsafe_ == nullptr || pointer.origin == Origin::No ||
            pointer.null_state == NullState::NonNull) {
            return;
        }
        unsafe_->insert(expression);
    }

    State& state_;
    const NullSource& target_;
    std::unordered_set<const clang::Expr*>* unsafe_;
    clang::ASTContext& context_;
};

void transfer_block(
    const clang::CFGBlock& block,
    State& state,
    const NullSource& target,
    std::unordered_set<const clang::Expr*>* unsafe,
    const clang::ASTContext& context) {
    Transfer transfer(state, target, unsafe, const_cast<clang::ASTContext&>(context));
    for (const clang::CFGElement& element : block) {
        if (const auto statement = element.getAs<clang::CFGStmt>()) {
            transfer.process(statement->getStmt());
        }
    }

    if (const clang::Expr* condition = block.getLastCondition()) {
        transfer.evaluate(condition);
    }
}

State state_for_successor(
    const clang::CFGBlock& block,
    State state,
    unsigned successor_index,
    const NullSource& target,
    const clang::ASTContext& context) {
    if (block.succ_size() != 2U || successor_index > 1U) {
        return state;
    }

    const BranchConstraint constraint =
        condition_constraint(block.getLastCondition(), state, target, context);
    if (!constraint.recognized) {
        return state;
    }

    state.refine(
        successor_index == 0U
            ? constraint.true_state
            : constraint.false_state);
    return state;
}
