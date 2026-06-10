#pragma once

namespace returnguard::internal {

inline void refine_checked_alias(
    State& state,
    const BranchConstraint& constraint,
    NullState null_state) {
    state.refine(constraint.subject, null_state);
}

} // namespace returnguard::internal
