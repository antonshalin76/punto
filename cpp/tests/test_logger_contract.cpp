#include "punto/logger.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct LogRecord {
  int priority = 0;
  std::string message;
};

std::mutex g_sink_mutex;
std::vector<LogRecord> g_records;
std::atomic<bool> g_sink_capture_failed{false};
std::atomic<bool> g_reenter_on_outer{false};
std::atomic<bool> g_block_sink{false};
std::atomic<bool> g_block_sink_entered{false};
int g_block_sink_marker_fd = -1;

void clear_records() {
  const std::lock_guard lock{g_sink_mutex};
  g_records.clear();
  g_sink_capture_failed.store(false, std::memory_order_relaxed);
  g_block_sink.store(false, std::memory_order_relaxed);
  g_block_sink_entered.store(false, std::memory_order_relaxed);
  g_block_sink_marker_fd = -1;
}

[[nodiscard]] std::vector<LogRecord> records_snapshot() {
  const std::lock_guard lock{g_sink_mutex};
  return g_records;
}

void capture_syslog_record(int priority, const char *format,
                           va_list arguments) noexcept {
  if (g_block_sink.load(std::memory_order_relaxed)) {
    g_block_sink_entered.store(true, std::memory_order_release);
    if (g_block_sink_marker_fd >= 0) {
      constexpr char marker = 'S';
      [[maybe_unused]] const ssize_t written =
          ::write(g_block_sink_marker_fd, &marker, 1);
    }
    for (;;) {
      (void)::pause();
    }
  }

  try {
    std::string message;
    if (format != nullptr && std::strcmp(format, "%s") == 0) {
      const char *const value = va_arg(arguments, const char *);
      message = value == nullptr ? "" : value;
    } else if (format != nullptr) {
      message = format;
    }

    {
      const std::lock_guard lock{g_sink_mutex};
      g_records.push_back(LogRecord{priority, message});
    }

    if (message == "reentrant-outer" &&
        g_reenter_on_outer.exchange(false, std::memory_order_relaxed)) {
      std::cerr << "reentrant-inner\n";
    }
  } catch (...) {
    g_sink_capture_failed.store(true, std::memory_order_relaxed);
  }
}

class Runner {
public:
  void expect(bool condition, std::string_view message) {
    if (condition) {
      return;
    }
    ++failures_;
    std::cerr << "FAIL: " << message << '\n';
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

private:
  int failures_ = 0;
};

void reset_logging() {
  punto::shutdown_logging();
  (void)::unsetenv("PUNTO_LOG_STDERR");
  clear_records();
  std::cerr.clear();
}

void test_partial_sync_is_delivered(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-contract", punto::LogLevel::Debug);

  constexpr std::string_view payload = "partial-without-newline";
  std::cerr.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  std::cerr.flush();
  punto::shutdown_logging();
  const auto records = records_snapshot();
  runner.expect(records.size() == 1,
                "shutdown drains one synced partial record without duplicate");
  if (records.size() == 1) {
    runner.expect(records.front().message == payload,
                  "sync preserves the exact partial record");
  }
}

void test_reentrant_sink_does_not_deadlock(Runner &runner) {
  reset_logging();

  const pid_t child = ::fork();
  if (child < 0) {
    runner.expect(false, "fork succeeds for bounded reentrancy watchdog");
    return;
  }

  if (child == 0) {
    clear_records();
    g_reenter_on_outer.store(true, std::memory_order_relaxed);
    punto::init_logging("punto-logger-reentrant", punto::LogLevel::Debug);
    std::cerr << "reentrant-outer\n";
    punto::shutdown_logging();

    const auto records = records_snapshot();
    const bool ok = records.size() == 2 &&
                    records[0].message == "reentrant-outer" &&
                    records[1].message == "reentrant-inner";
    ::_exit(ok ? 0 : 1);
  }

  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      runner.expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                    "reentrant sink completes and preserves both records");
      return;
    }
    if (result < 0 && errno != EINTR) {
      runner.expect(false, "waitpid succeeds for reentrancy watchdog");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  (void)::kill(child, SIGKILL);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  runner.expect(false, "reentrant sink must not deadlock");
}

