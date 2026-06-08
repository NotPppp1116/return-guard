#include <returnguard/Options.hpp>

namespace returnguard {
namespace {
Options global_options;
}

void set_options(Options new_options) {
    global_options = new_options;
}

const Options& options() {
    return global_options;
}

} // namespace returnguard
