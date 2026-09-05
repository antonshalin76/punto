#include "punto/analysis_worker_pool.hpp"
#include "punto/config.hpp"
#include "punto/control_plane_state.hpp"
#include "punto/history_manager.hpp"
#include "punto/input_buffer.hpp"
#include "punto/ipc_server.hpp"
#include "punto/layout_sync_sound.hpp"
#include "punto/runtime_tuning.hpp"
#include "punto/terminal_detection.hpp"
#include "punto/text_processor.hpp"
#include "punto/typo_corrector.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace publication_fault {
enum class Target { None, TemporaryFile, Directory };
Target target = Target::None;
std::string directory;
dev_t directory_device = 0;
ino_t directory_inode = 0;
unsigned int injected = 0;
} // namespace publication_fault

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  using namespace publication_fault;
  struct stat metadata {};
  bool owned = false;
  if (target != Target::None && ::fstat(fd, &metadata) == 0) {
    if (target == Target::Directory) {
      owned = S_ISDIR(metadata.st_mode) &&
              metadata.st_dev == directory_device &&
              metadata.st_ino == directory_inode;
    } else if (S_ISREG(metadata.st_mode)) {
      char path[PATH_MAX + 1]{};
      const std::string descriptor = "/proc/self/fd/" + std::to_string(fd);
      const ssize_t length = ::readlink(descriptor.c_str(), path, PATH_MAX);
      owned = length > 0 &&
              std::string_view{path, static_cast<std::size_t>(length)}
                  .starts_with(directory + "/.control.state.tmp.");
    }
  }
  if (owned) {
    ++injected;
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

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

  for (std::size_t i = 0; i < kMaxWordLen; ++i) {
    expect(buffer.push_char(KEY_A, false), "fill buffer");
  }

  expect(buffer.current_length() == kMaxWordLen, "buffer length");
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

void test_analysis_pool_terminality_on_stop() {
  Dictionary dictionary;
  AnalysisWorkerPool pool(dictionary);

  WordTask task;
  task.task_id = 7;
  expect(pool.submit(task).accepted, "analysis task accepted");
  expect(!pool.submit(task).accepted, "duplicate analysis task rejected");

  pool.stop();

  WordResult result;
  expect(pool.try_pop_result(result), "accepted task has terminal result");
  expect(result.task_id == 7, "terminal result preserves task id");
  expect(result.terminal_status == WordTerminalStatus::Cancelled,
         "queued task cancelled on stop");
  expect(!pool.try_pop_result(result), "accepted task has one terminal result");
  expect(!pool.submit(std::move(task)).accepted,
         "analysis admission closed after stop");
}

void test_analysis_pool_epoch_has_one_terminal_winner() {
  Dictionary dictionary;
  AnalysisWorkerPool pool(dictionary);

  WordTask task;
  task.task_id = 11;
  expect(pool.submit(task).accepted, "epoch task accepted");

  pool.begin_new_epoch();
  expect(pool.pending_task_count() == 0,
         "retired epoch removes queued work before new submissions");
  pool.stop();

  WordResult result;
  expect(pool.try_pop_result(result), "retired epoch publishes terminal");
  expect(result.task_id == 11, "retired epoch preserves task id");
  expect(result.terminal_status == WordTerminalStatus::Cancelled,
         "retired epoch cancels outstanding task");
  expect(!pool.try_pop_result(result),
         "stop cannot publish a second terminal for retired task");
}

void test_analysis_pool_worker_stop_race_has_one_terminal() {
  Dictionary dictionary;
  for (std::uint64_t task_id = 20; task_id < 36; ++task_id) {
    AnalysisWorkerPool pool(dictionary);
    pool.start(1);

    WordTask task;
    task.task_id = task_id;
    task.analysis_len = 0;
    expect(pool.submit(std::move(task)).accepted, "worker race task accepted");
    pool.stop();

    WordResult result;
    expect(pool.try_pop_result(result), "worker race has terminal result");
    expect(result.task_id == task_id, "worker race preserves task id");
    expect(result.terminal_status == WordTerminalStatus::Completed ||
               result.terminal_status == WordTerminalStatus::Cancelled,
           "worker race terminal status");
    expect(!pool.try_pop_result(result), "worker race has one terminal result");
  }
}

void test_analysis_pool_concurrent_stop_is_a_barrier() {
  Dictionary dictionary;
  std::mutex gate_mu;
  std::condition_variable gate_cv;
  bool task_started = false;
  bool release_task = false;
  bool stop_waiter_entered = false;
  bool task_gate_timed_out = false;
  AnalysisWorkerPool pool(
      dictionary,
      [&] {
        std::unique_lock<std::mutex> lock(gate_mu);
        task_started = true;
        gate_cv.notify_all();
        if (!gate_cv.wait_for(lock, std::chrono::seconds(1),
                              [&] { return release_task; })) {
          task_gate_timed_out = true;
        }
      },
      [&] {
        std::lock_guard<std::mutex> lock(gate_mu);
        stop_waiter_entered = true;
        gate_cv.notify_all();
      });
  pool.start(1);
  WordTask task;
  task.task_id = 40;
  task.analysis_len = 0;
  expect(pool.submit(std::move(task)).accepted,
         "concurrent stop task accepted");
  {
    std::unique_lock<std::mutex> lock(gate_mu);
    expect(gate_cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return task_started; }),
           "worker reaches controlled task gate");
  }

  std::thread first([&] { pool.stop(); });

  WordTask probe;
  probe.task_id = 41;
  bool admission_closed = false;
  for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
    if (!pool.submit(probe).accepted) {
      admission_closed = true;
      break;
    }
    ++probe.task_id;
    std::this_thread::yield();
  }
  expect(admission_closed, "first stop closes admission");

  std::mutex returned_mu;
  std::condition_variable returned_cv;
  bool second_returned = false;
  std::thread second([&] {
    pool.stop();
    {
      std::lock_guard<std::mutex> lock(returned_mu);
      second_returned = true;
    }
    returned_cv.notify_one();
  });
  {
    std::unique_lock<std::mutex> lock(gate_mu);
    expect(gate_cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return stop_waiter_entered; }),
           "second stop reaches actual stopping wait path");
  }
  {
    std::unique_lock<std::mutex> lock(returned_mu);
    expect(!returned_cv.wait_for(lock, std::chrono::milliseconds(20),
                                 [&] { return second_returned; }),
           "second stop waits for shutdown owner");
  }

  {
    std::lock_guard<std::mutex> lock(gate_mu);
    release_task = true;
  }
  gate_cv.notify_all();
  first.join();
  second.join();
  expect(!task_gate_timed_out, "controlled task gate released before deadline");

  WordResult result;
  std::size_t task_terminal_count = 0;
  while (pool.try_pop_result(result)) {
    if (result.task_id == 40) {
      ++task_terminal_count;
      expect(result.terminal_status == WordTerminalStatus::Completed,
             "running task completes before stop barrier returns");
    }
  }
  expect(task_terminal_count == 1,
         "concurrent stop returns after one running-task terminal");
}

