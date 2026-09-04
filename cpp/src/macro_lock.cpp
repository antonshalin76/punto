/**
 * @file macro_lock.cpp
 * @brief Реализация межпроцессной блокировки макросов
 */

#include "punto/macro_lock.hpp"
#include "punto/runtime_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace punto {

MacroLock::MacroLock(std::string path) : path_{std::move(path)} {}

MacroLock::~MacroLock() {
  std::lock_guard<std::mutex> lock{mutex_};
  if (recursion_depth_ > 0 && fd_ >= 0) {
    (void)::flock(fd_, LOCK_UN);
    recursion_depth_ = 0;
    owner_ = {};
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool MacroLock::ensure_fd() {
  if (fd_ >= 0) {
    return true;
  }

  const RuntimeFileSecurity security = default_runtime_file_security();
  do {
    fd_ = ::open(path_.c_str(),
                 O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                 security.mode);
  } while (fd_ < 0 && errno == EINTR);
  if (fd_ < 0) {
    const int err = errno;
    std::cerr << "[punto] MacroLock: failed to open " << path_ << ": "
              << std::strerror(err) << "\n";
    return false;
  }

  if (!apply_runtime_file_security(fd_, security)) {
    std::cerr << "[punto] MacroLock: unsafe runtime file " << path_ << "\n";
    (void)::close(fd_);
    fd_ = -1;
    return false;
  }
  return true;
}

bool MacroLock::try_lock(std::chrono::milliseconds timeout) {
  timeout = std::max(timeout, std::chrono::milliseconds::zero());
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock<std::mutex> guard{mutex_};
  const std::thread::id caller = std::this_thread::get_id();
  if (recursion_depth_ > 0 && owner_ == caller) {
    ++recursion_depth_;
    return true;
  }
  while (recursion_depth_ > 0) {
    if (released_.wait_until(guard, deadline) == std::cv_status::timeout) {
      return false;
    }
  }

  if (!ensure_fd()) {
    return false;
  }

  // Сначала пробуем неблокирующий захват.
  int lock_rc = -1;
  do {
    lock_rc = ::flock(fd_, LOCK_EX | LOCK_NB);
  } while (lock_rc != 0 && errno == EINTR);
  if (lock_rc == 0) {
    owner_ = caller;
    recursion_depth_ = 1;
    return true;
  }

  if (errno != EWOULDBLOCK && errno != EAGAIN) {
    std::cerr << "[punto] MacroLock: flock failed: " << std::strerror(errno)
              << "\n";
    return false;
  }

  // Ретраим с короткими паузами до таймаута.
  constexpr auto kRetryInterval = std::chrono::milliseconds{5};

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      std::cerr << "[punto] MacroLock: timeout after " << timeout.count()
                << "ms\n";
      return false;
    }

    const auto retry_interval =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kRetryInterval);
    std::this_thread::sleep_for(std::min(retry_interval, deadline - now));

    do {
      lock_rc = ::flock(fd_, LOCK_EX | LOCK_NB);
    } while (lock_rc != 0 && errno == EINTR);
    if (lock_rc == 0) {
      owner_ = caller;
      recursion_depth_ = 1;
      return true;
    }

    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      std::cerr << "[punto] MacroLock: flock retry failed: "
                << std::strerror(errno) << "\n";
      return false;
    }
  }
}

void MacroLock::unlock() {
  std::unique_lock<std::mutex> guard{mutex_};
  if (recursion_depth_ == 0 || fd_ < 0) {
    return;
  }
  if (owner_ != std::this_thread::get_id()) {
    std::cerr << "[punto] MacroLock: unlock attempted by non-owner thread\n";
    return;
  }
  --recursion_depth_;
  if (recursion_depth_ > 0) {
    return;
  }

  int rc = -1;
  do {
    rc = ::flock(fd_, LOCK_UN);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) {
    std::cerr << "[punto] MacroLock: unlock failed: " << std::strerror(errno)
              << "\n";
    recursion_depth_ = 1;
    return;
  }
  owner_ = {};
  guard.unlock();
  released_.notify_all();
}

bool MacroLock::is_locked() const noexcept {
  std::lock_guard<std::mutex> lock{mutex_};
  return recursion_depth_ > 0;
}

} // namespace punto