void test_concurrent_lines_are_atomic(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-concurrent", punto::LogLevel::Debug);

  constexpr int kThreadCount = 12;
  constexpr int kLinesPerThread = 400;
  const std::uint64_t dropped_before = punto::dropped_log_records();
  std::barrier start{kThreadCount};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([thread_id, &start] {
      start.arrive_and_wait();
      for (int sequence = 0; sequence < kLinesPerThread; ++sequence) {
        std::cerr << "worker=" << thread_id << " sequence=" << sequence
                  << " payload=logger-contract\n";
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }
  punto::shutdown_logging();

  const auto records = records_snapshot();
  const std::uint64_t dropped = punto::dropped_log_records() - dropped_before;
  runner.expect(!g_sink_capture_failed.load(std::memory_order_relaxed),
                "capture sink remains healthy during concurrent writes");
  runner.expect(records.size() + dropped ==
                    static_cast<std::size_t>(kThreadCount * kLinesPerThread),
                "bounded queue accounts for every delivered or dropped line");

  std::set<std::string> actual;
  for (const auto &record : records) {
    actual.insert(record.message);
  }

  std::set<std::string> expected;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    for (int sequence = 0; sequence < kLinesPerThread; ++sequence) {
      expected.insert("worker=" + std::to_string(thread_id) + " sequence=" +
                      std::to_string(sequence) + " payload=logger-contract");
    }
  }
  runner.expect(actual.size() == records.size() &&
                    std::all_of(actual.begin(), actual.end(),
                                [&expected](const std::string &record) {
                                  return expected.contains(record);
                                }),
                "delivered concurrent records are complete, unique and atomic");
}

void test_blocked_sink_never_blocks_producers_and_shutdown_is_bounded(
    Runner &runner) {
  reset_logging();
  int markers[2] = {-1, -1};
  if (::pipe2(markers, O_CLOEXEC | O_NONBLOCK) != 0) {
    runner.expect(false, "create blocked-sink marker pipe");
    return;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    ::close(markers[0]);
    ::close(markers[1]);
    runner.expect(false, "fork succeeds for blocked-sink watchdog");
    return;
  }
  if (child == 0) {
    ::close(markers[0]);
    clear_records();
    g_block_sink_marker_fd = markers[1];
    g_block_sink.store(true, std::memory_order_relaxed);
    punto::init_logging("punto-logger-blocked", punto::LogLevel::Debug);
    std::cerr << "block-sink\n";

    const auto entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!g_block_sink_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entry_deadline) {
      std::this_thread::yield();
    }

    const std::uint64_t dropped_before = punto::dropped_log_records();
    const auto producer_start = std::chrono::steady_clock::now();
    for (int sequence = 0; sequence < 4096; ++sequence) {
      std::cerr << "queued-while-sink-blocked=" << sequence << '\n';
    }
    const auto producer_elapsed =
        std::chrono::steady_clock::now() - producer_start;
    const bool producer_ok =
        g_block_sink_entered.load(std::memory_order_acquire) &&
        producer_elapsed < std::chrono::seconds{1} &&
        punto::dropped_log_records() > dropped_before;
    const char marker = producer_ok ? 'P' : 'F';
    [[maybe_unused]] const ssize_t written = ::write(markers[1], &marker, 1);

    // The sink cannot drain. shutdown_logging must terminate the process via
    // its own deadline instead of joining the stuck libc call forever.
    punto::shutdown_logging();
    ::_exit(99);
  }

  ::close(markers[1]);
  bool producer_completed = false;
  const auto marker_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < marker_deadline &&
         !producer_completed) {
    pollfd descriptor{markers[0], POLLIN, 0};
    const int polled = ::poll(&descriptor, 1, 50);
    if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
      char buffer[32];
      const ssize_t count = ::read(markers[0], buffer, sizeof(buffer));
      for (ssize_t index = 0; index < count; ++index) {
        if (buffer[index] == 'P') {
          producer_completed = true;
        }
      }
    }
  }
  ::close(markers[0]);
  runner.expect(producer_completed,
                "blocked sink does not stall bounded producer/drop policy");

  int status = 0;
  const auto exit_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < exit_deadline) {
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      runner.expect(WIFEXITED(status) && WEXITSTATUS(status) == 3,
                    "blocked sink shutdown reaches bounded _Exit(3)");
      return;
    }
    if (result < 0 && errno != EINTR) {
      runner.expect(false, "waitpid succeeds for blocked-sink watchdog");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  (void)::kill(child, SIGKILL);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  runner.expect(false, "blocked sink process must not outlive logger deadline");
}

