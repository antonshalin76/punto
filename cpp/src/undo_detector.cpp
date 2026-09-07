#include "punto/undo_detector.hpp"

#include "punto/control_plane_state.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
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
using Publication = ControlPlanePublicationResult;
namespace {

constexpr std::string_view kResetMarkerPrefix = "PUNTO_RESET_V1 ";

std::uint64_t monotonic_timestamp() noexcept {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return nanoseconds > 0 ? static_cast<std::uint64_t>(nanoseconds) : 1U;
}

bool valid_boot_id(std::string_view value) noexcept {
  if (value.size() != 36U) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8U || index == 13U || index == 18U || index == 23U) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string read_boot_id() {
  int fd = -1;
  do {
    fd = ::open("/proc/sys/kernel/random/boot_id",
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return {};
  }
  const auto payload = detail::read_bounded(fd, 37U);
  const bool closed = ::close(fd) == 0;
  if (!payload || !closed) {
    return {};
  }
  std::string boot_id = *payload;
  if (!boot_id.empty() && boot_id.back() == '\n') {
    boot_id.pop_back();
  }
  return valid_boot_id(boot_id) ? boot_id : std::string{};
}

struct ResetMarker {
  std::string boot_id;
  std::uint64_t timestamp = 0;
};

std::optional<ResetMarker>
read_reset_marker_at(int directory_fd, std::string_view name,
                     const RuntimeFileSecurity &security) {
  const std::string marker_name = "." + std::string{name} + ".reset";
  int fd = -1;
  do {
    fd = ::openat(directory_fd, marker_name.c_str(),
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    return errno == ENOENT ? std::optional<ResetMarker>{ResetMarker{}}
                           : std::nullopt;
  }
  struct stat metadata {};
  if (::fstat(fd, &metadata) != 0 || metadata.st_size < 0 ||
      metadata.st_size > 128 || !verify_runtime_file_security(fd, security)) {
    (void)::close(fd);
    return std::nullopt;
  }
  if (metadata.st_size == 0) {
    (void)::close(fd);
    return std::nullopt;
  }
  std::string payload(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const ssize_t count = ::pread(fd, payload.data() + offset,
                                  payload.size() - offset,
                                  static_cast<off_t>(offset));
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    (void)::close(fd);
    return std::nullopt;
  }
  const bool closed = ::close(fd) == 0;
  if (!closed) {
    return std::nullopt;
  }
  if (!payload.starts_with(kResetMarkerPrefix) || payload.back() != '\n') {
    return std::nullopt;
  }
  payload.pop_back();
  std::string_view fields{payload};
  fields.remove_prefix(kResetMarkerPrefix.size());
  const auto separator = fields.find(' ');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  ResetMarker marker;
  marker.boot_id = std::string{fields.substr(0, separator)};
  if (!valid_boot_id(marker.boot_id)) {
    return std::nullopt;
  }
  fields.remove_prefix(separator + 1U);
  if (fields.empty()) {
    return std::nullopt;
  }
  const auto parsed = std::from_chars(fields.data(),
                                      fields.data() + fields.size(),
                                      marker.timestamp);
  if (marker.timestamp == 0 || parsed.ec != std::errc{} ||
      parsed.ptr != fields.data() + fields.size() || fields.front() == '0') {
    return std::nullopt;
  }
  return marker;
}

bool publish_reset_marker_at(int directory_fd, std::string_view name,
                             const RuntimeFileSecurity &security,
                             std::string_view boot_id,
                             std::uint64_t timestamp) {
  if (!valid_boot_id(boot_id) || timestamp == 0) {
    return false;
  }
  const std::string payload = std::string{kResetMarkerPrefix} +
                              std::string{boot_id} + " " +
                              std::to_string(timestamp) + "\n";
  const std::string marker_name = "." + std::string{name} + ".reset";
  static std::atomic<std::uint64_t> sequence{0};
  int temp_fd = -1;
  std::string temp_name;
  for (unsigned int attempt = 0; attempt < 64U && temp_fd < 0; ++attempt) {
    temp_name = marker_name + ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(sequence.fetch_add(1,
                                                  std::memory_order_relaxed));
    do {
      temp_fd = ::openat(directory_fd, temp_name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0600);
    } while (temp_fd < 0 && errno == EINTR);
    if (temp_fd < 0 && errno != EEXIST) {
      break;
    }
  }
  if (temp_fd < 0) {
    return false;
  }
  bool ok = apply_runtime_file_security(temp_fd, security) &&
            detail::write_all(temp_fd, payload) && ::fsync(temp_fd) == 0;
  if (::close(temp_fd) != 0) {
    ok = false;
  }
  if (ok) {
    ok = ::renameat(directory_fd, temp_name.c_str(), directory_fd,
                    marker_name.c_str()) == 0;
  }
  if (!ok) {
    (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
  }
  return ok;
}

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
          return character >= '!' && character <= '~';
        });
    if (!valid || (!parsed.contains(word) &&
                   parsed.size() >= UndoDetector::maximum_entries())) {
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
  const auto payload =
      detail::read_bounded(fd, UndoDetector::maximum_file_bytes());
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

struct UndoDetector::Store {
  explicit Store(std::string path)
      : file_path_{std::move(path)}, boot_id_{read_boot_id()} {}
  std::string file_path_;
  std::string boot_id_;
  std::unordered_set<std::string> exclusions_;
  bool reset_in_current_boot_ = false;
  std::uint64_t reset_timestamp_ = 0;
  bool last_add_skipped_ = false;
  bool refresh_from_file();
  Publication persist(PersistenceMutation mutation, std::string_view word,
                      std::uint64_t accepted_at);
  int sync_directory(int fd) {
#ifdef PUNTO_TESTING
    if (directory_sync)
      return directory_sync(fd);
#endif
    return ::fsync(fd);
  }
#ifdef PUNTO_TESTING
  std::function<int(int)> directory_sync{};
#endif
};

struct UndoDetector::SharedState {
  struct Request {
    PersistenceMutation mutation = PersistenceMutation::Refresh;
    std::string word;
    std::uint64_t generation = 0;
    std::uint64_t accepted_at = 0;
  };
  explicit SharedState(std::string path) : path{std::move(path)} {
    requests.push_back({PersistenceMutation::Refresh, {}, ++generation});
  }
  std::string path;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<Request> requests;
  std::unordered_set<std::string> exclusions;
  bool initialized = false;
  bool storage_ready = false;
  bool working = false;
  bool failed = false;
  bool retry = true;
  bool stopping = false;
  bool exited = false;
  std::uint64_t generation = 0;
  std::uint64_t completed_generation = 0;
  std::uint64_t failed_generation = 0;
#ifdef PUNTO_TESTING
  std::function<void()> before_io;
  std::function<int(int)> directory_sync;
#endif
};

UndoDetector::UndoDetector(std::string path)
    : state_{std::make_shared<SharedState>(std::move(path))} {
  start();
}

#ifdef PUNTO_TESTING
UndoDetector::UndoDetector(std::string path, std::function<void()> before_io,
                           std::function<int(int)> directory_sync)
    : state_{std::make_shared<SharedState>(std::move(path))} {
  state_->before_io = std::move(before_io);
  state_->directory_sync = std::move(directory_sync);
  start();
}
#endif

void UndoDetector::start() {
  const auto state = state_;
  thread_ = std::thread{[state] { worker(state); }};
}

UndoDetector::~UndoDetector() {
  if (shutdown())
    return;
  thread_.detach();
  std::cerr << "[punto] Undo storage shutdown timed out; persistence may be "
               "pending\n";
}

bool UndoDetector::shutdown(std::chrono::milliseconds timeout) {
  if (!thread_.joinable())
    return true;
  bool exited = false;
  {
    std::unique_lock lock{state_->mutex};
    state_->stopping = true;
    state_->condition.notify_all();
    exited = state_->condition.wait_for(lock, timeout,
                                        [&] { return state_->exited; });
  }
  if (!exited)
    return false;
  thread_.join();
  return true;
}

void UndoDetector::worker(const std::shared_ptr<SharedState> &state) noexcept {
  try {
    Store store{state->path};
#ifdef PUNTO_TESTING
    store.directory_sync = state->directory_sync;
#endif
    for (;;) {
      SharedState::Request request;
      {
        std::unique_lock lock{state->mutex};
        state->condition.wait(lock, [&] {
          return state->stopping || (state->retry && !state->requests.empty());
        });
        if (state->stopping && (state->requests.empty() || !state->retry))
          break;
        request = std::move(state->requests.front());
        state->requests.pop_front();
        state->working = true;
      }
#ifdef PUNTO_TESTING
      if (state->before_io)
        state->before_io();
#endif
      const auto publication =
          request.mutation == PersistenceMutation::Refresh
              ? (store.refresh_from_file() ? Publication::Durable
                                           : Publication::NotPublished)
              : store.persist(request.mutation, request.word,
                              request.accepted_at);
      const bool ok = publication == Publication::Durable;
      {
        std::lock_guard lock{state->mutex};
        state->initialized = true;
        state->working = false;
        state->failed = !ok;
        if (ok) {
          state->completed_generation =
              std::max(state->completed_generation, request.generation);
        } else {
          state->failed_generation =
              std::max(state->failed_generation, request.generation);
        }
        if (publication != Publication::NotPublished) {
          state->storage_ready = true;
          state->exclusions.clear();
          bool cleared = false;
          for (auto pending = state->requests.rbegin();
               pending != state->requests.rend(); ++pending) {
            if (pending->mutation == PersistenceMutation::Clear) {
              cleared = true;
              break;
            }
            if (pending->mutation == PersistenceMutation::Add &&
                (!store.reset_in_current_boot_ ||
                 pending->accepted_at > store.reset_timestamp_))
              state->exclusions.insert(pending->word);
          }
          if (!cleared) {
            if ((request.mutation == PersistenceMutation::Add &&
                !store.last_add_skipped_) ||
                (request.mutation == PersistenceMutation::SyncDirectory &&
                 !request.word.empty() && !store.last_add_skipped_)) {
              state->exclusions.insert(request.word);
            }
            for (const auto &word : store.exclusions_) {
              if (state->exclusions.size() >= maximum_entries())
                break;
              state->exclusions.insert(word);
            }
          }
        }
        if (!ok) {
          if (publication == Publication::PublishedNotDurable)
            request.mutation = PersistenceMutation::SyncDirectory;
          const bool superseded = std::any_of(
              state->requests.begin(), state->requests.end(),
              [](const auto &pending) {
                return pending.mutation == PersistenceMutation::Clear;
              });
          if (!superseded)
            state->requests.push_front(std::move(request));
          state->retry = superseded;
        }
      }
      state->condition.notify_all();
      if (!ok)
        std::cerr << "[punto] Undo storage operation failed; cached learning "
                     "retained\n";
    }
  } catch (...) {
    std::lock_guard lock{state->mutex};
    state->initialized = true;
    state->working = false;
    state->failed = true;
    // A fatal worker failure makes every generation accepted so far
    // unfulfillable by this instance. Publish that terminal boundary so IPC
    // clients do not wait until their own timeout.
    state->failed_generation =
        std::max(state->failed_generation, state->generation);
  }
  {
    std::lock_guard lock{state->mutex};
    state->exited = true;
  }
  state->condition.notify_all();
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

bool UndoDetector::is_excluded(const std::string &word) const {
  std::lock_guard lock{state_->mutex};
  return state_->exclusions.contains(word);
}

std::size_t UndoDetector::exclusion_count() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->exclusions.size();
}

bool UndoDetector::ready() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->initialized;
}

bool UndoDetector::pending() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->working || !state_->requests.empty();
}

bool UndoDetector::persistence_failed() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->failed;
}

