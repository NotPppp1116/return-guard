#include <string>
#include <tuple>
#include <optional>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>
#include <vector>

using namespace std::string_literals;

void standard_value_helpers(std::string text) {
    std::vector<int> values;
    "/tmp/returnguard"s;
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

static unsigned int strhash(const char* text, int length) {
    unsigned int hash = 0;
    for (int index = 0; index < length; ++index)
        hash = hash * 33U + static_cast<unsigned char>(text[index]);
    return hash;
}

static int str_ends_with(const char* text, int length, const char* suffix) {
    int suffix_length = static_cast<int>(std::strlen(suffix));
    if (suffix_length > length)
        return 0;
    return std::memcmp(text + length - suffix_length, suffix, suffix_length) == 0;
}

static bool is_no_parse_file(const char* text, int length) {
    return str_ends_with(text, length, ".rlib") || str_ends_with(text, length, ".rmeta");
}

static bool has_known_prefix(const char* text) {
    return std::strstr(text, "CONFIG_") == text;
}

static bool in_hashtable(const char*, int) {
    return false;
}

static bool sym_has_value(const char*) {
    return true;
}

static const char* next(const char* text) {
    return text + 1;
}

static const char* menu_next(const char* text) {
    return text + 1;
}

static const char* str_get(const char* text) {
    return text;
}

static const char* str_new() {
    return "";
}

struct AccessorSample {
    std::string message() const { return {}; }
    const char* protocol() const { return ""; }
    const char* specName() const { return ""; }
    int specVer() const { return 1; }
};

static void* xmalloc(unsigned long) {
    return nullptr;
}

void c_value_helpers(const char* text, int length) {
    std::strlen(text);
    std::strcmp(text, "CONFIG");
    std::strncmp(text, "CONFIG", 6U);
    std::strstr(text, "CONFIG_");
    std::strchr(text, '_');
    std::memcmp(text, "CONFIG", 6U);
    std::isalnum(static_cast<unsigned char>(text[0]));
    strhash(text, length);
    std::rand();
    str_get(text);
    str_new();
    xmalloc(16U);
    AccessorSample{}.message();
    AccessorSample{}.protocol();
    AccessorSample{}.specName();
    AccessorSample{}.specVer();

    if (!is_no_parse_file(text, length) && has_known_prefix(text) &&
        !in_hashtable(text, length) && sym_has_value(text))
        std::strlen(text);
    while ((text = next(text)) && *text != '\0')
        std::strlen(text);
    while ((text = menu_next(text)) && *text != '\0')
        std::strlen(text);
}

static inline long PTR_ERR(const void* ptr) {
    return reinterpret_cast<long>(ptr);
}

static inline bool IS_ERR(const void* ptr) {
    return reinterpret_cast<unsigned long>(ptr) >= static_cast<unsigned long>(-4095);
}

void kernel_error_pointer_helpers(void* ptr) {
    PTR_ERR(ptr);
    IS_ERR(ptr);
}
