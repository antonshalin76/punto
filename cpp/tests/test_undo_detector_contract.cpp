#include "punto/undo_detector.hpp"
#include "punto/control_plane_state.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void expect(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

class TempDir {
public:
  TempDir() {
    char pattern[] = "/tmp/punto-undo-contract-XXXXXX";
    char *created = ::mkdtemp(pattern);
    expect(created != nullptr, "mkdtemp");
    path_ = created;
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

void wait_ready(punto::UndoDetector &detector) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!detector.ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(detector.ready(), "initial storage read reaches terminal readiness");
}

void wait_saved(punto::UndoDetector &detector) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (detector.pending() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(!detector.pending() && !detector.persistence_failed(),
         "accepted storage mutation is durable");
}

void test_read_bounded_preserves_default_and_small_limits() {
  TempDir dir;
  const auto path = dir.path() / "bounded.txt";
  const auto read = [&](const std::string &payload,
                        std::optional<std::size_t> limit) {
    {
      std::ofstream output(path, std::ios::binary);
      output << payload;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    expect(fd >= 0, "open bounded read fixture");
    const auto result = limit ? punto::detail::read_bounded(fd, *limit)
                              : punto::detail::read_bounded(fd);
    expect(::close(fd) == 0, "close bounded read fixture");
    return result;
  };
  expect(read("a", 1) == std::optional<std::string>{"a"}, "exact one-byte budget accepted");
  expect(!read("ab", 1), "small budget cannot underflow when chunk is larger");
  expect(!read("a", 0) && read("", 0).has_value(), "zero budget admits only empty input");
  expect(read(std::string(8192, 'a'), std::nullopt).has_value(),
         "default control-plane read limit remains 8192 bytes");
  expect(!read(std::string(8193, 'a'), std::nullopt),
         "default control-plane read still rejects over 8192 bytes");
}

void test_initial_io_does_not_block_constructor() {
  TempDir dir;
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool permitted = false;
  std::atomic<bool> constructed{false};
  std::unique_ptr<punto::UndoDetector> detector;
  std::thread caller{[&] {
    detector = std::make_unique<punto::UndoDetector>(
        (dir.path() / "undo.txt").string(), [&] {
          std::unique_lock lock{mutex};
          entered = true;
          condition.notify_all();
          condition.wait(lock, [&] { return permitted; });
        });
    constructed.store(true);
    condition.notify_all();
  }};
  bool started = false;
  bool returned_while_blocked = false;
  bool lookup_was_nonblocking = false;
  {
    std::unique_lock lock{mutex};
    started = condition.wait_for(lock, std::chrono::seconds{1},
                                 [&] { return entered; });
    returned_while_blocked =
        condition.wait_for(lock, std::chrono::milliseconds{200},
                           [&] { return constructed.load(); });
    if (returned_while_blocked) {
      const auto begin = std::chrono::steady_clock::now();
      lookup_was_nonblocking = !detector->ready() &&
                               !detector->is_excluded("absent") &&
                               std::chrono::steady_clock::now() - begin <
                                   std::chrono::milliseconds{100};
    }
    permitted = true;
  }
  condition.notify_all();
  caller.join();
  expect(started, "initial read reaches controlled I/O seam");
  expect(returned_while_blocked,
         "constructor returns while initial I/O is stalled");
  expect(lookup_was_nonblocking,
         "unready cached lookup stays nonblocking during initial I/O");
}

void test_shutdown_is_bounded_with_stalled_io() {
  TempDir dir;
  struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool permitted = false;
  };
  auto gate = std::make_shared<Gate>();
  auto detector = std::make_unique<punto::UndoDetector>(
      (dir.path() / "undo.txt").string(), [gate] {
        std::unique_lock lock{gate->mutex};
        gate->entered = true;
        gate->condition.notify_all();
        gate->condition.wait(lock, [&] { return gate->permitted; });
      });
  {
    std::unique_lock lock{gate->mutex};
    expect(gate->condition.wait_for(lock, std::chrono::seconds{1},
                                    [&] { return gate->entered; }),
           "shutdown fixture reaches initial I/O");
  }
  const auto begin = std::chrono::steady_clock::now();
  detector.reset();
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  {
    std::lock_guard lock{gate->mutex};
    gate->permitted = true;
  }
  gate->condition.notify_all();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (gate.use_count() != 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(gate.use_count() == 1,
         "detached worker releases owned state after I/O recovery");
  expect(elapsed < std::chrono::seconds{3},
         "shutdown does not wait indefinitely for storage");
}

void test_exclusion_lookup_does_not_read_file() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  punto::UndoDetector detector{path.string()};
  wait_ready(detector);
  detector.add_exclusion("cached");
  wait_saved(detector);
  {
    std::ofstream output(path);
    output << "# Punto Switcher Undo Exclusions\n# legacy file\nexternal\n";
  }
  expect(detector.is_excluded("cached") && !detector.is_excluded("external"),
         "cached lookup performs no implicit disk refresh");
  detector.load_from_file();
  wait_saved(detector);
  expect(detector.is_excluded("external") && !detector.is_excluded("cached"),
         "explicit refresh reads legacy comment headers");
}

void test_learning_is_cached_while_write_is_blocked() {
  TempDir dir;
  std::mutex mutex;
  std::condition_variable condition;
  bool writing = false;
  bool permitted = false;
  unsigned int calls = 0;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string(), [&] {
                                 if (++calls == 1)
                                   return;
                                 std::unique_lock lock{mutex};
                                 writing = true;
                                 condition.notify_all();
                                 condition.wait(lock,
                                                [&] { return permitted; });
                               }};
  wait_ready(detector);
  detector.on_correction_applied(1, "learned");
  detector.on_undo();
  bool started;
  {
    std::unique_lock lock{mutex};
    started = condition.wait_for(lock, std::chrono::seconds{1},
                                 [&] { return writing; });
  }
  const auto begin = std::chrono::steady_clock::now();
  const bool immediately_excluded = detector.is_excluded("learned");
  const bool pending = detector.pending();
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  {
    std::lock_guard lock{mutex};
    permitted = true;
  }
  condition.notify_all();
  expect(started, "learning reaches real controlled write boundary");
  expect(immediately_excluded && pending,
         "learning is cached before durable completion");
  expect(elapsed < std::chrono::milliseconds{100},
         "input-side lookup never waits for disk");
  wait_saved(detector);
}

