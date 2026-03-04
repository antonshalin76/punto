/**
 * @file macro_lock.cpp
 * @brief Реализация межпроцессной блокировки макросов
 */

#include "punto/macro_lock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

namespace punto {

MacroLock::MacroLock() = default;

MacroLock::~MacroLock() {
  if (locked_) {
    unlock();
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

  // O_CREAT | O_RDWR: создаём файл если не существует.
  // Файл НЕ удаляется — stale lock файлы безопасны для flock().
  fd_ = ::open(kLockPath, O_CREAT | O_RDWR, 0666);
  if (fd_ < 0) {
    const int err = errno;
    std::cerr << "[punto] MacroLock: failed to open " << kLockPath << ": "
              << std::strerror(err) << "\n";
    return false;
  }

  return true;
}

bool MacroLock::try_lock(std::chrono::milliseconds timeout) {
  if (locked_) {
    // Уже захвачена (реентрантно в рамках одного процесса).
    return true;
  }

  if (!ensure_fd()) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();

  // Сначала пробуем неблокирующий захват.
  if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
    locked_ = true;
    return true;
  }

  if (errno != EWOULDBLOCK) {
    std::cerr << "[punto] MacroLock: flock failed: " << std::strerror(errno)
              << "\n";
    return false;
  }

  // Ретраим с короткими паузами до таймаута.
  constexpr auto kRetryInterval = std::chrono::milliseconds{5};

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now - start >= timeout) {
      std::cerr << "[punto] MacroLock: timeout after "
                << std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                        start)
                       .count()
                << "ms\n";
      return false;
    }

    std::this_thread::sleep_for(kRetryInterval);

    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
      locked_ = true;
      return true;
    }

    if (errno != EWOULDBLOCK) {
      std::cerr << "[punto] MacroLock: flock retry failed: "
                << std::strerror(errno) << "\n";
      return false;
    }
  }
}

void MacroLock::unlock() {
  if (!locked_ || fd_ < 0) {
    return;
  }

  if (::flock(fd_, LOCK_UN) != 0) {
    std::cerr << "[punto] MacroLock: unlock failed: " << std::strerror(errno)
              << "\n";
  }
  locked_ = false;
}

} // namespace punto
