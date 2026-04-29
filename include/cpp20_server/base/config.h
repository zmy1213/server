#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cpp20_server::base {

class Config {
public:
    static Config from_file(const std::string& path);
    static Config from_text(std::string_view text);

    void set(std::string key, std::string value);

    [[nodiscard]] bool contains(std::string_view key) const;
    [[nodiscard]] std::string get_string(std::string_view key, std::string default_value = {}) const;
    [[nodiscard]] bool get_bool(std::string_view key, bool default_value) const;
    [[nodiscard]] int get_int(std::string_view key, int default_value) const;
    [[nodiscard]] std::uint16_t get_port(std::string_view key, std::uint16_t default_value) const;
    [[nodiscard]] std::uint64_t get_u64(std::string_view key, std::uint64_t default_value) const;
    [[nodiscard]] std::size_t get_size(std::string_view key, std::size_t default_value) const;

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace cpp20_server::base
