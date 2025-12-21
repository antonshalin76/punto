/**
 * @file sound_manager.hpp
 * @brief Звуковая индикация переключения раскладки
 *
 * Реализация построена так, чтобы не блокировать критический путь ввода
 * и не использовать fork()+exec в многопоточном контексте небезопасным образом.
 *
 * Подход:
 * - подготовка аргументов/окружения в родителе
 * - double-fork: родитель ждёт только промежуточного процесса (быстро),
 *   финальный плеер становится сиротой и не оставляет zombie
 */

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <sys/types.h>

namespace punto {

class X11Session;
struct SoundConfig;

class SoundManager {
public:
  SoundManager(const X11Session &x11_session, const SoundConfig &config);
  ~SoundManager();

  SoundManager(const SoundManager &) = delete;
  SoundManager &operator=(const SoundManager &) = delete;

  void set_enabled(bool enabled) noexcept;

  /// @param new_layout 0 = EN, 1 = RU
  void play_for_layout(int new_layout);

private:
  void play_file(const char *wav_path);

  const X11Session &x11_session_;
  std::atomic<bool> enabled_{true};

  // Определяется один раз при старте (paplay -> aplay).
  std::string player_path_;

  // UID/GID активного пользователя (берём из X11Session на старте)
  uid_t user_uid_ = 0;
  gid_t user_gid_ = 0;

  // Группы пользователя (для aplay/ALSA может быть важно)
  std::vector<gid_t> user_groups_;

  // Подготовленное окружение для execve (валидно, пока жив объект)
  std::vector<std::string> env_;
  std::vector<char *> envp_;

  // /dev/null для перенаправления stdin/stdout/stderr дочернему процессу
  int devnull_fd_ = -1;
};

} // namespace punto
