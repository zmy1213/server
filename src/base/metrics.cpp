#include "cpp20_server/base/metrics.h"

#include <sstream>

namespace cpp20_server::base {

std::string format_server_stats_text(const net::ServerStats& stats) {
    std::ostringstream out;
    out << "accepted_connections=" << stats.accepted_connections
        << " active_connections=" << stats.active_connections
        << " bytes_read=" << stats.bytes_read
        << " bytes_written=" << stats.bytes_written;
    return out.str();
}

std::string format_server_stats_prometheus(const net::ServerStats& stats, std::string_view backend) {
    const std::string labels = backend.empty() ? "" : "{backend=\"" + std::string{backend} + "\"}";

    std::ostringstream out;
    out << "# HELP cpp20_server_accepted_connections_total Total accepted TCP connections.\n"
        << "# TYPE cpp20_server_accepted_connections_total counter\n"
        << "cpp20_server_accepted_connections_total" << labels << ' ' << stats.accepted_connections << '\n'
        << "# HELP cpp20_server_active_connections Current open TCP connections.\n"
        << "# TYPE cpp20_server_active_connections gauge\n"
        << "cpp20_server_active_connections" << labels << ' ' << stats.active_connections << '\n'
        << "# HELP cpp20_server_bytes_read_total Total bytes read from clients.\n"
        << "# TYPE cpp20_server_bytes_read_total counter\n"
        << "cpp20_server_bytes_read_total" << labels << ' ' << stats.bytes_read << '\n'
        << "# HELP cpp20_server_bytes_written_total Total bytes written to clients.\n"
        << "# TYPE cpp20_server_bytes_written_total counter\n"
        << "cpp20_server_bytes_written_total" << labels << ' ' << stats.bytes_written << '\n';
    return out.str();
}

} // namespace cpp20_server::base