void test_failed_write_retains_learning_and_recovers() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  punto::UndoDetector detector{path.string()};
  wait_ready(detector);
  expect(std::filesystem::create_directory(path),
         "install invalid write target");
  detector.add_exclusion("retained");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!detector.persistence_failed() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(detector.persistence_failed() && detector.pending(),
         "failed write remains explicitly pending");
  expect(detector.is_excluded("retained"),
         "failed persistence does not erase session learning");
  expect(std::filesystem::remove(path), "remove exact empty invalid target");
  detector.load_from_file();
  wait_saved(detector);
  punto::UndoDetector restarted{path.string()};
  wait_ready(restarted);
  expect(restarted.is_excluded("retained"),
         "retried learning survives restart");
}

void test_clear_and_new_add_survive_older_write_completion() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  std::mutex mutex;
  std::condition_variable condition;
  bool writing = false;
  bool permitted = false;
  unsigned int calls = 0;
  punto::UndoDetector detector{path.string(), [&] {
                                 if (++calls != 2)
                                   return;
                                 std::unique_lock lock{mutex};
                                 writing = true;
                                 condition.notify_all();
                                 condition.wait(lock,
                                                [&] { return permitted; });
                               }};
  wait_ready(detector);
  detector.add_exclusion("obsolete");
  {
    std::unique_lock lock{mutex};
    expect(condition.wait_for(lock, std::chrono::seconds{1},
                              [&] { return writing; }),
           "older add reaches controlled storage boundary");
  }
  detector.clear_exclusions();
  detector.add_exclusion("current");
  const bool immediate =
      detector.is_excluded("current") && !detector.is_excluded("obsolete");
  {
    std::lock_guard lock{mutex};
    permitted = true;
  }
  condition.notify_all();
  expect(immediate, "pending Clear/Add immediately updates cached decision");
  wait_saved(detector);
  expect(detector.is_excluded("current") && !detector.is_excluded("obsolete"),
         "older write completion cannot overwrite newer accepted deltas");
  punto::UndoDetector restarted{path.string()};
  wait_ready(restarted);
  expect(restarted.is_excluded("current") && !restarted.is_excluded("obsolete"),
         "serialized Clear/Add persists exactly after older completion");
}

