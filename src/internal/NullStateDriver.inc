NullStateAnalysis::NullStateAnalysis(
    std::unique_ptr<clang::CFG> cfg,
    clang::ASTContext& context)
    : cfg_(std::move(cfg)), context_(context) {}

std::unique_ptr<NullStateAnalysis> NullStateAnalysis::build(
    const clang::FunctionDecl& function,
    clang::ASTContext& context) {
    if (!function.doesThisDeclarationHaveABody()) {
        return nullptr;
    }

    clang::CFG::BuildOptions options;
    options.PruneTriviallyFalseEdges = true;
    options.AddInitializers = true;

    std::unique_ptr<clang::CFG> cfg = clang::CFG::buildCFG(
        &function,
        function.getBody(),
        &context,
        options);
    if (!cfg) {
        return nullptr;
    }

    return std::unique_ptr<NullStateAnalysis>(
        new NullStateAnalysis(std::move(cfg), context));
}

std::vector<const clang::Expr*>
NullStateAnalysis::unsafe_dereferences_for(const clang::CallExpr& call) const {
    const unsigned block_count = cfg_->getNumBlockIDs();
    std::vector<std::optional<State>> inputs(block_count);
    std::deque<const clang::CFGBlock*> worklist;

    const clang::CFGBlock& entry = cfg_->getEntry();
    inputs[entry.getBlockID()] = State{};
    worklist.push_back(&entry);

    while (!worklist.empty()) {
        const clang::CFGBlock* block = worklist.front();
        worklist.pop_front();

        State output = *inputs[block->getBlockID()];
        transfer_block(*block, output, call, nullptr);

        unsigned successor_index = 0U;
        for (const clang::CFGBlock* successor : block->succs()) {
            if (successor != nullptr) {
                State edge = state_for_successor(
                    *block,
                    output,
                    successor_index,
                    call,
                    context_);
                std::optional<State>& input = inputs[successor->getBlockID()];
                if (!input.has_value()) {
                    input = std::move(edge);
                    worklist.push_back(successor);
                } else {
                    State merged = *input;
                    if (merged.join(edge)) {
                        input = std::move(merged);
                        worklist.push_back(successor);
                    }
                }
            }
            ++successor_index;
        }
    }

    std::unordered_set<const clang::Expr*> unsafe;
    for (const clang::CFGBlock* block : *cfg_) {
        if (block == nullptr || !inputs[block->getBlockID()].has_value()) {
            continue;
        }
        State state = *inputs[block->getBlockID()];
        transfer_block(*block, state, call, &unsafe);
    }

    std::vector<const clang::Expr*> result(unsafe.begin(), unsafe.end());
    const clang::SourceManager& source_manager = context_.getSourceManager();
    std::sort(
        result.begin(),
        result.end(),
        [&source_manager](const clang::Expr* lhs, const clang::Expr* rhs) {
            const clang::SourceLocation lhs_location =
                source_manager.getExpansionLoc(lhs->getExprLoc());
            const clang::SourceLocation rhs_location =
                source_manager.getExpansionLoc(rhs->getExprLoc());
            return source_manager.isBeforeInTranslationUnit(
                lhs_location,
                rhs_location);
        });
    return result;
}
