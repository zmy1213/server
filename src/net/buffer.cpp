#include "cpp20_server/net/buffer.h"

#include <algorithm>

namespace cpp20_server::net {

bool Buffer::empty() const noexcept {
    return readable_bytes() == 0;
}

std::size_t Buffer::readable_bytes() const noexcept {
    return data_.size() - read_pos_;
}

std::string_view Buffer::readable_view() const noexcept {
    return std::string_view{data_.data() + read_pos_, readable_bytes()};
}

void Buffer::append(std::string_view data) {
    data_.append(data.data(), data.size());
}

void Buffer::retrieve(std::size_t length) {
    read_pos_ += std::min(length, readable_bytes());
    compact_if_needed();
}

void Buffer::clear() noexcept {
    data_.clear();
    read_pos_ = 0;
}

void Buffer::compact_if_needed() {
    if (read_pos_ == data_.size()) {
        clear();
        return;
    }

    if (read_pos_ > 4096 && read_pos_ * 2 >= data_.size()) {
        data_.erase(0, read_pos_);
        read_pos_ = 0;
    }
}

} // namespace cpp20_server::net
