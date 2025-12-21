/**
 * @file x11_session.hpp
 * @brief Управление X11 сессией из root-контекста
 *
 * Решает проблему доступа к X11 от root (udevmon запускает процессы от root).
 * Находит активную GUI сессию и получает DISPLAY/XAUTHORITY.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace punto {

/**
 * @brief Информация о GUI сессии пользователя
 */
struct X11SessionInfo {
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
  /**
   * @brief Инициализирует сессию, находя активного GUI пользователя
   * @return true если сессия найдена и инициализирована
   */
  bool initialize();

  /**
   * @brief Проверяет, инициализирована ли сессия
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Возвращает информацию о сессии
   */
  [[nodiscard]] const X11SessionInfo &info() const noexcept;

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
  /**
   * @brief Находит GUI пользователя через loginctl
   */
  std::optional<std::string> find_active_user();

  /**
   * @brief Находит DISPLAY/XAUTHORITY из /proc/<pid>/environ
   */
  bool find_session_env(const std::string &username);

  X11SessionInfo info_;
  bool initialized_ = false;
  uid_t original_uid_ = 0;
  gid_t original_gid_ = 0;
};

} // namespace punto
