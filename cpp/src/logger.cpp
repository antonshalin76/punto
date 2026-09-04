/**
 * @file logger.cpp
 * @brief Syslog-backed stream redirection for daemon diagnostics
 */

#include "punto/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <streambuf>
#include <string>
#include <syslog.h>
#include <thread>
#include <unordered_map>
#include <utility>

namespace punto {

namespace {

// Атомик: update_log_level() вызывается из IPC-потока (reload_config), а
// чтение уровня происходит из всех потоков, пишущих в std::cerr.
std::atomic<LogLevel> g_min_log_level{LogLevel::Info};
std::streambuf *g_original_cerr = nullptr;
bool g_restore_cerr_unitbuf = false;
std::string g_ident = "punto";
std::atomic<std::uint64_t> g_dropped_records{0};

using WriterId = std::uint64_t;

[[nodiscard]] WriterId current_writer_id() noexcept {
  static std::atomic<WriterId> next_writer_id{1};
  thread_local const WriterId writer_id =
      next_writer_id.fetch_add(1, std::memory_order_relaxed);
  return writer_id;
}

[[nodiscard]] bool contains_case_insensitive(std::string_view haystack,
                                             std::string_view needle) {
  auto it =
      std::search(haystack.begin(), haystack.end(), needle.begin(),
                  needle.end(), [](char lhs, char rhs) {
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
  explicit SyslogStreamBuf(std::streambuf *fallback)
      : fallback_{fallback}, echo_to_stderr_{should_echo_to_stderr()},
        sink_thread_{[this] { sink_loop(); }} {}

  ~SyslogStreamBuf() override { shutdown(); }

  void shutdown() noexcept {
    if (shutdown_complete_) {
      return;
    }
    try {
      flush_all();
    } catch (...) {
      note_drop();
    }
    {
      const std::lock_guard lock{queue_mutex_};
      stop_requested_ = true;
    }
    queue_condition_.notify_one();

    std::unique_lock completion_lock{completion_mutex_};
    if (!completion_condition_.wait_for(completion_lock, kShutdownDeadline,
                                        [this] { return sink_complete_; })) {
      // A sink stuck in libc or a diagnostic fallback may still reference
      // this object and process-static state. Destruction would race it.
      std::_Exit(3);
    }
    completion_lock.unlock();
    if (sink_thread_.joinable()) {
      sink_thread_.join();
    }
    shutdown_complete_ = true;
  }

protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return sync() == 0 ? traits_type::not_eof(ch) : traits_type::eof();
    }

    const char value = traits_type::to_char_type(ch);
    return xsputn(&value, 1) == 1 ? ch : traits_type::eof();
  }

  std::streamsize xsputn(const char *data, std::streamsize count) override {
    if (count <= 0) {
      return 0;
    }

    const auto size = static_cast<std::size_t>(count);
    std::size_t offset = 0;
    while (offset < size) {
      auto record = take_next_record(data, size, offset);
      if (record.has_value()) {
        enqueue_line(std::move(*record));
      }
    }
    return count;
  }

  int sync() override {
    try {
      auto record = take_current_partial();
      if (record.has_value()) {
        enqueue_line(std::move(*record));
      }
      return 0;
    } catch (...) {
      return -1;
    }
  }

private:
  static constexpr std::size_t kMaxRecordBytes = 8192;
  static constexpr std::size_t kMaxPartialWriters =
      config_limits::kAnalysisThreadsMax + 32U;
  static constexpr std::size_t kMaxQueuedRecords = 1024;
  static constexpr auto kShutdownDeadline = std::chrono::seconds{3};

  struct QueuedRecord {
    int priority = LOG_INFO;
    std::string line;
  };

  [[nodiscard]] std::optional<std::string>
  take_next_record(const char *data, std::size_t size, std::size_t &offset) {
    const std::lock_guard lock{state_mutex_};
    const WriterId writer = current_writer_id();

    auto it = partials_.find(writer);
    if (it == partials_.end()) {
      if (partials_.size() >= kMaxPartialWriters) {
        auto evicted = partials_.begin();
        std::string record = std::move(evicted->second);
        partials_.erase(evicted);
        return record;
      }
      it = partials_.try_emplace(writer).first;
    }

    std::string &partial = it->second;
    while (offset < size) {
      if (data[offset] == '\n') {
        ++offset;
        if (partial.empty()) {
          partials_.erase(it);
          return std::nullopt;
        }
        std::string record = std::move(partial);
        partials_.erase(it);
        return record;
      }

      const std::size_t available = kMaxRecordBytes - partial.size();
      const std::size_t newline =
          std::string_view{data + offset, size - offset}.find('\n');
      const std::size_t until_newline =
          newline == std::string_view::npos ? size - offset : newline;
      const std::size_t append_size = std::min(available, until_newline);
      partial.append(data + offset, append_size);
      offset += append_size;

      if (partial.size() == kMaxRecordBytes) {
        std::string record = std::move(partial);
        partials_.erase(it);
        return record;
      }
    }

    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string> take_current_partial() {
    const std::lock_guard lock{state_mutex_};
    const auto it = partials_.find(current_writer_id());
    if (it == partials_.end() || it->second.empty()) {
      return std::nullopt;
    }

    std::string record = std::move(it->second);
    partials_.erase(it);
    return record;
  }

  void flush_all() {
    decltype(partials_) partials;
    {
      const std::lock_guard lock{state_mutex_};
      partials.swap(partials_);
    }

    for (auto &entry : partials) {
      std::string &record = entry.second;
      if (!record.empty()) {
        enqueue_line(std::move(record));
      }
    }
  }

  void enqueue_line(std::string line) noexcept {
    if (line.empty()) {
      return;
    }

    const LogLevel level = infer_log_level(line);
    if (static_cast<int>(level) >
        static_cast<int>(g_min_log_level.load(std::memory_order_relaxed))) {
      return;
    }

    try {
      {
        const std::lock_guard lock{queue_mutex_};
        if (!accepting_ || queue_.size() >= kMaxQueuedRecords) {
          note_drop();
          return;
        }
        queue_.push_back(
            QueuedRecord{to_syslog_priority(level), std::move(line)});
      }
      queue_condition_.notify_one();
    } catch (...) {
      note_drop();
    }
  }

  [[nodiscard]] static bool should_echo_to_stderr() noexcept {
    const char *env = std::getenv("PUNTO_LOG_STDERR");
    return env != nullptr && std::string_view{env} == "1";
  }

  static void note_drop() noexcept {
    g_dropped_records.fetch_add(1, std::memory_order_relaxed);
  }

  void sink_loop() noexcept {
    for (;;) {
      QueuedRecord record;
      {
        std::unique_lock lock{queue_mutex_};
        queue_condition_.wait(
            lock, [this] { return stop_requested_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_requested_) {
            accepting_ = false;
            break;
          }
          continue;
        }
        record = std::move(queue_.front());
        queue_.pop_front();
      }

      try {
        ::syslog(record.priority, "%s", record.line.c_str());
        if (fallback_ != nullptr && echo_to_stderr_) {
          fallback_->sputn(record.line.data(),
                           static_cast<std::streamsize>(record.line.size()));
          fallback_->sputc('\n');
        }
      } catch (...) {
        note_drop();
      }
    }

    {
      const std::lock_guard lock{completion_mutex_};
      sink_complete_ = true;
    }
    completion_condition_.notify_all();
  }

  std::streambuf *fallback_ = nullptr;
  bool echo_to_stderr_ = false;
  std::mutex state_mutex_;
  std::unordered_map<WriterId, std::string> partials_;
  std::mutex queue_mutex_;
  std::condition_variable queue_condition_;
  std::deque<QueuedRecord> queue_;
  bool stop_requested_ = false;
  bool accepting_ = true;
  std::mutex completion_mutex_;
  std::condition_variable completion_condition_;
  bool sink_complete_ = false;
  bool shutdown_complete_ = false;
  std::thread sink_thread_;
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

  std::streambuf *const original_cerr = std::cerr.rdbuf();
  auto syslog_buf = std::make_unique<SyslogStreamBuf>(original_cerr);
  const bool restore_cerr_unitbuf =
      (std::cerr.flags() & std::ios_base::unitbuf) != std::ios_base::fmtflags{};
  std::cerr.unsetf(std::ios_base::unitbuf);
  std::cerr.rdbuf(syslog_buf.get());
  g_original_cerr = original_cerr;
  g_restore_cerr_unitbuf = restore_cerr_unitbuf;
  g_syslog_buf = std::move(syslog_buf);
}

void update_log_level(LogLevel level) noexcept { g_min_log_level = level; }

std::uint64_t dropped_log_records() noexcept {
  return g_dropped_records.load(std::memory_order_relaxed);
}

void shutdown_logging() noexcept {
  if (g_syslog_buf) {
    // EventLoop has already stopped every producer. Keep the streambuf
    // installed during the drain so sink-side reentrant diagnostics cannot
    // race the original stderr buffer.
    g_syslog_buf->shutdown();
  }
  if (g_original_cerr != nullptr) {
    std::cerr.rdbuf(g_original_cerr);
    g_original_cerr = nullptr;
  }
  if (g_restore_cerr_unitbuf) {
    std::cerr.setf(std::ios_base::unitbuf);
    g_restore_cerr_unitbuf = false;
  }
  g_syslog_buf.reset();
  ::closelog();
}

} // namespace punto