void test_atomic_private_round_trip() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    punto::UndoDetector detector{path.string()};
    wait_ready(detector);
    detector.add_exclusion("word");
    detector.add_exclusion("alpha");
    expect(detector.exclusion_count() == 2, "two valid entries persist");
    wait_saved(detector);
  }
  struct stat metadata {};
  expect(::lstat(path.c_str(), &metadata) == 0, "persistent file exists");
  expect(S_ISREG(metadata.st_mode) && metadata.st_nlink == 1,
         "persistent file is a single regular inode");
  expect((metadata.st_mode & 0777) == 0600, "persistent file is private");
  expect(metadata.st_uid == ::geteuid(), "persistent file owner is exact");

  punto::UndoDetector loaded{path.string()};
  wait_ready(loaded);
  expect(loaded.is_excluded("alpha") && loaded.is_excluded("word"),
         "valid file round-trips");
  expect(read_file(path).find("alpha\nword\n") != std::string::npos,
         "serialization is deterministic");
}

void test_invalid_words_and_capacity_are_bounded() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    punto::UndoDetector detector{path.string()};
    wait_ready(detector);
    detector.add_exclusion("UPPER");
    detector.add_exclusion("#hidden");
    detector.add_exclusion("white space");
    detector.add_exclusion(std::string{"nul\0byte", 8});
    detector.add_exclusion("tab\tbyte");
    detector.add_exclusion("line\nbreak");
    detector.add_exclusion(
        std::string(punto::UndoDetector::maximum_word_bytes() + 1U, 'a'));
    expect(detector.exclusion_count() == 0, "invalid entries are rejected");
  }
  std::vector<std::string> words;
  for (std::size_t index = 0;
       index < punto::UndoDetector::maximum_entries() + 10U; ++index) {
    std::string word;
    std::size_t value = index;
    do {
      word.push_back(static_cast<char>('a' + (value % 26U)));
      value /= 26U;
    } while (value != 0U);
    words.push_back("w" + word);
  }
  const auto seeded = punto::UndoDetector::maximum_entries() - 10U;
  {
    std::ofstream output(path);
    for (std::size_t index = 0; index < seeded; ++index)
      output << words[index] << '\n';
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure near-capacity fixture");
  punto::UndoDetector detector{path.string()};
  wait_ready(detector);
  expect(detector.exclusion_count() == seeded, "near-capacity fixture loads");
  for (std::size_t index = seeded; index < words.size(); ++index)
    detector.add_exclusion(words[index]);
  expect(detector.exclusion_count() == punto::UndoDetector::maximum_entries(),
         "entry count is capped");
  wait_saved(detector);
  for (std::size_t index = 0; index < punto::UndoDetector::maximum_entries(); ++index)
    expect(detector.is_excluded(words[index]), "every admitted entry remains cached at capacity");
}

std::string maximum_word(std::size_t index) {
  std::string word(63, 'a');
  for (std::size_t offset = 0; offset < 3; ++offset) {
    word[word.size() - 1 - offset] = static_cast<char>('a' + index % 26);
    index /= 26;
  }
  return word;
}

