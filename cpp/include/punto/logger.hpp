/**
 * @file logger.hpp
 * @brief Syslog/journald integration for daemon logs
 */

#pragma once

#include <string_view>

#include "punto/config.hpp"

namespace punto {

void init_logging(std::string_view ident, LogLevel level);
void update_log_level(LogLevel level) noexcept;
void shutdown_logging() noexcept;

} // namespace punto