void test_shutdown_drains_each_thread_partial(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-shutdown", punto::LogLevel::Debug);

  constexpr int kThreadCount = 8;
  std::barrier start{kThreadCount};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([thread_id, &start] {
      start.arrive_and_wait();
      std::cerr << "shutdown-partial-" << thread_id;
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  punto::shutdown_logging();
  const auto records = records_snapshot();
  std::set<std::string> actual;
  for (const auto &record : records) {
    actual.insert(record.message);
  }

  std::set<std::string> expected;
  for (int thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    expected.insert("shutdown-partial-" + std::to_string(thread_id));
  }
  runner.expect(actual == expected,
                "shutdown drains each writer partial without combining them");
}

void test_finished_writer_identity_is_not_reused(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-writer-lifetime", punto::LogLevel::Debug);

  constexpr int kWriterCount = 80;
  for (int writer = 0; writer < kWriterCount; ++writer) {
    std::thread thread{
        [writer] { std::cerr << "lifetime-partial-" << writer; }};
    thread.join();
  }

  punto::shutdown_logging();
  const auto records = records_snapshot();
  std::set<std::string> actual;
  for (const auto &record : records) {
    actual.insert(record.message);
  }

  std::set<std::string> expected;
  for (int writer = 0; writer < kWriterCount; ++writer) {
    expected.insert("lifetime-partial-" + std::to_string(writer));
  }
  runner.expect(records.size() == static_cast<std::size_t>(kWriterCount) &&
                    actual == expected,
                "finished writer state is never joined to a reused thread id");
}

void test_maximum_runtime_writers_keep_lines_atomic(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-runtime-writers", punto::LogLevel::Debug);

  // The public configuration permits 128 analysis workers. Account for the
  // main, IPC and session threads as well, and keep every unfinished line
  // attached to its writer while they are simultaneously active.
  constexpr int kWriterCount = 160;
  std::barrier prefixes_written{kWriterCount};
  std::vector<std::thread> threads;
  threads.reserve(kWriterCount);
  for (int writer = 0; writer < kWriterCount; ++writer) {
    threads.emplace_back([writer, &prefixes_written] {
      const std::string prefix = "wide-writer=" + std::to_string(writer) + "/";
      std::cerr.write(prefix.data(),
                      static_cast<std::streamsize>(prefix.size()));
      prefixes_written.arrive_and_wait();
      constexpr std::string_view suffix = "complete";
      std::cerr.write(suffix.data(),
                      static_cast<std::streamsize>(suffix.size()));
      std::cerr.put('\n');
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  punto::shutdown_logging();

  const auto records = records_snapshot();
  std::set<std::string> actual;
  for (const auto &record : records) {
    actual.insert(record.message);
  }
  std::set<std::string> expected;
  for (int writer = 0; writer < kWriterCount; ++writer) {
    expected.insert("wide-writer=" + std::to_string(writer) + "/complete");
  }
  runner.expect(records.size() == static_cast<std::size_t>(kWriterCount) &&
                    actual == expected,
                "maximum configured worker fan-out preserves atomic lines");
}

void test_unterminated_input_is_chunked_and_bounded(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-bounded", punto::LogLevel::Debug);

  constexpr std::size_t kMaxExpectedRecordBytes = 8192;
  const std::string payload(kMaxExpectedRecordBytes * 32 + 137, 'x');
  std::cerr.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  std::cerr.flush();
  punto::shutdown_logging();

  const auto records = records_snapshot();
  runner.expect(records.size() > 1,
                "long unterminated input is emitted in bounded chunks");
  std::string reconstructed;
  reconstructed.reserve(payload.size());
  for (const auto &record : records) {
    runner.expect(!record.message.empty(), "bounded chunks are non-empty");
    runner.expect(record.message.size() <= kMaxExpectedRecordBytes,
                  "no emitted record exceeds the buffering bound");
    reconstructed += record.message;
  }
  runner.expect(reconstructed == payload,
                "bounded chunking does not lose or alter input bytes");
}

void test_level_inference_filtering_and_echo(Runner &runner) {
  reset_logging();
  punto::init_logging("punto-logger-levels", punto::LogLevel::Debug);
  std::cerr << "plain info\n";
  std::cerr << "warning condition\n";
  std::cerr << "fatal condition\n";
  std::cerr << "telemetry detail\n";
  punto::shutdown_logging();

  auto records = records_snapshot();
  runner.expect(records.size() == 4,
                "debug level accepts every inferred log level");
  if (records.size() == 4) {
    runner.expect(records[0].priority == LOG_INFO,
                  "plain line is inferred as info");
    runner.expect(records[1].priority == LOG_WARNING,
                  "warning line is inferred as warning");
    runner.expect(records[2].priority == LOG_ERR,
                  "fatal line is inferred as error");
    runner.expect(records[3].priority == LOG_DEBUG,
                  "telemetry line is inferred as debug");
  }

  clear_records();
  punto::init_logging("punto-logger-filter", punto::LogLevel::Warning);
  std::cerr << "filtered info\n";
  std::cerr << "warning retained\n";
  punto::update_log_level(punto::LogLevel::Error);
  std::cerr << "warning filtered\n";
  std::cerr << "error retained\n";
  punto::shutdown_logging();
  records = records_snapshot();
  runner.expect(records.size() == 2 &&
                    records[0].message == "warning retained" &&
                    records[1].message == "error retained",
                "runtime level updates preserve filtering semantics");

  clear_records();
  std::stringbuf fallback;
  std::streambuf *const original = std::cerr.rdbuf(&fallback);
  (void)::setenv("PUNTO_LOG_STDERR", "1", 1);
  punto::init_logging("punto-logger-echo", punto::LogLevel::Info);
  std::cerr << "echo-line\n";
  punto::shutdown_logging();
  std::cerr.rdbuf(original);
  (void)::unsetenv("PUNTO_LOG_STDERR");
  runner.expect(fallback.str() == "echo-line\n",
                "PUNTO_LOG_STDERR=1 preserves stderr echo behavior");
}

} // namespace

extern "C" void openlog(const char *, int, int) {}

extern "C" void closelog() {}

extern "C" void syslog(int priority, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  capture_syslog_record(priority, format, arguments);
  va_end(arguments);
}

extern "C" void __syslog_chk(int priority, int, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  capture_syslog_record(priority, format, arguments);
  va_end(arguments);
}

int main() {
  Runner runner;
  test_partial_sync_is_delivered(runner);
  test_reentrant_sink_does_not_deadlock(runner);
  test_concurrent_lines_are_atomic(runner);
  test_shutdown_drains_each_thread_partial(runner);
  test_finished_writer_identity_is_not_reused(runner);
  test_maximum_runtime_writers_keep_lines_atomic(runner);
  test_unterminated_input_is_chunked_and_bounded(runner);
  test_level_inference_filtering_and_echo(runner);
  test_blocked_sink_never_blocks_producers_and_shutdown_is_bounded(runner);
  reset_logging();

  if (runner.failures() != 0) {
    std::cerr << runner.failures() << " logger contract assertion(s) failed\n";
    return 1;
  }
  return 0;
}