void test_legacy_exclusions_survive_learning_and_restart() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  std::vector<std::string> legacy;
  for (std::size_t index = 0; index < 199; ++index)
    legacy.push_back("old" + maximum_word(index).substr(60));
  for (std::size_t index = 0; index < 49; ++index)
    legacy.push_back("Old'" + std::to_string(index) + "[]");
  std::string payload = "# Punto Switcher Undo Exclusions\n";
  const std::string learned = "new'word42+*";
  for (const auto &word : legacy)
    payload += word + '\n';
  {
    std::ofstream output(path);
    output << payload;
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure migrated legacy fixture");
  {
    punto::UndoDetector detector{path.string()};
    wait_ready(detector);
    expect(!detector.persistence_failed() && detector.exclusion_count() == 248,
           "all 248 legacy exclusions load without truncation");
    for (const auto &word : legacy)
      expect(detector.is_excluded(word), "legacy token spelling is preserved");
    expect(read_file(path) == payload, "loading legacy exclusions never rewrites them");
    detector.add_exclusion(learned);
    expect(detector.is_excluded(learned), "physical punctuation and digits can be learned");
    wait_saved(detector);
  }
  punto::UndoDetector restarted{path.string()};
  wait_ready(restarted);
  expect(restarted.exclusion_count() == legacy.size() + 1,
         "learning preserves the complete legacy set across restart");
  for (const auto &word : legacy)
    expect(restarted.is_excluded(word), "no legacy token is lost on publication");
  expect(restarted.is_excluded(learned), "new printable exclusion survives restart");
  struct stat metadata {};
  expect(::lstat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600,
         "legacy rewrite retains private file mode");
}

void test_full_capacity_long_words_round_trip() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    std::ofstream output(path);
    for (std::size_t index = 0; index < 1023; ++index)
      output << maximum_word(index) << '\n';
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure maximum word fixture");
  punto::UndoDetector detector{path.string()};
  wait_ready(detector);
  expect(detector.exclusion_count() == 1023, "1023 maximum length words load");
  detector.add_exclusion(maximum_word(1023));
  expect(detector.is_excluded(maximum_word(1023)),
         "1024th maximum word is accepted");
  wait_saved(detector);
  expect(read_file(path).size() == 65536,
         "full valid exclusion set fits storage bound");
  punto::UndoDetector restarted{path.string()};
  wait_ready(restarted);
  for (std::size_t index = 0; index < 1024; ++index)
    expect(restarted.is_excluded(maximum_word(index)),
           "every maximum word survives restart");
}

void test_duplicate_at_capacity_preserves_all_entries() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  std::string payload;
  for (std::size_t index = 0; index < punto::UndoDetector::maximum_entries(); ++index)
    payload += maximum_word(index).substr(60) + '\n';
  payload += maximum_word(0).substr(60) + '\n';
  {
    std::ofstream output(path);
    output << payload;
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure duplicate-at-capacity fixture");
  punto::UndoDetector detector{path.string()};
  wait_ready(detector);
  expect(!detector.persistence_failed() && detector.exclusion_count() == punto::UndoDetector::maximum_entries(),
         "duplicate row at capacity does not reject existing distinct entries");
  for (std::size_t index = 0; index < punto::UndoDetector::maximum_entries(); ++index)
    expect(detector.is_excluded(maximum_word(index).substr(60)), "every distinct entry remains available");
  expect(read_file(path) == payload, "duplicate row load leaves source unchanged");
}