void test_analysis_pool_admission_can_close_before_drain() {
  Dictionary dictionary;
  AnalysisWorkerPool pool(dictionary);
  pool.start(1);

  pool.close_admission();

  WordTask task;
  task.task_id = 50;
  task.layout_at_boundary = 0;
  expect(!pool.submit(std::move(task)).accepted,
         "explicit admission barrier rejects work before drain");
  pool.stop();
}

void test_analysis_pool_admission_receipt_and_bound() {
  Dictionary dictionary;
  AnalysisWorkerPool pool(dictionary, {}, {}, 2);

  const auto before = std::chrono::steady_clock::now();
  WordTask first;
  first.task_id = 50;
  const AnalysisAdmission first_admission = pool.submit(first);
  const auto after = std::chrono::steady_clock::now();
  expect(first_admission.accepted, "first bounded task accepted");
  expect(first_admission.accepted_at >= before &&
             first_admission.accepted_at <= after,
         "admission receipt uses queue-commit time");

  WordTask second;
  second.task_id = 51;
  expect(pool.submit(second).accepted, "second bounded task accepted");

  WordTask overflow;
  overflow.task_id = 52;
  expect(!pool.submit(overflow).accepted,
         "bounded pool rejects excess outstanding task");
  expect(pool.pending_task_count() == 2,
         "rejected overflow never enters task queue");

  pool.stop();
  WordResult result;
  std::size_t terminals = 0;
  while (pool.try_pop_result(result)) {
    ++terminals;
  }
  expect(terminals == 2, "each bounded accepted task has one terminal");
}

