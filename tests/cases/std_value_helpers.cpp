#include <string>
#include <tuple>
#include <optional>
#include <iterator>
#include <algorithm>
#include <span>
#include <vector>

void standard_value_helpers(std::string text) {
    std::vector<int> values;
    text.c_str();
    text.empty();
    text.substr(0U);
    text.find('x');
    static_cast<std::string_view>(text);
    std::optional<int>{}.has_value();
    std::optional<int>{}.value_or(0);
    values.insert(values.end(), 1);
    values.rbegin();
    values.rend();
    std::back_inserter(values);
    std::distance(values.begin(), values.end());
    std::for_each(values.begin(), values.end(), [](int) {});
    std::make_move_iterator(values.begin());
    std::max(1, 2);
    std::prev(values.end());
    std::rotate(values.begin(), values.begin(), values.end());
    std::span<int>(values).first(0U);
    std::make_tuple(1, 2);
    std::to_string(42);
}
