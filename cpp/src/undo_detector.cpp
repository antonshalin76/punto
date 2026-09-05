#include "punto/undo_detector.hpp"

#include "punto/control_plane_state.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <atomic>
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
  const auto payload = detail::read_bounded(fd, UndoDetector::maximum_file_bytes());
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
  std::string file_path_;
  std::unordered_set<std::string> exclusions_;
  bool refresh_from_file();
  Publication persist(PersistenceMutation mutation, std::string_view word);
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
  };
  explicit SharedState(std::string path) : path{std::move(path)} {}
  std::string path;
  std::mutex mutex;
  std::condition_variable condition;
  std::deque<Request> requests{{PersistenceMutation::Refresh, {}}};
  std::unordered_set<std::string> exclusions;
  bool initialized = false;
  bool storage_ready = false;
  bool working = false;
  bool failed = false;
  bool retry = true;
  bool stopping = false;
  bool exited = false;
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
  if (!thread_.joinable())
    return;
  bool exited = false;
  {
    std::unique_lock lock{state_->mutex};
    state_->stopping = true;
    state_->condition.notify_all();
    exited = state_->condition.wait_for(lock, std::chrono::milliseconds{2500},
                                        [&] { return state_->exited; });
  }
  if (exited)
    thread_.join();
  else {
    thread_.detach();
    std::cerr << "[punto] Undo storage shutdown timed out; persistence may be "
                 "pending\n";
  }
}

void UndoDetector::worker(const std::shared_ptr<SharedState> &state) noexcept {
  try {
    Store store{state->path, {}};
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
              : store.persist(request.mutation, request.word);
      const bool ok = publication == Publication::Durable;
      {
        std::lock_guard lock{state->mutex};
        state->initialized = true;
        state->working = false;
        state->failed = !ok;
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
            if (pending->mutation == PersistenceMutation::Add)
              state->exclusions.insert(pending->word);
          }
          if (!cleared) {
            if (request.mutation == PersistenceMutation::Add ||
                (request.mutation == PersistenceMutation::SyncDirectory &&
                 !request.word.empty()))
              state->exclusions.insert(request.word);
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

bool UndoDetector::valid_word(const std::string &word) noexcept {
  return !word.empty() && word.front() != '#' && word.size() <= maximum_word_bytes() &&
         std::all_of(word.begin(), word.end(), [](char character) {
           return character != '\0' &&
                  std::find(kScancodeToChar.begin(), kScancodeToChar.end(), character) !=
                      kScancodeToChar.end();
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
  auto parsed = read_exclusions_at(directory_fd, path->name, security);
  (void)::close(directory_fd);
  if (!parsed) {
    return false;
  }
  exclusions_ = std::move(*parsed);
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
    state_->requests.push_back({PersistenceMutation::Refresh, {}});
  }
  state_->retry = true;
  state_->condition.notify_all();
}

Publication UndoDetector::Store::persist(PersistenceMutation mutation,
                                         std::string_view word) {
  const auto path = detail::split_runtime_path(file_path_);
  if (!path) {
    return Publication::NotPublished;
  }
  const RuntimeFileSecurity security = exclusion_security();
  const int directory_fd = detail::open_runtime_directory(*path, security);
  if (directory_fd < 0) {
    return Publication::NotPublished;
  }
  if (mutation == PersistenceMutation::SyncDirectory) {
    const bool synced = sync_directory(directory_fd) == 0;
    (void)::close(directory_fd);
    return synced ? Publication::Durable : Publication::NotPublished;
  }
  const int lock_fd =
      acquire_persistence_lock(directory_fd, path->name, security);
  if (lock_fd < 0) {
    (void)::close(directory_fd);
    return Publication::NotPublished;
  }

  const auto existing = read_exclusions_at(directory_fd, path->name, security);
  if (!existing) {
    (void)::close(lock_fd);
    (void)::close(directory_fd);
    return Publication::NotPublished;
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

void UndoDetector::clear_exclusions() {
  std::lock_guard lock{state_->mutex};
  if (state_->stopping || state_->exited)
    return;
  state_->requests.clear();
  state_->requests.push_back({PersistenceMutation::Clear, {}});
  state_->exclusions.clear();
  state_->retry = true;
  state_->condition.notify_all();
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
    state_->requests.push_back({PersistenceMutation::Add, word});
    state_->exclusions.insert(word);
  }
  state_->retry = true;
  state_->condition.notify_all();
}

} // namespace punto
