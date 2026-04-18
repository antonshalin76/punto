#include "punto/input_buffer.hpp"
#include "punto/ipc_server.hpp"
#include "punto/history_manager.hpp"
#include "punto/layout_sync_sound.hpp"
#include "punto/control_plane_state.hpp"
#include "punto/runtime_tuning.hpp"
#include "punto/text_processor.hpp"
#include "punto/typo_corrector.hpp"
#include "punto/config.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace punto;

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "punto-tests failure: " << message << "\n";
    std::abort();
  }
}

std::string send_ipc_command(const std::string &socket_path,
                             const std::string &command) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  expect(fd >= 0, "socket creation failed");

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  expect(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0,
         "connect failed");

  const char *data = command.c_str();
  std::size_t remaining = command.size();
  while (remaining > 0) {
    const ssize_t written = ::write(fd, data, remaining);
    expect(written > 0, "write failed");
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }

  ::shutdown(fd, SHUT_WR);

  std::string response;
  char buffer[256];
  while (true) {
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n == 0) {
      break;
    }
    expect(n > 0, "read failed");
    response.append(buffer, static_cast<std::size_t>(n));
  }

  ::close(fd);
  return response;
}

std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  expect(input.good(), "source file open failed");
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

void test_text_processor() {
  expect(utf8_codepoint_count("") == 0, "utf8 empty");
  expect(utf8_codepoint_count("a") == 1, "utf8 ascii");
  expect(utf8_codepoint_count("привет") == 6, "utf8 cyrillic");
  expect(utf8_codepoint_count("aпривет") == 7, "utf8 mixed");

  expect(invert_layout("ghbdtn") == "привет", "invert en->ru");
  expect(invert_layout("привет") == "ghbdtn", "invert ru->en");
  expect(invert_case("AbC") == "aBc", "invert case");
  expect(transliterate("привет") == "privet", "transliterate");
}

void test_input_buffer_overflow() {
  InputBuffer buffer;

  for (std::size_t i = 0; i < kMaxWordLen - 1; ++i) {
    expect(buffer.push_char(KEY_A, false), "fill buffer");
  }

  expect(buffer.current_length() == kMaxWordLen - 1, "buffer length");
  expect(!buffer.current_overflowed(), "buffer not overflowed initially");
  expect(!buffer.push_char(KEY_B, false), "buffer overflow trigger");
  expect(buffer.current_overflowed(), "overflow flag set");
  expect(buffer.current_length() == 0, "overflow clears current word");

  buffer.commit_word();
  expect(!buffer.current_overflowed(), "overflow flag reset on commit");
  expect(buffer.current_length() == 0, "current length reset after commit");
  expect(buffer.last_length() == 0, "overflowed word not committed");

  expect(buffer.push_char(KEY_C, true), "buffer reusable after overflow");
  expect(buffer.current_length() == 1, "buffer reusable length");
}

void test_ipc_server() {
  char dir_template[] = "/tmp/punto-tests-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "mkdtemp failed");

  const std::filesystem::path socket_path =
      std::filesystem::path(dir) / "punto-test.sock";

  std::atomic<bool> enabled{true};
  std::string reloaded_path;
  IpcServer server(
      enabled,
      [&reloaded_path](const std::string &path) {
        reloaded_path = path;
        return IpcResult{true, "reloaded"};
      },
      []() { return IpcResult{true, "analyzed=3 corrections=1"}; },
      socket_path.string());

  expect(server.start(), "ipc server start");

  struct stat st {};
  expect(::stat(socket_path.c_str(), &st) == 0, "socket stat");
  expect((st.st_mode & 0777) == 0660, "socket mode");

  expect(send_ipc_command(socket_path.string(), "GET_STATUS\n") ==
             "OK ENABLED\n",
         "GET_STATUS response");
  expect(send_ipc_command(socket_path.string(), "SET_STATUS 0\n") ==
             "OK DISABLED\n",
         "SET_STATUS response");
  expect(!enabled.load(), "enabled flag changed");
  expect(send_ipc_command(socket_path.string(), "STATS\n") ==
             "OK analyzed=3 corrections=1\n",
         "STATS response");
  expect(send_ipc_command(socket_path.string(), "RELOAD /tmp/config.yaml\n") ==
             "OK reloaded\n",
         "RELOAD response");
  expect(reloaded_path == "/tmp/config.yaml", "reload callback arg");
  expect(send_ipc_command(socket_path.string(), "NOPE\n") ==
             "ERROR Unknown command\n",
         "unknown command response");

  server.stop();
  expect(!std::filesystem::exists(socket_path), "socket removed on stop");
  expect(::rmdir(dir) == 0, "tmp dir removed");
}