void test_full_refresh_preserves_accepted_local_additions(
    bool completed_add = false) {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  std::mutex mutex;
  std::condition_variable condition;
  bool refreshing = false;
  bool permitted = false;
  unsigned int calls = 0;
  punto::UndoDetector detector{path.string(), [&] {
                                 if (++calls != 2)
                                   return;
                                 std::unique_lock lock{mutex};
                                 refreshing = true;
                                 condition.notify_all();
                                 condition.wait(lock,
                                                [&] { return permitted; });
                               }};
  wait_ready(detector);
  if (completed_add)
    detector.add_exclusion("localone");
  else
    detector.load_from_file();
  {
    std::unique_lock lock{mutex};
    expect(condition.wait_for(lock, std::chrono::seconds{1},
                              [&] { return refreshing; }),
           "refresh reaches controlled read boundary");
  }
  std::vector<std::string> local_words{"localone"};
  if (completed_add) {
    for (std::size_t index = 0; index < punto::UndoDetector::maximum_entries() - 1; ++index)
      local_words.push_back("pending" + maximum_word(index).substr(60));
  } else
    local_words.push_back("localtwo");
  for (const auto &word : local_words)
    detector.add_exclusion(word);
  const auto all_local_cached = [&] {
    return std::all_of(
        local_words.begin(), local_words.end(),
        [&](const auto &word) { return detector.is_excluded(word); });
  };
  const bool accepted = all_local_cached();
  std::string external = completed_add ? "localone\n" : "";
  for (std::size_t index = 0; index < punto::UndoDetector::maximum_entries() - (completed_add ? 1U : 0U); ++index) {
    auto word = maximum_word(index);
    if (completed_add)
      word[0] = 'z';
    external += word + '\n';
  }
  {
    std::ofstream output(path);
    output << external;
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure full external snapshot");
  {
    std::lock_guard lock{mutex};
    permitted = true;
  }
  condition.notify_all();
  expect(accepted,
         "all local additions accepted before older operation completes");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!detector.persistence_failed() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(detector.pending() && detector.persistence_failed(),
         "full persistent store reports pending failure");
  expect(all_local_cached(),
         "full older operation cannot drop accepted local additions");
  expect(detector.exclusion_count() <= punto::UndoDetector::maximum_entries(), "local overlay stays bounded");
  expect(read_file(path) == external,
         "capacity failure leaves external snapshot intact");
}

void test_unsafe_and_oversized_files_fail_closed() {
  TempDir dir;
  const auto victim = dir.path() / "victim";
  const auto link = dir.path() / "undo.txt";
  {
    std::ofstream output(victim);
    output << "secret\n";
  }
  expect(::symlink(victim.c_str(), link.c_str()) == 0, "create symlink");
  punto::UndoDetector linked{link.string()};
  wait_ready(linked);
  linked.add_exclusion("word");
  expect(linked.exclusion_count() == 0, "symlink persistence fails closed");
  expect(read_file(victim) == "secret\n", "symlink target is untouched");

  const auto oversized = dir.path() / "oversized.txt";
  std::string oversized_payload;
  for (std::size_t index = 0; index <= punto::UndoDetector::maximum_file_bytes() / 2; ++index)
    oversized_payload += "a\n";
  {
    std::ofstream output(oversized, std::ios::binary);
    output << oversized_payload;
  }
  expect(::chmod(oversized.c_str(), 0600) == 0, "chmod oversized fixture");
  punto::UndoDetector rejected{oversized.string()};
  wait_ready(rejected);
  expect(rejected.exclusion_count() == 0 && rejected.persistence_failed(), "oversized file is rejected");
  rejected.add_exclusion("newword");
  expect(read_file(oversized) == oversized_payload,
         "oversized file remains unchanged after rejected learning");
}

void test_published_mutation_is_not_replayed(bool clear) {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    std::ofstream output(path);
    output << "obsolete\n";
  }
  expect(::chmod(path.c_str(), 0600) == 0, "secure publication fault fixture");
  unsigned int calls = 0;
  punto::UndoDetector detector{path.string(), {}, [&](int fd) {
                                 if (++calls == 1) {
                                   errno = EIO;
                                   return -1;
                                 }
                                 return ::fsync(fd);
                               }};
  wait_ready(detector);
  if (clear)
    detector.clear_exclusions();
  else
    detector.add_exclusion("localone");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!detector.persistence_failed() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  expect(detector.persistence_failed() && detector.pending(),
         "published mutation reports failed durability");
  expect(clear ? read_file(path).empty()
               : read_file(path).find("localone\n") != std::string::npos,
         "mutation was already published before directory sync failure");
  {
    punto::UndoDetector peer{path.string()};
    wait_ready(peer);
    if (clear)
      peer.add_exclusion("peer");
    else
      peer.clear_exclusions();
    wait_saved(peer);
  }
  detector.load_from_file();
  wait_saved(detector);
  expect(read_file(path) == (clear ? "peer\n" : ""),
         "durability retry never replays published mutation over newer peer "
         "state");
}

