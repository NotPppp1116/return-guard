#pragma once

#include <clang/Tooling/Tooling.h>

#include <memory>

namespace clang {
class FrontendAction;
}

namespace returnguard {

[[nodiscard]] std::unique_ptr<clang::FrontendAction> make_frontend_action();

class ActionFactory final : public clang::tooling::FrontendActionFactory {
public:
    [[nodiscard]] std::unique_ptr<clang::FrontendAction> create() override;
};

} // namespace returnguard