void test_typo_corrector() {
  const std::vector<KeyEntry> api{
      {KEY_A, true}, {KEY_P, true}, {KEY_I, true}};
  expect(detect_case_pattern(api) == CasePattern::Mixed,
         "known abbreviation is not corrected");

  const std::vector<KeyEntry> ghbdtn{
      {KEY_G, true}, {KEY_H, true}, {KEY_B, false},
      {KEY_D, false}, {KEY_T, false}, {KEY_Y, false}};
  const StickyShiftResult sticky =
      detect_sticky_shift_with_layout(ghbdtn, /*current_layout=*/0);
  expect(sticky.detected, "sticky shift with layout detected");
  expect(sticky.needs_layout_fix, "sticky shift requires layout fix");
  expect(sticky.corrected.size() == ghbdtn.size(),
         "sticky shift corrected size");
  expect(sticky.corrected.front().shifted, "first corrected letter stays upper");
  for (std::size_t i = 1; i < sticky.corrected.size(); ++i) {
    expect(!sticky.corrected[i].shifted, "remaining corrected letters lower");
  }
}

void test_history_manager() {
  HistoryManager history{3};

  history.push_token(KeyEntry{KEY_H, false});
  history.push_token(KeyEntry{KEY_I, false});
  history.push_token(KeyEntry{KEY_SPACE, false});
  const auto first = history.commit_word(1, 2, KEY_SPACE);
  expect(first.has_value(), "first history word committed");

  history.push_token(KeyEntry{KEY_T, false});
  history.push_token(KeyEntry{KEY_H, false});
  history.push_token(KeyEntry{KEY_E, false});
  history.push_token(KeyEntry{KEY_TAB, false});
  const auto second = history.commit_word(2, 3, KEY_TAB);
  expect(second.has_value(), "second history word committed");

  std::vector<KeyEntry> range;
  expect(history.get_range(first->start_pos, first->delim_pos + 1, range),
         "history range available");
  expect(range.size() == 3, "history range length");
  expect(history.pop_token(), "pop tab delimiter");
  expect(history.cursor_pos() == second->delim_pos, "cursor rewound");
}

void test_config_logging_level() {
  char dir_template[] = "/tmp/punto-config-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "config mkdtemp failed");

  const std::filesystem::path config_path =
      std::filesystem::path(dir) / "config.yaml";
  {
    FILE *fp = std::fopen(config_path.c_str(), "w");
    expect(fp != nullptr, "config fopen failed");
    std::fputs("hotkey:\n  modifier: leftctrl\n  key: grave\n", fp);
    std::fputs("auto_switch:\n  enabled: true\n  threshold: 2.0\n  min_word_len: 3\n  min_score: 5.0\n  max_rollback_words: 5\n", fp);
    std::fputs("sound:\n  enabled: true\n", fp);
    std::fputs("logging:\n  level: debug\n", fp);
    std::fputs(
        "runtime:\n  analysis_threads: 3\n  max_analysis_threads_per_daemon: 2\n",
        fp);
    std::fclose(fp);
  }

  const ConfigLoadOutcome loaded = load_config_checked(config_path);
  expect(loaded.result == ConfigResult::Ok, "config load ok");
  expect(loaded.config.logging.level == LogLevel::Debug,
         "config parsed logging level");
  expect(loaded.config.runtime.analysis_threads == 3,
         "config parsed runtime analysis_threads");
  expect(loaded.config.runtime.max_analysis_threads_per_daemon == 2,
         "config parsed runtime max threads per daemon");
  expect(std::filesystem::remove(config_path), "config removed");
  expect(::rmdir(dir) == 0, "config tmp dir removed");
}