std::uint64_t UndoDetector::generation() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->generation;
}

std::uint64_t UndoDetector::completed_generation() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->completed_generation;
}

std::uint64_t UndoDetector::failed_generation() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->failed_generation;
}

UndoDetector::PersistenceSnapshot
UndoDetector::persistence_snapshot() const noexcept {
  std::lock_guard lock{state_->mutex};
  return PersistenceSnapshot{
      state_->initialized,
      state_->working || !state_->requests.empty(),
      state_->failed,
      state_->exclusions.size(),
      state_->generation,
      state_->completed_generation,
      state_->failed_generation,
  };
}

bool UndoDetector::valid_word(const std::string &word) noexcept {
  return !word.empty() && word.front() != '#' &&
         word.size() <= maximum_word_bytes() &&
         std::all_of(word.begin(), word.end(), [](char character) {
           return character != '\0' &&
                  std::find(kScancodeToChar.begin(), kScancodeToChar.end(),
                            character) != kScancodeToChar.end();
         });
}

bool UndoDetector::Store::refresh_from_file() {
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
  auto parsed = read_exclusions_at(directory_fd, path->name, security);
  const auto marker = read_reset_marker_at(directory_fd, path->name, security);
  (void)::close(lock_fd);
  (void)::close(directory_fd);
  if (!parsed || !marker) {
    return false;
  }
  exclusions_ = std::move(*parsed);
  reset_in_current_boot_ = marker->boot_id == boot_id_;
  reset_timestamp_ = reset_in_current_boot_ ? marker->timestamp : 0;
  return true;
}

