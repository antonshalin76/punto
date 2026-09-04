#include "punto/control_plane_state.hpp"
#include "punto/macro_lock.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using punto::ControlPlaneLease;
using punto::MacroLock;
using punto::SharedControlPlaneState;

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
    char pattern[] = "/tmp/punto-runtime-files-XXXXXX";
    char *created = ::mkdtemp(pattern);
    expect(created != nullptr, "mkdtemp");
    path_ = created;
  }

  ~TempDir() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

gid_t runtime_group() {
  return punto::default_runtime_file_security().group_gid;
}

void write_runtime_file(const std::filesystem::path &path,
                        std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(output.is_open(), "open runtime fixture");
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  expect(output.good(), "write runtime fixture");
  expect(::chmod(path.c_str(), 0660) == 0, "chmod runtime fixture");
  expect(::chown(path.c_str(), ::geteuid(), runtime_group()) == 0,
         "chown runtime fixture");
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  expect(input.is_open(), "open fixture for read");
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

constexpr std::string_view kValidState = "config_generation=7\n"
                                         "status_generation=9\n"
                                         "enabled=0\n"
                                         "config_path=/tmp/punto/config.yaml\n";

void test_state_round_trip_and_permissions() {
  TempDir dir;
  const auto state_path = dir.path() / "control.state";
  SharedControlPlaneState input{7, 9, false, "/tmp/punto/config.yaml"};
  expect(punto::write_shared_control_plane_state(input, state_path.string()),
         "write valid state");

  struct stat info {};
  expect(::lstat(state_path.c_str(), &info) == 0, "stat state");
  expect(S_ISREG(info.st_mode), "state is regular");
  expect(info.st_nlink == 1, "state has one link");
  expect((info.st_mode & 0777) == 0660, "state mode is exact");
  expect(info.st_uid == ::geteuid(), "state owner is current daemon uid");
  expect(info.st_gid == runtime_group(), "state group is runtime group");

  SharedControlPlaneState output;
  expect(punto::read_shared_control_plane_state(output, state_path.string()),
         "read valid state");
  expect(output.config_generation == input.config_generation,
         "round-trip config generation");
  expect(output.status_generation == input.status_generation,
         "round-trip status generation");
  expect(output.enabled == input.enabled, "round-trip enabled");
  expect(output.config_path == input.config_path, "round-trip config path");
}

void expect_state_rejected(std::string_view payload, std::string_view message) {
  TempDir dir;
  const auto state_path = dir.path() / "control.state";
  write_runtime_file(state_path, payload);

  SharedControlPlaneState output{41, 42, false, "/unchanged"};
  expect(!punto::read_shared_control_plane_state(output, state_path.string()),
         message);
  expect(output.config_generation == 41 && output.status_generation == 42 &&
             !output.enabled && output.config_path == "/unchanged",
         "failed read preserves caller state");
}

void test_state_parser_is_strict_and_bounded() {
  expect_state_rejected("config_generation=7\nstatus_generation=9\nenabled=0\n",
                        "missing key rejected");
  expect_state_rejected(std::string{kValidState} + "enabled=1\n",
                        "duplicate key rejected");
  expect_state_rejected(std::string{kValidState} + "unknown=1\n",
                        "unknown key rejected");
  expect_state_rejected(
      "config_generation=bad\nstatus_generation=9\nenabled=0\n"
      "config_path=/tmp/punto/config.yaml\n",
      "invalid generation rejected");
  expect_state_rejected(
      "config_generation=7\nstatus_generation=9\nenabled=yes\n"
      "config_path=/tmp/punto/config.yaml\n",
      "non-canonical boolean rejected");
  expect_state_rejected("config_generation=7\nstatus_generation=9\nenabled=0\n"
                        "config_path=/tmp/punto/config.yaml",
                        "unterminated record rejected");

  std::string with_nul{kValidState};
  with_nul.push_back('\0');
  expect_state_rejected(with_nul, "embedded NUL rejected");

  std::string oversized = "config_generation=7\nstatus_generation=9\n"
                          "enabled=0\nconfig_path=/tmp/";
  oversized.append(16'384, 'x');
  oversized.push_back('\n');
  expect_state_rejected(oversized, "oversized state rejected");
}

void test_state_rejects_symlinks_and_injection() {
  TempDir dir;
  const auto target = dir.path() / "target";
  const auto link = dir.path() / "control.state";
  write_runtime_file(target, kValidState);
  expect(::symlink(target.c_str(), link.c_str()) == 0, "create state symlink");

  SharedControlPlaneState output;
  expect(!punto::read_shared_control_plane_state(output, link.string()),
         "state reader rejects symlink");
  SharedControlPlaneState replacement{1, 2, true, "/tmp/config.yaml"};
  expect(!punto::write_shared_control_plane_state(replacement, link.string()),
         "state writer rejects existing symlink");
  expect(read_file(target) == kValidState, "state symlink target unchanged");

  const auto injected_path = dir.path() / "injected.state";
  SharedControlPlaneState injected{1, 2, true, "/tmp/good\nenabled=0"};
  expect(!punto::write_shared_control_plane_state(injected,
                                                  injected_path.string()),
         "state writer rejects line injection");
  expect(!std::filesystem::exists(injected_path),
         "rejected state was not published");
}

void test_state_write_does_not_follow_predictable_temp_symlink() {
  TempDir dir;
  const auto state_path = dir.path() / "control.state";
  const auto victim = dir.path() / "victim";
  write_runtime_file(victim, "do-not-touch\n");

  const auto attacker_temp = std::filesystem::path{
      state_path.string() + ".tmp." + std::to_string(::getpid())};
  expect(::symlink(victim.c_str(), attacker_temp.c_str()) == 0,
         "create predictable temp symlink");
  const auto colliding_temp =
      dir.path() / (".control.state.tmp." + std::to_string(::getpid()) + ".1");
  expect(::symlink(victim.c_str(), colliding_temp.c_str()) == 0,
         "create colliding exclusive temp symlink");

  SharedControlPlaneState state{1, 2, true, "/tmp/config.yaml"};
  expect(punto::write_shared_control_plane_state(state, state_path.string()),
         "secure state write succeeds beside attacker symlink");
  expect(read_file(victim) == "do-not-touch\n",
         "state write did not overwrite symlink target");
}

void test_state_rejects_non_regular_and_linked_files() {
  TempDir dir;
  const auto state_path = dir.path() / "control.state";
  const auto alias_path = dir.path() / "control.alias";
  write_runtime_file(state_path, kValidState);
  expect(::link(state_path.c_str(), alias_path.c_str()) == 0,
         "create state hard link");
  SharedControlPlaneState output;
  expect(!punto::read_shared_control_plane_state(output, state_path.string()),
         "state reader rejects multiply-linked inode");

  expect(std::filesystem::remove(alias_path), "remove state hard link");
  expect(std::filesystem::remove(state_path), "remove state file");
  expect(::mkfifo(state_path.c_str(), 0660) == 0, "create state FIFO");
  expect(::chown(state_path.c_str(), ::geteuid(), runtime_group()) == 0,
         "chown state FIFO");
  const auto started = std::chrono::steady_clock::now();
  expect(!punto::read_shared_control_plane_state(output, state_path.string()),
         "state reader rejects FIFO");
  expect(std::chrono::steady_clock::now() - started <
             std::chrono::milliseconds{250},
         "FIFO rejection is nonblocking");
}

void test_control_plane_lease_rejects_symlink_and_serializes() {
  TempDir dir;
  const auto victim = dir.path() / "victim";
  const auto symlink_path = dir.path() / "lease-link";
  write_runtime_file(victim, "unchanged\n");
  expect(::symlink(victim.c_str(), symlink_path.c_str()) == 0,
         "create lease symlink");
  ControlPlaneLease bad{symlink_path.string()};
  expect(!bad.try_acquire(), "control-plane lease rejects symlink");
  expect(read_file(victim) == "unchanged\n", "lease target unchanged");

  const auto lock_path = dir.path() / "control.lock";
  ControlPlaneLease first{lock_path.string()};
  ControlPlaneLease second{lock_path.string()};
  expect(first.try_acquire(), "first control-plane lease acquires");
  expect(!second.try_acquire(), "second control-plane lease is excluded");
}

void test_macro_lock_rejects_symlink_and_has_bounded_handoff() {
  TempDir dir;
  const auto victim = dir.path() / "victim";
  const auto symlink_path = dir.path() / "macro-link";
  write_runtime_file(victim, "unchanged\n");
  expect(::symlink(victim.c_str(), symlink_path.c_str()) == 0,
         "create macro symlink");

  MacroLock bad{symlink_path.string()};
  expect(!bad.try_lock(std::chrono::milliseconds{0}),
         "macro lock rejects symlink");
  expect(read_file(victim) == "unchanged\n", "macro target unchanged");

  const auto lock_path = dir.path() / "macro.lock";
  MacroLock first{lock_path.string()};
  MacroLock second{lock_path.string()};
  expect(first.try_lock(std::chrono::milliseconds{0}),
         "first macro lock acquires");
  const auto started = std::chrono::steady_clock::now();
  expect(!second.try_lock(std::chrono::milliseconds{20}),
         "second macro lock times out");
  expect(std::chrono::steady_clock::now() - started <
             std::chrono::milliseconds{250},
         "macro lock timeout is bounded");
  first.unlock();
  expect(second.try_lock(std::chrono::milliseconds{20}),
         "macro lock acquires after handoff");
}

void test_nested_macro_guards_hold_physical_lock_until_outer_exit() {
  TempDir dir;
  const auto lock_path = dir.path() / "nested-macro.lock";
  MacroLock nested{lock_path.string()};
  MacroLock contender{lock_path.string()};
  {
    punto::MacroLockGuard outer{nested, std::chrono::milliseconds{0}};
    expect(outer.owns_lock(), "outer macro guard acquires");
    {
      punto::MacroLockGuard inner{nested, std::chrono::milliseconds{0}};
      expect(inner.owns_lock(), "same-thread nested guard acquires");
      expect(!contender.try_lock(std::chrono::milliseconds{0}),
             "contender excluded while inner guard exists");
    }
    expect(nested.is_locked(), "inner exit retains outer recursion level");
    expect(!contender.try_lock(std::chrono::milliseconds{0}),
           "contender excluded until outer guard exits");
  }
  expect(!nested.is_locked(), "outer exit releases physical lock");
  expect(contender.try_lock(std::chrono::milliseconds{20}),
         "contender acquires after full nested unwind");
}

} // namespace

int main() {
  test_state_round_trip_and_permissions();
  test_state_parser_is_strict_and_bounded();
  test_state_rejects_symlinks_and_injection();
  test_state_write_does_not_follow_predictable_temp_symlink();
  test_state_rejects_non_regular_and_linked_files();
  test_control_plane_lease_rejects_symlink_and_serializes();
  test_macro_lock_rejects_symlink_and_has_bounded_handoff();
  test_nested_macro_guards_hold_physical_lock_until_outer_exit();
  std::cout << "punto-runtime-files-contract: OK\n";
}
