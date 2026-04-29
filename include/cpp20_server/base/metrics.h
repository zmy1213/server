#pragma once

#include "cpp20_server/net/tcp_server.h"

#include <string>
#include <string_view>

namespace cpp20_server::base {

[[nodiscard]] std::string format_server_stats_text(const net::ServerStats& stats);
[[nodiscard]] std::string format_server_stats_prometheus(const net::ServerStats& stats,
                                                         std::string_view backend = {});

} // namespace cpp20_server::base
