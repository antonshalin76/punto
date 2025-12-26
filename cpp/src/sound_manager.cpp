/**
 * @file sound_manager.cpp
 * @brief Реализация SoundManager
 */

#include "punto/sound_manager.hpp"

#include "punto/config.hpp"
#include "punto/x11_session.hpp"

#include <fcntl.h>
#include <grp.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace punto {

namespace {

inline constexpr const char *kSoundEnToRu =
    "/usr/share/punto-switcher/sounds/en_ru.wav";
inline constexpr const char *kSoundRuToEn =
    "/usr/share/punto-switcher/sounds/ru_en.wav";

inline constexpr const char *kPaplayPath = "/usr/bin/paplay";
inline constexpr const char *kAplayPath = "/usr/bin/aplay";

[[nodiscard]] bool is_executable(const char *path) {
  return ::access(path, X_OK) == 0;
}

[[nodiscard]] std::vector<gid_t> get_user_groups(const std::string &username,
                                                gid_t primary_gid) {
  std::vector<gid_t> groups;

  // Стартуем с небольшого буфера и увеличиваем при необходимости.
  int ngroups = 16;
  groups.resize(static_cast<std::size_t>(ngroups));

  while (true) {
    int tmp = ngroups;
    int ret = ::getgrouplist(username.c_str(), primary_gid, groups.data(), &tmp);

    if (ret >= 0) {
      // tmp = фактическое число групп
      groups.resize(static_cast<std::size_t>(tmp));
      return groups;
    }

    // tmp = требуемое число групп
    if (tmp <= 0) {
      groups.clear();
      return groups;
    }

    ngroups = tmp;
    groups.resize(static_cast<std::size_t>(ngroups));
  }
}

} // namespace

SoundManager::SoundManager(const X11Session &x11_session, const SoundConfig &config)
    : x11_session_{x11_session}, enabled_{config.enabled} {

  devnull_fd_ = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  if (devnull_fd_ < 0) {
    std::cerr << "[punto] Sound: failed to open /dev/null: " << std::strerror(errno)
              << "\n";
  }

  if (is_executable(kPaplayPath)) {
    player_path_ = kPaplayPath;
  } else if (is_executable(kAplayPath)) {
    player_path_ = kAplayPath;
  } else {
    std::cerr << "[punto] Sound: neither paplay nor aplay found. "
                 "Sound will be disabled.\n";
    enabled_.store(false, std::memory_order_relaxed);
    return;
  }

  if (!x11_session_.is_valid()) {
    std::cerr << "[punto] Sound: X11 session not initialized. "
                 "Sound may be unavailable.\n";
    return;
  }

  const X11SessionInfo info = x11_session_.info();

  user_uid_ = static_cast<uid_t>(info.uid);
  user_gid_ = static_cast<gid_t>(info.gid);

  if (!info.username.empty() && user_gid_ != 0) {
    user_groups_ = get_user_groups(info.username, user_gid_);
  }

  // Готовим окружение для execve.
  env_.reserve(8);

  if (!info.home_dir.empty()) {
    env_.push_back("HOME=" + info.home_dir);
  }
  if (!info.username.empty()) {
    env_.push_back("USER=" + info.username);
    env_.push_back("LOGNAME=" + info.username);
  }
  if (!info.xdg_runtime_dir.empty()) {
    env_.push_back("XDG_RUNTIME_DIR=" + info.xdg_runtime_dir);
  }
  if (!info.display.empty()) {
    env_.push_back("DISPLAY=" + info.display);
  }
  if (!info.xauthority_path.empty()) {
    env_.push_back("XAUTHORITY=" + info.xauthority_path);
  }

  envp_.reserve(env_.size() + 1);
  for (auto &e : env_) {
    envp_.push_back(e.data());
  }
  envp_.push_back(nullptr);
}

SoundManager::~SoundManager() {
  if (devnull_fd_ >= 0) {
    ::close(devnull_fd_);
  }
}

void SoundManager::set_enabled(bool enabled) noexcept {
  enabled_.store(enabled, std::memory_order_relaxed);
}

void SoundManager::play_for_layout(int new_layout) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  if (player_path_.empty()) {
    return;
  }
  if (!x11_session_.is_valid()) {
    return;
  }

  if (new_layout == 1) {
    play_file(kSoundEnToRu);
  } else if (new_layout == 0) {
    play_file(kSoundRuToEn);
  }
}

void SoundManager::play_file(const char *wav_path) {
  if (!enabled_.load(std::memory_order_relaxed)) {
    return;
  }
  if (player_path_.empty()) {
    return;
  }
  if (envp_.empty()) {
    return;
  }

  // Готовим argv в родителе (в дочернем процессе никаких аллокаций/iostream).
  std::array<char *, 3> argv{
      player_path_.data(),
      const_cast<char *>(wav_path),
      nullptr,
  };

  pid_t pid = ::fork();
  if (pid < 0) {
    std::cerr << "[punto] Sound: fork() failed: " << std::strerror(errno) << "\n";
    return;
  }

  if (pid == 0) {
    // Промежуточный процесс
    pid_t pid2 = ::fork();
    if (pid2 < 0) {
      _exit(1);
    }
    if (pid2 > 0) {
      _exit(0);
    }

    // Финальный процесс: сбрасываем stdio в /dev/null
    if (devnull_fd_ >= 0) {
      (void)::dup2(devnull_fd_, STDIN_FILENO);
      (void)::dup2(devnull_fd_, STDOUT_FILENO);
      (void)::dup2(devnull_fd_, STDERR_FILENO);
    }

    // Сбрасываем группы/UID/GID (важно делать до exec).
    if (!user_groups_.empty()) {
      (void)::setgroups(user_groups_.size(), user_groups_.data());
    }

    if (::setgid(user_gid_) != 0) {
      _exit(1);
    }
    if (::setuid(user_uid_) != 0) {
      _exit(1);
    }

    ::execve(player_path_.c_str(), argv.data(), envp_.data());
    _exit(127);
  }

  // Родитель: ждём ТОЛЬКО промежуточный процесс, чтобы не оставлять зомби.
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    break;
  }
}

} // namespace punto