void test_ipc_server() {
  char dir_template[] = "/tmp/punto-tests-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "mkdtemp failed");

  const std::filesystem::path socket_path =
      std::filesystem::path(dir) / "punto-test.sock";

  auto mailbox = std::make_shared<IpcCommandMailbox>();
  std::atomic<bool> enabled{true};
  std::mutex reload_mutex;
  std::string reloaded_path;
  std::jthread owner([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested() || mailbox->size() != 0) {
      if (auto pending = mailbox->try_dequeue()) {
        IpcResult response;
        switch (pending->request.verb) {
        case IpcVerb::GetStatus:
          response = {true, enabled.load() ? "ENABLED" : "DISABLED"};
          break;
        case IpcVerb::SetStatus:
          enabled.store(pending->request.argument == "1");
          response = {true, enabled.load() ? "ENABLED" : "DISABLED"};
          break;
        case IpcVerb::Reload: {
          std::lock_guard<std::mutex> lock(reload_mutex);
          reloaded_path = pending->request.argument;
        }
          response = {true, "reloaded"};
          break;
        case IpcVerb::Stats:
          response = {true, "analyzed=3 corrections=1"};
          break;
        case IpcVerb::Shutdown:
          response = {false, "Shutdown not allowed via IPC"};
          break;
        }
        pending->complete(std::move(response));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
  });

  IpcServerOptions options;
  options.primary_socket_path = socket_path.string();
  const RuntimeFileSecurity runtime_security = default_runtime_file_security();
  options.socket_identity = {runtime_security.owner_uid,
                             runtime_security.group_gid, runtime_security.mode};
  IpcServer server(mailbox, std::move(options));

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
  {
    std::lock_guard<std::mutex> lock(reload_mutex);
    expect(reloaded_path == "/tmp/config.yaml", "reload callback arg");
  }
  expect(send_ipc_command(socket_path.string(), "NOPE\n") ==
             "ERROR Unknown command\n",
         "unknown command response");

  expect(mailbox->close(), "mailbox close reached its admission barrier");
  server.stop();
  owner.request_stop();
  owner.join();
  expect(!std::filesystem::exists(socket_path), "socket removed on stop");
  const std::filesystem::path lease_path =
      socket_path.parent_path() /
      ("." + socket_path.filename().string() + ".lock");
  expect(::unlink(lease_path.c_str()) == 0, "persistent socket lease removed");
  expect(::rmdir(dir) == 0, "tmp dir removed");
}

void test_typo_corrector() {
  const std::vector<KeyEntry> api{{KEY_A, true}, {KEY_P, true}, {KEY_I, true}};
  expect(detect_case_pattern(api) == CasePattern::Mixed,
         "known abbreviation is not corrected");

  const std::vector<KeyEntry> ghbdtn{{KEY_G, true},  {KEY_H, true},
                                     {KEY_B, false}, {KEY_D, false},
                                     {KEY_T, false}, {KEY_Y, false}};
  const StickyShiftResult sticky =
      detect_sticky_shift_with_layout(ghbdtn, /*current_layout=*/0);
  expect(sticky.detected, "sticky shift with layout detected");
  expect(sticky.needs_layout_fix, "sticky shift requires layout fix");
  expect(sticky.corrected.size() == ghbdtn.size(),
         "sticky shift corrected size");
  expect(sticky.corrected.front().shifted,
         "first corrected letter stays upper");
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
    std::fputs("auto_switch:\n  enabled: true\n  threshold: 2.0\n  "
               "min_word_len: 3\n  min_score: 5.0\n  max_rollback_words: 5\n",
               fp);
    std::fputs("sound:\n  enabled: true\n", fp);
    std::fputs("logging:\n  level: debug\n", fp);
    std::fputs("runtime:\n  analysis_threads: 3\n  "
               "max_analysis_threads_per_daemon: 2\n",
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
  expect(external_layout_sound_expired(state,
                                       now + std::chrono::milliseconds{700}),
         "external sound state expires after window");

  clear_external_layout_sound(state);
  expect(!state.pending, "external sound state cleared");
}

void test_terminal_detection_boundaries() {
  expect(is_terminal_wm_class("st", "St"), "st exact terminal match");
  expect(is_terminal_wm_class("org.suckless.st", ""),
         "st identifier component match");
  expect(is_terminal_wm_class("gnome-terminal-server", "Gnome-terminal"),
         "gnome terminal match");
  expect(is_terminal_wm_class("org.example.Terminal", ""),
         "generic terminal component match");
  expect(!is_terminal_wm_class("postman", "Postman"),
         "Postman is not classified as st");
  expect(!is_terminal_wm_class("steam", "Steam"),
         "Steam is not classified as st");
  expect(!is_terminal_wm_class("studio", "JetBrains Studio"),
         "Studio is not classified as st");
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

void test_control_plane_generation_seeding() {
  char dir_template[] = "/tmp/punto-seed-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "seed mkdtemp failed");

  const std::filesystem::path state_path =
      std::filesystem::path(dir) / "control.state";

  // Файл отсутствует: состояние не меняется.
  SharedControlPlaneState fresh;
  fresh.enabled = false;
  fresh.config_path = "/etc/punto/config.yaml";
  SharedControlPlaneState seeded =
      seed_control_plane_generations(fresh, state_path.string());
  expect(seeded.config_generation == 0, "seed keeps zero config gen");
  expect(seeded.status_generation == 0, "seed keeps zero status gen");

  // Файл от предыдущего запуска: поколения продолжаются, а локальные
  // enabled/config_path не затираются стейл-значениями с диска.
  SharedControlPlaneState previous;
  previous.config_generation = 42;
  previous.status_generation = 17;
  previous.enabled = true;
  previous.config_path = "/stale/path.yaml";
  expect(write_shared_control_plane_state(previous, state_path.string()),
         "seed state write");

  seeded = seed_control_plane_generations(fresh, state_path.string());
  expect(seeded.config_generation == 42, "seed continues config generation");
  expect(seeded.status_generation == 17, "seed continues status generation");
  expect(!seeded.enabled, "seed keeps local enabled flag");
  expect(seeded.config_path == "/etc/punto/config.yaml",
         "seed keeps local config path");

  expect(std::filesystem::remove(state_path), "seed state removed");
  expect(::rmdir(dir) == 0, "seed dir removed");
}

void test_control_plane_publication_outcomes() {
  char dir_template[] = "/tmp/punto-publication-XXXXXX";
  char *dir = ::mkdtemp(dir_template);
  expect(dir != nullptr, "publication mkdtemp failed");
  const std::string path = std::string{dir} + "/control.state";
  struct stat metadata {};
  expect(::stat(dir, &metadata) == 0, "publication directory identity");
  publication_fault::directory = dir;
  publication_fault::directory_device = metadata.st_dev;
  publication_fault::directory_inode = metadata.st_ino;
  const SharedControlPlaneState before{7, 9, false, "/etc/punto/config.yaml"};
  const SharedControlPlaneState after{8, 10, true, "/etc/punto/new.yaml"};
  const auto expect_visible = [&](const SharedControlPlaneState &expected) {
    SharedControlPlaneState visible;
    expect(read_shared_control_plane_state(visible, path),
           "published state remains readable");
    expect(visible.config_generation == expected.config_generation &&
               visible.status_generation == expected.status_generation &&
               visible.enabled == expected.enabled &&
               visible.config_path == expected.config_path,
           "publication readback matches exact authoritative state");
  };
  expect(publish_shared_control_plane_state(before, path) ==
             ControlPlanePublicationResult::Durable,
         "healthy publication is durable");
  expect_visible(before);
  publication_fault::target = publication_fault::Target::TemporaryFile;
  publication_fault::injected = 0;
  const auto unpublished = publish_shared_control_plane_state(after, path);
  publication_fault::target = publication_fault::Target::None;
  expect(publication_fault::injected == 1, "owned temporary fsync fault fired");
  expect(unpublished == ControlPlanePublicationResult::NotPublished,
         "temporary fsync failure does not publish");
  expect_visible(before);
  publication_fault::target = publication_fault::Target::Directory;
  publication_fault::injected = 0;
  const auto published = publish_shared_control_plane_state(after, path);
  publication_fault::target = publication_fault::Target::None;
  expect(publication_fault::injected == 1, "owned directory fsync fault fired");
  expect_visible(after);
  expect(published == ControlPlanePublicationResult::PublishedNotDurable,
         "directory fsync failure reports visible nondurable publication");
  expect(publish_shared_control_plane_state(after, path) ==
             ControlPlanePublicationResult::Durable,
         "publication retry confirms durability");
  expect_visible(after);
  expect(std::filesystem::remove(path), "publication state removed");
  expect(::rmdir(dir) == 0, "publication leaves no temporary files");
  publication_fault::directory.clear();
}

void test_control_plane_promotion_planning() {
  SharedControlPlaneState committed;
  committed.config_generation = 6;
  committed.status_generation = 4;
  committed.config_path = "/home/b/.config/punto/config.yaml";

  auto action = plan_control_plane_promotion(
      committed, 5, "/home/a/.config/punto/config.yaml",
      /*authoritative_path_allowed=*/true,
      /*current_path_allowed=*/true);
  expect(action == ControlPlanePromotionAction::ReloadAuthoritativePath,
         "stale secondary reloads the committed primary path");

  action = plan_control_plane_promotion(committed, 6,
                                        "/home/b/.config/punto/config.yaml",
                                        /*authoritative_path_allowed=*/true,
                                        /*current_path_allowed=*/true);
  expect(action == ControlPlanePromotionAction::Ready,
         "reconciled secondary may promote");

  committed.config_path = "/home/a/.config/punto/config.yaml";
  action = plan_control_plane_promotion(committed, 6,
                                        "/home/a/.config/punto/config.yaml",
                                        /*authoritative_path_allowed=*/false,
                                        /*current_path_allowed=*/false);
  expect(action == ControlPlanePromotionAction::ReloadCurrentAuthority,
         "new user replaces an unauthorized prior-user snapshot");

  action = plan_control_plane_promotion(committed, 6,
                                        "/home/b/.config/punto/config.yaml",
                                        /*authoritative_path_allowed=*/false,
                                        /*current_path_allowed=*/true);
  expect(action == ControlPlanePromotionAction::Ready,
         "new-user config reconciled at the inherited generation");

  // Three roles: primary A commits generation 6, stale B must apply it before
  // publishing generation 7, and already-synced peer C must observe 7 != 6.
  committed.config_path = "/etc/punto/alternate.yaml";
  action = plan_control_plane_promotion(committed, 5, "/etc/punto/config.yaml",
                                        /*authoritative_path_allowed=*/true,
                                        /*current_path_allowed=*/true);
  expect(action == ControlPlanePromotionAction::ReloadAuthoritativePath,
         "three-role failover fences stale role B");
  action =
      plan_control_plane_promotion(committed, 6, "/etc/punto/alternate.yaml",
                                   /*authoritative_path_allowed=*/true,
                                   /*current_path_allowed=*/true);
  expect(action == ControlPlanePromotionAction::Ready,
         "three-role failover admits reconciled role B");
  const std::uint64_t promoted_generation = committed.config_generation + 1;
  expect(promoted_generation == 7,
         "promoted primary publishes a strictly newer generation");
  expect(promoted_generation != committed.config_generation,
         "synced role C observes the promoted publication");

  action = plan_control_plane_promotion(committed, 5, "/etc/punto/config.yaml",
                                        /*authoritative_path_allowed=*/true,
                                        /*current_path_allowed=*/true,
                                        /*authority_fallback_applied=*/true);
  expect(action == ControlPlanePromotionAction::ReloadAuthoritativePath,
         "failed snapshot load cannot authorize stale promotion");

  action = plan_control_plane_promotion(committed, 6, "/etc/punto/config.yaml",
                                        /*authoritative_path_allowed=*/true,
                                        /*current_path_allowed=*/true,
                                        /*authority_fallback_applied=*/true);
  expect(action == ControlPlanePromotionAction::Ready,
         "successfully committed authority fallback may promote");
}

} // namespace

int main() {
  test_text_processor();
  test_input_buffer_overflow();
  test_analysis_pool_terminality_on_stop();
  test_analysis_pool_epoch_has_one_terminal_winner();
  test_analysis_pool_worker_stop_race_has_one_terminal();
  test_analysis_pool_concurrent_stop_is_a_barrier();
  test_analysis_pool_admission_can_close_before_drain();
  test_analysis_pool_admission_receipt_and_bound();
  test_ipc_server();
  test_typo_corrector();
  test_history_manager();
  test_config_logging_level();
  test_external_layout_sound_state();
  test_terminal_detection_boundaries();
  test_runtime_thread_budget();
  test_control_plane_state_round_trip();
  test_control_plane_generation_seeding();
  test_control_plane_publication_outcomes();
  test_control_plane_promotion_planning();

  std::cout << "punto-tests: OK\n";
  return 0;
}
#include <latch>
