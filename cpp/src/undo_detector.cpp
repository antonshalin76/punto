#include "punto/undo_detector.hpp"

#include "punto/control_plane_state.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace punto {
namespace {

constexpr std::string_view kHeader =
    "# Punto Switcher Undo Exclusions\n"
    "# Automatically learned words; one lowercase ASCII word per line.\n\n";

RuntimeFileSecurity exclusion_security() noexcept {
  return RuntimeFileSecurity{::geteuid(), ::getegid(), 0600};
}

std::optional<std::unordered_set<std::string>>
parse_exclusions(std::string_view payload) {
  if (payload.find('\0') != std::string_view::npos ||
      (!payload.empty() && payload.back() != '\n')) {
    return std::nullopt;
  }
  std::unordered_set<std::string> parsed;
  std::size_t begin = 0;
  while (begin < payload.size()) {
    const std::size_t end = payload.find('\n', begin);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string word{payload.substr(begin, end - begin)};
    begin = end + 1U;
    if (word.empty() || word.front() == '#') {
      continue;
    }
    const bool valid =
        word.size() <= UndoDetector::maximum_word_bytes() &&
        std::all_of(word.begin(), word.end(), [](char character) {
          return character >= 'a' && character <= 'z';
        });
    if (!valid || parsed.size() >= UndoDetector::maximum_entries()) {
      return std::nullopt;
    }
    parsed.insert(word);
  }
  return parsed;
}

std::optional<std::unordered_set<std::string>>
read_exclusions_at(int directory_fd, std::string_view name,
                   const RuntimeFileSecurity &security) {
  int fd = -1;
  const std::string name_string{name};
  do {
    fd = ::openat(directory_fd, name_string.c_str(),
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return errno == ENOENT
               ? std::optional<std::unordered_set<
                     std::string>>{std::unordered_set<std::string>{}}
               : std::nullopt;
  }
  if (!verify_runtime_file_security(fd, security)) {
    (void)::close(fd);
    return std::nullopt;
  }
  const auto payload = detail::read_bounded(fd);
  const bool closed = ::close(fd) == 0;
  if (!payload || !closed) {
    return std::nullopt;
  }
  return parse_exclusions(*payload);
}

int acquire_persistence_lock(int directory_fd, std::string_view name,
                             const RuntimeFileSecurity &security) noexcept {
  const std::string lock_name = "." + std::string{name} + ".lock";
  int fd = -1;
  do {
    fd = ::openat(directory_fd, lock_name.c_str(),
                  O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0 || !apply_runtime_file_security(fd, security)) {
    if (fd >= 0) {
      (void)::close(fd);
    }
    return -1;
  }
  for (unsigned int attempt = 0; attempt < 50U; ++attempt) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      return fd;
    }
    if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  (void)::close(fd);
  return -1;
}

} // namespace

UndoDetector::UndoDetector(std::string path) : file_path_{std::move(path)} {
  load_from_file();
}

void UndoDetector::on_correction_applied(std::uint64_t task_id,
                                         const std::string &original_word) {
  if (!valid_word(original_word)) {
    last_correction_.reset();
    backspace_count_since_correction_ = 0;
    return;
  }
  last_correction_ = RecentCorrection{task_id, original_word,
                                      std::chrono::steady_clock::now()};
  backspace_count_since_correction_ = 0;
}

bool UndoDetector::on_backspace(std::chrono::steady_clock::time_point now) {
  if (!last_correction_) {
    return false;
  }
  if (now - last_correction_->applied_at > kUndoWindow) {
    last_correction_.reset();
    backspace_count_since_correction_ = 0;
    return false;
  }
  ++backspace_count_since_correction_;
  if (backspace_count_since_correction_ < kMinBackspaceCount) {
    return false;
  }
  add_exclusion(last_correction_->original_word);
  last_correction_.reset();
  backspace_count_since_correction_ = 0;
  return true;
}

void UndoDetector::on_undo() {
  if (!last_correction_) {
    return;
  }
  add_exclusion(last_correction_->original_word);
  last_correction_.reset();
  backspace_count_since_correction_ = 0;
}

void UndoDetector::on_key_typed() noexcept {
  last_correction_.reset();
  backspace_count_since_correction_ = 0;
}

bool UndoDetector::is_excluded(const std::string &word) {
  (void)refresh_from_file();
  return exclusions_.contains(word);
}

std::size_t UndoDetector::exclusion_count() const noexcept {
  return exclusions_.size();
}

bool UndoDetector::valid_word(const std::string &word) noexcept {
  return !word.empty() && word.size() <= maximum_word_bytes() &&
         std::all_of(word.begin(), word.end(), [](char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte >= static_cast<unsigned char>('a') &&
                  byte <= static_cast<unsigned char>('z');
         });
}

bool UndoDetector::refresh_from_file() {
  const auto path = detail::split_runtime_path(file_path_);
  if (!path) {
    return false;
  }
  const RuntimeFileSecurity security = exclusion_security();
  const int directory_fd = detail::open_runtime_directory(*path, security);
  if (directory_fd < 0) {
    return false;
  }
  auto parsed = read_exclusions_at(directory_fd, path->name, security);
  (void)::close(directory_fd);
  if (!parsed) {
    return false;
  }
  exclusions_ = std::move(*parsed);
  return true;
}

void UndoDetector::load_from_file() {
  if (!refresh_from_file()) {
    std::cerr << "[punto] Ignoring unsafe or malformed undo exclusions file\n";
    return;
  }
  if (!exclusions_.empty()) {
    std::cerr << "[punto] Loaded " << exclusions_.size()
              << " persistent undo exclusions\n";
  }
}

bool UndoDetector::persist(PersistenceMutation mutation,
                           std::string_view word) {
  const auto path = detail::split_runtime_path(file_path_);
  if (!path) {
    return false;
  }
  const RuntimeFileSecurity security = exclusion_security();
  const int directory_fd = detail::open_runtime_directory(*path, security);
  if (directory_fd < 0) {
    return false;
  }
  const int lock_fd =
      acquire_persistence_lock(directory_fd, path->name, security);
  if (lock_fd < 0) {
    (void)::close(directory_fd);
    return false;
  }

  const auto existing = read_exclusions_at(directory_fd, path->name, security);
  if (!existing) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return false;
  }