void UndoDetector::load_from_file() {
  std::lock_guard lock{state_->mutex};
  if (state_->stopping || state_->exited)
    return;
  if (std::none_of(state_->requests.begin(), state_->requests.end(),
                   [](const auto &request) {
                     return request.mutation == PersistenceMutation::Refresh;
                   })) {
    state_->requests.push_back(
        {PersistenceMutation::Refresh, {}, ++state_->generation});
  }
  state_->retry = true;
  state_->condition.notify_all();
}

Publication UndoDetector::Store::persist(PersistenceMutation mutation,
                                         std::string_view word,
                                         std::uint64_t accepted_at) {
  last_add_skipped_ = false;
  const auto path = detail::split_runtime_path(file_path_);
  if (!path) {
    return Publication::NotPublished;
  }
  const RuntimeFileSecurity security = exclusion_security();
  const int directory_fd = detail::open_runtime_directory(*path, security);
  if (directory_fd < 0) {
    return Publication::NotPublished;
  }
  const int lock_fd =
      acquire_persistence_lock(directory_fd, path->name, security);
  if (lock_fd < 0) {
    (void)::close(directory_fd);
    return Publication::NotPublished;
  }

  const auto existing = read_exclusions_at(directory_fd, path->name, security);
  auto marker = read_reset_marker_at(directory_fd, path->name, security);
  if (!existing || (!marker && mutation != PersistenceMutation::Clear) ||
      (mutation == PersistenceMutation::Clear && boot_id_.empty())) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return Publication::NotPublished;
  }
  if (!marker) {
    marker = ResetMarker{};
  }
  reset_in_current_boot_ = marker->boot_id == boot_id_;
  reset_timestamp_ = reset_in_current_boot_ ? marker->timestamp : 0;

  if (mutation == PersistenceMutation::SyncDirectory) {
    if (!word.empty() && reset_in_current_boot_ && accepted_at != 0 &&
        accepted_at <= reset_timestamp_) {
      last_add_skipped_ = true;
      exclusions_ = *existing;
    }
    const bool synced = sync_directory(directory_fd) == 0;
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return synced ? Publication::Durable : Publication::NotPublished;
  }

  if (mutation == PersistenceMutation::Add &&
      marker->boot_id == boot_id_ && accepted_at != 0 &&
      accepted_at <= marker->timestamp) {
    last_add_skipped_ = true;
    exclusions_ = *existing;
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return Publication::Durable;
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
        return Publication::NotPublished;
      }
      next.insert(added_word);
    }
  }

  std::vector<std::string> words{next.begin(), next.end()};
  std::sort(words.begin(), words.end());
  std::string payload;
  for (const auto &word : words) {
    payload += word;
    payload.push_back('\n');
  }
  if (payload.size() > maximum_file_bytes()) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return Publication::NotPublished;
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
    return Publication::NotPublished;
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
  if (ok && mutation == PersistenceMutation::Clear) {
    const std::uint64_t effective_reset =
        reset_in_current_boot_ ? std::max(accepted_at, reset_timestamp_)
                               : accepted_at;
    ok = publish_reset_marker_at(directory_fd, path->name, security, boot_id_,
                                 effective_reset);
    if (ok) {
      reset_in_current_boot_ = true;
      reset_timestamp_ = effective_reset;
    }
  }
  Publication publication = Publication::NotPublished;
  if (!ok) {
    (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
  } else {
    exclusions_ = std::move(next);
    publication = sync_directory(directory_fd) == 0
                      ? Publication::Durable
                      : Publication::PublishedNotDurable;
  }
  (void)::close(lock_fd);
  (void)::close(directory_fd);
  return publication;
}

std::uint64_t UndoDetector::clear_exclusions() {
  std::lock_guard lock{state_->mutex};
  if (state_->stopping || state_->exited)
    return 0;
  state_->requests.clear();
  const std::uint64_t generation = ++state_->generation;
  state_->requests.push_back(
      {PersistenceMutation::Clear, {}, generation, monotonic_timestamp()});
  state_->exclusions.clear();
  state_->retry = true;
  state_->condition.notify_all();
  return generation;
}

void UndoDetector::add_exclusion(const std::string &word) {
  if (!valid_word(word)) {
    return;
  }
  std::lock_guard lock{state_->mutex};
  if (state_->stopping || state_->exited)
    return;
  if (!state_->storage_ready)
    return;
  if (!state_->exclusions.contains(word) &&
      state_->exclusions.size() < maximum_entries()) {
    state_->requests.push_back(
        {PersistenceMutation::Add, word, ++state_->generation,
         monotonic_timestamp()});
    state_->exclusions.insert(word);
  }
  state_->retry = true;
  state_->condition.notify_all();
}

} // namespace punto