void test_external_layout_sound_state() {
  ExternalLayoutSoundState state;
  const auto now = std::chrono::steady_clock::now();

  arm_external_layout_sound(state, now, std::chrono::milliseconds{500});
  expect(state.pending, "external layout sound armed");
  expect(should_play_external_layout_sound(state, now, 0, 1),
         "external layout change within window plays sound");
  expect(!should_play_external_layout_sound(state, now, 1, 1),
         "same layout does not play sound");
  expect(!should_play_external_layout_sound(state, now, 0, -1),
         "invalid layout does not play sound");
  expect(!external_layout_sound_expired(state, now),
         "fresh external sound state not expired");
  expect(external_layout_sound_expired(state, now + std::chrono::milliseconds{700}),
         "external sound state expires after window");

  clear_external_layout_sound(state);
  expect(!state.pending, "external sound state cleared");
}

void test_x11_threading_regression_guards() {
  const std::filesystem::path source_root = PUNTO_SOURCE_DIR;
  const std::string x11_session =
      read_text_file(source_root / "src" / "x11_session.cpp");
  expect(x11_session.find("seteuid(") == std::string::npos,
         "x11 session must not switch euid in multithreaded daemon");
  expect(x11_session.find("setegid(") == std::string::npos,
         "x11 session must not switch egid in multithreaded daemon");

  const std::string main_source =
      read_text_file(source_root / "src" / "main.cpp");
  expect(main_source.find("XInitThreads()") != std::string::npos,
         "main must initialize Xlib threading support");
}

void test_runtime_thread_budget() {
  AnalysisThreadBudget budget =
      compute_analysis_thread_budget(/*hardware_threads=*/32,
                                     /*daemon_count=*/4,
                                     /*analysis_threads_override=*/0,
                                     /*max_threads_per_daemon=*/4);
  expect(budget.worker_threads == 4, "auto budget capped per daemon");
  expect(budget.daemon_count == 4, "auto budget tracks daemon count");
  expect(!budget.manual_override, "auto budget mode");

  budget = compute_analysis_thread_budget(/*hardware_threads=*/8,
                                          /*daemon_count=*/4,
                                          /*analysis_threads_override=*/0,
                                          /*max_threads_per_daemon=*/4);
  expect(budget.worker_threads == 1, "auto budget shrinks on many daemons");

  budget = compute_analysis_thread_budget(/*hardware_threads=*/32,
                                          /*daemon_count=*/4,
                                          /*analysis_threads_override=*/6,
                                          /*max_threads_per_daemon=*/4);
  expect(budget.worker_threads == 6, "manual override wins");
  expect(budget.manual_override, "manual override mode");
}

void test_control_plane_state_round_trip() {
  char dir_template[] = "/tmp/punto-control-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "control plane mkdtemp failed");

  const std::filesystem::path state_path =
      std::filesystem::path(dir) / "control.state";

  SharedControlPlaneState input;
  input.config_generation = 7;
  input.status_generation = 9;
  input.enabled = false;
  input.config_path = "/tmp/punto/config.yaml";

  expect(write_shared_control_plane_state(input, state_path.string()),
         "control plane state write");

  SharedControlPlaneState output;
  expect(read_shared_control_plane_state(output, state_path.string()),
         "control plane state read");
  expect(output.config_generation == input.config_generation,
         "control plane config generation");
  expect(output.status_generation == input.status_generation,
         "control plane status generation");
  expect(output.enabled == input.enabled, "control plane enabled flag");
  expect(output.config_path == input.config_path, "control plane config path");

  expect(std::filesystem::remove(state_path), "control plane state removed");
  expect(::rmdir(dir) == 0, "control plane dir removed");
}

} // namespace

int main() {
  test_text_processor();
  test_input_buffer_overflow();
  test_ipc_server();
  test_typo_corrector();
  test_history_manager();
  test_config_logging_level();
  test_external_layout_sound_state();
  test_x11_threading_regression_guards();
  test_runtime_thread_budget();
  test_control_plane_state_round_trip();

  std::cout << "punto-tests: OK\n";
  return 0;
}
