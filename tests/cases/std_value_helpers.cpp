#include <string>
#include <tuple>
#include <optional>

void standard_value_helpers(std::string text) {
    text.c_str();
    text.empty();
    text.substr(0U);
    text.find('x');
    static_cast<std::string_view>(text);
    std::optional<int>{}.has_value();
    std::optional<int>{}.value_or(0);
    std::make_tuple(1, 2);
    std::to_string(42);
}