void test_undo_window_learning() {
  TempDir dir;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string()};
  wait_ready(detector);
  detector.on_correction_applied(1, "learn");
  const auto now = std::chrono::steady_clock::now();
  expect(!detector.on_backspace(now), "first backspace does not learn");
  expect(!detector.on_backspace(now), "second backspace does not learn");
  expect(detector.on_backspace(now), "third backspace learns");
  expect(detector.is_excluded("learn"), "learned word is immediately cached");
  wait_saved(detector);
}

void test_intervening_input_invalidates_learning_candidate() {
  TempDir dir;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string()};
  wait_ready(detector);
  detector.on_correction_applied(1, "original");
  detector.on_key_typed();
  const auto now = std::chrono::steady_clock::now();
  expect(!detector.on_backspace(now), "first later backspace is unrelated");
  expect(!detector.on_backspace(now), "second later backspace is unrelated");
  expect(!detector.on_backspace(now), "third later backspace is unrelated");
  expect(!detector.is_excluded("original"),
         "typing before deletion does not learn old correction");
}

void test_concurrent_writers_merge_under_lock() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    punto::UndoDetector seed{path.string()};
    wait_ready(seed);
    seed.clear_exclusions();
    wait_saved(seed);
  }

  int gate[2]{};
  expect(::pipe(gate) == 0, "create writer gate");
  const pid_t first = ::fork();
  expect(first >= 0, "fork first writer");
  if (first == 0) {
    (void)::close(gate[1]);
    punto::UndoDetector writer{path.string()};
    wait_ready(writer);
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    writer.add_exclusion("first");
    wait_saved(writer);
    _exit(writer.is_excluded("first") ? 0 : 3);
  }

  const pid_t second = ::fork();
  expect(second >= 0, "fork second writer");
  if (second == 0) {
    (void)::close(gate[1]);
    punto::UndoDetector writer{path.string()};
    wait_ready(writer);
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    writer.add_exclusion("second");
    wait_saved(writer);
    _exit(writer.is_excluded("second") ? 0 : 3);
  }
  (void)::close(gate[0]);
  expect(::write(gate[1], "xx", 2) == 2, "release both writers");
  (void)::close(gate[1]);

  int first_status = 0;
  int second_status = 0;
  expect(::waitpid(first, &first_status, 0) == first, "wait first writer");
  expect(::waitpid(second, &second_status, 0) == second, "wait second writer");
  expect(WIFEXITED(first_status) && WEXITSTATUS(first_status) == 0,
         "first writer succeeds");
  expect(WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0,
         "second writer succeeds");

  punto::UndoDetector merged{path.string()};
  wait_ready(merged);
  expect(merged.is_excluded("first") && merged.is_excluded("second"),
         "concurrent snapshots retain both entries");
}

void test_clear_then_stale_process_add_does_not_resurrect_entries() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    punto::UndoDetector seed{path.string()};
    wait_ready(seed);
    seed.add_exclusion("obsolete");
    wait_saved(seed);
  }
  int gate[2]{};
  int loaded[2]{};
  expect(::pipe(gate) == 0, "create clear/add gate");
  expect(::pipe(loaded) == 0, "create child snapshot gate");
  const pid_t child = ::fork();
  expect(child >= 0, "fork stale writer");
  if (child == 0) {
    (void)::close(gate[1]);
    (void)::close(loaded[0]);
    punto::UndoDetector stale{path.string()};
    wait_ready(stale);
    expect(stale.is_excluded("obsolete"), "child loads pre-clear snapshot");
    expect(::write(loaded[1], "x", 1) == 1, "publish child snapshot readiness");
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    stale.add_exclusion("current");
    wait_saved(stale);
    _exit(0);
  }

  (void)::close(gate[0]);
  (void)::close(loaded[1]);
  char snapshot_ready = 0;
  expect(::read(loaded[0], &snapshot_ready, 1) == 1,
         "wait for child pre-clear snapshot");
  (void)::close(loaded[0]);
  {
    punto::UndoDetector clearing{path.string()};
    wait_ready(clearing);
    clearing.clear_exclusions();
    wait_saved(clearing);
  }
  expect(::write(gate[1], "x", 1) == 1, "release stale writer after clear");
  (void)::close(gate[1]);

  int child_status = 0;
  expect(::waitpid(child, &child_status, 0) == child, "wait stale writer");
  expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
         "stale writer exits cleanly");

  punto::UndoDetector observer{path.string()};
  wait_ready(observer);
  expect(observer.is_excluded("current"), "post-clear delta is retained");
  expect(!observer.is_excluded("obsolete"),
         "stale process does not resurrect cleared entries");
}

