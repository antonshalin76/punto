/**
 * @file x11_session.hpp
 * @brief Управление X11 сессией из root-контекста
 *
 * Решает проблему доступа к X11 от root (udevmon запускает процессы от root).
 * Находит активную GUI сессию и получает DISPLAY/XAUTHORITY.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <sys/types.h>

namespace punto {

/**
 * @brief Информация о GUI сессии пользователя
 */
struct X11SessionInfo {
  // Идентификатор логин-сессии systemd (loginctl), если доступен.
  // Используется для детекта смены сессии после логина/логаута.
  std::string session_id;

  std::string username;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::string display;         // e.g., ":0"
  std::string xauthority_path; // e.g., "/run/user/1000/gdm/Xauthority"
  std::string home_dir;
  std::string xdg_runtime_dir; // e.g., "/run/user/1000"
};

/**
 * @brief Менеджер X11 сессии
 *
 * Находит активную X11 сессию пользователя и предоставляет
 * необходимые переменные окружения для взаимодействия с X сервером.
 */
class X11Session {
public:
  enum class RefreshResult {
    Unchanged,
    Updated,
    Invalidated,
  };

  /**
   * @brief Инициализирует сессию, находя активного GUI пользователя
   * @return true если сессия найдена и инициализирована
   */
  bool initialize();

  /**
   * @brief Переопределяет активную GUI-сессию и при необходимости переинициализирует
   *
   * Используется, чтобы не «прилипать» к greeter (gdm/lightdm) на этапе boot
   * и корректно переживать logout/login.
   */
  [[nodiscard]] RefreshResult refresh();

  /**
   * @brief Сбрасывает состояние (сессия считается невалидной)
   */
  void reset() noexcept;

  /**
   * @brief Проверяет, инициализирована ли сессия
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Возвращает снапшот информации о сессии (thread-safe)
   */
  [[nodiscard]] X11SessionInfo info() const;

  /**
   * @brief Устанавливает переменные окружения для X11 операций
   *
   * Вызывается перед операциями с X11 API.
   * Устанавливает DISPLAY и XAUTHORITY.
   */
  void apply_environment() const;

  /**
   * @brief Переключает эффективный UID на пользователя сессии
   * @return true если успешно (или если мы уже этот пользователь)
   */
  bool switch_to_user() const;

  /**
   * @brief Возвращает эффективный UID к root
   */
  bool switch_to_root() const;

  /**
   * @brief Определяет текущую раскладку клавиатуры
   * @return 0 = английская (первая), 1 = русская (вторая), -1 = ошибка
   */
  [[nodiscard]] int get_current_keyboard_layout() const;

  /**
   * @brief Устанавливает текущую раскладку клавиатуры
   * @param index 0 = первая (EN), 1 = вторая (RU)
   * @return true если успешно
   */
  bool set_keyboard_layout(int index) const;

private:
  struct ActiveSession {
    std::string session_id;
    std::string username;
    std::string leader_pid;
  };

  /**
   * @brief Находит активную user-сессию на seat0 через loginctl
   */
  std::optional<ActiveSession> find_active_session_loginctl();

  /**
   * @brief Fallback: находит пользователя (who/tty1)
   */
  std::optional<std::string> find_active_user_fallback();

  /**
   * @brief Находит DISPLAY/XAUTHORITY из /proc/<pid>/environ
   */
  bool find_session_env_by_pid(const std::string &pid, X11SessionInfo &out);

  /**
   * @brief Fallback: ищем env по процессам пользователя
   */
  bool find_session_env_by_user(const std::string &username, X11SessionInfo &out);

  /**
   * @brief Проверяет доступ к X серверу (минимальный healthcheck)
   */
  [[nodiscard]] bool verify_x11_access() const;

  mutable std::mutex mu_;
  X11SessionInfo info_;
  std::atomic<bool> initialized_{false};
  uid_t original_uid_ = 0;
  gid_t original_gid_ = 0;
};

} // namespace punto
