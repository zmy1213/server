#include "cpp20_server/base/config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cpp20_server::base {

namespace {

std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    return std::string{begin, end};
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::uint64_t parse_u64(std::string_view value, std::string_view key) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::runtime_error("config key '" + std::string{key} + "' must be an unsigned integer");
    }
    return parsed;
}

int parse_i32(std::string_view value, std::string_view key) {
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::runtime_error("config key '" + std::string{key} + "' must be an integer");
    }
    return parsed;
}

} // namespace

Config Config::from_file(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("failed to open config file: " + path);
    }

    std::ostringstream content;
    content << file.rdbuf();
    return from_text(content.str());
}

Config Config::from_text(std::string_view text) {
    Config config;
    std::size_t line_number = 0;
    std::size_t offset = 0;

    while (offset <= text.size()) {
        ++line_number;
        const auto line_end = text.find('\n', offset);
        const auto length = line_end == std::string_view::npos ? text.size() - offset : line_end - offset;
        auto line = std::string_view{text.data() + offset, length};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const auto comment = line.find_first_of("#;");
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }

        const std::string clean = trim(line);
        if (!clean.empty()) {
            const auto equal = clean.find('=');
            if (equal == std::string::npos) {
                throw std::runtime_error("invalid config line " + std::to_string(line_number) + ": missing '='");
            }
            config.set(trim(std::string_view{clean}.substr(0, equal)),
                       trim(std::string_view{clean}.substr(equal + 1)));
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        offset = line_end + 1;
    }

    return config;
}

void Config::set(std::string key, std::string value) {
    if (key.empty()) {
        throw std::runtime_error("config key cannot be empty");
    }
    values_[std::move(key)] = std::move(value);
}

bool Config::contains(std::string_view key) const {
    return values_.find(std::string{key}) != values_.end();
}

std::string Config::get_string(std::string_view key, std::string default_value) const {
    const auto it = values_.find(std::string{key});
    if (it == values_.end()) {
        return default_value;
    }
    return it->second;
}

bool Config::get_bool(std::string_view key, bool default_value) const {
    const auto it = values_.find(std::string{key});
    if (it == values_.end()) {
        return default_value;
    }

    const std::string value = lower_copy(it->second);
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error("config key '" + std::string{key} + "' must be a boolean");
}

int Config::get_int(std::string_view key, int default_value) const {
    const auto it = values_.find(std::string{key});
    if (it == values_.end()) {
        return default_value;
    }
    return parse_i32(it->second, key);
}

std::uint16_t Config::get_port(std::string_view key, std::uint16_t default_value) const {
    const auto value = get_u64(key, default_value);
    if (value == 0 || value > 65535) {
        throw std::runtime_error("config key '" + std::string{key} + "' must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::uint64_t Config::get_u64(std::string_view key, std::uint64_t default_value) const {
    const auto it = values_.find(std::string{key});
    if (it == values_.end()) {
        return default_value;
    }
    return parse_u64(it->second, key);
}

std::size_t Config::get_size(std::string_view key, std::size_t default_value) const {
    return static_cast<std::size_t>(get_u64(key, static_cast<std::uint64_t>(default_value)));
}

} // namespace cpp20_server::base