  std::unordered_set<std::string> next;
  if (mutation == PersistenceMutation::Add) {
    next = *existing;
    const std::string added_word{word};
    if (!next.contains(added_word)) {
      if (next.size() >= maximum_entries()) {
        exclusions_ = std::move(next);
        (void)::close(lock_fd);
        (void)::close(directory_fd);
        return true;
      }
      next.insert(added_word);
    }
  }

  std::vector<std::string> words{next.begin(), next.end()};
  std::sort(words.begin(), words.end());
  std::string payload{kHeader};
  for (const auto &word : words) {
    payload += word;
    payload.push_back('\n');
  }
  if (payload.size() > detail::kMaxControlPlaneStateBytes) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return false;
  }

  static std::atomic<std::uint64_t> sequence{0};
  int temp_fd = -1;
  std::string temp_name;
  for (unsigned int attempt = 0; attempt < 64U && temp_fd < 0; ++attempt) {
    const std::uint64_t nonce =
        sequence.fetch_add(1, std::memory_order_relaxed);
    temp_name = "." + path->name + ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(nonce);
    do {
      temp_fd =
          ::openat(directory_fd, temp_name.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (temp_fd < 0 && errno == EINTR);
    if (temp_fd < 0 && errno != EEXIST) {
      break;
    }
  }
  if (temp_fd < 0) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return false;
  }

  bool ok = apply_runtime_file_security(temp_fd, security) &&
            detail::write_all(temp_fd, payload) && ::fsync(temp_fd) == 0;
  if (::close(temp_fd) != 0) {
    ok = false;
  }
  if (ok) {
    ok = ::renameat(directory_fd, temp_name.c_str(), directory_fd,
                    path->name.c_str()) == 0;
  }
  if (ok) {
    ok = ::fsync(directory_fd) == 0;
  }
  if (!ok) {
    (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
  } else {
    exclusions_ = std::move(next);
  }
  (void)::close(lock_fd);
  (void)::close(directory_fd);
  return ok;
}

void UndoDetector::clear_exclusions() {
  if (!persist(PersistenceMutation::Clear)) {
    std::cerr << "[punto] Warning: cannot clear undo exclusions\n";
  }
}

void UndoDetector::add_exclusion(const std::string &word) {
  if (!valid_word(word)) {
    return;
  }
  const bool already_known = exclusions_.contains(word);
  if (!persist(PersistenceMutation::Add, word)) {
    std::cerr << "[punto] Warning: cannot persist undo exclusion\n";
    return;
  }
  if (!already_known && exclusions_.contains(word)) {
    std::cerr << "[punto] Added undo exclusion entry\n";
  }
}

} // namespace punto
