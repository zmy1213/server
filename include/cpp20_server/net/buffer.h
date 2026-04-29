#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cpp20_server::net {

class Buffer {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t readable_bytes() const noexcept;
    [[nodiscard]] std::string_view readable_view() const noexcept;

    void append(std::string_view data);
    void retrieve(std::size_t length);
    void clear() noexcept;

private:
    void compact_if_needed();

    std::string data_;
    std::size_t read_pos_{0};
};

} // namespace cpp20_server::net
