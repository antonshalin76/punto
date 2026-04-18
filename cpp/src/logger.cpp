/**
 * @file logger.cpp
 * @brief Syslog-backed stream redirection for daemon diagnostics
 */

#include "punto/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <syslog.h>

namespace punto {

namespace {

LogLevel g_min_log_level = LogLevel::Info;
std::streambuf *g_original_cerr = nullptr;
std::string g_ident = "punto";

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack,
                                             std::string_view needle) {
  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) ==
               std::tolower(static_cast<unsigned char>(rhs));
      });
  return it != haystack.end();
}

[[nodiscard]] LogLevel infer_log_level(std::string_view line) {
  if (contains_case_insensitive(line, "fatal") ||
      contains_case_insensitive(line, "error") ||
      contains_case_insensitive(line, "failed") ||
      contains_case_insensitive(line, "abort")) {
    return LogLevel::Error;
  }

  if (contains_case_insensitive(line, "warn") ||
      contains_case_insensitive(line, "предупреждение")) {
    return LogLevel::Warning;
  }

  if (contains_case_insensitive(line, "telemetry") ||
      contains_case_insensitive(line, "async-") ||
      contains_case_insensitive(line, "detect_case_pattern") ||
      contains_case_insensitive(line, "input guard") ||
      contains_case_insensitive(line, "startup layout") ||
      contains_case_insensitive(line, "x11 session:") ||
      contains_case_insensitive(line, "loaded en dict") ||
      contains_case_insensitive(line, "loaded ru dict") ||
      contains_case_insensitive(line, "hash memory") ||
      contains_case_insensitive(line, "bloom fill")) {
    return LogLevel::Debug;
  }

  return LogLevel::Info;
}

[[nodiscard]] int to_syslog_priority(LogLevel level) {
  switch (level) {
  case LogLevel::Error:
    return LOG_ERR;
  case LogLevel::Warning:
    return LOG_WARNING;
  case LogLevel::Info:
    return LOG_INFO;
  case LogLevel::Debug:
  default:
    return LOG_DEBUG;
  }
}

class SyslogStreamBuf final : public std::streambuf {
public:
  explicit SyslogStreamBuf(std::streambuf *fallback) : fallback_{fallback} {}

  ~SyslogStreamBuf() override { sync(); }

protected:
  int overflow(int ch) override {
    if (ch == traits_type::eof()) {
      return sync() == 0 ? 0 : traits_type::eof();
    }

    buffer_.push_back(static_cast<char>(ch));
    if (ch == '\n') {
      flush_buffer();
    }
    return ch;
  }

  int sync() override {
    flush_buffer();
    return 0;
  }

private:
  void flush_buffer() {
    while (!buffer_.empty()) {
      const std::size_t newline = buffer_.find('\n');
      if (newline == std::string::npos) {
        break;
      }

      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      emit_line(line);
    }
  }

  void emit_line(const std::string &line) const {
    if (line.empty()) {
      return;
    }

    const LogLevel level = infer_log_level(line);
    if (static_cast<int>(level) > static_cast<int>(g_min_log_level)) {
      return;
    }

    syslog(to_syslog_priority(level), "%s", line.c_str());
    if (fallback_ != nullptr && should_echo_to_stderr()) {
      fallback_->sputn(line.data(), static_cast<std::streamsize>(line.size()));
      fallback_->sputc('\n');
    }
  }

  [[nodiscard]] bool should_echo_to_stderr() const {
    const char *env = std::getenv("PUNTO_LOG_STDERR");
    return env != nullptr && std::string_view{env} == "1";
  }

  std::streambuf *fallback_ = nullptr;
  std::string buffer_;
};

std::unique_ptr<SyslogStreamBuf> g_syslog_buf;

} // namespace

void init_logging(std::string_view ident, LogLevel level) {
  g_min_log_level = level;
  g_ident.assign(ident.begin(), ident.end());
  ::openlog(g_ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);

  if (g_syslog_buf) {
    return;
  }

  g_original_cerr = std::cerr.rdbuf();
  g_syslog_buf = std::make_unique<SyslogStreamBuf>(g_original_cerr);
  std::cerr.rdbuf(g_syslog_buf.get());
}

void update_log_level(LogLevel level) noexcept { g_min_log_level = level; }

void shutdown_logging() noexcept {
  if (g_original_cerr != nullptr) {
    std::cerr.rdbuf(g_original_cerr);
    g_original_cerr = nullptr;
  }
  g_syslog_buf.reset();
  ::closelog();
}

} // namespace punto
