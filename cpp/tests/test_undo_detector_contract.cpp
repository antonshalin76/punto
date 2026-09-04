#include "punto/undo_detector.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include <sys/stat.h>
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

void test_atomic_private_round_trip() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  {
    punto::UndoDetector detector{path.string()};
    detector.add_exclusion("word");
    detector.add_exclusion("alpha");
    expect(detector.exclusion_count() == 2, "two valid entries persist");
  }
  struct stat metadata {};
  expect(::lstat(path.c_str(), &metadata) == 0, "persistent file exists");
  expect(S_ISREG(metadata.st_mode) && metadata.st_nlink == 1,
         "persistent file is a single regular inode");
  expect((metadata.st_mode & 0777) == 0600, "persistent file is private");
  expect(metadata.st_uid == ::geteuid(), "persistent file owner is exact");

  punto::UndoDetector loaded{path.string()};
  expect(loaded.is_excluded("alpha") && loaded.is_excluded("word"),
         "valid file round-trips");
  expect(read_file(path).find("alpha\nword\n") != std::string::npos,
         "serialization is deterministic");
}

void test_invalid_words_and_capacity_are_bounded() {
  TempDir dir;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string()};
  detector.add_exclusion("UPPER");
  detector.add_exclusion("line\nbreak");
  detector.add_exclusion(
      std::string(punto::UndoDetector::maximum_word_bytes() + 1U, 'a'));
  expect(detector.exclusion_count() == 0, "invalid entries are rejected");
  for (std::size_t index = 0;
       index < punto::UndoDetector::maximum_entries() + 10U; ++index) {
    std::string word;
    std::size_t value = index;
    do {
      word.push_back(static_cast<char>('a' + (value % 26U)));
      value /= 26U;
    } while (value != 0U);
    detector.add_exclusion("w" + word);
  }
  expect(detector.exclusion_count() == punto::UndoDetector::maximum_entries(),
         "entry count is capped");
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
  linked.add_exclusion("word");
  expect(linked.exclusion_count() == 0, "symlink persistence fails closed");
  expect(read_file(victim) == "secret\n", "symlink target is untouched");

  const auto oversized = dir.path() / "oversized.txt";
  {
    std::ofstream output(oversized, std::ios::binary);
    output << std::string(9000U, 'a') << '\n';
  }
  expect(::chmod(oversized.c_str(), 0600) == 0, "chmod oversized fixture");
  punto::UndoDetector rejected{oversized.string()};
  expect(rejected.exclusion_count() == 0, "oversized file is rejected");
}

void test_undo_window_learning() {
  TempDir dir;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string()};
  detector.on_correction_applied(1, "learn");
  const auto now = std::chrono::steady_clock::now();
  expect(!detector.on_backspace(now), "first backspace does not learn");
  expect(!detector.on_backspace(now), "second backspace does not learn");
  expect(detector.on_backspace(now), "third backspace learns");
  expect(detector.is_excluded("learn"), "learned word is persisted");
}

void test_intervening_input_invalidates_learning_candidate() {
  TempDir dir;
  punto::UndoDetector detector{(dir.path() / "undo.txt").string()};
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
  punto::UndoDetector seed{path.string()};
  seed.clear_exclusions();

  int gate[2]{};
  expect(::pipe(gate) == 0, "create writer gate");
  const pid_t first = ::fork();
  expect(first >= 0, "fork first writer");
  if (first == 0) {
    (void)::close(gate[1]);
    punto::UndoDetector writer{path.string()};
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    writer.add_exclusion("first");
    _exit(writer.is_excluded("first") ? 0 : 3);
  }

  const pid_t second = ::fork();
  expect(second >= 0, "fork second writer");
  if (second == 0) {
    (void)::close(gate[1]);
    punto::UndoDetector writer{path.string()};
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    writer.add_exclusion("second");
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
  expect(merged.is_excluded("first") && merged.is_excluded("second"),
         "concurrent snapshots retain both entries");
}

void test_clear_then_stale_process_add_does_not_resurrect_entries() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  punto::UndoDetector seed{path.string()};
  seed.add_exclusion("obsolete");

  punto::UndoDetector stale{path.string()};
  int gate[2]{};
  expect(::pipe(gate) == 0, "create clear/add gate");
  const pid_t child = ::fork();
  expect(child >= 0, "fork stale writer");
  if (child == 0) {
    (void)::close(gate[1]);
    char byte = 0;
    if (::read(gate[0], &byte, 1) != 1) {
      _exit(2);
    }
    stale.add_exclusion("current");
    _exit(0);
  }

  (void)::close(gate[0]);
  stale.clear_exclusions();
  expect(::write(gate[1], "x", 1) == 1, "release stale writer after clear");
  (void)::close(gate[1]);

  int child_status = 0;
  expect(::waitpid(child, &child_status, 0) == child, "wait stale writer");
  expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
         "stale writer exits cleanly");

  punto::UndoDetector observer{path.string()};
  expect(observer.is_excluded("current"), "post-clear delta is retained");
  expect(!observer.is_excluded("obsolete"),
         "stale process does not resurrect cleared entries");
}

void test_live_instances_converge_before_exclusion_decision() {
  TempDir dir;
  const auto path = dir.path() / "undo.txt";
  punto::UndoDetector first{path.string()};
  punto::UndoDetector second{path.string()};

  first.add_exclusion("shared");
  expect(second.is_excluded("shared"),
         "live peer observes a completed exclusion add");

  first.clear_exclusions();
  expect(!second.is_excluded("shared"),
         "live peer observes a completed exclusion clear");
}

} // namespace

int main() {
  test_atomic_private_round_trip();
  test_invalid_words_and_capacity_are_bounded();
  test_unsafe_and_oversized_files_fail_closed();
  test_undo_window_learning();
  test_intervening_input_invalidates_learning_candidate();
  test_concurrent_writers_merge_under_lock();
  test_clear_then_stale_process_add_does_not_resurrect_entries();
  test_live_instances_converge_before_exclusion_decision();
  std::cout << "PASS: undo detector contract\n";
  return 0;
}
