#pragma once

#include <string>

namespace returnguard {

enum class Mode {
    Practical,
    Strict,
    IgnoredOnly,
};

struct Options {
    Mode mode = Mode::Practical;
    bool analyze_headers = false;
    bool include_operators = false;
    bool include_reference_returns = false;
    bool explicit_void_is_handled = true;
    bool fail_on_diagnostics = false;
    bool color = true;
    std::string instrument_output;
    std::string site_map_output;
    std::string site_root;
};

void set_options(Options options);
[[nodiscard]] const Options& options();

} // namespace returnguard