void test_live_instances_converge_after_background_refresh() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  punto::UndoDetector first{path.string()};
  punto::UndoDetector second{path.string()};
  wait_ready(first);
  wait_ready(second);

  first.add_exclusion("shared");
  wait_saved(first);
  second.load_from_file();
  wait_saved(second);
  expect(second.is_excluded("shared"),
         "live peer observes a completed exclusion add");

  first.clear_exclusions();
  wait_saved(first);
  second.load_from_file();
  wait_saved(second);
  expect(!second.is_excluded("shared"),
         "live peer observes a completed exclusion clear");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view{argv[1]} == "legacy") {
    test_legacy_exclusions_survive_learning_and_restart();
    return 0;
  }
  if (argc == 2 && std::string_view{argv[1]} == "capacity") {
    test_full_capacity_long_words_round_trip();
    return 0;
  }
  if (argc == 2 && std::string_view{argv[1]} == "invalid-capacity") {
    test_invalid_words_and_capacity_are_bounded();
    return 0;
  }
  if (argc == 2 && std::string_view{argv[1]} == "duplicate") {
    test_duplicate_at_capacity_preserves_all_entries();
    return 0;
  }
  if (argc == 2 && (std::string_view{argv[1]} == "published-clear" ||
                    std::string_view{argv[1]} == "published-add")) {
    test_published_mutation_is_not_replayed(std::string_view{argv[1]} ==
                                            "published-clear");
    return 0;
  }
  if (argc == 2 && std::string_view{argv[1]} == "full-refresh") {
    test_full_refresh_preserves_accepted_local_additions();
    return 0;
  }
  if (argc == 2 && std::string_view{argv[1]} == "current-add") {
    test_full_refresh_preserves_accepted_local_additions(true);
    return 0;
  }
  if (argc != 1)
    return 2;
  test_read_bounded_preserves_default_and_small_limits();
  test_legacy_exclusions_survive_learning_and_restart();
  test_full_capacity_long_words_round_trip();
  test_duplicate_at_capacity_preserves_all_entries();
  test_published_mutation_is_not_replayed(true);
  test_published_mutation_is_not_replayed(false);
  test_full_refresh_preserves_accepted_local_additions();
  test_full_refresh_preserves_accepted_local_additions(true);
  test_initial_io_does_not_block_constructor();
  test_shutdown_is_bounded_with_stalled_io();
  test_exclusion_lookup_does_not_read_file();
  test_learning_is_cached_while_write_is_blocked();
  test_failed_write_retains_learning_and_recovers();
  test_clear_and_new_add_survive_older_write_completion();
  test_atomic_private_round_trip();
  test_invalid_words_and_capacity_are_bounded();
  test_unsafe_and_oversized_files_fail_closed();
  test_undo_window_learning();
  test_intervening_input_invalidates_learning_candidate();
  test_concurrent_writers_merge_under_lock();
  test_clear_then_stale_process_add_does_not_resurrect_entries();
  test_live_instances_converge_after_background_refresh();
  std::cout << "PASS: undo detector contract\n";
  return 0;
}
